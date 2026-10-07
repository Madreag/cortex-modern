#pragma once

#include "Timer.h"
#include "Vector.h"
#include "Box.h"
#include "FloatText.h"
#include "Writer.h"

#include <array>
#include <atomic>
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
#include <optional>
#include <tuple>
#include <cstddef>
#include <algorithm>

namespace RTE {

	// Portable runtime values. Floats retain their bits; strings retain arbitrary bytes.
	// Native pointers are deliberately excluded: their owners encode stable graph links.
	class CheckpointWriter {
	public:
		explicit CheckpointWriter(std::string_view version) : m_Recording(IsCapturing()), m_Capture(!(s_Capture && s_Capture->inlineOutput)),
		    m_Output(s_Capture ? s_Capture->inlineOutput : nullptr) {
			if (m_Recording && BatchEnabled()) Buffer().String(version); else Value(std::string(version));
		}
		const std::string& Text() const {
			if (!m_Recording) return m_Text;
			if (m_Output) {
				if (s_Capture->inlineWritten) throw std::logic_error("nested checkpoint capture requires Native");
				s_Capture->inlineWritten = true;
				return m_Text;
			}
			if (s_Capture->output) throw std::logic_error("nested checkpoint capture requires Native");
			s_Capture->output = Buffer().Finish();
			return m_Text;
		}
		static bool IsCapturing() { return s_Capture != nullptr; }
		/// Enables bounded page-copy batches for the joined workers of a multiplayer image.
		/// Manual and single-player saves keep their existing visitors.
		class BatchScope {
		public:
			explicit BatchScope(bool enabled) : m_Enabled(enabled) { if (m_Enabled) s_Batches.fetch_add(1, std::memory_order_relaxed); }
			~BatchScope() { if (m_Enabled) s_Batches.fetch_sub(1, std::memory_order_relaxed); }
			BatchScope(const BatchScope&) = delete;
			BatchScope& operator=(const BatchScope&) = delete;
		private:
			bool m_Enabled;
		};
		static bool BatchEnabled() { return s_Batches.load(std::memory_order_relaxed) != 0; }
		class CacheScope {
		public:
			explicit CacheScope(CheckpointCache* cache) : m_Previous(s_Cache) { s_Cache = cache; }
			~CacheScope() { s_Cache = m_Previous; }
		private:
			CheckpointCache* m_Previous;
		};
		static CheckpointCache* CurrentCache() { return s_Cache; }
		/// Captures an existing checkpoint visitor into owned values.
		static CheckpointText CaptureNative(const std::function<std::string()>& visit) {
			CheckpointBuffer::AllocationScope allocation(BatchEnabled());
			CaptureScope scope;
			std::string existing = visit();
			if (scope.output && !existing.empty()) throw std::logic_error("checkpoint capture result has uncaptured text");
			return scope.output ? std::move(*scope.output) : CheckpointText(std::move(existing));
		}
		static CheckpointText Native(const std::function<std::string()>& visit) {
			return IsCapturing() ? CaptureNative(visit) : CheckpointText(visit());
		}
		static CheckpointText CaptureValues(const std::function<CheckpointText()>& visit) {
			CheckpointBuffer::AllocationScope allocation(BatchEnabled());
			CaptureScope scope;
			CheckpointText result = visit();
			if (scope.output) throw std::logic_error("checkpoint capture result was not consumed");
			return result;
		}
		template <class... Values> void operator()(const Values&... values) {
			if (m_Recording && BatchEnabled()) {
				CaptureFields<0>(std::forward_as_tuple(values...));
			} else {
				(Value(values), ...);
			}
		}
		/// Writes values only this machine holds (its clocks, pacing, seat or view): the archive carries them as before,
		/// and the shared state a peer is compared on leaves them out.
		template <class... Values> void PerPeer(const Values&... values) {
			BeginPerPeer();
			(*this)(values...);
			EndPerPeer();
		}
		/// Opens and closes such a run around values a visitor writes itself.
		void BeginPerPeer() { if (m_Recording) Buffer().PeerBegin(); }
		void EndPerPeer() { if (m_Recording) Buffer().PeerEnd(); }
		/// Appends an owned sequence that already carries its counts and element lengths.
		void AppendFields(const CheckpointText& value) {
			if (m_Recording) { RefuseDivertedValue(); Buffer().Child(value); }
			else m_Text += value.Text();
		}
		template<class Visit> void NativeValue(Visit visit) {
			if (m_Recording && BatchEnabled()) InlineNative(visit);
			else Value(Native(visit));
		}

		template <class T> requires std::is_integral_v<T>
		void Value(T value) {
			if (m_Recording) {
				if constexpr (std::is_signed_v<T>) Buffer().Integer(static_cast<int64_t>(value), true);
				else Buffer().Unsigned(static_cast<uint64_t>(value), true);
				return;
			}
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
		void Value(const std::string& value) {
			if (m_Recording) { RefuseDivertedValue(); Buffer().String(value); return; }
			Value(value.size()); m_Text += value; m_Text.push_back(' ');
		}
		void Value(const CheckpointText& value) {
			if (m_Recording) { RefuseDivertedValue(); Buffer().Child(value, true); } else Value(value.Text());
		}
		void Value(const Vector& value) { (*this)(value.m_X, value.m_Y); }
		void Value(const Box& value) { (*this)(value.m_Corner, value.m_Width, value.m_Height); }
		// The real-time half is this machine's wall clock.
		void Value(const Timer& value) { (*this)(value.GetStartSimTimeMS(), value.GetSimTimeLimitTicks()); PerPeer(value.GetStartRealTimeMS(), value.GetRealTimeLimitTicks()); }
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
		void Value(const T& value) {
			if (m_Recording && BatchEnabled() && s_Cache && s_Cache->IsTransient()) {
				InlineNative([&value] { return value.SaveCheckpoint(); });
				return;
			}
			CheckpointText captured = Native([&value] { return value.SaveCheckpoint(); });
			if (m_Recording && s_Cache) captured = s_Cache->Remember(&value, 0, std::move(captured));
			Value(captured);
		}

	private:
		CheckpointBuffer& Buffer() const { return m_Output ? *m_Output : m_Capture; }
		template<class Visit> void InlineNative(Visit visit) {
			RefuseDivertedValue();
			Buffer().SizedRunBegin();
			{
				CaptureScope scope(&Buffer());
				std::string existing = visit();
				if (scope.inlineWritten && !existing.empty()) throw std::logic_error("checkpoint capture result has uncaptured text");
				if (scope.output) Buffer().Child(*scope.output);
				else if (!existing.empty()) Buffer().Raw(existing);
			}
			Buffer().SizedRunEnd();
		}
		template<class T> static constexpr size_t PrimitiveWords() {
			if constexpr (std::is_integral_v<T> || std::is_enum_v<T> || std::is_same_v<T, float> || std::is_same_v<T, double>) return 1;
			else if constexpr (std::is_same_v<T, Vector>) return 2;
			else if constexpr (std::is_same_v<T, Box>) return 4;
			else if constexpr (std::is_array_v<T>) return std::extent_v<T> * PrimitiveWords<std::remove_extent_t<T>>();
			else if constexpr (CheckpointArray<T>) return std::tuple_size_v<T> * PrimitiveWords<typename T::value_type>();
			else return 0;
		}
		template<class Fields, size_t Index> using FieldType = std::remove_cvref_t<std::tuple_element_t<Index, Fields>>;
		template<size_t Index, class Fields, size_t Words = 0> static constexpr size_t PrimitiveEnd() {
			if constexpr (Index == std::tuple_size_v<Fields>) {
				return Index;
			} else {
				constexpr size_t words = PrimitiveWords<FieldType<Fields, Index>>();
				if constexpr (words == 0 || words + Words > 512) return Index;
				else return PrimitiveEnd<Index + 1, Fields, Words + words>();
			}
		}
		template<size_t Begin, class Fields, size_t... Index> void CapturePrimitives(const Fields& fields, std::index_sequence<Index...>) {
			std::array<char, (PrimitiveBytes<FieldType<Fields, Begin + Index>>() + ... + 0)> record;
			size_t at = 0;
			(WritePrimitive(record.data(), at, std::get<Begin + Index>(fields)), ...);
			Buffer().PrimitiveBlock(std::string_view(record.data(), at), &DecodePrimitives<FieldType<Fields, Begin + Index>...>);
		}
		template<size_t Begin, class Fields> void CaptureFields(const Fields& fields) {
			if constexpr (Begin < std::tuple_size_v<Fields>) {
				constexpr size_t end = PrimitiveEnd<Begin, Fields>();
				if constexpr (end == Begin) {
					Value(std::get<Begin>(fields));
					CaptureFields<Begin + 1>(fields);
				} else {
					CapturePrimitives<Begin>(fields, std::make_index_sequence<end - Begin>());
					CaptureFields<end>(fields);
				}
			}
		}
		template<class T> static void WritePrimitive(char* bytes, size_t& at, const T& value) {
			if constexpr (std::is_same_v<T, Vector>) {
				WritePrimitive(bytes, at, value.m_X); WritePrimitive(bytes, at, value.m_Y);
			} else if constexpr (std::is_same_v<T, Box>) {
				WritePrimitive(bytes, at, value.m_Corner); WritePrimitive(bytes, at, value.m_Width); WritePrimitive(bytes, at, value.m_Height);
			} else if constexpr (std::is_array_v<T> || CheckpointArray<T>) {
				for (const auto& item: value) WritePrimitive(bytes, at, item);
			} else {
				std::memcpy(bytes + at, &value, sizeof(value));
				at += sizeof(value);
			}
		}
		template<class T> static constexpr size_t PrimitiveBytes() {
			if constexpr (std::is_same_v<T, Vector>) return 2 * sizeof(float);
			else if constexpr (std::is_same_v<T, Box>) return 4 * sizeof(float);
			else if constexpr (std::is_array_v<T>) return std::extent_v<T> * PrimitiveBytes<std::remove_extent_t<T>>();
			else if constexpr (CheckpointArray<T>) return std::tuple_size_v<T> * PrimitiveBytes<typename T::value_type>();
			else return sizeof(T);
		}
		template<class T> static T ReadPrimitive(std::string_view& values) {
			if (values.size() < sizeof(T)) throw std::logic_error("truncated owned primitive block");
			T value;
			std::memcpy(&value, values.data(), sizeof(value));
			values.remove_prefix(sizeof(value));
			return value;
		}
		template<class T> static void DecodePrimitive(std::string& text, std::string_view& values, bool tape) {
			if constexpr (std::is_same_v<T, Vector>) {
				DecodePrimitive<float>(text, values, tape); DecodePrimitive<float>(text, values, tape);
			} else if constexpr (std::is_same_v<T, Box>) {
				for (size_t index = 0; index < 4; ++index) DecodePrimitive<float>(text, values, tape);
			} else if constexpr (std::is_array_v<T>) {
				for (size_t index = 0; index < std::extent_v<T>; ++index) DecodePrimitive<std::remove_extent_t<T>>(text, values, tape);
			} else if constexpr (CheckpointArray<T>) {
				for (size_t index = 0; index < std::tuple_size_v<T>; ++index) DecodePrimitive<typename T::value_type>(text, values, tape);
			} else if constexpr (std::is_enum_v<T>) {
				DecodePrimitive<std::underlying_type_t<T>>(text, values, tape);
			} else if constexpr (std::is_same_v<T, float>) {
				DecodePrimitive<uint32_t>(text, values, tape);
			} else if constexpr (std::is_same_v<T, double>) {
				DecodePrimitive<uint64_t>(text, values, tape);
			} else if constexpr (std::is_same_v<T, bool>) {
				DecodePrimitive<unsigned char>(text, values, tape);
			} else {
				const T value = ReadPrimitive<T>(values);
				using Wide = std::conditional_t<std::is_signed_v<T>, int64_t, uint64_t>;
				const Wide word = static_cast<Wide>(value);
				if (tape) {
					text.push_back(static_cast<char>(std::is_signed_v<T> ? CheckpointBuffer::ValueKind::SpacedInteger : CheckpointBuffer::ValueKind::SpacedUnsigned));
					text.append(reinterpret_cast<const char*>(&word), sizeof(word));
				} else {
					char buffer[32];
					const auto result = std::to_chars(buffer, buffer + sizeof(buffer), word);
					if (result.ec != std::errc{}) throw std::logic_error("could not format owned primitive");
					text.append(buffer, result.ptr); text.push_back(' ');
				}
			}
		}
		template<class... T> static void DecodePrimitives(std::string& text, std::string_view values, bool tape) {
			(DecodePrimitive<T>(text, values, tape), ...);
			if (!values.empty()) throw std::logic_error("trailing owned primitive block");
		}
		// A nested writer that already published into this scope was composed by hand, so its text
		// never reached this writer: name the mistake here instead of losing the value.
		void RefuseDivertedValue() const {
			if (s_Capture && (s_Capture->output || s_Capture->inlineWritten)) throw std::logic_error("a nested checkpoint value must be produced with CheckpointWriter::Native");
		}
		struct CaptureScope {
			CaptureScope* previous = s_Capture;
			std::optional<CheckpointText> output;
			CheckpointBuffer* inlineOutput;
			bool inlineWritten = false;
			explicit CaptureScope(CheckpointBuffer* into = nullptr) : inlineOutput(into) { s_Capture = this; }
			~CaptureScope() { s_Capture = previous; }
		};
		inline static thread_local CaptureScope* s_Capture = nullptr;
		inline static thread_local CheckpointCache* s_Cache = nullptr;
		inline static std::atomic<unsigned> s_Batches{0};
		bool m_Recording = false;
		mutable CheckpointBuffer m_Capture;
		CheckpointBuffer* m_Output;
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
		template <class... Values> void PerPeer(Values&... values) { (Stage(values), ...); }
		/// PieMenuRuntime1 only: four timer ticks, integer or an exact-integer dotted token.
		void StageRuntime1Timer(Timer& value) {
			int64_t start = 0;
			int64_t limit = 0;
			int64_t realStart = 0;
			int64_t realLimit = 0;
			ReadRuntime1Ticks(start);
			ReadRuntime1Ticks(limit);
			ReadRuntime1Ticks(realStart);
			ReadRuntime1Ticks(realLimit);
			Timer candidate;
			candidate.SetStartSimTimeTicks(start);
			candidate.SetSimTimeLimitTicks(limit);
			candidate.SetStartRealTimeTicks(realStart);
			candidate.SetRealTimeLimitTicks(realLimit);
			OnCommit([&value, candidate = std::move(candidate)]() mutable { value = std::move(candidate); });
		}

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
		void ReadRuntime1Ticks(int64_t& ticks) {
			const size_t end = m_Text.find(' ');
			if (end == std::string_view::npos || end == 0) throw std::runtime_error("truncated runtime checkpoint timer");
			const char* first = m_Text.data();
			const char* last = first + end;
			int64_t integer = 0;
			const auto asInt = std::from_chars(first, last, integer);
			if (asInt.ec == std::errc() && asInt.ptr == last) {
				ticks = integer;
				m_Text.remove_prefix(end + 1);
				return;
			}
			double number = 0;
			const auto asDouble = ParseNumberExact(first, last, number);
			if (asDouble.ec != std::errc() || asDouble.ptr != last || std::trunc(number) != number) {
				throw std::runtime_error("runtime checkpoint timer is not an exact integer");
			}
			ticks = static_cast<int64_t>(number);
			m_Text.remove_prefix(end + 1);
		}
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
