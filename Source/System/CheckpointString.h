#pragma once

#include "CheckpointNativeStorage.h"

#include <compare>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RTE {

	// The legacy string keeps its reference API; the saver reads only its owned byte mirror.
	class CheckpointString {
	public:
		using value_type = char;
		using size_type = std::string::size_type;
		using const_iterator = std::string::const_iterator;
		static constexpr size_type npos = std::string::npos;
		static std::string SelfTestMismatch();
		CheckpointString() : m_Mirrored(CheckpointNativeStorage::Enabled()) {}
		CheckpointString(const CheckpointString& other) { Assign(other.Value()); }
		CheckpointString(CheckpointString&& other) { Assign(std::move(other)); }
		CheckpointString(const std::string& value) { Assign(value); }
		CheckpointString(std::string&& value) { Assign(std::move(value)); }
		CheckpointString(std::string_view value) { Assign(std::string(value)); }
		CheckpointString(const char* value) { Assign(std::string(value)); }
		CheckpointString(const char* value, size_t size) { Assign(std::string(value, size)); }
		CheckpointString(size_t size, char value) { Assign(std::string(size, value)); }
		template<class Iterator> CheckpointString(Iterator first, Iterator last) { Assign(std::string(first, last)); }
		CheckpointString& operator=(const CheckpointString& other) { if (this != &other) Assign(other.Value()); return *this; }
		CheckpointString& operator=(CheckpointString&& other) { if (this != &other) Assign(std::move(other)); return *this; }
		CheckpointString& operator=(const std::string& value) { Assign(value); return *this; }
		CheckpointString& operator=(std::string&& value) { Assign(std::move(value)); return *this; }
		CheckpointString& operator=(std::string_view value) { Assign(std::string(value)); return *this; }
		CheckpointString& operator=(const char* value) { Assign(std::string(value)); return *this; }
		CheckpointString& operator=(char value) { Assign(std::string(1, value)); return *this; }

		const std::string& Value() const {
			if (m_Mirrored) if (const auto* value = CheckpointNativeStorage::ReadString(m_Bytes.data(), m_Bytes.size())) return *value;
			return m_Value;
		}
		operator const std::string&() const { return Value(); }
		operator std::string_view() const { return Value(); }
		bool empty() const { return Value().empty(); }
		size_t size() const { return Value().size(); }
		size_t length() const { return Value().length(); }
		const char* c_str() const { return Value().c_str(); }
		const char* data() const { return Value().data(); }
		const char& operator[](size_t at) const { return Value()[at]; }
		const char& at(size_t at) const { return Value().at(at); }
		const char& front() const { return Value().front(); }
		const char& back() const { return Value().back(); }
		const_iterator begin() const { return Value().begin(); }
		const_iterator end() const { return Value().end(); }
		const_iterator cbegin() const { return Value().cbegin(); }
		const_iterator cend() const { return Value().cend(); }
		std::string substr(size_t at = 0, size_t count = npos) const { return Value().substr(at, count); }
		template<class... Args> size_t find(Args&&... args) const { return Value().find(std::forward<Args>(args)...); }
		template<class... Args> size_t rfind(Args&&... args) const { return Value().rfind(std::forward<Args>(args)...); }
		template<class... Args> size_t find_first_of(Args&&... args) const { return Value().find_first_of(std::forward<Args>(args)...); }
		template<class... Args> size_t find_last_of(Args&&... args) const { return Value().find_last_of(std::forward<Args>(args)...); }
		template<class... Args> size_t find_first_not_of(Args&&... args) const { return Value().find_first_not_of(std::forward<Args>(args)...); }
		template<class... Args> size_t find_last_not_of(Args&&... args) const { return Value().find_last_not_of(std::forward<Args>(args)...); }
		template<class... Args> int compare(Args&&... args) const { return Value().compare(std::forward<Args>(args)...); }
		template<class T> bool starts_with(const T& value) const { return Value().starts_with(value); }
		template<class T> bool ends_with(const T& value) const { return Value().ends_with(value); }

		void clear() noexcept { m_Value.clear(); m_Bytes.clear(); }
		void reserve(size_t size) { m_Value.reserve(size); }
		void shrink_to_fit() { m_Value.shrink_to_fit(); m_Bytes.shrink_to_fit(); }
		void resize(size_t size, char value = char()) { Mutate([&](std::string& into) { into.resize(size, value); }); }
		void push_back(char value) { Mutate([&](std::string& into) { into.push_back(value); }); }
		void pop_back() { Mutate([](std::string& into) { into.pop_back(); }); }
		template<class T> CheckpointString& operator+=(const T& value) { return Mutate([&](std::string& into) { into += value; }); }
		template<class... Args> CheckpointString& assign(Args&&... args) { return Mutate([&](std::string& into) { into.assign(std::forward<Args>(args)...); }); }
		template<class... Args> CheckpointString& append(Args&&... args) { return Mutate([&](std::string& into) { into.append(std::forward<Args>(args)...); }); }
		template<class... Args> CheckpointString& replace(Args&&... args) { return Mutate([&](std::string& into) { into.replace(std::forward<Args>(args)...); }); }
		CheckpointString& erase(size_t at = 0, size_t count = npos) { return Mutate([&](std::string& into) { into.erase(at, count); }); }
		template<class... Args> CheckpointString& insert(size_t at, Args&&... args) { return Mutate([&](std::string& into) { into.insert(at, std::forward<Args>(args)...); }); }
		void swap(CheckpointString& other) noexcept { m_Value.swap(other.m_Value); m_Bytes.swap(other.m_Bytes); std::swap(m_Mirrored, other.m_Mirrored); }

		friend bool operator==(const CheckpointString& left, const CheckpointString& right) { return left.Value() == right.Value(); }
		friend auto operator<=>(const CheckpointString& left, const CheckpointString& right) { return left.Value() <=> right.Value(); }
		friend bool operator==(const CheckpointString& left, std::string_view right) { return left.Value() == right; }
		friend auto operator<=>(const CheckpointString& left, std::string_view right) { return std::string_view(left.Value()) <=> right; }
		friend bool operator==(const CheckpointString& left, const std::string& right) { return left.Value() == right; }
		friend auto operator<=>(const CheckpointString& left, const std::string& right) { return left.Value() <=> right; }
		friend bool operator==(const CheckpointString& left, const char* right) { return left.Value() == right; }
		friend auto operator<=>(const CheckpointString& left, const char* right) { return left.Value() <=> right; }
		friend std::string operator+(const CheckpointString& left, const CheckpointString& right) { return left.Value() + right.Value(); }
		friend std::string operator+(const CheckpointString& left, const std::string& right) { return left.Value() + right; }
		friend std::string operator+(const std::string& left, const CheckpointString& right) { return left + right.Value(); }
		friend std::string operator+(const CheckpointString& left, const char* right) { return left.Value() + right; }
		friend std::string operator+(const char* left, const CheckpointString& right) { return left + right.Value(); }
		friend std::string operator+(const CheckpointString& left, char right) { return left.Value() + right; }
		friend std::string operator+(char left, const CheckpointString& right) { return left + right.Value(); }
		friend std::ostream& operator<<(std::ostream& stream, const CheckpointString& value) { return stream << value.Value(); }
		friend std::istream& operator>>(std::istream& stream, CheckpointString& value) { std::string read; stream >> read; value = std::move(read); return stream; }
		template<class Json> friend void to_json(Json& json, const CheckpointString& value) { json = value.Value(); }

	private:
		using Bytes = std::vector<char, CheckpointNativeAllocator<char>>;
		void Assign(std::string value) {
			const bool mirrored = CheckpointNativeStorage::Enabled();
			Bytes bytes;
			if (mirrored) bytes.assign(value.begin(), value.end());
			m_Value.swap(value); m_Bytes.swap(bytes); m_Mirrored = mirrored;
		}
		void Assign(CheckpointString&& other) {
			if (CheckpointNativeStorage::Enabled() && !other.m_Mirrored) { Assign(other.Value()); other.clear(); }
			else { m_Value = std::move(other.m_Value); m_Bytes = std::move(other.m_Bytes); m_Mirrored = other.m_Mirrored; }
		}
		template<class Work> CheckpointString& Mutate(Work work) {
			if (!CheckpointNativeStorage::Enabled()) { work(m_Value); m_Bytes.clear(); m_Mirrored = false; }
			else { std::string next = Value(); work(next); Assign(std::move(next)); }
			return *this;
		}
		std::string m_Value;
		Bytes m_Bytes;
		bool m_Mirrored = false;
	};
}

template<> struct std::hash<RTE::CheckpointString> {
	size_t operator()(const RTE::CheckpointString& value) const { return std::hash<std::string>{}(value.Value()); }
};
