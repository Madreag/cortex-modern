#include "Writer.h"
#include "System.h"
#include "CheckpointArchive.h"
#include "Base64/base64.h"
#include "SceneLayer.h"

#include <iomanip>
#include <fstream>
#include <mutex>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <atomic>
#include <unordered_set>
#include <utility>
#include <future>
#include <iostream>

using namespace RTE;

namespace {
	enum class CaptureValue : uint8_t { Raw, Integer, Unsigned, SpacedInteger, SpacedUnsigned, Float, Double, String, Child, SizedChild, Base64, UrlBase64, GraphString, NewLine, Property };
	template<class T> T ReadCaptureValue(std::string_view values, size_t& cursor) {
		if (sizeof(T) > values.size() - cursor) throw std::logic_error("truncated owned checkpoint values");
		T value;
		std::memcpy(&value, values.data() + cursor, sizeof(value));
		cursor += sizeof(value);
		return value;
	}
	std::string_view ReadCaptureString(std::string_view values, size_t& cursor) {
		const uint64_t size = ReadCaptureValue<uint64_t>(values, cursor);
		if (size > values.size() - cursor) throw std::logic_error("truncated owned checkpoint string");
		const std::string_view text = values.substr(cursor, static_cast<size_t>(size));
		cursor += static_cast<size_t>(size);
		return text;
	}
	template<class T> void AppendCaptureNumber(std::string& text, T number) {
		char buffer[64];
		const auto result = [&] {
			if constexpr (std::is_floating_point_v<T>) return ToCharsExact(buffer, buffer + sizeof(buffer), number);
			else return std::to_chars(buffer, buffer + sizeof(buffer), number);
		}();
		if (result.ec != std::errc{}) throw std::runtime_error("could not format checkpoint value");
		text.append(buffer, result.ptr);
	}
}

struct CheckpointText::Data {
	struct Pair {
		const Data* current;
		const Data* previous;
		bool operator==(const Pair&) const = default;
	};
	struct PairHash {
		size_t operator()(const Pair& pair) const {
			const size_t first = std::hash<const Data*>{}(pair.current), second = std::hash<const Data*>{}(pair.previous);
			return first ^ (second + 0x9e3779b9u + (first << 6) + (first >> 2));
		}
	};
	std::string values;
	std::vector<CheckpointText> children;
	std::function<std::string()> produce;
	std::string identity;
	size_t ownedBytes = 0;
	mutable std::once_flag ready;
	mutable std::atomic<bool> formatted{false};
	mutable std::string text;
	std::shared_ptr<Data> drainNext;

	~Data() {
		std::shared_ptr<Data> pending;
		const auto enqueue = [&pending](Data& node) {
			for (auto& child: node.children) {
				if (child.m_Data && child.m_Data.use_count() == 1) {
					// Only unshared children enter the destruction chain.
					child.m_Data->drainNext = std::move(pending);
					pending = std::move(child.m_Data);
				} else child.m_Data.reset();
			}
		};
		enqueue(*this);
		while (pending) {
			auto node = std::move(pending);
			pending = std::move(node->drainNext);
			enqueue(*node);
			node.reset();
		}
	}
};

CheckpointText::CheckpointText() = default;

CheckpointText::CheckpointText(std::string text) {
	CheckpointBuffer buffer;
	buffer.Raw(text);
	*this = buffer.Finish();
}

CheckpointText CheckpointText::Deferred(std::function<std::string()> produce, size_t ownedBytes, std::string identity) {
	auto data = std::make_shared<Data>();
	data->produce = std::move(produce);
	data->ownedBytes = ownedBytes;
	data->identity = std::move(identity);
	return CheckpointText(std::move(data));
}

CheckpointText CheckpointText::Base64(bool url) const {
	CheckpointBuffer buffer;
	buffer.Base64(*this, url);
	return buffer.Finish();
}

size_t CheckpointText::OwnedBytes() const { return m_Data ? m_Data->ownedBytes : 0; }

bool CheckpointText::SameValues(const CheckpointText& other) const {
	if (m_Data == other.m_Data) return true;
	if (!m_Data || !other.m_Data) return false;
	if (m_Data->produce || other.m_Data->produce) return m_Data->produce && other.m_Data->produce && !m_Data->identity.empty() && m_Data->identity == other.m_Data->identity;
	if (m_Data->children.empty() || other.m_Data->children.empty()) return m_Data->values == other.m_Data->values && m_Data->children.size() == other.m_Data->children.size();
	std::vector<Data::Pair> pending{{m_Data.get(), other.m_Data.get()}};
	std::unordered_set<Data::Pair, Data::PairHash> seen;
	while (!pending.empty()) {
		const auto [current, previous] = pending.back();
		pending.pop_back();
		if (current == previous) continue;
		if (!current || !previous) return false;
		if (current->produce || previous->produce) {
			if (!current->produce || !previous->produce || current->identity.empty() || current->identity != previous->identity) return false;
			continue;
		}
		if (current->values != previous->values || current->children.size() != previous->children.size()) return false;
		if (!seen.insert({current, previous}).second) continue;
		for (size_t index = current->children.size(); index > 0; --index) pending.push_back({current->children[index - 1].m_Data.get(), previous->children[index - 1].m_Data.get()});
	}
	return true;
}

CheckpointText CheckpointText::ReuseChildren(const CheckpointText& previous) const {
	if (m_Data == previous.m_Data) return previous;
	if (!m_Data || !previous.m_Data) return *this;
	if (m_Data->produce || previous.m_Data->produce) return SameValues(previous) ? previous : *this;
	if (m_Data->children.empty() || previous.m_Data->children.empty()) return SameValues(previous) ? previous : *this;
	struct Result { bool equal; std::shared_ptr<Data> value; };
	struct Frame {
		std::shared_ptr<Data> current, previous;
		size_t next = 0;
		bool entered = false, equal = false, changed = false;
	};
	std::unordered_map<Data::Pair, Result, Data::PairHash> results;
	std::vector<Frame> pending{{m_Data, previous.m_Data}};
	while (!pending.empty()) {
		Frame& frame = pending.back();
		const Data::Pair pair{frame.current.get(), frame.previous.get()};
		if (!frame.entered) {
			if (frame.current == frame.previous) {
				results.emplace(pair, Result{true, frame.previous}); pending.pop_back(); continue;
			}
			if (!frame.current || !frame.previous) {
				results.emplace(pair, Result{false, frame.current}); pending.pop_back(); continue;
			}
			if (frame.current->produce || frame.previous->produce) {
				const bool equal = frame.current->produce && frame.previous->produce && !frame.current->identity.empty() && frame.current->identity == frame.previous->identity;
				results.emplace(pair, Result{equal, equal ? frame.previous : frame.current}); pending.pop_back(); continue;
			}
			frame.equal = frame.current->values == frame.previous->values && frame.current->children.size() == frame.previous->children.size();
			frame.entered = true;
		}
		const size_t count = std::min(frame.current->children.size(), frame.previous->children.size());
		if (frame.next < count) {
			const auto& currentChild = frame.current->children[frame.next].m_Data;
			const auto& previousChild = frame.previous->children[frame.next].m_Data;
			const auto child = results.find({currentChild.get(), previousChild.get()});
			if (child == results.end()) { pending.push_back({currentChild, previousChild}); continue; }
			frame.equal = frame.equal && child->second.equal;
			frame.changed = frame.changed || child->second.value != currentChild;
			++frame.next;
			continue;
		}
		std::shared_ptr<Data> value = frame.equal ? frame.previous : frame.current;
		if (!frame.equal && frame.changed) {
			value = std::make_shared<Data>();
			value->values = frame.current->values;
			value->children = frame.current->children;
			value->ownedBytes = frame.current->ownedBytes;
			for (size_t index = 0; index < count; ++index) {
				const Data::Pair child{frame.current->children[index].m_Data.get(), frame.previous->children[index].m_Data.get()};
				value->children[index] = CheckpointText(results.at(child).value);
			}
		}
		results.emplace(pair, Result{frame.equal, std::move(value)});
		pending.pop_back();
	}
	return CheckpointText(results.at({m_Data.get(), previous.m_Data.get()}).value);
}

const std::string& CheckpointText::Text() const {
	static const std::string empty;
	if (!m_Data) return empty;
	const auto root = m_Data;
	if (root->formatted.load(std::memory_order_acquire)) return root->text;
	struct Frame { const Data* node; size_t next = 0; };
	std::vector<Frame> pending{{root.get()}};
	while (!pending.empty()) {
		Frame& frame = pending.back();
		const Data* node = frame.node;
		if (node->formatted.load(std::memory_order_acquire)) { pending.pop_back(); continue; }
		if (!node->produce && frame.next < node->children.size()) {
			const Data* child = node->children[frame.next++].m_Data.get();
			if (child && !child->formatted.load(std::memory_order_acquire)) pending.push_back({child});
			continue;
		}
		std::call_once(node->ready, [node] {
			if (node->produce) {
				node->text = node->produce();
				node->formatted.store(true, std::memory_order_release);
				return;
			}
			std::string text;
			const std::string_view values = node->values;
			text.reserve(values.size());
			size_t cursor = 0;
			while (cursor < values.size()) {
				const auto kind = ReadCaptureValue<CaptureValue>(values, cursor);
				switch (kind) {
					case CaptureValue::Raw: text += ReadCaptureString(values, cursor); break;
					case CaptureValue::Integer:
					case CaptureValue::SpacedInteger:
						AppendCaptureNumber(text, ReadCaptureValue<int64_t>(values, cursor));
						if (kind == CaptureValue::SpacedInteger) text.push_back(' ');
						break;
					case CaptureValue::Unsigned:
					case CaptureValue::SpacedUnsigned:
						AppendCaptureNumber(text, ReadCaptureValue<uint64_t>(values, cursor));
						if (kind == CaptureValue::SpacedUnsigned) text.push_back(' ');
						break;
					case CaptureValue::Float: AppendCaptureNumber(text, ReadCaptureValue<float>(values, cursor)); break;
					case CaptureValue::Double: AppendCaptureNumber(text, ReadCaptureValue<double>(values, cursor)); break;
					case CaptureValue::String: {
						const auto string = ReadCaptureString(values, cursor);
						AppendCaptureNumber(text, string.size()); text.push_back(' '); text += string; text.push_back(' ');
						break;
					}
					case CaptureValue::Child:
					case CaptureValue::SizedChild:
					case CaptureValue::Base64:
					case CaptureValue::UrlBase64:
					case CaptureValue::GraphString: {
						const auto index = ReadCaptureValue<uint64_t>(values, cursor);
						const auto& data = node->children.at(static_cast<size_t>(index)).m_Data;
						const std::string& child = data ? data->text : empty;
						if (kind == CaptureValue::SizedChild) { AppendCaptureNumber(text, child.size()); text.push_back(' '); }
						if (kind == CaptureValue::GraphString) { text.push_back('s'); AppendCaptureNumber(text, child.size()); text.push_back(':'); }
						if (kind == CaptureValue::Base64 || kind == CaptureValue::UrlBase64) text += base64_encode(child, kind == CaptureValue::UrlBase64);
						else text += child;
						if (kind == CaptureValue::SizedChild) text.push_back(' ');
						break;
					}
					case CaptureValue::NewLine: {
						const int indent = ReadCaptureValue<int>(values, cursor), count = ReadCaptureValue<int>(values, cursor);
						for (int i = 0; i < count; ++i) { text.push_back('\n'); if (indent > 0) text.append(static_cast<size_t>(indent), '\t'); }
						break;
					}
					case CaptureValue::Property: {
						const int indent = ReadCaptureValue<int>(values, cursor);
						text.push_back('\n'); text.append(static_cast<size_t>(indent), '\t'); text += ReadCaptureString(values, cursor); text += " = ";
						break;
					}
					default: throw std::logic_error("unknown owned checkpoint value");
				}
			}
			node->text = std::move(text);
			node->formatted.store(true, std::memory_order_release);
		});
		pending.pop_back();
	}
	return root->text;
}

void CheckpointBuffer::Raw(std::string_view text) { Copy(CaptureValue::Raw); Copy(static_cast<uint64_t>(text.size())); m_Values.append(text); }
void CheckpointBuffer::Integer(int64_t value, bool space) { Copy(space ? CaptureValue::SpacedInteger : CaptureValue::Integer); Copy(value); }
void CheckpointBuffer::Unsigned(uint64_t value, bool space) { Copy(space ? CaptureValue::SpacedUnsigned : CaptureValue::Unsigned); Copy(value); }
void CheckpointBuffer::Real(float value) { Copy(CaptureValue::Float); Copy(value); }
void CheckpointBuffer::Real(double value) { Copy(CaptureValue::Double); Copy(value); }
void CheckpointBuffer::String(std::string_view value) { Copy(CaptureValue::String); Copy(static_cast<uint64_t>(value.size())); m_Values.append(value); }
void CheckpointBuffer::Child(const CheckpointText& value, bool sized) { Copy(sized ? CaptureValue::SizedChild : CaptureValue::Child); Copy(static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::Base64(const CheckpointText& value, bool url) { Copy(url ? CaptureValue::UrlBase64 : CaptureValue::Base64); Copy(static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::GraphString(const CheckpointText& value) { Copy(CaptureValue::GraphString); Copy(static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::NewLine(int indent, int count) { Copy(CaptureValue::NewLine); Copy(indent); Copy(count); }
void CheckpointBuffer::Property(std::string_view name, int indent) { Copy(CaptureValue::Property); Copy(indent); Copy(static_cast<uint64_t>(name.size())); m_Values.append(name); }

CheckpointText CheckpointBuffer::Finish() {
	auto data = std::make_shared<CheckpointText::Data>();
	data->values = std::move(m_Values);
	data->children = std::move(m_Children);
	data->ownedBytes = data->values.size();
	for (const auto& child: data->children) data->ownedBytes += child.OwnedBytes();
	return CheckpointText(std::move(data));
}

CheckpointText CheckpointCache::Remember(const void* owner, unsigned channel, CheckpointText value) {
	return Remember(owner, channel, std::move(value), 0);
}

CheckpointText CheckpointCache::Remember(const void* owner, unsigned channel, CheckpointText value, uint64_t stamp) {
	Entry& entry = m_Entries[owner][channel];
	++m_Touched;
	entry.generation = m_Generation;
	entry.stamp = stamp;
	if (entry.text.SameValues(value)) { ++m_Reused; m_Retired.push_back(std::move(value)); return entry.text; }
	m_Retired.push_back(value);
	value = value.ReuseChildren(entry.text);
	m_Retired.push_back(std::move(entry.text));
	entry.text = std::move(value);
	return entry.text;
}

const CheckpointText* CheckpointCache::Peek(const void* owner, unsigned channel) const {
	const auto owners = m_Entries.find(owner);
	if (owners == m_Entries.end()) return nullptr;
	const auto entry = owners->second.find(channel);
	if (entry == owners->second.end()) return nullptr;
	return &entry->second.text;
}

uint64_t CheckpointCache::Stamp(const void* owner, unsigned channel) const {
	const auto owners = m_Entries.find(owner);
	if (owners == m_Entries.end()) return 0;
	const auto entry = owners->second.find(channel);
	return entry == owners->second.end() ? 0 : entry->second.stamp;
}

bool CheckpointCache::Touch(const void* owner, unsigned channel) {
	const auto owners = m_Entries.find(owner);
	if (owners == m_Entries.end()) return false;
	const auto entry = owners->second.find(channel);
	if (entry == owners->second.end()) return false;
	entry->second.generation = m_Generation;
	++m_Touched;
	++m_Reused;
	return true;
}

CheckpointText CheckpointCache::CapturePixels(const BITMAP* bitmap) {
	if (!bitmap) return CheckpointText(std::string());
	Pixels& previous = m_Pixels[bitmap];
	previous.generation = m_Generation;
	auto snapshot = BitmapSnapshot::Capture(bitmap, previous.snapshot);
	if (previous.snapshot && snapshot->SamePixels(*previous.snapshot)) return previous.text;
	CheckpointText text = CheckpointText::Deferred([snapshot] { return snapshot->PixelBytes(); }, snapshot->LogicalBytes());
	m_Retired.push_back(std::move(previous.text));
	previous.snapshot = std::move(snapshot);
	previous.text = std::move(text);
	return previous.text;
}

std::vector<CheckpointText> CheckpointCache::RetireUnused() {
	for (auto pixels = m_Pixels.begin(); pixels != m_Pixels.end();) {
		if (pixels->second.generation != m_Generation) {
			m_Retired.push_back(std::move(pixels->second.text));
			pixels = m_Pixels.erase(pixels);
		} else ++pixels;
	}
	for (auto owners = m_Entries.begin(); owners != m_Entries.end();) {
		for (auto entry = owners->second.begin(); entry != owners->second.end();) {
			if (entry->second.generation != m_Generation) {
				m_Retired.push_back(std::move(entry->second.text));
				entry = owners->second.erase(entry);
			} else ++entry;
		}
		if (owners->second.empty()) owners = m_Entries.erase(owners); else ++owners;
	}
	return std::exchange(m_Retired, {});
}

CheckpointText Writer::Capture(const std::function<void(Writer&)>& visit, int indent) {
	return CheckpointWriter::CaptureValues([&] {
		CheckpointBuffer buffer;
		Writer writer;
		writer.m_Capture = &buffer;
		writer.m_IndentCount = indent;
		writer.m_Snapshot = true;
		visit(writer);
		return buffer.Finish();
	});
}

void Writer::Append(const CheckpointText& text) {
	if (m_Capture) m_Capture->Child(text); else *m_Stream << text.Text();
}

void Writer::Clear() {
	m_Stream = nullptr;
	m_FilePath.clear();
	m_FolderPath.clear();
	m_FileName.clear();
	m_IndentCount = 0;
	m_Snapshot = false;
	m_Capture = nullptr;
	m_SaveOverrides = nullptr;
	m_CaptureObject = nullptr;
}

Writer::Writer(const std::string& fileName, bool append, bool createDir) {
	Clear();
	Create(fileName, append, createDir);
}

Writer::Writer(std::unique_ptr<std::ostream>&& stream) {
	Clear();
	Create(std::move(stream));
}

int Writer::Create(const std::string& fileName, bool append, bool createDir) {
	m_FilePath = fileName;

	// Extract filename and folder path
	size_t slashPos = m_FilePath.find_last_of("/\\");
	m_FileName = m_FilePath.substr(slashPos + 1);
	m_FolderPath = m_FilePath.substr(0, slashPos + 1);

	if (createDir && !std::filesystem::exists(System::GetWorkingDirectory() + m_FolderPath)) {
		System::MakeDirectory(System::GetWorkingDirectory() + m_FolderPath);
	}

	auto ofStream = std::make_unique<std::ofstream>(fileName, append ? (std::ios::out | std::ios::app | std::ios::ate) : (std::ios::out | std::ios::trunc));
	*ofStream << std::fixed << std::setprecision(6);
	return Create(std::move(ofStream));
}

int Writer::Create(std::unique_ptr<std::ostream>&& stream) {
	m_Stream = std::move(stream);
	if (!m_Stream->good()) {
		return -1;
	}

	return 0;
}

void Writer::NewLine(bool toIndent, int lineCount) const {
	if (m_Capture) { m_Capture->NewLine(toIndent ? m_IndentCount : 0, lineCount); return; }
	for (int lines = 0; lines < lineCount; ++lines) {
		*m_Stream << "\n";
		if (toIndent) {
			*m_Stream << std::string(m_IndentCount, '\t');
		}
	}
}

bool RTE::RunOwnedCheckpointSelfTest() {
	bool passed = true;
	const auto check = [&passed](bool result, const char* name) {
		std::cout << "[script-graph-selftest] " << (result ? "PASS" : "FAIL") << " " << name << std::endl;
		passed = result && passed;
	};
	try {
		int value = 17;
		std::string binary("x\0y", 3);
		const auto save = [&]() -> std::string { CheckpointWriter writer("Owned1"); writer(value, binary); return writer.Text(); };
		const std::string reference = save();
		const CheckpointText captured = CheckpointWriter::CaptureNative(save);
		value = 91; binary.assign("changed");
		check(captured.Text() == reference, "owned_checkpoint_copies_native_values");

		const CheckpointText bytes(std::string("x\0y", 3));
		CheckpointBuffer tokens;
		tokens.Raw("prefix|"); tokens.GraphString(bytes); tokens.Raw("|"); tokens.Base64(bytes, true);
		std::string tokenReference = "prefix|s3:"; tokenReference.append("x\0y", 3); tokenReference += "|eAB5";
		check(tokens.Finish().Text() == tokenReference, "owned_checkpoint_preserves_graph_strings_and_base64");

		const bool capturing = CheckpointWriter::IsCapturing();
		bool rejected = false;
		try {
			CheckpointWriter::CaptureNative([]() -> std::string { CheckpointWriter writer("Decorated1"); return "prefix" + writer.Text(); });
		} catch (const std::logic_error&) { rejected = true; }
		check(rejected && CheckpointWriter::IsCapturing() == capturing, "owned_checkpoint_refuses_uncaptured_text_and_restores_scope");

		auto calls = std::make_shared<std::atomic<int>>(0);
		auto replacements = std::make_shared<std::atomic<int>>(0);
		const auto original = CheckpointText::Deferred([calls] { ++*calls; return std::string("stable"); }, 0, "OwnedCheckpointStable1");
		const auto replacement = CheckpointText::Deferred([replacements] { ++*replacements; return std::string("stable"); }, 0, "OwnedCheckpointStable1");
		CheckpointBuffer oldParent; oldParent.Raw("old|"); oldParent.Child(original);
		const auto previous = oldParent.Finish();
		const bool formatted = previous.Text() == "old|stable";
		CheckpointBuffer newParent; newParent.Raw("new|"); newParent.Child(replacement);
		const auto reused = newParent.Finish().ReuseChildren(previous);
		check(formatted && reused.Text() == "new|stable" && calls->load() == 1 && replacements->load() == 0, "owned_checkpoint_reuses_unchanged_child_producer");
		const auto unkeyed = CheckpointText::Deferred([] { return std::string("stable"); });
		const auto otherUnkeyed = CheckpointText::Deferred([] { return std::string("stable"); });
		check(unkeyed.SameValues(unkeyed) && !unkeyed.SameValues(otherUnkeyed) && !unkeyed.SameValues(original), "owned_checkpoint_preserves_deferred_identity_rules");

		auto attempts = std::make_shared<std::atomic<int>>(0);
		const auto flaky = CheckpointText::Deferred([attempts] {
			if (attempts->fetch_add(1) == 0) throw std::runtime_error("owned checkpoint retry probe");
			return std::string("tail");
		});
		CheckpointBuffer retryBuffer; retryBuffer.Raw("prefix|"); retryBuffer.Child(flaky);
		const auto retry = retryBuffer.Finish();
		bool failed = false;
		try { retry.Text(); } catch (const std::runtime_error&) { failed = true; }
		check(failed && retry.Text() == "prefix|tail" && retry.Text() == "prefix|tail" && attempts->load() == 2, "owned_checkpoint_retry_publishes_no_partial_prefix");

		auto concurrentCalls = std::make_shared<std::atomic<int>>(0);
		const auto concurrent = CheckpointText::Deferred([concurrentCalls] { ++*concurrentCalls; return std::string("shared"); });
		CheckpointBuffer concurrentBuffer; concurrentBuffer.Raw("read|"); concurrentBuffer.Child(concurrent);
		const auto readersText = concurrentBuffer.Finish();
		std::vector<std::future<std::string>> readers;
		std::promise<void> start;
		const auto ready = start.get_future().share();
		for (int index = 0; index < 4; ++index) readers.push_back(std::async(std::launch::async, [readersText, ready] { ready.wait(); return readersText.Text(); }));
		start.set_value();
		bool same = true;
		for (auto& reader: readers) same = reader.get() == "read|shared" && same;
		check(same && concurrentCalls->load() == 1, "owned_checkpoint_concurrent_readers_share_publication");

		const auto chain = [](CheckpointText value) {
			for (int depth = 0; depth < 32768; ++depth) { CheckpointBuffer parent; parent.Child(value); value = parent.Finish(); }
			return value;
		};
		auto payload = std::make_shared<int>(7);
		const std::weak_ptr<int> witness(payload);
		auto first = chain(CheckpointText::Deferred([payload] { return std::string("a"); }, sizeof(int), "OwnedCheckpointDeepA1"));
		payload.reset();
		auto equal = chain(CheckpointText::Deferred([] { return std::string("a"); }, 0, "OwnedCheckpointDeepA1"));
		auto changed = chain(CheckpointText(std::string("b")));
		auto deep = std::async(std::launch::async, [first = std::move(first), equal = std::move(equal), changed = std::move(changed), witness]() mutable {
			bool result = !witness.expired() && first.SameValues(equal) && !first.SameValues(changed);
			auto reused = changed.ReuseChildren(first);
			result = reused.SameValues(changed) && reused.Text() == "b" && first.Text() == "a" && result;
			reused = {}; changed = {}; equal = {}; first = {};
			return result && witness.expired();
		});
		check(deep.get(), "owned_checkpoint_deep_graph_compares_formats_and_releases_on_worker");
	} catch (const std::exception& error) {
		std::cout << "[script-graph-selftest] owned checkpoint exception: " << error.what() << std::endl;
		check(false, "owned_checkpoint_no_unexpected_exception");
	}
	return passed;
}
