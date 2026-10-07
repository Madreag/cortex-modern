#include "Writer.h"
#include "System.h"
#include "CheckpointArchive.h"
#include "CheckpointProperties.h"
#include "CheckpointImage.h"
#include "BitmapCheckpoint.h"
#include "Base64/base64.h"
#include "SceneLayer.h"
#include "Scene.h"
#include "MOPixel.h"
#include "Deployment.h"
#include "Reader.h"
#include "Timer.h"

#include <format>
#include <iomanip>
#include <fstream>
#include <map>
#include <mutex>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <atomic>
#include <unordered_set>
#include <utility>
#include <future>
#include <iostream>
#include <locale>

using namespace RTE;

std::string RTE::CheckpointFieldText(const std::function<std::string()>& observe) {
	return CheckpointWriter::CaptureNative(observe).Text();
}

namespace {
	using CaptureValue = CheckpointBuffer::ValueKind;
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
	std::string CanonicalCaptureValues(std::string_view values) {
		std::string result;
		size_t cursor = 0;
		while (cursor < values.size()) {
			const size_t begin = cursor;
			const auto kind = ReadCaptureValue<CaptureValue>(values, cursor);
			switch (kind) {
				case CaptureValue::PrimitiveBlock: {
					const auto decode = ReadCaptureValue<CheckpointBuffer::PrimitiveDecoder>(values, cursor);
					decode(result, ReadCaptureString(values, cursor), true);
					continue;
				}
				case CaptureValue::Raw:
				case CaptureValue::String: ReadCaptureString(values, cursor); break;
				case CaptureValue::Property: ReadCaptureValue<int>(values, cursor); ReadCaptureString(values, cursor); break;
				case CaptureValue::NewLine: ReadCaptureValue<int>(values, cursor); ReadCaptureValue<int>(values, cursor); break;
				case CaptureValue::Float: ReadCaptureValue<float>(values, cursor); break;
				case CaptureValue::ElapsedSimTime: ReadCaptureValue<int64_t>(values, cursor); ReadCaptureValue<double>(values, cursor); break;
				case CaptureValue::PeerBegin:
				case CaptureValue::PeerEnd:
				case CaptureValue::SizedRunBegin:
				case CaptureValue::SizedRunEnd: break;
				case CaptureValue::Integer:
				case CaptureValue::Unsigned:
				case CaptureValue::SpacedInteger:
				case CaptureValue::SpacedUnsigned:
				case CaptureValue::Double:
				case CaptureValue::Child:
				case CaptureValue::SizedChild:
				case CaptureValue::Base64:
				case CaptureValue::UrlBase64:
				case CaptureValue::GraphString: ReadCaptureValue<uint64_t>(values, cursor); break;
				default: throw std::logic_error("unknown owned checkpoint value");
			}
			result.append(values.substr(begin, cursor - begin));
		}
		return result;
	}
}

struct RTE::CheckpointArena {
	std::atomic<size_t> owners{1};
	std::pmr::monotonic_buffer_resource storage{16384};
	void RetainBlock() { owners.fetch_add(1, std::memory_order_relaxed); }
	void ReleaseBlock() noexcept { if (owners.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this; }
};

namespace {
	template<class T> struct CheckpointAllocator {
		using value_type = T;
		CheckpointArena* arena;
		explicit CheckpointAllocator(CheckpointArena* owner) : arena(owner) {}
		template<class U> CheckpointAllocator(const CheckpointAllocator<U>& other) : arena(other.arena) {}
		T* allocate(size_t count) {
			if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
			T* block = static_cast<T*>(arena->storage.allocate(count * sizeof(T), alignof(T)));
			arena->RetainBlock();
			return block;
		}
		// A block retains its storage through destruction of its control block.
		// Allocator copies carry an address, so rebinding makes no ownership traffic.
		void deallocate(T*, size_t) noexcept { arena->ReleaseBlock(); }
		template<class U> bool operator==(const CheckpointAllocator<U>& other) const noexcept { return arena == other.arena; }
	};
}

struct CheckpointText::Data {
	explicit Data(std::pmr::memory_resource* resource = std::pmr::get_default_resource()) : values(resource), children(resource) {}
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
	std::pmr::string values;
	std::pmr::vector<CheckpointText> children;
	// A deferred node's producer, dropped once it has produced: what it captured (a frozen heap, a pixel snapshot) goes with it.
	mutable std::function<std::string()> produce;
	bool deferred = false;
	std::string peerMark;
	mutable std::vector<std::pair<size_t, size_t>> peerRuns;
	std::string identity;
	size_t ownedBytes = 0;
	bool hasPeer = false;
	bool hasPrimitiveBlocks = false;
	bool usesSimTime = false;
	int64_t simTimeTicks = 0;
	mutable std::mutex ready; // Held while this node formats; a producer that throws leaves the node for the next read.
	mutable std::atomic<bool> formatted{false};
	mutable std::string text;
	std::shared_ptr<Data> drainNext;
	bool SameTape(const Data& other) const {
		if (values == other.values) return true;
		if (hasPrimitiveBlocks == other.hasPrimitiveBlocks) return false;
		return CanonicalCaptureValues(values) == CanonicalCaptureValues(other.values);
	}

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
	data->deferred = true;
	data->ownedBytes = ownedBytes;
	data->identity = std::move(identity);
	return CheckpointText(std::move(data));
}

CheckpointText CheckpointText::DeferredWithPeerRuns(std::function<std::string()> produce, std::string mark, size_t ownedBytes) {
	if (mark.empty()) throw std::logic_error("a per-peer run mark cannot be empty");
	auto data = std::make_shared<Data>();
	data->produce = std::move(produce);
	data->deferred = true;
	data->peerMark = std::move(mark);
	data->ownedBytes = ownedBytes;
	data->hasPeer = true;
	return CheckpointText(std::move(data));
}

namespace {
	// Drops the marks a producer put around its per-peer runs and remembers where the runs lie in the text that is left.
	void StripPeerMarks(std::string& text, const std::string& mark, std::vector<std::pair<size_t, size_t>>& runs) {
		std::string kept;
		kept.reserve(text.size());
		size_t at = 0, runStart = 0;
		bool inside = false;
		for (size_t found = text.find(mark); found != std::string::npos; found = text.find(mark, at)) {
			kept.append(text, at, found - at);
			if (inside) runs.emplace_back(runStart, kept.size()); else runStart = kept.size();
			inside = !inside;
			at = found + mark.size();
		}
		if (inside) throw std::logic_error("a deferred checkpoint text left a per-peer run open");
		kept.append(text, at, std::string::npos);
		text = std::move(kept);
	}
} // namespace

CheckpointText CheckpointText::Base64(bool url) const {
	CheckpointBuffer buffer;
	buffer.Base64(*this, url);
	return buffer.Finish();
}

CheckpointText CheckpointText::BindSimTime(int64_t ticks) const {
	if (!m_Data || !m_Data->usesSimTime) return *this;
	// The worker binds timer paths while timeless branches keep their published text.
	return Deferred([source = *this, ticks] { return source.AtSimTime(ticks).Text(); }, OwnedBytes());
}

CheckpointText CheckpointText::AtSimTime(int64_t ticks) const {
	if (!m_Data || !m_Data->usesSimTime) return *this;
	struct Frame { const Data* source; std::shared_ptr<Data> bound; size_t next = 0; };
	std::unordered_map<const Data*, std::shared_ptr<Data>> bound;
	std::vector<Frame> pending;
	const auto copy = [&](const Data* source) {
		auto node = std::make_shared<Data>();
		node->values = source->values;
		node->children.reserve(source->children.size());
		node->ownedBytes = source->ownedBytes;
		node->hasPeer = source->hasPeer;
		node->hasPrimitiveBlocks = source->hasPrimitiveBlocks;
		node->simTimeTicks = ticks;
		bound.emplace(source, node);
		pending.push_back({source, node});
		return node;
	};
	const auto root = copy(m_Data.get());
	while (!pending.empty()) {
		Frame& frame = pending.back();
		if (frame.next == frame.source->children.size()) { pending.pop_back(); continue; }
		const auto& child = frame.source->children[frame.next++];
		if (!child.m_Data || !child.m_Data->usesSimTime) {
			frame.bound->children.push_back(child);
			continue;
		}
		const auto found = bound.find(child.m_Data.get());
		if (found != bound.end()) {
			frame.bound->children.emplace_back(CheckpointText(found->second));
			continue;
		}
		const auto parent = frame.bound;
		parent->children.emplace_back(CheckpointText(copy(child.m_Data.get())));
	}
	return CheckpointText(root);
}

size_t CheckpointText::OwnedBytes() const { return m_Data ? m_Data->ownedBytes : 0; }

bool CheckpointText::HasPeerRuns() const { return m_Data && m_Data->hasPeer; }

bool CheckpointText::SameValues(const CheckpointText& other) const {
	if (m_Data == other.m_Data) return true;
	if (!m_Data || !other.m_Data) return false;
	if (m_Data->deferred || other.m_Data->deferred) return m_Data->deferred && other.m_Data->deferred && !m_Data->identity.empty() && m_Data->identity == other.m_Data->identity;
	if (m_Data->children.size() != other.m_Data->children.size()) return false;
	if (m_Data->children.empty()) return m_Data->SameTape(*other.m_Data);
	std::vector<Data::Pair> pending{{m_Data.get(), other.m_Data.get()}};
	std::unordered_set<Data::Pair, Data::PairHash> seen;
	while (!pending.empty()) {
		const auto [current, previous] = pending.back();
		pending.pop_back();
		if (current == previous) continue;
		if (!current || !previous) return false;
		if (current->deferred || previous->deferred) {
			if (!current->deferred || !previous->deferred || current->identity.empty() || current->identity != previous->identity) return false;
			continue;
		}
		if (current->children.size() != previous->children.size() || !current->SameTape(*previous)) return false;
		if (!seen.insert({current, previous}).second) continue;
		for (size_t index = current->children.size(); index > 0; --index) pending.push_back({current->children[index - 1].m_Data.get(), previous->children[index - 1].m_Data.get()});
	}
	return true;
}

CheckpointText CheckpointText::ReuseChildren(const CheckpointText& previous) const {
	if (m_Data == previous.m_Data) return previous;
	if (!m_Data || !previous.m_Data) return *this;
	if (m_Data->deferred || previous.m_Data->deferred) return SameValues(previous) ? previous : *this;
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
			if (frame.current->deferred || frame.previous->deferred) {
				const bool equal = frame.current->deferred && frame.previous->deferred && !frame.current->identity.empty() && frame.current->identity == frame.previous->identity;
				results.emplace(pair, Result{equal, equal ? frame.previous : frame.current}); pending.pop_back(); continue;
			}
			frame.equal = frame.current->children.size() == frame.previous->children.size() && frame.current->SameTape(*frame.previous);
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
			value->hasPeer = frame.current->hasPeer;
			value->hasPrimitiveBlocks = frame.current->hasPrimitiveBlocks;
			value->usesSimTime = frame.current->usesSimTime;
			value->simTimeTicks = frame.current->simTimeTicks;
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
	if (root->usesSimTime) {
		std::lock_guard lock(root->ready);
		if (!root->formatted.load(std::memory_order_acquire)) {
			root->text = AtSimTime(root->simTimeTicks).Text();
			root->formatted.store(true, std::memory_order_release);
		}
		return root->text;
	}
	struct Frame { const Data* node; size_t next = 0; };
	std::vector<Frame> pending{{root.get()}};
	while (!pending.empty()) {
		Frame& frame = pending.back();
		const Data* node = frame.node;
		if (node->formatted.load(std::memory_order_acquire)) { pending.pop_back(); continue; }
		if (!node->deferred && frame.next < node->children.size()) {
			const Data* child = node->children[frame.next++].m_Data.get();
			if (child && !child->formatted.load(std::memory_order_acquire)) pending.push_back({child});
			continue;
		}
		const auto format = [node] {
			if (node->deferred) {
				node->text = node->produce();
				if (!node->peerMark.empty()) StripPeerMarks(node->text, node->peerMark, node->peerRuns);
				node->formatted.store(true, std::memory_order_release);
				node->produce = nullptr;
				return;
			}
			std::string text;
			std::vector<size_t> sizedRuns;
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
					case CaptureValue::ElapsedSimTime: {
						const int64_t startTicks = ReadCaptureValue<int64_t>(values, cursor);
						const double ticksPerMS = ReadCaptureValue<double>(values, cursor);
						AppendCaptureNumber(text, static_cast<double>(node->simTimeTicks - startTicks) / ticksPerMS);
						break;
					}
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
					case CaptureValue::PeerBegin:
					case CaptureValue::PeerEnd: break;
					case CaptureValue::PrimitiveBlock: {
						const auto decode = ReadCaptureValue<CheckpointBuffer::PrimitiveDecoder>(values, cursor);
						decode(text, ReadCaptureString(values, cursor), false);
						break;
					}
					case CaptureValue::SizedRunBegin: sizedRuns.push_back(text.size()); break;
					case CaptureValue::SizedRunEnd: {
						if (sizedRuns.empty()) throw std::logic_error("unmatched owned sized run");
						const size_t begin = sizedRuns.back(); sizedRuns.pop_back();
						std::string size; AppendCaptureNumber(size, text.size() - begin); size.push_back(' ');
						text.insert(begin, size); text.push_back(' ');
						break;
					}
					default: throw std::logic_error("unknown owned checkpoint value");
				}
			}
			if (!sizedRuns.empty()) throw std::logic_error("open owned sized run");
			node->text = std::move(text);
			node->formatted.store(true, std::memory_order_release);
		};
		{
			std::lock_guard lock(node->ready);
			if (!node->formatted.load(std::memory_order_acquire)) format();
		}
		pending.pop_back();
	}
	return root->text;
}

std::string CheckpointText::SharedText(int64_t simTimeTicks) const {
	if (!m_Data || !m_Data->hasPeer) return BindSimTime(simTimeTicks).Text();
	return m_Data->usesSimTime ? AtSimTime(simTimeTicks).SharedText() : SharedText();
}

std::string CheckpointText::SharedText() const {
	if (!m_Data || !m_Data->hasPeer) return Text();
	if (m_Data->usesSimTime) return AtSimTime(m_Data->simTimeTicks).SharedText();
	if (m_Data->deferred) {
		const std::string& text = Text();
		std::string shared;
		size_t at = 0;
		for (const auto& [begin, end]: m_Data->peerRuns) {
			shared.append(text, at, begin - at);
			at = end;
		}
		shared.append(text, at, std::string::npos);
		return shared;
	}
	const Data& node = *m_Data;
	const std::string_view values = node.values;
	std::string text;
	size_t cursor = 0;
	int peer = 0;
	std::vector<std::optional<size_t>> sizedRuns;
	// Appends what the full text would, unless it lies inside a per-peer run.
	const auto put = [&text, &peer](std::string_view part) { if (peer == 0) text += part; };
	const auto number = [&put](auto value) { std::string formatted; AppendCaptureNumber(formatted, value); put(formatted); };
	while (cursor < values.size()) {
		const auto kind = ReadCaptureValue<CaptureValue>(values, cursor);
		switch (kind) {
			case CaptureValue::Raw: put(ReadCaptureString(values, cursor)); break;
			case CaptureValue::Integer:
			case CaptureValue::SpacedInteger:
				number(ReadCaptureValue<int64_t>(values, cursor));
				if (kind == CaptureValue::SpacedInteger) put(" ");
				break;
			case CaptureValue::Unsigned:
			case CaptureValue::SpacedUnsigned:
				number(ReadCaptureValue<uint64_t>(values, cursor));
				if (kind == CaptureValue::SpacedUnsigned) put(" ");
				break;
			case CaptureValue::Float: number(ReadCaptureValue<float>(values, cursor)); break;
			case CaptureValue::Double: number(ReadCaptureValue<double>(values, cursor)); break;
			case CaptureValue::ElapsedSimTime: {
				const int64_t startTicks = ReadCaptureValue<int64_t>(values, cursor);
				const double ticksPerMS = ReadCaptureValue<double>(values, cursor);
				number(static_cast<double>(node.simTimeTicks - startTicks) / ticksPerMS);
				break;
			}
			case CaptureValue::String: {
				const auto string = ReadCaptureString(values, cursor);
				number(string.size()); put(" "); put(string); put(" ");
				break;
			}
			case CaptureValue::Child:
			case CaptureValue::SizedChild:
			case CaptureValue::Base64:
			case CaptureValue::UrlBase64:
			case CaptureValue::GraphString: {
				const auto index = ReadCaptureValue<uint64_t>(values, cursor);
				const std::string child = node.children.at(static_cast<size_t>(index)).SharedText();
				if (kind == CaptureValue::SizedChild) { number(child.size()); put(" "); }
				if (kind == CaptureValue::GraphString) { put("s"); number(child.size()); put(":"); }
				if (kind == CaptureValue::Base64 || kind == CaptureValue::UrlBase64) put(base64_encode(child, kind == CaptureValue::UrlBase64));
				else put(child);
				if (kind == CaptureValue::SizedChild) put(" ");
				break;
			}
			case CaptureValue::NewLine: {
				const int indent = ReadCaptureValue<int>(values, cursor), count = ReadCaptureValue<int>(values, cursor);
				for (int i = 0; i < count; ++i) { put("\n"); if (indent > 0) put(std::string(static_cast<size_t>(indent), '\t')); }
				break;
			}
			case CaptureValue::Property: {
				const int indent = ReadCaptureValue<int>(values, cursor);
				put("\n"); put(std::string(static_cast<size_t>(indent), '\t')); put(ReadCaptureString(values, cursor)); put(" = ");
				break;
			}
			case CaptureValue::PeerBegin: ++peer; break;
			case CaptureValue::PeerEnd: --peer; break;
			case CaptureValue::PrimitiveBlock: {
				const auto decode = ReadCaptureValue<CheckpointBuffer::PrimitiveDecoder>(values, cursor);
				const auto block = ReadCaptureString(values, cursor);
				if (peer == 0) decode(text, block, false);
				break;
			}
			case CaptureValue::SizedRunBegin: sizedRuns.push_back(peer == 0 ? std::optional<size_t>(text.size()) : std::nullopt); break;
			case CaptureValue::SizedRunEnd: {
				if (sizedRuns.empty()) throw std::logic_error("unmatched owned sized run");
				const auto begin = sizedRuns.back(); sizedRuns.pop_back();
				if (begin) {
					std::string size; AppendCaptureNumber(size, text.size() - *begin); size.push_back(' ');
					text.insert(*begin, size); text.push_back(' ');
				}
				break;
			}
			default: throw std::logic_error("unknown owned checkpoint value");
		}
	}
	if (!sizedRuns.empty()) throw std::logic_error("open owned sized run");
	return text;
}

CheckpointBuffer::AllocationScope::AllocationScope(bool enabled) {
	if (enabled && !s_Arena) {
		s_Arena = std::shared_ptr<CheckpointArena>(new CheckpointArena, [](CheckpointArena* arena) { arena->ReleaseBlock(); });
		m_Entered = true;
	}
}

CheckpointBuffer::AllocationScope::~AllocationScope() {
	if (m_Entered) s_Arena.reset();
}

CheckpointBuffer::CheckpointBuffer(bool reserve) : m_Arena(reserve ? s_Arena : nullptr),
	m_Values(m_Arena ? &m_Arena->storage : std::pmr::get_default_resource()),
	m_Children(m_Arena ? &m_Arena->storage : std::pmr::get_default_resource()) {
	if (m_Arena && reserve) m_Values.reserve(64);
}

void CheckpointBuffer::Raw(std::string_view text) { Copy(CaptureValue::Raw, static_cast<uint64_t>(text.size())); m_Values.append(text); }
void CheckpointBuffer::Integer(int64_t value, bool space) { Copy(space ? CaptureValue::SpacedInteger : CaptureValue::Integer, value); }
void CheckpointBuffer::Unsigned(uint64_t value, bool space) { Copy(space ? CaptureValue::SpacedUnsigned : CaptureValue::Unsigned, value); }
void CheckpointBuffer::Real(float value) { Copy(CaptureValue::Float, value); }
void CheckpointBuffer::Real(double value) { Copy(CaptureValue::Double, value); }
void CheckpointBuffer::ElapsedSimTime(int64_t startTicks, double ticksPerMS) {
	Copy(CaptureValue::ElapsedSimTime, startTicks, ticksPerMS);
	m_UsesSimTime = true;
	m_SimTimeTicks = g_TimerMan.GetSimTickCount();
}
void CheckpointBuffer::String(std::string_view value) { Copy(CaptureValue::String, static_cast<uint64_t>(value.size())); m_Values.append(value); }
void CheckpointBuffer::Child(const CheckpointText& value, bool sized) { Copy(sized ? CaptureValue::SizedChild : CaptureValue::Child, static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::Base64(const CheckpointText& value, bool url) { Copy(url ? CaptureValue::UrlBase64 : CaptureValue::Base64, static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::GraphString(const CheckpointText& value) { Copy(CaptureValue::GraphString, static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::NewLine(int indent, int count) { Copy(CaptureValue::NewLine, indent, count); }
void CheckpointBuffer::Property(std::string_view name, int indent) { Copy(CaptureValue::Property, indent, static_cast<uint64_t>(name.size())); m_Values.append(name); }
void CheckpointBuffer::PeerBegin() { Copy(CaptureValue::PeerBegin); m_HasPeer = true; }
void CheckpointBuffer::PeerEnd() { Copy(CaptureValue::PeerEnd); }
void CheckpointBuffer::PrimitiveBlock(std::string_view values, PrimitiveDecoder decoder) {
	Copy(CaptureValue::PrimitiveBlock, decoder, static_cast<uint64_t>(values.size()));
	m_Values.append(values);
	m_HasPrimitiveBlocks = true;
}

void CheckpointBuffer::SizedRunBegin() { Copy(CaptureValue::SizedRunBegin); }
void CheckpointBuffer::SizedRunEnd() { Copy(CaptureValue::SizedRunEnd); }

CheckpointText CheckpointBuffer::Finish() {
	auto data = m_Arena ? std::allocate_shared<CheckpointText::Data>(CheckpointAllocator<CheckpointText::Data>(m_Arena.get()), &m_Arena->storage)
	                    : std::make_shared<CheckpointText::Data>();
	data->values = std::move(m_Values);
	data->children = std::move(m_Children);
	data->ownedBytes = data->values.size();
	data->hasPeer = m_HasPeer;
	data->hasPrimitiveBlocks = m_HasPrimitiveBlocks;
	data->usesSimTime = m_UsesSimTime;
	data->simTimeTicks = m_SimTimeTicks;
	for (const auto& child: data->children) {
		data->ownedBytes += child.OwnedBytes();
		data->hasPeer = data->hasPeer || (child.m_Data && child.m_Data->hasPeer);
		if (!data->usesSimTime && child.m_Data && child.m_Data->usesSimTime) {
			data->usesSimTime = true;
			data->simTimeTicks = child.m_Data->simTimeTicks;
		}
	}
	return CheckpointText(std::move(data));
}

CheckpointText CheckpointCache::Remember(const void* owner, unsigned channel, CheckpointText value) {
	if (channel != 16) return Remember(owner, channel, std::move(value), 0);
	const auto start = std::chrono::steady_clock::now();
	CheckpointText result = Remember(owner, channel, std::move(value), 0);
	CheckpointGraphIndex::Get().NoteWalkPart(nullptr, "cache", 0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(), false, {});
	return result;
}

CheckpointText CheckpointCache::Remember(const void* owner, unsigned channel, CheckpointText value, uint64_t stamp, uint64_t identity, const MovableObject* object) {
	Entry& entry = m_Entries[owner][channel];
	++m_Touched;
	entry.generation = m_Generation;
	entry.stamp = stamp;
	entry.identity = identity;
	entry.object = object;
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

const CheckpointText* CheckpointCache::PeekCurrent(const void* owner, unsigned channel) const {
	const auto owners = m_Entries.find(owner);
	if (owners == m_Entries.end()) return nullptr;
	const auto entry = owners->second.find(channel);
	return entry == owners->second.end() || entry->second.generation != m_Generation ? nullptr : &entry->second.text;
}

uint64_t CheckpointCache::Identity(const void* owner, unsigned channel) const {
	const auto owners = m_Entries.find(owner);
	if (owners == m_Entries.end()) return 0;
	const auto entry = owners->second.find(channel);
	return entry == owners->second.end() || !entry->second.object.get() ? 0 : entry->second.identity;
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
	if (const auto shared = BitmapPixelCaptureScope::Capture(bitmap, previous.snapshot)) {
		m_Retired.push_back(std::move(previous.text));
		previous.snapshot = shared->first;
		previous.text = shared->second;
		return previous.text;
	}
	auto snapshot = BitmapSnapshot::Capture(bitmap, previous.snapshot);
	if (previous.snapshot && snapshot->SamePixels(*previous.snapshot)) return previous.text;
	CheckpointText text = CheckpointText::Deferred([snapshot] { return snapshot->PixelBytes(); }, snapshot->LogicalBytes());
	m_Retired.push_back(std::move(previous.text));
	previous.snapshot = std::move(snapshot);
	previous.text = std::move(text);
	return previous.text;
}

struct BitmapPixelCaptureScope::State {
	struct Cell { std::once_flag once; std::shared_ptr<const BitmapSnapshot> snapshot; CheckpointText text; };
	std::mutex mutex;
	std::unordered_map<const BITMAP*, std::shared_ptr<Cell>> cells;
};
std::atomic<BitmapPixelCaptureScope::State*> BitmapPixelCaptureScope::s_Current{nullptr};
BitmapPixelCaptureScope::BitmapPixelCaptureScope() : m_State(std::make_unique<State>()), m_Previous(s_Current.exchange(m_State.get())) {}
BitmapPixelCaptureScope::~BitmapPixelCaptureScope() { s_Current.store(m_Previous); }
std::optional<std::pair<std::shared_ptr<const BitmapSnapshot>, CheckpointText>> BitmapPixelCaptureScope::Capture(
    const BITMAP* bitmap, const std::shared_ptr<const BitmapSnapshot>& previous) {
	State* state = s_Current.load();
	if (!state) return std::nullopt;
	std::shared_ptr<State::Cell> cell;
	{
		std::lock_guard lock(state->mutex);
		auto& entry = state->cells[bitmap];
		if (!entry) entry = std::make_shared<State::Cell>();
		cell = entry;
	}
	std::call_once(cell->once, [&] {
		cell->snapshot = BitmapSnapshot::Capture(bitmap, previous);
		cell->text = CheckpointText::Deferred([snapshot = cell->snapshot] { return snapshot->PixelBytes(); }, cell->snapshot->LogicalBytes());
	});
	return std::pair{cell->snapshot, cell->text};
}

size_t CheckpointCache::PixelBytes() const {
	size_t bytes = 0;
	for (const auto& [bitmap, pixels]: m_Pixels) if (pixels.snapshot) bytes += pixels.snapshot->OwnedBytes();
	return bytes;
}

std::string CheckpointCache::Census() const {
	size_t entries = 0, bytes = 0, retiredBytes = 0;
	for (const auto& [owner, channels]: m_Entries) {
		entries += channels.size();
		for (const auto& [channel, entry]: channels) bytes += entry.text.OwnedBytes();
	}
	for (const CheckpointText& text: m_Retired) retiredBytes += text.OwnedBytes();
	// Which kind of owner holds the entries: per channel, and how many belong to a world object.
	std::map<unsigned, size_t> perChannel;
	size_t withObject = 0;
	for (const auto& [owner, channels]: m_Entries) {
		for (const auto& [channel, entry]: channels) {
			++perChannel[channel];
			if (entry.identity != 0) ++withObject;
		}
	}
	std::string channelText;
	for (const auto& [channel, count]: perChannel) channelText += std::format("{}{}:{}", channelText.empty() ? "" : ",", channel, count);
	return std::format("entries={} entry_mb={} pixels={} retired={} retired_mb={} owners={} with_identity={} channels={}", entries, bytes >> 20, m_Pixels.size(), m_Retired.size(),
	                   retiredBytes >> 20, m_Entries.size(), withObject, channelText.empty() ? "none" : channelText);
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

void Writer::ElapsedSimTime(const Timer& timer) {
	if (m_Capture) m_Capture->ElapsedSimTime(timer.GetStartSimTimeMS(), static_cast<double>(g_TimerMan.GetTicksPerSecond()) * 0.001);
	else *this << timer.GetElapsedSimTimeMS();
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
	// Integer grouping is part of a stream's locale too; saved text never uses it.
	m_Stream->imbue(std::locale::classic());
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
	const auto check = [&passed](bool result, const char* name, const std::string& detail = {}) {
		std::cout << "[script-graph-selftest] " << (result ? "PASS" : "FAIL") << " " << name;
		if (!result && !detail.empty()) std::cout << " " << detail;
		std::cout << std::endl;
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
		{
			CheckpointText frozen;
			std::string full, shared;
			{
				std::unordered_map<std::string, float> numbers{{"z", -0.0F}, {std::string("a\0b", 3), std::bit_cast<float>(uint32_t{0x7fc01234})}, {"a", 1.25F}};
				std::unordered_map<uint64_t, int64_t> integers{{std::numeric_limits<uint64_t>::max(), std::numeric_limits<int64_t>::min()}, {0, 17}, {31, -7}};
				std::unordered_map<int, std::string> peers{{9, std::string("x\0y\xff", 4)}, {-7, "first"}, {0, ""}};
				std::unordered_map<std::string, int> empty;
				const auto saveMaps = [&] {
					CheckpointWriter writer("OwnedMaps1");
					writer(numbers, integers, empty);
					writer.PerPeer(peers);
					return writer.Text();
				};
				const CheckpointText ordinary = CheckpointWriter::CaptureNative(saveMaps);
				full = ordinary.Text(); shared = ordinary.SharedText();
				{
					CheckpointWriter::BatchScope batch(true);
					frozen = CheckpointWriter::CaptureNative(saveMaps);
				}
				numbers.clear(); integers.clear(); peers.clear(); empty.emplace("changed", 77);
			}
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == full && actual.second == shared, "owned_unordered_maps_sort_after_source_death_and_keep_full_and_shared_bytes");
		}
		{
			int64_t integer = std::numeric_limits<int64_t>::min();
			bool flag = true;
			float scalar = -0.0F;
			Vector vector(-0.0F, std::bit_cast<float>(uint32_t{0x7fc01234}));
			std::string binary("named\0field\xff", 12);
			const auto write = [&](Writer& writer) {
				WriteCapturedProperties(writer, CheckpointProperty<"Integer">(integer), CheckpointProperty<"Flag">(flag),
				    CheckpointProperty<"Scalar">(scalar), CheckpointProperty<"Vector">(vector), CheckpointProperty<"Binary">(binary));
				writer.PerPeerBegin();
				WriteCapturedProperties(writer, CheckpointProperty<"PeerInteger">(integer), CheckpointProperty<"PeerFlag">(flag));
				writer.PerPeerEnd();
			};
			const CheckpointText ordinary = Writer::Capture(write, 2);
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batch(true);
				frozen = Writer::Capture(write, 2);
			}
			integer = 99; flag = false; scalar = 1.25F; vector.SetXY(33, 37); binary.assign("changed");
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == ordinary.Text() && actual.second == ordinary.SharedText(), "owned_named_properties_preserve_full_and_shared_bytes_after_mutation");
		}
		{
			CheckpointText frozen;
			std::string full, shared;
			{
				std::vector<Vector> offsets{Vector(-0.0F, std::bit_cast<float>(uint32_t{0x7fc01234})), Vector(11.25F, -17.5F)};
				std::vector<int64_t> ids{std::numeric_limits<int64_t>::min(), 0, std::numeric_limits<int64_t>::max()};
				std::vector<std::string> binary{std::string("seq\0value\xff", 10), "", "repeated"};
				const auto write = [&](Writer& writer) {
					WriteCapturedPropertySequence<"Offsets">(writer, offsets);
					WriteCapturedPropertySequence<"IDs">(writer, ids);
					WriteCapturedPropertySequence<"Empty">(writer, std::vector<int>{});
					writer.PerPeerBegin();
					WriteCapturedPropertySequence<"Binary">(writer, binary);
					writer.PerPeerEnd();
				};
				const auto ordinary = Writer::Capture(write, 3);
				full = ordinary.Text(); shared = ordinary.SharedText();
				{
					CheckpointWriter::BatchScope batches(true);
					frozen = Writer::Capture(write, 3);
				}
			}
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == full && actual.second == shared, "owned_property_sequences_outlive_sources_and_preserve_full_and_shared_bytes");
		}
		{
			enum class SignedByte : int8_t { Low = -127 };
			const uint8_t unusualBool = 0xFE;
			bool flag;
			std::memcpy(&flag, &unusualBool, sizeof(flag));
			int8_t bytes[] = {-128, 0, 127};
			std::array<uint64_t, 2> wide{0, std::numeric_limits<uint64_t>::max()};
			const auto saveBlock = [&] {
				CheckpointWriter writer("PrimitiveBlock1");
				writer(flag, bytes, wide, SignedByte::Low, std::numeric_limits<int64_t>::min(),
				       -0.0F, std::bit_cast<float>(uint32_t{0x7FC00031}), std::bit_cast<double>(uint64_t{0x7FF8000000000031}));
				writer.PerPeer(bytes, wide);
				return writer.Text();
			};
			const auto ordinary = CheckpointWriter::CaptureNative(saveBlock);
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batch(true);
				frozen = CheckpointWriter::CaptureNative(saveBlock);
			}
			flag = false; bytes[0] = 17; wide.fill(1);
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(frozen.SameValues(ordinary) && archived.first == ordinary.Text() && archived.second == ordinary.SharedText(),
			      "owned_primitive_blocks_preserve_width_bits_peer_runs_and_lifetime");
		}
		{
			CheckpointText frozen;
			std::string full, shared;
			{
				std::string binary("a\0b", 3);
				std::vector<Vector> positions{{-0.0F, std::bit_cast<float>(uint32_t{0x7FC00031})}, {17, 29}};
				Timer timer;
				timer.SetStartSimTimeTicks(37); timer.SetSimTimeLimitTicks(41);
				timer.SetStartRealTimeTicks(43); timer.SetRealTimeLimitTicks(47);
				const auto saveMixed = [&] {
					CheckpointWriter writer("MixedFields1");
					writer(-17, 19u, binary, -0.0F, 23, positions, 31, 37u, timer, 41, 43u, binary);
					writer.PerPeer(47, 53u, binary, 59, 61u);
					writer(67, 71u);
					return writer.Text();
				};
				const auto ordinary = CheckpointWriter::CaptureNative(saveMixed);
				full = ordinary.Text(); shared = ordinary.SharedText();
				CheckpointWriter::BatchScope batch(true);
				frozen = CheckpointWriter::CaptureNative(saveMixed);
				check(frozen.SameValues(ordinary), "mixed_primitive_runs_match_the_original_tape");
				binary.assign("changed"); positions.clear(); timer.Reset();
			}
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(archived.first == full && archived.second == shared,
			      "mixed_primitive_runs_outlive_strings_containers_and_clocks");
		}
		{
			struct InlineRecord {
				Vector position{-0.0F, std::bit_cast<float>(uint32_t{0x7FC00031})};
				Timer timer;
				std::string bytes{"x\0y", 3};
				std::string SaveCheckpoint() const {
					CheckpointWriter writer("InlineRecord1");
					writer(position, timer, bytes);
					return writer.Text();
				}
			};
			CheckpointText frozen;
			std::string fullReference, sharedReference;
			{
				std::vector<InlineRecord> records(512);
				for (auto& record: records) {
					record.timer.SetStartSimTimeTicks(17); record.timer.SetSimTimeLimitTicks(29);
					record.timer.SetStartRealTimeTicks(41); record.timer.SetRealTimeLimitTicks(53);
				}
				const auto saveInline = [&] {
					CheckpointWriter writer("InlineParent1"); writer(records);
					return writer.Text();
				};
				const auto ordinary = CheckpointWriter::CaptureNative(saveInline);
				fullReference = ordinary.Text(); sharedReference = ordinary.SharedText();
				CheckpointCache transient(true); transient.Begin();
				CheckpointWriter::CacheScope cache(&transient);
				CheckpointWriter::BatchScope batch(true);
				frozen = CheckpointWriter::CaptureNative(saveInline);
			}
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(frozen.HasPeerRuns() && archived.first == fullReference && archived.second == sharedReference,
			      "owned_inline_native_records_outlive_sources_and_keep_peer_lengths");
		}

		{
			std::vector<Vector> vectors(512, Vector(-0.0F, std::bit_cast<float>(uint32_t{0x7fc00031})));
			Timer timer;
			timer.SetStartSimTimeTicks(17); timer.SetSimTimeLimitTicks(29);
			timer.SetStartRealTimeTicks(41); timer.SetRealTimeLimitTicks(53);
			const auto saveArena = [&] {
				CheckpointWriter outer("Arena1");
				for (const Vector& vector: vectors) outer(CheckpointWriter::Native([&] {
					CheckpointWriter inner("ArenaChild1");
					inner(vector, timer, std::string("x\0y", 3));
					return inner.Text();
				}));
				return outer.Text();
			};
			const CheckpointText ordinary = CheckpointWriter::CaptureNative(saveArena);
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batches(true);
				frozen = CheckpointWriter::CaptureNative(saveArena);
			}
			vectors.clear(); timer.SetStartSimTimeTicks(91);
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(frozen.SameValues(ordinary) && archived.first == ordinary.Text() && archived.second == ordinary.SharedText(),
			      "owned_checkpoint_arena_outlives_capture_and_live_values");
		}

		{
			Timer timer;
			timer.SetStartSimTimeTicks(17); timer.SetSimTimeLimitTicks(29);
			timer.SetStartRealTimeTicks(41); timer.SetRealTimeLimitTicks(53);
			std::vector<std::pair<int, Timer>> timers{{7, timer}, {9, timer}};
			const auto saveTimers = [&] { CheckpointWriter writer("OwnedTimers1"); writer(timer, timers); return writer.Text(); };
			const std::string before = saveTimers();
			const CheckpointText frozen = CheckpointWriter::CaptureNative(saveTimers);
			timer.SetStartSimTimeTicks(61); timers[0].second.SetStartRealTimeTicks(71); timers.clear();
			const std::string shared = frozen.SharedText();
			check(frozen.HasPeerRuns() && frozen.Text() == before && shared == "12 OwnedTimers1 17 29 2 7 17 29 9 17 29 ",
			      "owned_checkpoint_copies_inline_timers_and_container_peer_runs");
		}

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

		const auto nested = [](int id) { CheckpointWriter inner("Nested1"); inner(id, std::string("payload")); return inner.Text(); };
		const auto composeWithNative = [&nested] {
			CheckpointWriter outer("Outer1");
			outer(CheckpointWriter::Native([&nested] { return nested(1); }));
			outer(CheckpointWriter::Native([&nested] { return nested(2); }));
			return outer.Text();
		};
		const std::string composedPlainly = composeWithNative();
		const CheckpointText composedUnderCapture = CheckpointWriter::CaptureNative(composeWithNative);
		check(!composedPlainly.empty() && composedUnderCapture.Text() == composedPlainly, "owned_checkpoint_keeps_two_nested_writers_under_native");

		std::string handRefusal;
		try {
			CheckpointWriter::CaptureNative([&nested] {
				CheckpointWriter outer("Outer1");
				outer(nested(1));
				outer(nested(2));
				return outer.Text();
			});
		} catch (const std::logic_error& error) { handRefusal = error.what(); }
		check(handRefusal == "a nested checkpoint value must be produced with CheckpointWriter::Native" &&
		          CheckpointWriter::IsCapturing() == capturing,
		      "owned_checkpoint_refuses_a_hand_composed_nested_value");

		CheckpointCache scopeCache;
		scopeCache.Begin();
		const int cacheOwner = 0;
		const CheckpointText firstReach = scopeCache.Remember(&cacheOwner, 7, CheckpointWriter::CaptureNative(composeWithNative));
		const CheckpointText secondReach = scopeCache.Remember(&cacheOwner, 7, CheckpointWriter::CaptureNative(composeWithNative));
		check(firstReach.SameValues(secondReach) && secondReach.Text() == composedPlainly && scopeCache.Reused() == 1,
		      "owned_checkpoint_cache_keeps_the_first_reach_identity_of_nested_writers");

		{
			struct RestoreClock {
				long long count = g_TimerMan.GetSimUpdateCount(), ticks = g_TimerMan.GetSimTickCount();
				~RestoreClock() { g_TimerMan.RestoreSimTickAfterPreview(count, ticks); }
			} restoreClock;
			constexpr int64_t firstTicks = 999960, secondTicks = 1999920;
			Timer past, future;
			past.SetStartSimTimeTicks(-1234567890123LL);
			future.SetStartSimTimeTicks(1500001);
			const auto saveTimers = [&](Writer& writer) {
				writer.NewPropertyWithValue("PastStart", past.GetStartSimTimeMS());
				writer.NewProperty("PastElapsed"); writer.ElapsedSimTime(past);
				writer.NewPropertyWithValue("FutureStart", future.GetStartSimTimeMS());
				writer.NewProperty("FutureElapsed"); writer.ElapsedSimTime(future);
			};
			const auto synchronous = [&] {
				auto stream = std::make_unique<std::ostringstream>();
				auto* output = stream.get();
				Writer writer(std::move(stream));
				writer << "timeless|";
				saveTimers(writer);
				return output->str();
			};
			auto timelessCalls = std::make_shared<std::atomic<int>>(0);
			const auto timeless = CheckpointText::Deferred([timelessCalls] { ++*timelessCalls; return std::string("timeless|"); });
			const auto capture = [&] {
				CheckpointBuffer buffer;
				buffer.Child(timeless);
				buffer.Child(Writer::Capture(saveTimers));
				return buffer.Finish();
			};
			CheckpointCache timerCache;
			timerCache.Begin();
			g_TimerMan.RestoreSimTickAfterPreview(60, firstTicks);
			const auto first = timerCache.Remember(&past, 0, capture());
			const auto firstImage = first.BindSimTime(firstTicks);
			const auto firstReference = synchronous();
			const bool signedElapsed = future.GetElapsedSimTimeMS() < 0;
			timerCache.Begin();
			g_TimerMan.RestoreSimTickAfterPreview(120, secondTicks);
			const auto fresh = capture();
			const auto second = timerCache.Remember(&past, 0, fresh);
			const auto secondImage = second.BindSimTime(secondTicks);
			const auto secondReference = synchronous();
			check(first.SameValues(fresh) && timerCache.Reused() == 1 && timelessCalls->load() == 0,
			      "owned_checkpoint_caches_timer_values_without_formatting_or_binding",
			      "same_values=" + std::to_string(first.SameValues(fresh)) + " reused=" + std::to_string(timerCache.Reused()) +
			          " timeless_calls=" + std::to_string(timelessCalls->load()));
			auto firstText = std::async(std::launch::async, [firstImage] { return firstImage.Text(); });
			auto secondText = std::async(std::launch::async, [secondImage] { return secondImage.Text(); });
			const std::string firstBound = firstText.get();
			const std::string secondBound = secondText.get();
			check(firstBound == firstReference && secondBound == secondReference && firstReference != secondReference &&
			          past.GetStartSimTimeMS() < 0 && signedElapsed && future.GetElapsedSimTimeMS() > 0,
			      "owned_checkpoint_binds_signed_timers_to_each_image_tick",
			      "first_bound=\"" + firstBound + "\" first_reference=\"" + firstReference + "\" second_bound=\"" + secondBound +
			          "\" second_reference=\"" + secondReference + "\" start_ms=" + std::to_string(past.GetStartSimTimeMS()) +
			          " signed_elapsed=" + std::to_string(signedElapsed) + " future_ms=" + std::to_string(future.GetElapsedSimTimeMS()));
			check(firstImage.Text() == firstReference && first.Text() == firstReference && fresh.Text() == secondReference && timelessCalls->load() == 1,
			      "owned_checkpoint_timer_images_preserve_prior_text_and_timeless_sharing",
			      "first_image=\"" + firstImage.Text() + "\" first=\"" + first.Text() + "\" first_reference=\"" + firstReference +
			          "\" fresh=\"" + fresh.Text() + "\" second_reference=\"" + secondReference + "\" timeless_calls=" +
			          std::to_string(timelessCalls->load()));
			CheckpointBuffer prior; prior.Raw("prior|"); prior.Child(first);
			CheckpointBuffer current; current.Raw("current|"); current.Child(fresh);
			const auto reusedTimers = current.Finish().ReuseChildren(prior.Finish());
			const std::string reusedText = reusedTimers.BindSimTime(secondTicks).Text();
			const std::string encodedText = first.Base64().BindSimTime(secondTicks).Text();
			check(reusedText == "current|" + secondReference && encodedText == base64_encode(secondReference, true),
			      "owned_checkpoint_binds_reused_and_encoded_timer_children",
			      "reused=\"" + reusedText + "\" expected=\"current|" + secondReference + "\" encoded=\"" + encodedText +
			          "\" expected_encoded=\"" + base64_encode(secondReference, true) + "\"");
		}

		{
			CheckpointCache sceneCache;
			CheckpointWriter::CacheScope cacheScope(&sceneCache);
			const auto capture = [](const SceneObject& object) {
				return Writer::Capture([&](Writer& writer) { Scene::SaveSceneObject(writer, &object, false, false); });
			};
			MOPixel pixel;
			pixel.Create();
			pixel.SetPos(Vector(10, 0));
			const long identity = pixel.GetUniqueID();
			const uint64_t stamp = pixel.CheckpointWriteGeneration();
			const auto before = capture(pixel);
			const auto unchanged = capture(pixel);
			pixel.Destroy();
			pixel.Create();
			Reader reader(std::make_unique<std::istringstream>(std::to_string(identity)), "checkpoint-identity", true);
			pixel.ReadProperty("UniqueID", reader);
			pixel.AdoptPersistedUniqueID();
			pixel.SetPos(Vector(20, 0));
			const auto restored = capture(pixel);
			std::cout << "[owned-checkpoint] restored_identity uid_kept=" << (pixel.GetUniqueID() == identity) << " generation_moved=" << (pixel.CheckpointWriteGeneration() != stamp)
			          << " unchanged_same=" << before.SameValues(unchanged) << " restored_same=" << before.SameValues(restored) << " text_differs=" << (before.Text() != restored.Text()) << std::endl;
			check(pixel.GetUniqueID() == identity && pixel.CheckpointWriteGeneration() != stamp &&
			          before.SameValues(unchanged) && !before.SameValues(restored) && before.Text() != restored.Text(),
			      "owned_checkpoint_does_not_reuse_a_restored_identity_at_the_same_address");

			Deployment deployment;
			deployment.SetPos(Vector(10, 0));
			const auto firstDeployment = capture(deployment);
			deployment.Reset();
			deployment.SetPos(Vector(20, 0));
			const auto nextDeployment = capture(deployment);
			check(firstDeployment.Text() != nextDeployment.Text(), "owned_checkpoint_recaptures_a_scene_object_without_a_lifetime_identity");
		}

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
		// A formatted deferred text keeps its text, not its producer: what the producer captured (a frozen heap copy) is let go.
		auto held = std::make_shared<int>(7);
		const auto holding = CheckpointText::Deferred([held] { return std::to_string(*held); }, 0, "OwnedCheckpointHolding1");
		const long before = held.use_count();
		const bool producedOnce = holding.Text() == "7" && holding.Text() == "7" && holding.SameValues(holding);
		check(before == 2 && producedOnce && held.use_count() == 1, "owned_checkpoint_formatted_text_lets_its_producer_go",
		      "use_count before=" + std::to_string(before) + " after=" + std::to_string(held.use_count()));

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
