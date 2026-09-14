#include "FloatTextSelfTest.h"

#include "FloatText.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace RTE::FloatTextSelfTest {

	namespace {

		constexpr const char* Tag = "[float-text-selftest]";
		constexpr uint64_t CorpusSeed = 0x5f32'0913'c0de'1234ULL;
		constexpr int RandomPatternCount = 10000;

		int failures = 0;

		void Fail(const std::string& detail) {
			if (failures < 20) {
				std::cerr << Tag << " FAIL " << detail << std::endl;
			}
			++failures;
		}

		template <class FloatType> struct Bits;
		template <> struct Bits<float> { using Type = uint32_t; };
		template <> struct Bits<double> { using Type = uint64_t; };

		template <class FloatType> typename Bits<FloatType>::Type BitsOf(FloatType value) { return std::bit_cast<typename Bits<FloatType>::Type>(value); }

		template <class FloatType> std::string Hex(FloatType value) {
			char buffer[32];
			const auto result = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<uint64_t>(BitsOf(value)), 16);
			return "0x" + std::string(buffer, result.ptr);
		}

		template <class FloatType> bool SameValue(FloatType left, FloatType right) {
			if (std::isnan(left) || std::isnan(right)) {
				return std::isnan(left) && std::isnan(right) && std::signbit(left) == std::signbit(right);
			}
			return BitsOf(left) == BitsOf(right);
		}

		/// Every value must survive format-then-parse bit for bit, and the portable fallback must agree with the
		/// shipped codec character for character on finite values and bit for bit on every parse.
		template <class FloatType> void CheckValue(FloatType value, const char* corpus) {
			char shipped[64];
			const std::to_chars_result shippedWrite = FormatFloatExact(shipped, shipped + sizeof(shipped), value);
			if (shippedWrite.ec != std::errc()) {
				Fail(std::string(corpus) + " codec write " + Hex(value));
				return;
			}
			const std::string shippedText(shipped, shippedWrite.ptr);

			char fallback[64];
			const std::to_chars_result fallbackWrite = FloatText::FormatFallback(fallback, fallback + sizeof(fallback), value);
			if (fallbackWrite.ec != std::errc()) {
				Fail(std::string(corpus) + " fallback write " + Hex(value));
				return;
			}
			const std::string fallbackText(fallback, fallbackWrite.ptr);

			// std::to_chars spells NaN and infinity differently per library, so only finite text is compared.
			const bool finite = std::isfinite(value);
			if (finite && fallbackText != shippedText) {
				Fail(std::string(corpus) + " text " + Hex(value) + " codec='" + shippedText + "' fallback='" + fallbackText + "'");
			}

			for (const std::string* text: {&shippedText, &fallbackText}) {
				FloatType shippedRead = FloatType(1);
				const std::from_chars_result shippedParse = ParseFloatExact(text->data(), text->data() + text->size(), shippedRead);
				if (shippedParse.ec != std::errc() || shippedParse.ptr != text->data() + text->size() || !SameValue(shippedRead, value)) {
					Fail(std::string(corpus) + " codec round trip " + Hex(value) + " '" + *text + "' got " + Hex(shippedRead));
				}
				FloatType fallbackRead = FloatType(1);
				const std::from_chars_result fallbackParse = FloatText::ParseFallback(text->data(), text->data() + text->size(), fallbackRead);
				if (fallbackParse.ec != std::errc() || fallbackParse.ptr != text->data() + text->size() || !SameValue(fallbackRead, value)) {
					Fail(std::string(corpus) + " fallback round trip " + Hex(value) + " '" + *text + "' got " + Hex(fallbackRead));
				}
				if (!SameValue(shippedRead, fallbackRead)) {
					Fail(std::string(corpus) + " parse disagreement '" + *text + "' codec " + Hex(shippedRead) + " fallback " + Hex(fallbackRead));
				}
			}
		}

		template <class FloatType> std::vector<FloatType> EdgeCases() {
			using BitType = typename Bits<FloatType>::Type;
			constexpr int mantissaBits = std::numeric_limits<FloatType>::digits - 1;
			std::vector<FloatType> values = {
			    FloatType(0),
			    -FloatType(0),
			    std::numeric_limits<FloatType>::denorm_min(),
			    -std::numeric_limits<FloatType>::denorm_min(),
			    std::bit_cast<FloatType>(static_cast<BitType>((BitType(1) << mantissaBits) - 1)),
			    std::numeric_limits<FloatType>::min(),
			    -std::numeric_limits<FloatType>::min(),
			    std::numeric_limits<FloatType>::max(),
			    std::numeric_limits<FloatType>::lowest(),
			    std::numeric_limits<FloatType>::epsilon(),
			    std::numeric_limits<FloatType>::infinity(),
			    -std::numeric_limits<FloatType>::infinity(),
			    std::numeric_limits<FloatType>::quiet_NaN(),
			    -std::numeric_limits<FloatType>::quiet_NaN(),
			    FloatType(1),
			    FloatType(-1),
			    FloatType(0.1),
			    FloatType(0.2),
			    FloatType(0.3),
			    FloatType(1) / FloatType(3),
			    FloatType(2) / FloatType(3),
			    FloatType(1e-5),
			    FloatType(1e-4),
			    FloatType(1e5),
			    FloatType(1e20),
			    std::numeric_limits<FloatType>::max() / FloatType(2),
			    FloatType(123456789),
			    FloatType(0.000123456789),
			    FloatType(3.14159265358979323846),
			    FloatType(2.71828182845904523536),
			    FloatType(1.2345678901234567),
			    FloatType(9.007199254740993e15),
			    -std::numeric_limits<FloatType>::max() / FloatType(4),
			};
			for (int exponent = -60; exponent <= 60; ++exponent) {
				values.push_back(std::ldexp(FloatType(1), exponent));
				values.push_back(-std::ldexp(FloatType(1), exponent));
				values.push_back(std::nextafter(std::ldexp(FloatType(1), exponent), std::numeric_limits<FloatType>::infinity()));
			}
			for (int digits = 1; digits <= 17; ++digits) {
				FloatType value = FloatType(0);
				for (int digit = 0; digit < digits; ++digit) {
					value = value * FloatType(10) + FloatType(digit % 9 + 1);
				}
				values.push_back(value);
				values.push_back(value / FloatType(1e10));
			}
			return values;
		}

		template <class FloatType> void CheckCorpus(const char* name) {
			for (FloatType value: EdgeCases<FloatType>()) {
				CheckValue(value, name);
			}
			std::mt19937_64 generator(CorpusSeed);
			for (int pattern = 0; pattern < RandomPatternCount; ++pattern) {
				const auto bits = static_cast<typename Bits<FloatType>::Type>(generator());
				CheckValue(std::bit_cast<FloatType>(bits), name);
			}
		}

		struct GrammarCase {
			const char* text;
			const char* description;
		};

		/// The fallback parser must accept and reject exactly what std::from_chars does: no leading space,
		/// no leading '+', no hexadecimal form, and the same consumed length on a partial match. Consumed
		/// length and error code are compared for every input; the parsed value only where both succeeded,
		/// because the libraries disagree on an out of range parse - MSVC writes the clamped value there,
		/// libstdc++ leaves the variable untouched, and the helper follows the standard and leaves it.
		void CheckGrammar() {
			static const GrammarCase cases[] = {
			    {"1.5", "plain"},
			    {"-1.5", "negative"},
			    {"+1.5", "leading plus"},
			    {" 1.5", "leading space"},
			    {"\t1.5", "leading tab"},
			    {".5", "leading point"},
			    {"5.", "trailing point"},
			    {"5.e3", "point then exponent"},
			    {"1e", "exponent with no digits"},
			    {"1e+", "exponent sign with no digits"},
			    {"1e5x", "trailing junk"},
			    {"0x1p3", "hexadecimal"},
			    {"0X10", "hexadecimal upper"},
			    {"inf", "infinity short"},
			    {"-inf", "negative infinity"},
			    {"infinity", "infinity long"},
			    {"infi", "infinity prefix"},
			    {"INF", "infinity upper"},
			    {"nan", "nan"},
			    {"-nan", "negative nan"},
			    {"nan(123_abc)", "nan payload"},
			    {"nan(", "nan open payload"},
			    {"1e400", "double overflow"},
			    {"1e-400", "double underflow"},
			    {"-", "lone sign"},
			    {"", "empty"},
			    {"00000000000000000000000000000000000000001.5", "long leading zeros"},
			    {"1.00000000000000000000000000000000000000000000000005", "long fraction"},
			    {"2.2250738585072011e-308", "double rounding boundary"},
			    {"1.00000005960464477539062500", "float tie"},
			    {"1.000000059604644775390625000000000001", "float tie plus"},
			};
			for (const GrammarCase& grammar: cases) {
				const char* first = grammar.text;
				const char* last = grammar.text + std::strlen(grammar.text);
				double fallbackDouble = 12345.0;
				const std::from_chars_result fallback = FloatText::ParseFallback(first, last, fallbackDouble);
#if RTE_STD_FLOAT_FROM_CHARS
				double standardDouble = 12345.0;
				const std::from_chars_result standard = std::from_chars(first, last, standardDouble);
				const bool bothParsedDouble = fallback.ec == std::errc() && standard.ec == std::errc();
				if (fallback.ec != standard.ec || fallback.ptr - first != standard.ptr - first || (bothParsedDouble && !SameValue(fallbackDouble, standardDouble))) {
					Fail(std::string("grammar double '") + grammar.text + "' (" + grammar.description + ") fallback ec=" + std::to_string(static_cast<int>(fallback.ec)) + " len=" + std::to_string(fallback.ptr - first) + " value=" + Hex(fallbackDouble) + " standard ec=" + std::to_string(static_cast<int>(standard.ec)) + " len=" + std::to_string(standard.ptr - first) + " value=" + Hex(standardDouble));
				}
				float fallbackFloat = 12345.0F;
				float standardFloat = 12345.0F;
				const std::from_chars_result fallbackSingle = FloatText::ParseFallback(first, last, fallbackFloat);
				const std::from_chars_result standardSingle = std::from_chars(first, last, standardFloat);
				const bool bothParsedFloat = fallbackSingle.ec == std::errc() && standardSingle.ec == std::errc();
				if (fallbackSingle.ec != standardSingle.ec || fallbackSingle.ptr - first != standardSingle.ptr - first || (bothParsedFloat && !SameValue(fallbackFloat, standardFloat))) {
					Fail(std::string("grammar float '") + grammar.text + "' (" + grammar.description + ") fallback ec=" + std::to_string(static_cast<int>(fallbackSingle.ec)) + " len=" + std::to_string(fallbackSingle.ptr - first) + " value=" + Hex(fallbackFloat) + " standard ec=" + std::to_string(static_cast<int>(standardSingle.ec)) + " len=" + std::to_string(standardSingle.ptr - first) + " value=" + Hex(standardFloat));
				}
#else
				(void)fallback;
				(void)fallbackDouble;
#endif
			}
		}

	} // namespace

	int Run() {
		failures = 0;
		std::cout << Tag << " from_chars=" << RTE_STD_FLOAT_FROM_CHARS << " to_chars=" << RTE_STD_FLOAT_TO_CHARS << " patterns=" << RandomPatternCount << std::endl;
		CheckCorpus<float>("float");
		CheckCorpus<double>("double");
		CheckGrammar();
		if (failures > 0) {
			std::cerr << Tag << " FAIL total=" << failures << std::endl;
			return 1;
		}
		std::cout << Tag << " PASS" << std::endl;
		return 0;
	}

}
