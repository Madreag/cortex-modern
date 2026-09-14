#pragma once

#include <cstdlib>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>

// libc++ gives the floating-point charconv overloads platform availability attributes - to_chars arrived in
// LLVM 14 (macOS 13.4), from_chars only in LLVM 20 (macOS 26) - so calling them below the deployment target
// is a hard compile error. Ask the library which overloads it has; never guess from the platform.
#if defined(_LIBCPP_VERSION)
	#if defined(_LIBCPP_AVAILABILITY_HAS_FROM_CHARS_FLOATING_POINT) && _LIBCPP_AVAILABILITY_HAS_FROM_CHARS_FLOATING_POINT
		#define RTE_STD_FLOAT_FROM_CHARS 1
	#else
		#define RTE_STD_FLOAT_FROM_CHARS 0
	#endif
	#if defined(_LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT) && _LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT
		#define RTE_STD_FLOAT_TO_CHARS 1
	#else
		#define RTE_STD_FLOAT_TO_CHARS 0
	#endif
#elif defined(_MSC_VER) || (defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L)
	#define RTE_STD_FLOAT_FROM_CHARS 1
	#define RTE_STD_FLOAT_TO_CHARS 1
#else
	#define RTE_STD_FLOAT_FROM_CHARS 0
	#define RTE_STD_FLOAT_TO_CHARS 0
#endif

#if defined(_WIN32)
	#include <locale.h>
#else
	#include <locale.h>
	#if defined(__APPLE__)
		#include <xlocale.h> // Darwin only declares the _l functions here, and only after <stdlib.h>.
	#endif
#endif

namespace RTE::FloatText {

	/// The process-wide C locale used by the fallback codec, created once and deliberately never freed.
#if defined(_WIN32)
	inline ::_locale_t CLocale() {
		static const ::_locale_t locale = ::_create_locale(LC_ALL, "C");
		return locale;
	}
#else
	inline ::locale_t CLocale() {
		static const ::locale_t locale = ::newlocale(LC_ALL_MASK, "C", static_cast<::locale_t>(nullptr));
		return locale;
	}
#endif

	inline bool IsDigit(char character) { return character >= '0' && character <= '9'; }

	inline char LowerAscii(char character) { return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character; }

	inline bool IsNanPayloadCharacter(char character) { return IsDigit(character) || character == '_' || (LowerAscii(character) >= 'a' && LowerAscii(character) <= 'z'); }

	/// Advances past a lower-case literal, case-insensitively; leaves the cursor alone when it does not match.
	inline bool MatchLiteral(const char*& cursor, const char* last, const char* literal) {
		const char* probe = cursor;
		for (const char* letter = literal; *letter != '\0'; ++letter) {
			if (probe == last || LowerAscii(*probe) != *letter) {
				return false;
			}
			++probe;
		}
		cursor = probe;
		return true;
	}

	/// Returns the end of the longest prefix matching std::from_chars' general grammar, or first when there is none.
	/// Deliberately stricter than strtod: no leading space, no leading '+', no hexadecimal form.
	inline const char* ScanNumber(const char* first, const char* last) {
		const char* cursor = first;
		if (cursor != last && *cursor == '-') {
			++cursor;
		}
		if (MatchLiteral(cursor, last, "inf")) {
			MatchLiteral(cursor, last, "inity");
			return cursor;
		}
		if (MatchLiteral(cursor, last, "nan")) {
			if (cursor != last && *cursor == '(') {
				const char* probe = cursor + 1;
				while (probe != last && IsNanPayloadCharacter(*probe)) {
					++probe;
				}
				if (probe != last && *probe == ')') {
					cursor = probe + 1;
				}
			}
			return cursor;
		}
		const char* digits = cursor;
		while (cursor != last && IsDigit(*cursor)) {
			++cursor;
		}
		bool anyDigits = cursor != digits;
		if (cursor != last && *cursor == '.') {
			const char* fraction = cursor + 1;
			const char* scan = fraction;
			while (scan != last && IsDigit(*scan)) {
				++scan;
			}
			if (anyDigits || scan != fraction) {
				anyDigits = true;
				cursor = scan;
			}
		}
		if (!anyDigits) {
			return first;
		}
		const char* mantissaEnd = cursor;
		if (cursor != last && (*cursor == 'e' || *cursor == 'E')) {
			const char* exponent = cursor + 1;
			if (exponent != last && (*exponent == '+' || *exponent == '-')) {
				++exponent;
			}
			const char* exponentDigits = exponent;
			while (exponent != last && IsDigit(*exponent)) {
				++exponent;
			}
			cursor = (exponent != exponentDigits) ? exponent : mantissaEnd;
		}
		return cursor;
	}

	inline double StrToFloating(const char* text, char** end, double) {
#if defined(_WIN32)
		return ::_strtod_l(text, end, CLocale());
#else
		return ::strtod_l(text, end, CLocale());
#endif
	}

	inline float StrToFloating(const char* text, char** end, float) {
#if defined(_WIN32)
		return ::_strtof_l(text, end, CLocale());
#else
		return ::strtof_l(text, end, CLocale());
#endif
	}

	/// Parses one number in std::from_chars' general grammar with the C locale's correctly rounded strtod.
	template <class FloatType> std::from_chars_result ParseFallback(const char* first, const char* last, FloatType& value) {
		static_assert(std::is_floating_point_v<FloatType>, "ParseFallback takes float or double");
		const char* end = ScanNumber(first, last);
		if (end == first) {
			return {first, std::errc::invalid_argument};
		}
		const size_t length = static_cast<size_t>(end - first);
		char stack[512];
		char* text = stack;
		std::string spill; // A token longer than the stack buffer never comes out of the codec; copy it rather than truncate it.
		if (length >= sizeof(stack)) {
			spill.assign(first, length);
			text = spill.data();
		} else {
			std::memcpy(stack, first, length);
			stack[length] = '\0';
		}
		char* parsedEnd = nullptr;
		const int savedErrno = errno;
		errno = 0;
		const FloatType parsed = StrToFloating(text, &parsedEnd, FloatType());
		const int parseErrno = errno;
		errno = savedErrno;
		if (parsedEnd != text + length) {
			return {first, std::errc::invalid_argument};
		}
		// strtod reports ERANGE for subnormal results too, but a subnormal is representable and from_chars takes it.
		if (parseErrno == ERANGE && (std::isinf(parsed) || parsed == FloatType(0))) {
			return {end, std::errc::result_out_of_range}; // Out of range leaves the target alone, as the standard says.
		}
		value = parsed;
		return {end, std::errc()};
	}

	/// Formats with the C locale, the one call in this header that needs a locale-parameterised printf.
	template <class... Args> int FormatC(char* buffer, size_t size, const char* format, Args... args) {
#if defined(_WIN32)
		return ::_snprintf_l(buffer, size, format, CLocale(), args...);
#elif defined(__APPLE__)
		return ::snprintf_l(buffer, size, CLocale(), format, args...);
#else
		const ::locale_t previous = ::uselocale(CLocale());
		const int written = std::snprintf(buffer, size, format, args...);
		::uselocale(previous);
		return written;
#endif
	}

	inline std::to_chars_result WriteLiteral(char* first, char* last, const char* literal) {
		const size_t length = std::strlen(literal);
		if (static_cast<size_t>(last - first) < length) {
			return {last, std::errc::value_too_large};
		}
		std::memcpy(first, literal, length);
		return {first + length, std::errc()};
	}

	/// Splits a printf %e rendering into its significant digits and decimal exponent.
	inline void SplitScientific(const char* text, char* digits, int& digitCount, int& exponent) {
		digitCount = 0;
		exponent = 0;
		for (const char* cursor = text; *cursor != '\0'; ++cursor) {
			if (IsDigit(*cursor)) {
				digits[digitCount++] = *cursor;
			} else if (*cursor == 'e' || *cursor == 'E') {
				const char* exponentText = cursor + 1;
				const bool exponentNegative = *exponentText == '-';
				if (*exponentText == '-' || *exponentText == '+') {
					++exponentText;
				}
				for (; IsDigit(*exponentText); ++exponentText) {
					exponent = exponent * 10 + (*exponentText - '0');
				}
				if (exponentNegative) {
					exponent = -exponent;
				}
				return;
			}
		}
	}

	/// Moves a decimal digit string one step up or down, renormalising the exponent on carry or borrow.
	inline void StepDigits(char* digits, int digitCount, int& exponent, int direction) {
		int position = digitCount - 1;
		if (direction > 0) {
			for (; position >= 0 && digits[position] == '9'; --position) {
				digits[position] = '0';
			}
			if (position < 0) {
				digits[0] = '1';
				++exponent;
			} else {
				++digits[position];
			}
		} else {
			for (; position >= 0 && digits[position] == '0'; --position) {
				digits[position] = '9';
			}
			if (position >= 0) {
				--digits[position];
			}
			if (digits[0] == '0') {
				std::memmove(digits, digits + 1, static_cast<size_t>(digitCount - 1));
				digits[digitCount - 1] = '0';
				--exponent;
			}
		}
	}

	/// True when the decimal digits, read back, give this exact value.
	template <class FloatType> bool DigitsRoundTrip(const char* digits, int digitCount, int exponent, FloatType value) {
		char text[80];
		char* cursor = text;
		*cursor++ = digits[0];
		if (digitCount > 1) {
			*cursor++ = '.';
			std::memcpy(cursor, digits + 1, static_cast<size_t>(digitCount - 1));
			cursor += digitCount - 1;
		}
		*cursor++ = 'e';
		*cursor++ = exponent < 0 ? '-' : '+';
		int magnitude = exponent < 0 ? -exponent : exponent;
		int exponentDigits = 1;
		for (int probe = magnitude; probe >= 10; probe /= 10) {
			++exponentDigits;
		}
		for (int position = exponentDigits - 1; position >= 0; --position) {
			cursor[position] = static_cast<char>('0' + magnitude % 10);
			magnitude /= 10;
		}
		cursor += exponentDigits;
		FloatType parsed = FloatType();
		const std::from_chars_result result = ParseFallback(text, cursor, parsed);
		return result.ec == std::errc() && parsed == value;
	}

	/// Writes a number exactly the way std::to_chars' plain overload does: the fewest characters that read
	/// back as the same value, printf's f or e style, a tie going to f and then to the most significant
	/// digits, C locale, no '+' on the mantissa. A candidate never fits in fewer characters than it has
	/// digits, so the search stops once it reaches the best length found. Rounding the value to a given
	/// digit count can land outside the value's rounding interval where the interval is lopsided - at a
	/// power of two - so the neighbouring decimals of that length are tried too.
	template <class FloatType> std::to_chars_result FormatFallback(char* first, char* last, FloatType value) {
		static_assert(std::is_floating_point_v<FloatType>, "FormatFallback takes float or double");
		const bool negative = std::signbit(value);
		if (std::isnan(value)) {
			return WriteLiteral(first, last, negative ? "-nan" : "nan");
		}
		if (std::isinf(value)) {
			return WriteLiteral(first, last, negative ? "-inf" : "inf");
		}
		if (value == FloatType(0)) {
			return WriteLiteral(first, last, negative ? "-0" : "0");
		}
		const FloatType unsignedValue = std::fabs(value);
		constexpr int maxSignificantDigits = 32;
		char digits[maxSignificantDigits + 1] = {};
		int digitCount = 0;
		int exponent = 0;
		int bestLength = maxSignificantDigits + 1;
		bool bestFixed = true;
		for (int significantDigits = 1; significantDigits <= maxSignificantDigits && significantDigits <= bestLength; ++significantDigits) {
			char scientific[80] = {};
			const int written = FormatC(scientific, sizeof(scientific), "%.*e", significantDigits - 1, static_cast<double>(unsignedValue));
			if (written <= 0 || static_cast<size_t>(written) >= sizeof(scientific)) {
				return {last, std::errc::value_too_large};
			}
			char nearest[maxSignificantDigits + 1] = {};
			int nearestCount = 0;
			int nearestExponent = 0;
			SplitScientific(scientific, nearest, nearestCount, nearestExponent);
			char candidate[maxSignificantDigits + 1] = {};
			int candidateExponent = 0;
			bool found = false;
			for (int step = 0; step <= 2 && !found; ++step) {
				std::memcpy(candidate, nearest, sizeof(candidate));
				candidateExponent = nearestExponent;
				if (step > 0) {
					StepDigits(candidate, nearestCount, candidateExponent, step == 1 ? 1 : -1);
				}
				found = DigitsRoundTrip(candidate, nearestCount, candidateExponent, unsignedValue);
			}
			if (!found) {
				continue;
			}
			int exponentDigits = 2;
			for (int magnitude = candidateExponent < 0 ? -candidateExponent : candidateExponent; magnitude >= 100; magnitude /= 10) {
				++exponentDigits;
			}
			const int scientificLength = 1 + (nearestCount > 1 ? nearestCount : 0) + 2 + exponentDigits;
			int fixedLength = 0;
			if (candidateExponent >= nearestCount - 1) {
				fixedLength = candidateExponent + 1;
			} else if (candidateExponent >= 0) {
				fixedLength = nearestCount + 1;
			} else {
				fixedLength = nearestCount - candidateExponent + 1;
			}
			const bool fixedWins = fixedLength <= scientificLength;
			const int candidateLength = fixedWins ? fixedLength : scientificLength;
			if (candidateLength < bestLength || (candidateLength == bestLength && fixedWins)) {
				bestLength = candidateLength;
				bestFixed = fixedWins;
				digitCount = nearestCount;
				exponent = candidateExponent;
				std::memcpy(digits, candidate, sizeof(digits));
			}
		}
		if (digitCount == 0) {
			return {last, std::errc::value_too_large};
		}
		const int length = (negative ? 1 : 0) + bestLength;
		if (last - first < length) {
			return {last, std::errc::value_too_large};
		}
		char* cursor = first;
		if (negative) {
			*cursor++ = '-';
		}
		if (bestFixed) {
			if (exponent >= digitCount - 1) {
				std::memcpy(cursor, digits, static_cast<size_t>(digitCount));
				std::memset(cursor + digitCount, '0', static_cast<size_t>(exponent - digitCount + 1));
			} else if (exponent >= 0) {
				std::memcpy(cursor, digits, static_cast<size_t>(exponent + 1));
				cursor[exponent + 1] = '.';
				std::memcpy(cursor + exponent + 2, digits + exponent + 1, static_cast<size_t>(digitCount - exponent - 1));
			} else {
				*cursor++ = '0';
				*cursor++ = '.';
				const int leadingZeros = -exponent - 1;
				std::memset(cursor, '0', static_cast<size_t>(leadingZeros));
				std::memcpy(cursor + leadingZeros, digits, static_cast<size_t>(digitCount));
			}
		} else {
			*cursor++ = digits[0];
			if (digitCount > 1) {
				*cursor++ = '.';
				std::memcpy(cursor, digits + 1, static_cast<size_t>(digitCount - 1));
				cursor += digitCount - 1;
			}
			*cursor++ = 'e';
			*cursor++ = exponent < 0 ? '-' : '+';
			int magnitude = exponent < 0 ? -exponent : exponent;
			int exponentDigits = 2;
			for (int probe = magnitude; probe >= 100; probe /= 10) {
				++exponentDigits;
			}
			for (int position = exponentDigits - 1; position >= 0; --position) {
				cursor[position] = static_cast<char>('0' + magnitude % 10);
				magnitude /= 10;
			}
		}
		return {first + length, std::errc()};
	}

} // namespace RTE::FloatText

namespace RTE {

	/// Parses one float or double in std::from_chars' grammar, locale-free and without allocating.
	inline std::from_chars_result ParseFloatExact(const char* first, const char* last, float& value) {
#if RTE_STD_FLOAT_FROM_CHARS
		return std::from_chars(first, last, value);
#else
		return FloatText::ParseFallback(first, last, value);
#endif
	}

	/// Parses one float or double in std::from_chars' grammar, locale-free and without allocating.
	inline std::from_chars_result ParseFloatExact(const char* first, const char* last, double& value) {
#if RTE_STD_FLOAT_FROM_CHARS
		return std::from_chars(first, last, value);
#else
		return FloatText::ParseFallback(first, last, value);
#endif
	}

	/// Writes a float or double in its shortest round-trip form, locale-free and without allocating.
	inline std::to_chars_result FormatFloatExact(char* first, char* last, float value) {
#if RTE_STD_FLOAT_TO_CHARS
		return std::to_chars(first, last, value);
#else
		return FloatText::FormatFallback(first, last, value);
#endif
	}

	/// Writes a float or double in its shortest round-trip form, locale-free and without allocating.
	inline std::to_chars_result FormatFloatExact(char* first, char* last, double value) {
#if RTE_STD_FLOAT_TO_CHARS
		return std::to_chars(first, last, value);
#else
		return FloatText::FormatFallback(first, last, value);
#endif
	}

	/// std::from_chars for any codec value, routing floating point through the availability-safe helper.
	template <class ValueType> std::from_chars_result FromCharsExact(const char* first, const char* last, ValueType& value) {
		if constexpr (std::is_floating_point_v<ValueType>) {
			return ParseFloatExact(first, last, value);
		} else {
			return std::from_chars(first, last, value);
		}
	}

	/// std::to_chars for any codec value, routing floating point through the availability-safe helper.
	template <class ValueType> std::to_chars_result ToCharsExact(char* first, char* last, ValueType value) {
		if constexpr (std::is_floating_point_v<ValueType>) {
			return FormatFloatExact(first, last, value);
		} else {
			return std::to_chars(first, last, value);
		}
	}

} // namespace RTE
