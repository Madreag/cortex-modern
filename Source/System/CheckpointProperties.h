#pragma once

#include "CheckpointArchive.h"

namespace RTE {

	template<size_t Size> struct CheckpointPropertyName {
		char text[Size];
		constexpr CheckpointPropertyName(const char (&name)[Size]) { std::copy_n(name, Size, text); }
	};

	namespace CheckpointProperties {
		template<class T> void Pack(char* into, size_t& at, const T& value) {
			if constexpr (std::is_same_v<T, std::string>) {
				const uint64_t size = value.size();
				Pack(into, at, size);
				std::memcpy(into + at, value.data(), value.size());
				at += value.size();
			} else {
				static_assert(std::is_trivially_copyable_v<T>);
				std::memcpy(into + at, &value, sizeof(value));
				at += sizeof(value);
			}
		}
		template<class T> T Unpack(std::string_view& values) {
			if constexpr (std::is_same_v<T, std::string>) {
				const uint64_t size = Unpack<uint64_t>(values);
				if (size > values.size()) throw std::logic_error("truncated property string");
				std::string value(values.substr(0, size));
				values.remove_prefix(size);
				return value;
			} else {
				static_assert(std::is_trivially_copyable_v<T>);
				if (values.size() < sizeof(T)) throw std::logic_error("truncated property value");
				T value;
				std::memcpy(&value, values.data(), sizeof(value));
				values.remove_prefix(sizeof(value));
				return value;
			}
		}
		struct VectorValue {
			float x, y;
			void Write(Writer& writer) const { writer << Vector(x, y); }
		};
		template<class T> auto Freeze(const T& value) {
			if constexpr (std::is_same_v<T, Vector>) return VectorValue{value.m_X, value.m_Y};
			else if constexpr (std::is_same_v<T, bool>) return static_cast<uint64_t>(value);
			else {
				static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>, "checkpoint properties require scalar or owned string values");
				return value;
			}
		}
		template<CheckpointPropertyName Name, class T> struct Owned {
			T value;
			static constexpr size_t StaticBytes = std::is_same_v<T, std::string> ? sizeof(uint64_t) : sizeof(T);
			void Read(std::string_view& values) { value = Unpack<T>(values); }
			static void WriteValue(Writer& writer, const T& value) {
				writer.NewProperty(Name.text);
				if constexpr (std::is_same_v<T, VectorValue>) value.Write(writer);
				else writer << value;
			}
			void Write(Writer& writer) const { WriteValue(writer, value); }
			size_t DynamicBytes() const {
				if constexpr (std::is_same_v<T, std::string>) return value.size();
				else return 0;
			}
		};
		template<CheckpointPropertyName Name, class T> struct Reference {
			const T& value;
			using OwnedType = Owned<Name, decltype(Freeze(value))>;
			static constexpr bool HasString = std::is_same_v<T, std::string>;
			size_t DynamicBytes() const { if constexpr (HasString) return value.size(); else return 0; }
			void PackValue(char* into, size_t& at) const {
				if constexpr (HasString) Pack(into, at, value);
				else Pack(into, at, Freeze(value));
			}
			void Write(Writer& writer) const { writer.NewPropertyWithValue(Name.text, value); }
			auto Capture() const { return Owned<Name, decltype(Freeze(value))>{Freeze(value)}; }
		};
		template<class... Fields> void Expand(std::string& text, std::string_view values, bool tape) {
			const int indent = Unpack<int>(values);
			std::tuple<Fields...> fields;
			std::apply([&values](auto&... field) { (field.Read(values), ...); }, fields);
			if (!values.empty()) throw std::logic_error("trailing property values");
			const CheckpointText output = Writer::Capture([&fields](Writer& writer) {
				std::apply([&writer](const auto&... field) { (field.Write(writer), ...); }, fields);
			}, indent);
			const std::string& plain = output.Text();
			if (tape) {
				text.push_back(static_cast<char>(CheckpointBuffer::ValueKind::Raw));
				const uint64_t size = plain.size();
				text.append(reinterpret_cast<const char*>(&size), sizeof(size));
			}
			text += plain;
		}
	}

	// The writer tape owns the exact fields. Property names and the decoder have
	// static storage; no callback, tuple or live reference survives the capture.
	template<CheckpointPropertyName Name, class T> auto CheckpointProperty(const T& value) {
		return CheckpointProperties::Reference<Name, T>{value};
	}
	template<class... Properties> void WriteCapturedProperties(Writer& writer, const Properties&... properties) {
		if (!writer.IsCapturing() || !CheckpointWriter::BatchEnabled()) {
			(properties.Write(writer), ...);
			return;
		}
		constexpr size_t staticBytes = sizeof(int) + (Properties::OwnedType::StaticBytes + ... + size_t{0});
		const auto capture = [&](char* into, size_t size) {
			size_t at = 0;
			CheckpointProperties::Pack(into, at, writer.GetIndent());
			(properties.PackValue(into, at), ...);
			if (at != size) throw std::logic_error("property capture size differs");
			writer.AppendPropertyBlock(std::string_view(into, size), &CheckpointProperties::Expand<typename Properties::OwnedType...>);
		};
		if constexpr ((Properties::HasString || ...)) {
			std::string record(staticBytes + (properties.DynamicBytes() + ... + size_t{0}), '\0');
			capture(record.data(), record.size());
		} else {
			std::array<char, staticBytes> record;
			capture(record.data(), record.size());
		}
	}
	template<CheckpointPropertyName Name, class T> void WriteCapturedPropertySequence(Writer& writer, std::vector<T> values) {
		if (!writer.IsCapturing() || !CheckpointWriter::BatchEnabled()) {
			for (const T& value: values) writer.NewPropertyWithValue(Name.text, value);
			return;
		}
		using Value = decltype(CheckpointProperties::Freeze(std::declval<const T&>()));
		auto owned = [&] {
			if constexpr (std::is_same_v<T, Value>) return std::move(values);
			else {
				std::vector<Value> frozen;
				frozen.reserve(values.size());
				for (const T& value: values) frozen.push_back(CheckpointProperties::Freeze(value));
				return frozen;
			}
		}();
		size_t bytes = sizeof(owned) + owned.size() * sizeof(Value);
		if constexpr (std::is_same_v<Value, std::string>) for (const Value& value: owned) bytes += value.size();
		const int indent = writer.GetIndent();
		writer.Append(CheckpointText::Deferred([owned = std::move(owned), indent] {
			return Writer::Capture([&](Writer& output) {
				for (const Value& value: owned) CheckpointProperties::Owned<Name, Value>::WriteValue(output, value);
			}, indent).Text();
		}, bytes));
	}
}
