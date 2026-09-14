#include "FloatTextSelfTest.h"

#include "FloatText.h"

#include "allegro.h"

#include "ActivityMan.h"
#include "Arm.h"
#include "AudioMan.h"
#include "MovableMan.h"
#include "PieMenu.h"
#include "PresetMan.h"
#include "Reader.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "Writer.h"

#include <bit>
#include <cerrno>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <locale>
#include <memory>
#include <random>
#include <sstream>
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

		/// What a parse of a grammar case must do. The bits of a NaN payload are implementation defined,
		/// so only its sign is pinned; an out of range or invalid parse must leave the target alone.
		enum class Expect {
			Value,
			NanPositive,
			NanNegative,
			OutOfRange,
			Invalid
		};

		struct GrammarCase {
			const char* text;
			const char* description;
			int length; //!< Characters std::from_chars' grammar consumes.
			Expect single;
			uint32_t singleBits;
			Expect wide;
			uint64_t wideBits;
		};

		/// Checks one parse against the fixed expectation: the error code, the characters consumed, and the
		/// value - or that the target was left alone, which is what the standard says an out of range or
		/// invalid parse does.
		template <class FloatType> void CheckAgainstExpectation(const GrammarCase& grammar, Expect expected, uint64_t expectedBits, const std::from_chars_result& result, FloatType value, FloatType untouched, const char* who, bool checkTarget = true) {
			const auto fail = [&](const std::string& detail) {
				Fail(std::string("grammar ") + who + " '" + grammar.text + "' (" + grammar.description + ") " + detail + " ec=" + std::to_string(static_cast<int>(result.ec)) + " len=" + std::to_string(result.ptr - grammar.text) + " value=" + Hex(value));
			};
			const std::errc expectedCode = expected == Expect::Invalid ? std::errc::invalid_argument : (expected == Expect::OutOfRange ? std::errc::result_out_of_range : std::errc());
			if (result.ec != expectedCode) {
				fail("expected ec=" + std::to_string(static_cast<int>(expectedCode)));
				return;
			}
			if (result.ptr - grammar.text != grammar.length) {
				fail("expected len=" + std::to_string(grammar.length));
				return;
			}
			if (!checkTarget) {
				return;
			}
			switch (expected) {
				case Expect::Value:
					if (BitsOf(value) != static_cast<typename Bits<FloatType>::Type>(expectedBits)) {
						fail("expected value=" + Hex(std::bit_cast<FloatType>(static_cast<typename Bits<FloatType>::Type>(expectedBits))));
					}
					break;
				case Expect::NanPositive:
				case Expect::NanNegative:
					if (!std::isnan(value) || std::signbit(value) != (expected == Expect::NanNegative)) {
						fail(expected == Expect::NanNegative ? "expected a negative NaN" : "expected a positive NaN");
					}
					break;
				default:
					if (!SameValue(value, untouched)) {
						fail("expected the target to be left alone");
					}
					break;
			}
		}

		/// The parser must accept and reject exactly what std::from_chars' grammar does - no leading space,
		/// no leading '+', no hexadecimal form - and give the value the standard says. Every expectation is
		/// a fixed constant derived from the grammar and exact arithmetic, so the fallback is checked on a
		/// host where no standard floating-point from_chars exists to compare it with.
		void CheckGrammar() {
			static const GrammarCase cases[] = {
			    {"1.5", "plain", 3, Expect::Value, 0x3FC00000, Expect::Value, 0x3FF8000000000000ULL},
			    {"-1.5", "negative", 4, Expect::Value, 0xBFC00000, Expect::Value, 0xBFF8000000000000ULL},
			    {"+1.5", "leading plus", 0, Expect::Invalid, 0, Expect::Invalid, 0},
			    {" 1.5", "leading space", 0, Expect::Invalid, 0, Expect::Invalid, 0},
			    {"\t1.5", "leading tab", 0, Expect::Invalid, 0, Expect::Invalid, 0},
			    {".5", "leading point", 2, Expect::Value, 0x3F000000, Expect::Value, 0x3FE0000000000000ULL},
			    {"5.", "trailing point", 2, Expect::Value, 0x40A00000, Expect::Value, 0x4014000000000000ULL},
			    {"5.e3", "point then exponent", 4, Expect::Value, 0x459C4000, Expect::Value, 0x40B3880000000000ULL},
			    {"1e", "exponent with no digits", 1, Expect::Value, 0x3F800000, Expect::Value, 0x3FF0000000000000ULL},
			    {"1e+", "exponent sign with no digits", 1, Expect::Value, 0x3F800000, Expect::Value, 0x3FF0000000000000ULL},
			    {"1e5x", "trailing junk", 3, Expect::Value, 0x47C35000, Expect::Value, 0x40F86A0000000000ULL},
			    {"0x1p3", "hexadecimal", 1, Expect::Value, 0x00000000, Expect::Value, 0x0000000000000000ULL},
			    {"0X10", "hexadecimal upper", 1, Expect::Value, 0x00000000, Expect::Value, 0x0000000000000000ULL},
			    {"inf", "infinity short", 3, Expect::Value, 0x7F800000, Expect::Value, 0x7FF0000000000000ULL},
			    {"-inf", "negative infinity", 4, Expect::Value, 0xFF800000, Expect::Value, 0xFFF0000000000000ULL},
			    {"infinity", "infinity long", 8, Expect::Value, 0x7F800000, Expect::Value, 0x7FF0000000000000ULL},
			    {"infi", "infinity prefix", 3, Expect::Value, 0x7F800000, Expect::Value, 0x7FF0000000000000ULL},
			    {"INF", "infinity upper", 3, Expect::Value, 0x7F800000, Expect::Value, 0x7FF0000000000000ULL},
			    {"nan", "nan", 3, Expect::NanPositive, 0, Expect::NanPositive, 0},
			    {"-nan", "negative nan", 4, Expect::NanNegative, 0, Expect::NanNegative, 0},
			    {"nan(123_abc)", "nan payload", 12, Expect::NanPositive, 0, Expect::NanPositive, 0},
			    {"nan(", "nan open payload", 3, Expect::NanPositive, 0, Expect::NanPositive, 0},
			    {"1e400", "double overflow", 5, Expect::OutOfRange, 0, Expect::OutOfRange, 0},
			    {"1e-400", "double underflow", 6, Expect::OutOfRange, 0, Expect::OutOfRange, 0},
			    {"-", "lone sign", 0, Expect::Invalid, 0, Expect::Invalid, 0},
			    {"", "empty", 0, Expect::Invalid, 0, Expect::Invalid, 0},
			    {"00000000000000000000000000000000000000001.5", "long leading zeros", 43, Expect::Value, 0x3FC00000, Expect::Value, 0x3FF8000000000000ULL},
			    {"1.00000000000000000000000000000000000000000000000005", "long fraction", 52, Expect::Value, 0x3F800000, Expect::Value, 0x3FF0000000000000ULL},
			    {"2.2250738585072011e-308", "double rounding boundary", 23, Expect::OutOfRange, 0, Expect::Value, 0x000FFFFFFFFFFFFFULL},
			    {"1.00000005960464477539062500", "float tie", 28, Expect::Value, 0x3F800000, Expect::Value, 0x3FF0000010000000ULL},
			    {"1.000000059604644775390625000000000001", "float tie plus", 38, Expect::Value, 0x3F800001, Expect::Value, 0x3FF0000010000000ULL},
			};
			for (const GrammarCase& grammar: cases) {
				const char* first = grammar.text;
				const char* last = grammar.text + std::strlen(grammar.text);
				const double untouchedDouble = 12345.0;
				const float untouchedFloat = 12345.0F;

				double fallbackDouble = untouchedDouble;
				const std::from_chars_result fallback = FloatText::ParseFallback(first, last, fallbackDouble);
				CheckAgainstExpectation(grammar, grammar.wide, grammar.wideBits, fallback, fallbackDouble, untouchedDouble, "fallback double");
				float fallbackFloat = untouchedFloat;
				const std::from_chars_result fallbackSingle = FloatText::ParseFallback(first, last, fallbackFloat);
				CheckAgainstExpectation(grammar, grammar.single, grammar.singleBits, fallbackSingle, fallbackFloat, untouchedFloat, "fallback float");

				double shippedDouble = untouchedDouble;
				const std::from_chars_result shipped = ParseFloatExact(first, last, shippedDouble);
				float shippedFloat = untouchedFloat;
				const std::from_chars_result shippedSingle = ParseFloatExact(first, last, shippedFloat);
				// MSVC writes the clamped value on an out of range parse where the fallback leaves the target,
				// so the shipped codec's target is compared everywhere except there. Its code and length always are.
				const bool shippedWritesTheTarget = RTE_STD_FLOAT_FROM_CHARS == 0;
				CheckAgainstExpectation(grammar, grammar.wide, grammar.wideBits, shipped, shippedDouble, untouchedDouble, "codec double", shippedWritesTheTarget || grammar.wide != Expect::OutOfRange);
				CheckAgainstExpectation(grammar, grammar.single, grammar.singleBits, shippedSingle, shippedFloat, untouchedFloat, "codec float", shippedWritesTheTarget || grammar.single != Expect::OutOfRange);
			}
		}

		struct FormatCase {
			uint64_t bits;
			const char* text;
		};

		/// Both codecs must write these values as exactly these characters. Where a library has no
		/// floating-point std::to_chars the fallback IS the shipped codec, so comparing the two would
		/// compare it with itself; the text is pinned here instead of trusting that comparison.
		template <class FloatType> void CheckFormatTable(const FormatCase* cases, size_t count, const char* name) {
			for (size_t index = 0; index < count; ++index) {
				const FloatType value = std::bit_cast<FloatType>(static_cast<typename Bits<FloatType>::Type>(cases[index].bits));
				char shipped[64];
				const std::to_chars_result shippedWrite = FormatFloatExact(shipped, shipped + sizeof(shipped), value);
				char fallback[64];
				const std::to_chars_result fallbackWrite = FloatText::FormatFallback(fallback, fallback + sizeof(fallback), value);
				const std::string shippedText(shipped, shippedWrite.ec == std::errc() ? shippedWrite.ptr : shipped);
				const std::string fallbackText(fallback, fallbackWrite.ec == std::errc() ? fallbackWrite.ptr : fallback);
				if (shippedText != cases[index].text) {
					Fail(std::string(name) + " codec text " + Hex(value) + " '" + shippedText + "' expected '" + cases[index].text + "'");
				}
				if (fallbackText != cases[index].text) {
					Fail(std::string(name) + " fallback text " + Hex(value) + " '" + fallbackText + "' expected '" + cases[index].text + "'");
				}
			}
		}

		void CheckFormatTables() {
			static const FormatCase singles[] = {
			    {0x3FC00000, "1.5"},
			    {0x3DCCCCCD, "0.1"},
			    {0x7F7FFFFF, "3.4028235e+38"},
			    {0x00000001, "1e-45"},
			    {0x00800000, "1.1754944e-38"},
			    {0x47C35000, "1e+05"},
			    {0x3727C5AC, "1e-05"},
			    // Two shortest forms are nine characters here; both codecs write the value's own digits.
			    {0x4CEB79A3, "123456792"},
			    {0x80000000, "-0"},
			    {0x34000000, "1.1920929e-07"},
			};
			static const FormatCase wides[] = {
			    {0x3FF8000000000000ULL, "1.5"},
			    {0x3FB999999999999AULL, "0.1"},
			    {0x7FEFFFFFFFFFFFFFULL, "1.7976931348623157e+308"},
			    {0x0000000000000001ULL, "5e-324"},
			    {0x3D30000000000000ULL, "5.684341886080802e-14"},
			    {0x3FD5555555555555ULL, "0.3333333333333333"},
			    {0x3EE4F8B588E368F1ULL, "1e-05"},
			    {0x40F86A0000000000ULL, "1e+05"},
			    {0x8000000000000000ULL, "-0"},
			    {0x000FFFFFFFFFFFFFULL, "2.225073858507201e-308"},
			};
			CheckFormatTable<float>(singles, std::size(singles), "float");
			CheckFormatTable<double>(wides, std::size(wides), "double");
		}

		/// The engine paths below build entities and Readers, which reach for the managers.
		void ConstructManagersForEntities() {
			if (!TimerMan::IsConstructed()) TimerMan::Construct();
			if (!PresetMan::IsConstructed()) PresetMan::Construct();
			if (!SettingsMan::IsConstructed()) SettingsMan::Construct();
			if (!MovableMan::IsConstructed()) MovableMan::Construct();
			if (!ActivityMan::IsConstructed()) ActivityMan::Construct();
			if (!AudioMan::IsConstructed()) AudioMan::Construct();
			install_allegro(SYSTEM_NONE, &errno, std::atexit); // SceneMan::Clear creates a bitmap.
			if (!SceneMan::IsConstructed()) SceneMan::Construct();
		}

		/// The hexadecimal text the codec writes has to be the text the streams wrote before it, or every
		/// saved game and packed state would change shape. Only meaningful in the C locale.
		void CheckHexFloatMatchesTheStream() {
			for (const float value: {1.5F, -0.25F, 0.75F, 0.1F, 3.14159265F, 0.0F, -0.0F, 1e-30F}) {
				std::ostringstream stream;
				stream << std::hexfloat << value;
				const std::string expected = stream.str();
				const std::string written = HexFloatString(value);
				if (written != expected) {
					Fail(std::string("hexfloat text '") + written + "' but a stream writes '" + expected + "'");
				}
				float readBack = 0.0F;
				const std::from_chars_result parsed = ParseHexFloatExact(written.data(), written.data() + written.size(), readBack);
				if (parsed.ec != std::errc() || parsed.ptr != written.data() + written.size() || !SameValue(readBack, value)) {
					Fail(std::string("hexfloat round trip '") + written + "' got " + Hex(readBack));
				}
			}
		}

		template <class FloatType> FloatType ReadThroughReader(const std::string& text) {
			Reader reader(std::make_unique<std::istringstream>(text), "float-text-selftest.ini");
			FloatType value = 0;
			reader >> value;
			return value;
		}

		std::string ProbeReaderFloat() { return Hex(ReadThroughReader<float>("1.5\n")) + "/" + Hex(ReadThroughReader<float>("1\n")); }

		std::string ProbeReaderDouble() { return Hex(ReadThroughReader<double>("-0.1\n")) + "/" + Hex(ReadThroughReader<double>("-1\n")); }

		/// A float read as a double and narrowed rounds twice, and lands one ulp below on this value.
		std::string ProbeReaderSingleRounding() { return Hex(ReadThroughReader<float>("1.000000059604644775390625000000000000001\n")); }

		/// Not pinned here: what the stream stored on an out of range read is compared control against tip.
		std::string ProbeReaderOutOfRange() { return Hex(ReadThroughReader<double>("1e400\n")) + "/" + Hex(ReadThroughReader<double>("-1e400\n")) + "/" + Hex(ReadThroughReader<float>("1e-400\n")); }

		std::string ProbeWriter() {
			Writer writer(std::make_unique<std::ostringstream>());
			writer << 1.5F;
			writer << "|";
			writer << -0.1;
			return static_cast<std::ostringstream*>(writer.GetStream())->str();
		}

		std::string ProbeArmHandTarget() {
			const std::string packed = HexFloatString(1.5F) + "|" + HexFloatString(-0.25F) + "|" + HexFloatString(0.75F) + "|1|reach";
			Arm arm;
			arm.AddHandTargetFromSave(packed);
			const std::vector<std::string> saved = arm.GetHandTargetsForSave();
			return saved.size() == 1 ? saved[0] : "targets=" + std::to_string(saved.size());
		}

		std::string ProbePieMenuState() {
			PieMenu menu;
			menu.UnpackInteractionState("0|0|0|0|0|" + HexFloatString(1.5F) + "|1|-1|-1|-1|-1");
			return menu.PackInteractionState();
		}

		std::string ProbeInheritedRotAngleDegOffset() {
			Arm arm;
			Reader fractional(std::make_unique<std::istringstream>("1.5\n"), "float-text-selftest.ini");
			arm.ReadProperty("InheritedRotAngleDegOffset", fractional);
			const float fractionalOffset = arm.GetInheritedRotAngleOffset();
			Reader whole(std::make_unique<std::istringstream>("1\n"), "float-text-selftest.ini");
			arm.ReadProperty("InheritedRotAngleDegOffset", whole);
			return Hex(fractionalOffset) + "/" + Hex(arm.GetInheritedRotAngleOffset());
		}

		std::string ProbeCustomNumberValue() {
			Arm arm;
			Reader reader(std::make_unique<std::istringstream>("NumberValue\n\tFloatTextKey = 1.5\n"), "float-text-selftest.ini");
			arm.ReadProperty("AddCustomValue", reader);
			return Hex(arm.GetNumberValue("FloatTextKey"));
		}

		struct LocaleProbe {
			const char* name;
			std::string (*run)();
			const char* expected; //!< The text or bits the site must produce in any locale, or null when the constant is not fixed here.
		};

		/// Installs a locale whose decimal point is a comma, and reports which one it got.
		std::string InstallCommaLocale() {
			for (const char* name: {"de-DE", "de_DE.UTF-8", "German_Germany.1252", "de_DE"}) {
				try {
					std::locale::global(std::locale(name));
				} catch (const std::exception&) {
					continue;
				}
				std::setlocale(LC_ALL, name);
				char probe[32] = {};
				std::snprintf(probe, sizeof(probe), "%.1f", 1.5);
				if (std::string(probe) == "1,5") {
					return name;
				}
				std::locale::global(std::locale::classic());
				std::setlocale(LC_ALL, "C");
			}
			return std::string();
		}

		/// Every site that parses or writes sim-visible or saved float text must produce the same bytes and
		/// the same bits whatever the process locale is. The C locale run fixes the reference; the comma
		/// locale run must match it character for character.
		void CheckLocaleIndependence() {
			ConstructManagersForEntities();
			std::cout << Tag << " stage=managers" << std::endl;
			CheckHexFloatMatchesTheStream();
			std::cout << Tag << " stage=hexfloat" << std::endl;
			static const LocaleProbe probes[] = {
			    {"reader_float", ProbeReaderFloat, "0x3fc00000/0x3f800000"},
			    {"reader_double", ProbeReaderDouble, "0xbfb999999999999a/0xbff0000000000000"},
			    // 0x3f800000 is the answer when Reader::c_ReadFloatsAsFloats is false: flipping that switch flips this.
			    {"reader_single_rounding", ProbeReaderSingleRounding, "0x3f800001"},
			    {"reader_out_of_range", ProbeReaderOutOfRange, nullptr},
			    {"writer_float", ProbeWriter, "1.5|-0.1"},
			    {"arm_hand_target", ProbeArmHandTarget, "0x1.8p+0|-0x1p-2|0x1.8p-1|1|reach"},
			    {"pie_menu_cursor_angle", ProbePieMenuState, "0|0|0|0|0|0x1.8p+0|1|-1|-1|-1|-1"},
			    {"attachable_deg_offset", ProbeInheritedRotAngleDegOffset, nullptr},
			    {"custom_number_value", ProbeCustomNumberValue, "0x3ff8000000000000"},
			};
			std::string references[std::size(probes)];
			for (size_t index = 0; index < std::size(probes); ++index) {
				// Named as it starts: a site that reaches for an engine manager it has not got dies here.
				std::cout << Tag << " probe=" << probes[index].name << std::endl;
				references[index] = probes[index].run();
				if (probes[index].expected != nullptr && references[index] != probes[index].expected) {
					Fail(std::string("locale=C ") + probes[index].name + " '" + references[index] + "' expected '" + probes[index].expected + "'");
				}
			}
			const std::string locale = InstallCommaLocale();
			std::cout << Tag << " locale=" << (locale.empty() ? "none" : locale) << std::endl;
			if (locale.empty()) {
				return;
			}
			for (size_t index = 0; index < std::size(probes); ++index) {
				const std::string result = probes[index].run();
				if (result != references[index]) {
					Fail(std::string("locale=") + locale + " " + probes[index].name + " '" + result + "' but locale=C gives '" + references[index] + "'");
				}
				if (probes[index].expected != nullptr && result != probes[index].expected) {
					Fail(std::string("locale=") + locale + " " + probes[index].name + " '" + result + "' expected '" + probes[index].expected + "'");
				}
			}
			std::locale::global(std::locale::classic());
			std::setlocale(LC_ALL, "C");
		}

	} // namespace

	int Run() {
		failures = 0;
		// With no standard codec at all the fallback is the shipped codec and the two compare with
		// themselves, so say which oracle is carrying the run instead of passing silently.
		const char* oracle = (RTE_STD_FLOAT_FROM_CHARS && RTE_STD_FLOAT_TO_CHARS) ? "library+tables" : (RTE_STD_FLOAT_FROM_CHARS || RTE_STD_FLOAT_TO_CHARS) ? "part-library+tables"
		                                                                                                                                                    : "tables-only";
		std::cout << Tag << " from_chars=" << RTE_STD_FLOAT_FROM_CHARS << " to_chars=" << RTE_STD_FLOAT_TO_CHARS << " patterns=" << RandomPatternCount << " oracle=" << oracle << std::endl;
		CheckCorpus<float>("float");
		CheckCorpus<double>("double");
		CheckGrammar();
		CheckFormatTables();
		CheckLocaleIndependence();
		if (failures > 0) {
			std::cerr << Tag << " FAIL total=" << failures << std::endl;
			return 1;
		}
		std::cout << Tag << " PASS" << std::endl;
		return 0;
	}

}
