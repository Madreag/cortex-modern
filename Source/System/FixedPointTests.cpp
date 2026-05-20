// Unit + determinism tests for FixedPoint.h (MP M3 Block A).
//
// Standalone console program — depends only on FixedPoint.h and the stdlib, no
// engine headers. Build it with FixedPointTests.vcxproj (MSBuild) or the Meson
// `FixedPointTests` target. Exit code 0 = all passed, 1 = a failure.
//
// The closing SELF-CHECK line is a hash over a scripted, integer-only sequence of
// fixed-point operations. CI runs this on Windows and Linux; the two hashes must
// be identical — that is the first cross-OS bit-identity proof of the M3 work,
// before any engine code is converted.

#include "FixedPoint.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace RTE;

namespace {

	int g_Passed = 0;
	int g_Failed = 0;

	void Check(bool ok, const char* name) {
		if (ok) {
			++g_Passed;
		} else {
			++g_Failed;
			std::printf("  [FAIL] %s\n", name);
		}
	}

	// Deterministic 64-bit LCG so the test inputs are integer-only and reproducible.
	struct Lcg {
		uint64_t state;
		explicit Lcg(uint64_t seed) :
		    state(seed) {}
		uint64_t Next() {
			state = state * 6364136223846793005ull + 1442695040888963407ull;
			return state;
		}
		int64_t NextRange(int64_t lo, int64_t hi) {
			uint64_t span = static_cast<uint64_t>(hi - lo) + 1u;
			return lo + static_cast<int64_t>(Next() % span);
		}
	};

	// FNV-1a over raw bytes — the self-check accumulator.
	struct Fnv {
		uint64_t h = 1469598103934665603ull;
		void Mix(int64_t v) {
			uint64_t u = static_cast<uint64_t>(v);
			for (int i = 0; i < 8; ++i) {
				h ^= (u >> (i * 8)) & 0xFFu;
				h *= 1099511628211ull;
			}
		}
	};

	// Mul64's constexpr dispatch path (Mul64Portable) — proven at compile time.
	static_assert(Mul64(int64_t(1) << 32, int64_t(1) << 32) == Int128(0, 1), "Mul64 constexpr: 2^32 * 2^32 == 2^64");
	static_assert(Mul64(int64_t(-5), int64_t(7)) == Int128::FromI64(-35), "Mul64 constexpr: negative operand");
	static_assert(Mul64(int64_t(123456789), int64_t(-987654321)) == Int128::FromI64(int64_t(123456789) * int64_t(-987654321)),
	              "Mul64 constexpr: matches int64 product when it fits");

	// --- 128-bit multiply: the portable and intrinsic paths must agree bit-for-bit. ---
	void TestMul128Agreement() {
		Lcg rng(0xA11CE5);
		bool agree = true;
		for (int i = 0; i < 200000; ++i) {
			int64_t a = static_cast<int64_t>(rng.Next());
			int64_t b = static_cast<int64_t>(rng.Next());
			Int128 p = Mul64Portable(a, b);
			Int128 q = Mul64Intrinsic(a, b);
			if (p != q) {
				agree = false;
				break;
			}
		}
		// Edge values.
		const int64_t edges[] = {0, 1, -1, INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1,
		                         int64_t(1) << 40, -(int64_t(1) << 40), kFixedOneRaw, -kFixedOneRaw};
		for (int64_t a: edges) {
			for (int64_t b: edges) {
				if (Mul64Portable(a, b) != Mul64Intrinsic(a, b)) {
					agree = false;
				}
			}
		}
		Check(agree, "Mul64 portable vs intrinsic agree (all bits)");

		// A hand-computed product: (2^32) * (2^32) = 2^64 = {hi:1, lo:0}.
		Int128 known = Mul64Portable(int64_t(1) << 32, int64_t(1) << 32);
		Check(known.lo == 0 && known.hi == 1, "Mul64 2^32 * 2^32 == 2^64");
		// Negative * positive sign.
		Int128 negp = Mul64Portable(-(int64_t(1) << 32), int64_t(1) << 32);
		Check(negp.IsNegative(), "Mul64 negative sign");

		// The production Mul64 dispatcher at runtime resolves to the intrinsic path.
		bool dispatch = true;
		for (int64_t a: edges) {
			for (int64_t b: edges) {
				if (Mul64(a, b) != Mul64Portable(a, b)) { dispatch = false; }
			}
		}
		Check(dispatch, "Mul64 runtime dispatcher agrees with portable");
	}

	// --- Fixed multiply value correctness. ---
	void TestFixedMul() {
		Check((Fixed(3) * Fixed(4)).Raw() == Fixed(12).Raw(), "3 * 4 == 12");
		Check((Fixed(-3) * Fixed(4)).Raw() == Fixed(-12).Raw(), "-3 * 4 == -12");
		Check((Fixed(1) * Fixed(1)).Raw() == kFixedOneRaw, "1 * 1 == 1");

		// half * half == quarter.
		Fixed half = Fixed::FromRaw(kFixedOneRaw / 2);
		Fixed quarter = Fixed::FromRaw(kFixedOneRaw / 4);
		Check((half * half).Raw() == quarter.Raw(), "0.5 * 0.5 == 0.25");

		// Random small operands: independent int64 reference (a*b)>>24.
		// Operands kept within +-2^31 so ra*rb stays inside int64 and a plain
		// int64 (a*b)>>24 is an exact, independent reference.
		Lcg rng(0xBEEF);
		bool ok = true;
		for (int i = 0; i < 100000; ++i) {
			int64_t ra = rng.NextRange(-(int64_t(1) << 31), int64_t(1) << 31);
			int64_t rb = rng.NextRange(-(int64_t(1) << 31), int64_t(1) << 31);
			int64_t plainRef = (ra * rb) >> kFractionBits;
			if ((Fixed::FromRaw(ra) * Fixed::FromRaw(rb)).Raw() != plainRef) {
				ok = false;
				break;
			}
		}
		Check(ok, "Fixed multiply matches (a*b)>>24 reference (small operands)");
	}

	// --- Fixed divide: both code paths, and round-trip. ---
	void TestFixedDiv() {
		Check((Fixed(12) / Fixed(4)).Raw() == Fixed(3).Raw(), "12 / 4 == 3");
		Check((Fixed(1) / Fixed(4)).Raw() == kFixedOneRaw / 4, "1 / 4 == 0.25");
		Check((Fixed(-12) / Fixed(4)).Raw() == Fixed(-3).Raw(), "-12 / 4 == -3");

		// Exact slow-path cases — |dividend raw| >= 2^39 forces the 128-bit divide.
		Check(DivFixedRaw(int64_t(1) << 40, kFixedOneRaw) == (int64_t(1) << 40), "slow-path divide 2^40 / 1");
		Check(DivFixedRaw(int64_t(3) << 40, int64_t(1) << 25) == (int64_t(3) << 39), "slow-path divide 3*2^40 / 2");
		Check(DivFixedRaw(-(int64_t(1) << 45), kFixedOneRaw) == -(int64_t(1) << 45), "slow-path divide -2^45 / 1");
		Check(DivFixedRaw(int64_t(7) << 40, -kFixedOneRaw) == -(int64_t(7) << 40), "slow-path divide sign 7*2^40 / -1");

		Lcg rng(0xD1D);
		bool ok = true;
		for (int i = 0; i < 100000; ++i) {
			int64_t ra = rng.NextRange(-(int64_t(1) << 45), int64_t(1) << 45); // spans fast + slow paths
			int64_t rb = rng.NextRange(int64_t(1) << 10, int64_t(1) << 30);
			if (rng.Next() & 1u) {
				rb = -rb;
			}
			Fixed result = Fixed::FromRaw(ra) / Fixed::FromRaw(rb);
			// Verify: result * divisor reproduces the dividend within the truncation budget.
			Fixed back = result * Fixed::FromRaw(rb);
			int64_t err = back.Raw() - Fixed::FromRaw(ra).Raw();
			if (err < 0) {
				err = -err;
			}
			if (err > ((rb < 0 ? -rb : rb) >> kFractionBits) + 8) {
				ok = false;
				break;
			}
		}
		Check(ok, "Fixed divide round-trips within one ulp-step (fast + slow paths)");
	}

	// --- Float boundary round-trip. ---
	void TestRoundTrip() {
		bool ok = true;
		double maxErr = 0.0;
		Lcg rng(0xF10A7);
		for (int i = 0; i < 100000; ++i) {
			// /1000 (not a power of two) so d is not exactly representable and the
			// truncating cast in FromDouble is actually exercised.
			double d = static_cast<double>(rng.NextRange(-(int64_t(1) << 20), int64_t(1) << 20)) / 1000.0;
			Fixed f = Fixed::FromDouble(d);
			double err = std::fabs(f.ToDouble() - d);
			if (err > maxErr) {
				maxErr = err;
			}
			if (err > 1.0 / 16777216.0) {
				ok = false;
			}
		}
		Check(ok, "FromDouble / ToDouble round-trip within 1 Q24 ulp");
		std::printf("  round-trip max error: %.3e\n", maxErr);

		Check(Fixed::FromDouble(3.7).TruncToInt() == 3 && Fixed::FromDouble(-3.7).TruncToInt() == -3,
		      "TruncToInt rounds toward zero");
		Check(Fixed::FromDouble(-3.7).FloorToInt() == -4 && Fixed::FromDouble(3.2).CeilToInt() == 4,
		      "FloorToInt / CeilToInt round toward -inf / +inf");
	}

	// --- Sqrt vs std::sqrt, and exactness of the integer root. ---
	void TestSqrt() {
		Check(Sqrt(Fixed(0)).Raw() == 0, "sqrt(0) == 0");
		Check(Sqrt(Fixed(4)).Raw() == Fixed(2).Raw(), "sqrt(4) == 2");
		Check(Sqrt(Fixed(-1)).Raw() == 0, "sqrt(negative) clamps to 0");

		bool ok = true;
		double maxErr = 0.0;
		Lcg rng(0x5081);
		for (int i = 0; i < 100000; ++i) {
			int64_t raw = rng.NextRange(0, int64_t(1) << 48);
			Fixed x = Fixed::FromRaw(raw);
			double got = Sqrt(x).ToDouble();
			double ref = std::sqrt(x.ToDouble());
			double err = std::fabs(got - ref);
			if (err > maxErr) {
				maxErr = err;
			}
			if (err > 1.0e-5) {
				ok = false;
			}
		}
		Check(ok, "Sqrt(Fixed) within 1e-5 of std::sqrt");
		std::printf("  Sqrt max error: %.3e\n", maxErr);

		// Integer-root exactness: r*r <= v < (r+1)*(r+1).
		bool exact = true;
		Lcg rng2(0x5082);
		for (int i = 0; i < 50000; ++i) {
			Int128 v = Int128::FromU64(rng2.Next()).ShlBits(static_cast<int>(rng2.Next() % 60));
			uint64_t r = Isqrt128(v);
			Int128 rr = Mul64Portable(static_cast<int64_t>(r), static_cast<int64_t>(r));
			Int128 r1 = Mul64Portable(static_cast<int64_t>(r + 1), static_cast<int64_t>(r + 1));
			if (!(rr <= v && v < r1)) {
				exact = false;
				break;
			}
		}
		Check(exact, "Isqrt128 exact: r*r <= v < (r+1)*(r+1)");
	}

	// --- Sin / Cos / Atan2 vs std::, across several full circles. ---
	void TestTrig() {
		bool sinOk = true, cosOk = true;
		double sinMax = 0.0, cosMax = 0.0;
		for (int i = -40000; i <= 40000; ++i) {
			double angle = static_cast<double>(i) * 0.0005; // sweep ~ -20..20 radians
			Fixed fa = Fixed::FromDouble(angle);
			double se = std::fabs(Sin(fa).ToDouble() - std::sin(angle));
			double ce = std::fabs(Cos(fa).ToDouble() - std::cos(angle));
			if (se > sinMax) {
				sinMax = se;
			}
			if (ce > cosMax) {
				cosMax = ce;
			}
			if (se > 5.0e-6) {
				sinOk = false;
			}
			if (ce > 5.0e-6) {
				cosOk = false;
			}
		}
		Check(sinOk, "Sin within 5e-6 of std::sin over -20..20 rad");
		Check(cosOk, "Cos within 5e-6 of std::cos over -20..20 rad");
		std::printf("  Sin max error: %.3e   Cos max error: %.3e\n", sinMax, cosMax);

		bool atanOk = true;
		double atanMax = 0.0;
		Lcg rng(0xA7A2);
		for (int i = 0; i < 100000; ++i) {
			int64_t y = rng.NextRange(-(int64_t(1) << 34), int64_t(1) << 34);
			int64_t x = rng.NextRange(-(int64_t(1) << 34), int64_t(1) << 34);
			double got = Atan2(Fixed::FromRaw(y), Fixed::FromRaw(x)).ToDouble();
			double ref = std::atan2(static_cast<double>(y), static_cast<double>(x));
			double err = std::fabs(got - ref);
			if (err > 6.2) {
				err = std::fabs(err - 2.0 * 3.14159265358979323846);
			}
			if (err > atanMax) {
				atanMax = err;
			}
			if (err > 5.0e-5) {
				atanOk = false;
			}
		}
		Check(atanOk, "Atan2 within 5e-5 of std::atan2");
		std::printf("  Atan2 max error: %.3e\n", atanMax);
	}

	// --- FixedVector + FixedWide. ---
	void TestVector() {
		// 3-4-5 right triangle.
		FixedVector v(Fixed(3), Fixed(4));
		Check((v.GetMagnitude() - Fixed(5)).Raw() == 0 ||
		          Abs(v.GetMagnitude() - Fixed(5)).Raw() < 4,
		      "FixedVector (3,4) magnitude == 5");

		FixedVector a(Fixed(2), Fixed(3));
		FixedVector b(Fixed(5), Fixed(7));
		Check(a.Dot(b).ToFixed().Raw() == Fixed(2 * 5 + 3 * 7).Raw(), "FixedVector dot product");

		// Perpendicular dot is zero.
		Check(v.Dot(v.GetPerpendicular()).Raw().IsZero(), "perpendicular dot == 0");

		// Rotating by 2*pi returns (approximately) the same vector.
		FixedVector r = v.GetRadRotatedCopy(Fixed::FromRaw(kTwoPiRaw));
		Check(Abs(r.m_X - v.m_X).Raw() < 8000 && Abs(r.m_Y - v.m_Y).Raw() < 8000,
		      "rotate by 2*pi is identity (approx)");

		// Squared magnitude of a large vector lives in FixedWide without overflow.
		FixedVector big(Fixed(100000), Fixed(100000));
		FixedWide sqr = big.GetSqrMagnitude();
		Check(sqr > FixedWide::Product(Fixed(0), Fixed(0)), "large sqr-magnitude stays positive in FixedWide");
		Check(big.MagnitudeIsGreaterThan(Fixed(141420)) && !big.MagnitudeIsGreaterThan(Fixed(141422)),
		      "large MagnitudeIsGreaterThan via wide compare");

		// Normalized vector has magnitude ~1.
		FixedVector n = FixedVector(Fixed(8), Fixed(-6)).GetNormalized();
		Check(Abs(n.GetMagnitude() - Fixed(1)).Raw() < 1024, "normalized magnitude ~ 1");
	}

	// --- Near-range valid values must not trip the debug overflow guards. ---
	void TestNearLimit() {
		Fixed big = Fixed::FromRaw(int64_t(1) << 50); // ~6.7e7, far from the 5.5e11 ceiling
		Fixed r = big + big;
		Check(r.Raw() == (int64_t(1) << 51), "near-limit add is correct");
		Fixed m = Fixed::FromRaw(int64_t(1) << 40) * Fixed::FromRaw(int64_t(1) << 40);
		Check(m.Raw() == (int64_t(1) << 56), "near-limit multiply is correct");
	}

	// --- The cross-OS determinism self-check: scripted, integer-only ops -> one hash. ---
	uint64_t SelfCheckHash() {
		Fnv fnv;
		Lcg rng(0xCC1DE7E6F1A7ull);
		Fixed acc = Fixed::FromRaw(1);
		for (int i = 0; i < 50000; ++i) {
			Fixed a = Fixed::FromRaw(rng.NextRange(-(int64_t(1) << 38), int64_t(1) << 38));
			Fixed b = Fixed::FromRaw(rng.NextRange(1, int64_t(1) << 30));
			acc = acc + (a * b);
			acc = acc - (a / b);
			fnv.Mix((a * b).Raw());
			fnv.Mix((a / b).Raw());
			fnv.Mix(Sqrt(Abs(a)).Raw());
			fnv.Mix(Sin(a).Raw());
			fnv.Mix(Cos(a).Raw());
			fnv.Mix(Atan2(a, b).Raw());
			FixedVector v(a, b);
			fnv.Mix(v.GetMagnitude().Raw());
			fnv.Mix(v.GetRadRotatedCopy(b).m_X.Raw());
			fnv.Mix(v.Dot(v.GetPerpendicular()).Raw().lo);
			acc = Fixed::FromRaw((acc.Raw() & 0x7FFFFFFFFFFF) | 1);
		}
		fnv.Mix(acc.Raw());
		return fnv.h;
	}

} // namespace

int main() {
	std::printf("FixedPoint.h tests (MP M3 Block A) — Q40.24 fixed-point\n");

	TestMul128Agreement();
	TestFixedMul();
	TestFixedDiv();
	TestRoundTrip();
	TestSqrt();
	TestTrig();
	TestVector();
	TestNearLimit();

	uint64_t selfHash = SelfCheckHash();
	std::printf("\nSELF-CHECK: %016llx\n", static_cast<unsigned long long>(selfHash));
	std::printf("%d passed, %d failed\n", g_Passed, g_Failed);
	return g_Failed == 0 ? 0 : 1;
}
