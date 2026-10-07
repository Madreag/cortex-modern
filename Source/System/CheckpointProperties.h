#pragma once

#include "CheckpointArchive.h"

namespace RTE {

	template<size_t Size> struct CheckpointPropertyName {
		char text[Size];
		constexpr CheckpointPropertyName(const char (&name)[Size]) { std::copy_n(name, Size, text); }
	};

	namespace CheckpointProperties {
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
			void Write(Writer& writer) const { writer.NewPropertyWithValue(Name.text, value); }
			auto Capture() const { return Owned<Name, decltype(Freeze(value))>{Freeze(value)}; }
		};
	}

	// References exist only for this call. Its deferred producer owns the exact
	// primitive/string/vector fields and the property names have static storage.
	template<CheckpointPropertyName Name, class T> auto CheckpointProperty(const T& value) {
		return CheckpointProperties::Reference<Name, T>{value};
	}
	template<class... Properties> void WriteCapturedProperties(Writer& writer, const Properties&... properties) {
		if (!writer.IsCapturing() || !CheckpointWriter::BatchEnabled()) {
			(properties.Write(writer), ...);
			return;
		}
		auto owned = std::tuple{properties.Capture()...};
		const size_t bytes = sizeof(owned) + std::apply([](const auto&... fields) { return (fields.DynamicBytes() + ... + size_t{0}); }, owned);
		const int indent = writer.GetIndent();
		writer.Append(CheckpointText::Deferred([owned = std::move(owned), indent] {
			return Writer::Capture([&](Writer& output) {
				std::apply([&output](const auto&... fields) { (fields.Write(output), ...); }, owned);
			}, indent).Text();
		}, bytes));
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
