#include "Writer.h"
#include "System.h"
#include "CheckpointArchive.h"
#include "Base64/base64.h"

#include <iomanip>
#include <fstream>
#include <mutex>
#include <cstring>
#include <sstream>
#include <stdexcept>

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
	std::string values;
	std::vector<CheckpointText> children;
	std::function<std::string()> produce;
	std::string identity;
	size_t ownedBytes = 0;
	mutable std::once_flag ready;
	mutable std::string text;
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
	if (m_Data->values != other.m_Data->values ||
	    m_Data->children.size() != other.m_Data->children.size()) return false;
	for (size_t i = 0; i < m_Data->children.size(); ++i) if (!m_Data->children[i].SameValues(other.m_Data->children[i])) return false;
	return true;
}

const std::string& CheckpointText::Text() const {
	static const std::string empty;
	if (!m_Data) return empty;
	std::call_once(m_Data->ready, [&] {
		if (m_Data->produce) { m_Data->text = m_Data->produce(); return; }
		std::string& text = m_Data->text;
		const std::string_view values = m_Data->values;
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
					const auto& child = m_Data->children.at(static_cast<size_t>(index)).Text();
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
	});
	return m_Data->text;
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
	Entry& entry = m_Entries[owner][channel];
	entry.generation = m_Generation;
	if (entry.text.SameValues(value)) return entry.text;
	m_Retired.push_back(std::move(entry.text));
	entry.text = std::move(value);
	return entry.text;
}

std::vector<CheckpointText> CheckpointCache::RetireUnused() {
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
