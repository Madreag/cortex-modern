#pragma once

#include "Timer.h"
#include "Vector.h"
#include "Box.h"

#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>

namespace RTE {

	// Portable runtime values. Floats retain their bits; strings retain arbitrary bytes.
	// Native pointers are deliberately excluded: their owners encode stable graph links.
	class CheckpointWriter {
	public:
		explicit CheckpointWriter(std::string_view version) { Value(std::string(version)); }
		const std::string& Text() const { return m_Text; }
		template <class... Values> void operator()(const Values&... values) { (Value(values), ...); }

		template <class T> requires std::is_integral_v<T>
		void Value(T value) {
			char buffer[32];
			const auto result = [&] {
				if constexpr (std::is_signed_v<T>) return std::to_chars(buffer, buffer + sizeof(buffer), static_cast<int64_t>(value));
				else return std::to_chars(buffer, buffer + sizeof(buffer), static_cast<uint64_t>(value));
			}();
			m_Text.append(buffer, result.ptr);
			m_Text.push_back(' ');
		}
		// A bool's storage byte is only 0 or 1 once someone has assigned it, and reading an unassigned
		// one as a bool is undefined: compilers variously mask it to the low bit, keep it, or take a
		// single-digit fast path on a value they assume is 0 or 1. Copy the byte and write what it is.
		void Value(const bool& value) {
			unsigned char byte;
			std::memcpy(&byte, &value, sizeof(byte));
			Value(static_cast<unsigned int>(byte));
		}
		template <class T> requires std::is_enum_v<T>
		void Value(T value) { Value(static_cast<std::underlying_type_t<T>>(value)); }
		void Value(float value) { Value(std::bit_cast<uint32_t>(value)); }
		void Value(double value) { Value(std::bit_cast<uint64_t>(value)); }
		void Value(const std::string& value) { Value(value.size()); m_Text += value; m_Text.push_back(' '); }
		void Value(const Vector& value) { (*this)(value.m_X, value.m_Y); }
		void Value(const Box& value) { (*this)(value.m_Corner, value.m_Width, value.m_Height); }
		void Value(const Timer& value) { (*this)(value.GetStartSimTimeMS(), value.GetSimTimeLimitTicks(), value.GetStartRealTimeMS(), value.GetRealTimeLimitTicks()); }
		template <class T, size_t N> void Value(const std::array<T, N>& values) { for (const auto& value: values) Value(value); }
		template <class T, size_t N> void Value(const T (&values)[N]) { for (const auto& value: values) Value(value); }
		template <class T, class U> void Value(const std::pair<T, U>& value) { (*this)(value.first, value.second); }
		template <class T> void Value(const std::vector<T>& values) { Value(values.size()); for (const auto& value: values) Value(value); }
		// vector<bool> packs bits, so its elements have no storage byte of their own to copy.
		void Value(const std::vector<bool>& values) { Value(values.size()); for (bool value: values) Value(value ? 1u : 0u); }
		template <class T> void Value(const std::list<T>& values) { Value(values.size()); for (const auto& value: values) Value(value); }
		template <class T> void Value(const std::deque<T>& values) { Value(values.size()); for (const auto& value: values) Value(value); }
		template <class T> void Value(const std::set<T>& values) { Value(values.size()); for (const auto& value: values) Value(value); }
		template <class K, class V> void Value(const std::map<K, V>& values) { Value(values.size()); for (const auto& [key, value]: values) (*this)(key, value); }
		template <class K, class V> void Value(const std::unordered_map<K, V>& values) { Value(std::map<K, V>(values.begin(), values.end())); }
		template <class T> requires requires(const T& value) { value.SaveCheckpoint(); }
		void Value(const T& value) { Value(value.SaveCheckpoint()); }

	private:
		std::string m_Text;
	};

	// The first field of a nested payload is its version tag, so a refusal can name the type that refused.
	inline std::string CheckpointTypeName(std::string_view text) {
		const size_t space = text.find(' ');
		size_t size = 0;
		if (space == std::string_view::npos || std::from_chars(text.data(), text.data() + space, size).ec != std::errc() || space + 1 + size > text.size()) return "runtime";
		return std::string(text.substr(space + 1, size));
	}

	class CheckpointReader {
	public:
		CheckpointReader(std::string_view text, std::string_view version, bool validateOnly = false) : m_Text(text), m_ValidateOnly(validateOnly) {
			std::string found;
			Value(found);
			if (found != version) throw std::runtime_error("unsupported runtime checkpoint version");
		}
		void Finish() {
			if (!m_Text.empty()) throw std::runtime_error("trailing runtime checkpoint data");
			for (auto& apply: m_Apply) apply();
			m_Apply.clear();
		}
		void OnCommit(std::function<void()> apply) { if (!m_ValidateOnly) m_Apply.push_back(std::move(apply)); }
		template <class... Values> void operator()(Values&... values) { (Stage(values), ...); }

		// Decode fields into independent storage. A malformed later field cannot partially
		// change the target, and validation never creates/replaces an owning native object.
		template <class T> void Stage(T& value) {
			if constexpr (requires { value.LoadCheckpoint(std::string_view{}, true); }) {
				std::string text;
				Value(text);
				if (!value.LoadCheckpoint(text, true)) throw std::runtime_error("invalid nested " + CheckpointTypeName(text) + " checkpoint");
				OnCommit([&value, text = std::move(text)] {
					if (!value.LoadCheckpoint(text)) throw std::runtime_error("could not apply nested " + CheckpointTypeName(text) + " checkpoint");
				});
			} else {
				T candidate{};
				Value(candidate);
				OnCommit([&value, candidate = std::move(candidate)]() mutable { value = std::move(candidate); });
			}
		}
		template <class T, size_t N> void Stage(T (&values)[N]) { for (auto& value: values) Stage(value); }
		template <class T, size_t N> void Stage(std::array<T, N>& values) { for (auto& value: values) Stage(value); }

		template <class T> requires std::is_integral_v<T>
		void Value(T& value) {
			const size_t end = m_Text.find(' ');
			if (end == std::string_view::npos || end == 0) throw std::runtime_error("truncated runtime checkpoint number");
			const auto refuse = [this, end] { return std::runtime_error("invalid runtime checkpoint integer '" + std::string(m_Text.substr(0, end)) + "'"); };
			if constexpr (std::is_signed_v<T>) {
				int64_t number = 0;
				const auto parsed = std::from_chars(m_Text.data(), m_Text.data() + end, number);
				if (parsed.ec != std::errc() || parsed.ptr != m_Text.data() + end || number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) throw refuse();
				value = static_cast<T>(number);
			} else {
				uint64_t number = 0;
				const auto parsed = std::from_chars(m_Text.data(), m_Text.data() + end, number);
				if (parsed.ec != std::errc() || parsed.ptr != m_Text.data() + end || number > static_cast<uint64_t>(std::numeric_limits<T>::max())) throw refuse();
				value = static_cast<T>(number);
			}
			m_Text.remove_prefix(end + 1);
		}
		template <class T> requires std::is_enum_v<T>
		void Value(T& value) { std::underlying_type_t<T> raw; Value(raw); value = static_cast<T>(raw); }
		void Value(float& value) { uint32_t bits; Value(bits); value = std::bit_cast<float>(bits); }
		void Value(double& value) { uint64_t bits; Value(bits); value = std::bit_cast<double>(bits); }
		void Value(std::string& value) {
			size_t size = 0;
			Value(size);
			if (size >= m_Text.size() || m_Text[size] != ' ') throw std::runtime_error("truncated runtime checkpoint string");
			value.assign(m_Text.data(), size);
			m_Text.remove_prefix(size + 1);
		}
		void Value(Vector& value) { Value(value.m_X); Value(value.m_Y); }
		void Value(Box& value) { Value(value.m_Corner); Value(value.m_Width); Value(value.m_Height); }
		void Value(Timer& value) {
			int64_t start, limit, realStart, realLimit;
			Value(start); Value(limit); Value(realStart); Value(realLimit);
			value.SetStartSimTimeTicks(start); value.SetSimTimeLimitTicks(limit);
			value.SetStartRealTimeTicks(realStart); value.SetRealTimeLimitTicks(realLimit);
		}
		template <class T, size_t N> void Value(std::array<T, N>& values) { for (auto& value: values) Value(value); }
		template <class T, size_t N> void Value(T (&values)[N]) { for (auto& value: values) Value(value); }
		template <class T, class U> void Value(std::pair<T, U>& value) { Value(value.first); Value(value.second); }
		template <class T> void Value(std::vector<T>& values) {
			size_t size = 0;
			Value(size);
			if (size > m_Text.size()) throw std::runtime_error("invalid runtime checkpoint count");
			std::vector<T> candidate(size);
			for (auto& value: candidate) Value(value);
			values = std::move(candidate);
		}
		void Value(std::vector<bool>& values) {
			const size_t size = Count();
			values.assign(size, false);
			for (size_t index = 0; index < size; ++index) { bool value; Value(value); values[index] = value; }
		}
		template <class T> void Value(std::list<T>& values) { Sequence(values); }
		template <class T> void Value(std::deque<T>& values) { Sequence(values); }
		template <class T> void Value(std::set<T>& values) {
			const size_t size = Count();
			for (size_t index = 0; index < size; ++index) { T value{}; Value(value); if (!values.insert(std::move(value)).second) throw std::runtime_error("duplicate runtime checkpoint set value"); }
		}
		template <class K, class V> void Value(std::map<K, V>& values) { Mapping(values); }
		template <class K, class V> void Value(std::unordered_map<K, V>& values) { Mapping(values); }
		template <class T> requires requires(T& value) { value.LoadCheckpoint(std::string_view{}, false); }
		void Value(T& value) {
			std::string text;
			Value(text);
			if (!value.LoadCheckpoint(text)) throw std::runtime_error("invalid runtime checkpoint value");
		}

	private:
		std::string_view m_Text;
		bool m_ValidateOnly;
		std::vector<std::function<void()>> m_Apply;
		size_t Count() {
			size_t size = 0;
			Value(size);
			if (size > m_Text.size()) throw std::runtime_error("invalid runtime checkpoint count");
			return size;
		}
		template <class T> void Sequence(T& values) {
			const size_t size = Count();
			values.clear();
			for (size_t index = 0; index < size; ++index) { typename T::value_type value{}; Value(value); values.push_back(std::move(value)); }
		}
		template <class T> void Mapping(T& values) {
			const size_t size = Count();
			values.clear();
			for (size_t index = 0; index < size; ++index) {
				typename T::key_type key{};
				typename T::mapped_type value{};
				Value(key); Value(value);
				if (!values.emplace(std::move(key), std::move(value)).second) throw std::runtime_error("duplicate runtime checkpoint map key");
			}
		}
	};
}
