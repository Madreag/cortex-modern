#include "DetMathSweep.h"

#include "DetMath.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace RTE::DetMathSweep {

	namespace {
		uint64_t Bits(double v) {
			uint64_t u;
			std::memcpy(&u, &v, sizeof(u));
			return u;
		}

		void HashIn(uint64_t& h, uint64_t v) {
			// FNV-1a over the output bit pattern, byte by byte
			for (int i = 0; i < 8; ++i) {
				h = (h ^ ((v >> (i * 8)) & 0xFF)) * 0x100000001B3ULL;
			}
		}

		// Deterministic mantissa patterns from a constant-seeded LCG
		uint64_t NextPattern(uint64_t& state) {
			state = state * 6364136223846793005ULL + 1442695040888963407ULL;
			return state;
		}

		using Fn1 = double (*)(double);

		// Grid over the full exponent range with varied mantissas; clamps to [domainMin, domainMax]
		uint64_t Sweep1(Fn1 fn, double domainMin, double domainMax) {
			uint64_t h = 0xCBF29CE484222325ULL;
			uint64_t patternState = 0x9E3779B97F4A7C15ULL;
			for (int e = -1022; e <= 1023; e += 3) {
				for (int m = 0; m < 96; ++m) {
					const uint64_t mantissa = NextPattern(patternState) & 0xFFFFFFFFFFFFFULL;
					const uint64_t biased = static_cast<uint64_t>(e + 1023);
					double x;
					const uint64_t xb = (biased << 52) | mantissa;
					std::memcpy(&x, &xb, sizeof(x));
					if (m & 1) { x = -x; }
					if (x < domainMin || x > domainMax) { continue; }
					HashIn(h, Bits(fn(x)));
				}
			}
			return h;
		}

		using Fn2 = double (*)(double, double);

		uint64_t Sweep2(Fn2 fn, double domainMin, double domainMax) {
			uint64_t h = 0xCBF29CE484222325ULL;
			uint64_t patternState = 0x2545F4914F6CDD1DULL;
			for (int i = 0; i < 600; ++i) {
				const int ea = -300 + i;
				for (int j = 0; j < 600; j += 1) {
					const int eb = -300 + j;
					const uint64_t ma = NextPattern(patternState) & 0xFFFFFFFFFFFFFULL;
					const uint64_t mb = NextPattern(patternState) & 0xFFFFFFFFFFFFFULL;
					double a, b;
					const uint64_t ab = (static_cast<uint64_t>(ea + 1023) << 52) | ma;
					const uint64_t bb = (static_cast<uint64_t>(eb + 1023) << 52) | mb;
					std::memcpy(&a, &ab, sizeof(a));
					std::memcpy(&b, &bb, sizeof(b));
					if ((i + j) & 1) { a = -a; }
					if (j & 2) { b = -b; }
					if (a < domainMin || a > domainMax) { continue; }
					HashIn(h, Bits(fn(a, b)));
				}
			}
			return h;
		}

		constexpr double c_Huge = 1.7976931348623157e308;
	} // namespace

	int Run() {
		static const struct {
			const char* name;
			Fn1 fn;
			double lo, hi;
		} fns1[] = {
		    {"sin", detmath_sin, -c_Huge, c_Huge},
		    {"cos", detmath_cos, -c_Huge, c_Huge},
		    {"tan", detmath_tan, -c_Huge, c_Huge},
		    {"asin", detmath_asin, -1.0, 1.0},
		    {"acos", detmath_acos, -1.0, 1.0},
		    {"atan", detmath_atan, -c_Huge, c_Huge},
		    {"exp", detmath_exp, -c_Huge, c_Huge},
		    {"log", detmath_log, 0.0, c_Huge},
		    {"log10", detmath_log10, 0.0, c_Huge},
		    {"sinh", detmath_sinh, -c_Huge, c_Huge},
		    {"cosh", detmath_cosh, -c_Huge, c_Huge},
		    {"tanh", detmath_tanh, -c_Huge, c_Huge},
		    {"expm1", detmath_expm1, -c_Huge, c_Huge},
		};

		for (const auto& f: fns1) {
			std::printf("[detmath-sweep] %s %016" PRIx64 "\n", f.name, Sweep1(f.fn, f.lo, f.hi));
			std::fflush(stdout);
		}
		std::printf("[detmath-sweep] atan2 %016" PRIx64 "\n", Sweep2(detmath_atan2, -c_Huge, c_Huge));
		std::fflush(stdout);
		std::printf("[detmath-sweep] pow %016" PRIx64 "\n", Sweep2(detmath_pow, 0.0, c_Huge));
		std::printf("[detmath-sweep] done\n");
		std::fflush(stdout);
		return 0;
	}
} // namespace RTE::DetMathSweep
