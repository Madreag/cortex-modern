#pragma once

// Q40.24 fixed-point math for the MP determinism island (MP M3).
//
// Integer fixed-point arithmetic is exactly specified by the C++ standard, so the
// same source produces the same bits on every conforming compiler and architecture
// — no /fp flags, no libm. The terrain-destruction and atom-collision math in
// SceneMan / AtomGroup / SLTerrain converts its decision arithmetic to these types
// at the function boundary; float storage (Vector, Material, INI, Lua API) is
// untouched. See D:\Projects\M3_PLAN.md §3.
//
// `Fixed` is int64 with 24 fractional bits. Squared magnitudes and dot products
// can exceed that range, so they are computed and compared in `FixedWide` (a
// 128-bit Q80.48) and never narrowed before the compare. Transcendentals are
// pure integer: `Sqrt` is a bit-by-bit integer root, `Sin`/`Cos` a Taylor-built
// LUT, `Atan2` a CORDIC.

#include <array>
#include <cassert>
#include <cstdint>
#include <type_traits>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace RTE {

	/// Q40.24 layout — single-sourced so the format is confirmed in one place.
	inline constexpr int kFractionBits = 24;
	inline constexpr int64_t kFixedOneRaw = int64_t(1) << kFractionBits;

#if defined(NDEBUG)
	#define RTE_FIXED_CHECK(cond) ((void)0)
#else
	#define RTE_FIXED_CHECK(cond) assert(cond)
#endif

#pragma region 128-bit integer

	/// Two's-complement signed 128-bit integer. Used for the multiply intermediate,
	/// the divide dividend, and `FixedWide`. All operations are pure integer code so
	/// they are bit-identical on every target.
	struct Int128 {
		uint64_t lo = 0;
		int64_t  hi = 0;

		constexpr Int128() = default;
		constexpr Int128(uint64_t low, int64_t high) :
		    lo(low), hi(high) {}

		static constexpr Int128 FromI64(int64_t v) { return Int128(static_cast<uint64_t>(v), v < 0 ? -1 : 0); }
		static constexpr Int128 FromU64(uint64_t v) { return Int128(v, 0); }

		constexpr bool IsNegative() const { return hi < 0; }
		constexpr bool IsZero() const { return lo == 0 && hi == 0; }

		constexpr Int128 operator-() const {
			uint64_t nlo = ~lo + 1u;
			uint64_t nhi = ~static_cast<uint64_t>(hi) + (nlo == 0u ? 1u : 0u);
			return Int128(nlo, static_cast<int64_t>(nhi));
		}
		constexpr Int128 operator+(const Int128& o) const {
			uint64_t s = lo + o.lo;
			int64_t carry = (s < lo) ? 1 : 0;
			return Int128(s, hi + o.hi + carry);
		}
		constexpr Int128 operator-(const Int128& o) const { return *this + (-o); }

		constexpr bool operator==(const Int128& o) const { return lo == o.lo && hi == o.hi; }
		constexpr bool operator!=(const Int128& o) const { return !(*this == o); }
		constexpr bool operator<(const Int128& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
		constexpr bool operator>(const Int128& o) const { return o < *this; }
		constexpr bool operator<=(const Int128& o) const { return !(o < *this); }
		constexpr bool operator>=(const Int128& o) const { return !(*this < o); }

		/// Left shift by 0..127 bits.
		constexpr Int128 ShlBits(int n) const {
			if (n <= 0) { return *this; }
			if (n >= 64) { return Int128(0, static_cast<int64_t>(lo << (n - 64))); }
			return Int128(lo << n, static_cast<int64_t>((static_cast<uint64_t>(hi) << n) | (lo >> (64 - n))));
		}
		/// Logical right shift by 0..127 bits. Callers use this on non-negative values only.
		constexpr Int128 ShrLogical(int n) const {
			if (n <= 0) { return *this; }
			uint64_t uhi = static_cast<uint64_t>(hi);
			if (n >= 64) { return Int128(uhi >> (n - 64), 0); }
			return Int128((lo >> n) | (uhi << (64 - n)), static_cast<int64_t>(uhi >> n));
		}
	};

#pragma endregion

#pragma region 64x64 -> 128 multiply

	/// Schoolbook 32-bit-limb 64x64->128 multiply. Pure integer — this is the
	/// portable fallback and the constexpr path; the intrinsic paths are checked
	/// against it bit-for-bit by FixedPointTests.
	constexpr Int128 Mul64Portable(int64_t a, int64_t b) {
		bool neg = (a < 0) != (b < 0);
		uint64_t ua = (a < 0) ? (0u - static_cast<uint64_t>(a)) : static_cast<uint64_t>(a);
		uint64_t ub = (b < 0) ? (0u - static_cast<uint64_t>(b)) : static_cast<uint64_t>(b);

		uint64_t aL = ua & 0xFFFFFFFFull, aH = ua >> 32;
		uint64_t bL = ub & 0xFFFFFFFFull, bH = ub >> 32;
		uint64_t ll = aL * bL, lh = aL * bH, hl = aH * bL, hh = aH * bH;

		uint64_t cross = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
		uint64_t lo = (ll & 0xFFFFFFFFull) | (cross << 32);
		uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);

		Int128 r(lo, static_cast<int64_t>(hi));
		return neg ? -r : r;
	}

	/// Intrinsic 64x64->128 multiply (MSVC _mul128 / GCC-Clang __int128).
	inline Int128 Mul64Intrinsic(int64_t a, int64_t b) {
#if defined(_MSC_VER) && defined(_M_X64)
		int64_t hi = 0;
		uint64_t lo = static_cast<uint64_t>(_mul128(a, b, &hi));
		return Int128(lo, hi);
#elif defined(__SIZEOF_INT128__)
		__int128 p = static_cast<__int128>(a) * static_cast<__int128>(b);
		return Int128(static_cast<uint64_t>(static_cast<unsigned __int128>(p)),
		              static_cast<int64_t>(p >> 64));
#else
		return Mul64Portable(a, b);
#endif
	}

	/// Production 64x64->128 multiply: portable in a constant-evaluated context,
	/// intrinsic at runtime. Both paths produce identical bits (FixedPointTests asserts it).
	constexpr Int128 Mul64(int64_t a, int64_t b) {
		if (std::is_constant_evaluated()) {
			return Mul64Portable(a, b);
		}
		return Mul64Intrinsic(a, b);
	}

	/// Unsigned 128/64 division, restoring algorithm. `num` >= 0, `den` > 0, quotient fits 64 bits.
	constexpr uint64_t DivU128by64(Int128 num, uint64_t den) {
		uint64_t quo = 0;
		Int128 rem(0, 0);
		const Int128 d(den, 0);
		for (int i = 127; i >= 0; --i) {
			rem = rem.ShlBits(1);
			uint64_t bit = (i >= 64) ? ((static_cast<uint64_t>(num.hi) >> (i - 64)) & 1u)
			                         : ((num.lo >> i) & 1u);
			rem.lo |= bit;
			if (rem >= d) {
				rem = rem - d;
				if (i < 64) { quo |= (uint64_t(1) << i); }
			}
		}
		return quo;
	}

	/// (a << 24) / b, truncated toward zero. Fast int64 path for small dividends,
	/// 128-bit path otherwise.
	constexpr int64_t DivFixedRaw(int64_t a, int64_t b) {
		RTE_FIXED_CHECK(b != 0);
		if (a > -(int64_t(1) << 39) && a < (int64_t(1) << 39)) {
			return (a << kFractionBits) / b;
		}
		bool neg = (a < 0) != (b < 0);
		uint64_t ua = (a < 0) ? (0u - static_cast<uint64_t>(a)) : static_cast<uint64_t>(a);
		uint64_t ub = (b < 0) ? (0u - static_cast<uint64_t>(b)) : static_cast<uint64_t>(b);
		uint64_t q = DivU128by64(Int128::FromU64(ua).ShlBits(kFractionBits), ub);
		return neg ? -static_cast<int64_t>(q) : static_cast<int64_t>(q);
	}

	/// floor(sqrt(v)) of a non-negative 128-bit value, bit-by-bit. Exact by construction.
	constexpr uint64_t Isqrt128(Int128 v) {
		Int128 res(0, 0);
		Int128 bit(0, int64_t(1) << 62); // 1 << 126
		while (bit > v) { bit = bit.ShrLogical(2); }
		while (!bit.IsZero()) {
			Int128 resPlusBit = res + bit;
			if (v >= resPlusBit) {
				v = v - resPlusBit;
				res = res.ShrLogical(1) + bit;
			} else {
				res = res.ShrLogical(1);
			}
			bit = bit.ShrLogical(2);
		}
		return res.lo;
	}

#pragma endregion

#pragma region Fixed

	class FixedWide;

	/// A scalar in Q40.24 fixed-point — int64 storage, 24 fractional bits.
	/// Range ~+-5.5e11, precision ~6e-8. Implicitly constructs from int.
	class Fixed {
	public:
		constexpr Fixed() = default;
		constexpr Fixed(int v) :
		    m_Raw(static_cast<int64_t>(v) << kFractionBits) {}

		static constexpr Fixed FromRaw(int64_t raw) {
			Fixed f;
			f.m_Raw = raw;
			return f;
		}
		static Fixed FromFloat(float f) { return FromRaw(static_cast<int64_t>(static_cast<double>(f) * 16777216.0)); }
		static Fixed FromDouble(double d) { return FromRaw(static_cast<int64_t>(d * 16777216.0)); }

		/// Exact integer-derived ratio — used for the sim delta-time, never a drifting float.
		static constexpr Fixed FromRatio(int64_t num, int64_t den) {
			RTE_FIXED_CHECK(den != 0);
			bool neg = (num < 0) != (den < 0);
			uint64_t un = (num < 0) ? (0u - static_cast<uint64_t>(num)) : static_cast<uint64_t>(num);
			uint64_t ud = (den < 0) ? (0u - static_cast<uint64_t>(den)) : static_cast<uint64_t>(den);
			uint64_t q = DivU128by64(Int128::FromU64(un).ShlBits(kFractionBits), ud);
			return FromRaw(neg ? -static_cast<int64_t>(q) : static_cast<int64_t>(q));
		}

		constexpr int64_t Raw() const { return m_Raw; }
		float ToFloat() const { return static_cast<float>(static_cast<double>(m_Raw) / 16777216.0); }
		double ToDouble() const { return static_cast<double>(m_Raw) / 16777216.0; }

		/// Convert to int: floor (toward -inf), trunc (toward zero), round-to-nearest, ceil.
		constexpr int FloorToInt() const { return static_cast<int>(m_Raw >> kFractionBits); }
		constexpr int TruncToInt() const { return static_cast<int>(m_Raw >= 0 ? (m_Raw >> kFractionBits) : -((-m_Raw) >> kFractionBits)); }
		constexpr int RoundToInt() const { return static_cast<int>((m_Raw + (int64_t(1) << (kFractionBits - 1))) >> kFractionBits); }
		constexpr int CeilToInt() const { return static_cast<int>((m_Raw + kFixedOneRaw - 1) >> kFractionBits); }

		constexpr Fixed operator-() const { return FromRaw(-m_Raw); }

		constexpr Fixed operator+(Fixed o) const {
			int64_t r = m_Raw + o.m_Raw;
			RTE_FIXED_CHECK(!((m_Raw > 0 && o.m_Raw > 0 && r < 0) || (m_Raw < 0 && o.m_Raw < 0 && r > 0)));
			return FromRaw(r);
		}
		constexpr Fixed operator-(Fixed o) const { return *this + (-o); }

		constexpr Fixed operator*(Fixed o) const {
			Int128 p = Mul64(m_Raw, o.m_Raw);
			// Arithmetic >>24 of the 128-bit product; the low 64 bits are the result.
			int64_t r = static_cast<int64_t>((p.lo >> kFractionBits) |
			                                 (static_cast<uint64_t>(p.hi) << (64 - kFractionBits)));
			// Valid iff the dropped high bits are pure sign extension of bit 87.
			RTE_FIXED_CHECK((p.hi >> 23) == 0 || (p.hi >> 23) == -1);
			return FromRaw(r);
		}
		constexpr Fixed operator/(Fixed o) const { return FromRaw(DivFixedRaw(m_Raw, o.m_Raw)); }

		constexpr Fixed& operator+=(Fixed o) { return *this = *this + o; }
		constexpr Fixed& operator-=(Fixed o) { return *this = *this - o; }
		constexpr Fixed& operator*=(Fixed o) { return *this = *this * o; }
		constexpr Fixed& operator/=(Fixed o) { return *this = *this / o; }

		constexpr bool operator==(Fixed o) const { return m_Raw == o.m_Raw; }
		constexpr bool operator!=(Fixed o) const { return m_Raw != o.m_Raw; }
		constexpr bool operator<(Fixed o) const { return m_Raw < o.m_Raw; }
		constexpr bool operator>(Fixed o) const { return m_Raw > o.m_Raw; }
		constexpr bool operator<=(Fixed o) const { return m_Raw <= o.m_Raw; }
		constexpr bool operator>=(Fixed o) const { return m_Raw >= o.m_Raw; }

	private:
		int64_t m_Raw = 0;
	};

	constexpr Fixed Abs(Fixed f) { return f.Raw() < 0 ? -f : f; }
	constexpr Fixed Min(Fixed a, Fixed b) { return a < b ? a : b; }
	constexpr Fixed Max(Fixed a, Fixed b) { return a > b ? a : b; }

#pragma endregion

#pragma region FixedWide

	/// A 128-bit Q80.48 value. Holds squared magnitudes and dot products — quantities
	/// that legitimately exceed `Fixed`'s range — so they are compared without narrowing.
	class FixedWide {
	public:
		constexpr FixedWide() = default;
		explicit constexpr FixedWide(Int128 raw) :
		    m_Raw(raw) {}

		/// Full Q24*Q24 -> Q48 product of two Fixed, no shift, no precision loss.
		static constexpr FixedWide Product(Fixed a, Fixed b) { return FixedWide(Mul64(a.Raw(), b.Raw())); }

		/// Widen a Fixed (Q24 -> Q48).
		static constexpr FixedWide FromFixed(Fixed f) { return FixedWide(Int128::FromI64(f.Raw()).ShlBits(kFractionBits)); }

		constexpr Int128 Raw() const { return m_Raw; }

		/// Narrow back to Fixed (Q48 -> Q24). Caller guarantees the value is in range.
		constexpr Fixed ToFixed() const {
			RTE_FIXED_CHECK((m_Raw.hi >> 23) == 0 || (m_Raw.hi >> 23) == -1);
			return Fixed::FromRaw(static_cast<int64_t>((m_Raw.lo >> kFractionBits) |
			                                           (static_cast<uint64_t>(m_Raw.hi) << (64 - kFractionBits))));
		}

		constexpr FixedWide operator+(FixedWide o) const { return FixedWide(m_Raw + o.m_Raw); }
		constexpr FixedWide operator-(FixedWide o) const { return FixedWide(m_Raw - o.m_Raw); }
		constexpr FixedWide operator-() const { return FixedWide(-m_Raw); }

		constexpr bool operator==(FixedWide o) const { return m_Raw == o.m_Raw; }
		constexpr bool operator!=(FixedWide o) const { return m_Raw != o.m_Raw; }
		constexpr bool operator<(FixedWide o) const { return m_Raw < o.m_Raw; }
		constexpr bool operator>(FixedWide o) const { return m_Raw > o.m_Raw; }
		constexpr bool operator<=(FixedWide o) const { return m_Raw <= o.m_Raw; }
		constexpr bool operator>=(FixedWide o) const { return m_Raw >= o.m_Raw; }

	private:
		Int128 m_Raw;
	};

#pragma endregion

#pragma region Sqrt

	/// sqrt of a non-negative Fixed. Negative input clamps to zero.
	constexpr Fixed Sqrt(Fixed x) {
		if (x.Raw() <= 0) { return Fixed::FromRaw(0); }
		Int128 v = Int128::FromU64(static_cast<uint64_t>(x.Raw())).ShlBits(kFractionBits);
		return Fixed::FromRaw(static_cast<int64_t>(Isqrt128(v)));
	}

	/// sqrt of a FixedWide — sqrt of a Q48 value is directly a Q24 Fixed raw.
	constexpr Fixed Sqrt(FixedWide w) {
		if (w.Raw().IsNegative()) { return Fixed::FromRaw(0); }
		return Fixed::FromRaw(static_cast<int64_t>(Isqrt128(w.Raw())));
	}

#pragma endregion

#pragma region Trig

	/// Q24 raw angle constants.
	inline constexpr int64_t kHalfPiRaw = 26353589;  // pi/2
	inline constexpr int64_t kPiRaw = 52707179;      // pi
	inline constexpr int64_t kTwoPiRaw = 105414357;  // 2*pi
	inline constexpr int64_t kSinScaleRaw = 43748177637; // 16384/(2*pi), Q24 — angle -> table position

	/// Quarter-wave sine table, 4097 entries of Q24, built at compile time by a pure
	/// integer Taylor series so the values are bit-identical on every compiler.
	constexpr std::array<int32_t, 4097> BuildSinQuarter() {
		std::array<int32_t, 4097> table{};
		constexpr int64_t halfPiQ30 = 1686629713; // pi/2, Q30
		for (int i = 0; i <= 4096; ++i) {
			int64_t x = (halfPiQ30 * i) / 4096;                    // Q30 angle, no per-step drift
			int64_t xSq = (x * x + (int64_t(1) << 29)) >> 30;      // Q30
			int64_t term = x;
			int64_t sum = x;
			for (int k = 1; k <= 6; ++k) {
				term = -((term * xSq + (int64_t(1) << 29)) >> 30) / static_cast<int64_t>((2 * k) * (2 * k + 1));
				sum += term;
			}
			table[i] = static_cast<int32_t>((sum + (1 << 5)) >> 6); // Q30 -> Q24, rounded
		}
		table[4096] = static_cast<int32_t>(kFixedOneRaw);          // sin(pi/2) is exactly 1
		return table;
	}

	inline constexpr std::array<int32_t, 4097> kSinQuarter = BuildSinQuarter();

	/// sin of a Fixed angle in radians.
	constexpr Fixed Sin(Fixed angle) {
		// Scale the angle so one full circle spans 16384 table positions.
		int64_t pos = (angle * Fixed::FromRaw(kSinScaleRaw)).Raw();
		int64_t idx = (pos >> kFractionBits) & 16383;              // 0..16383
		int64_t frac = pos & (kFixedOneRaw - 1);                   // Q24 fraction in [0,1)
		int quadrant = static_cast<int>(idx >> 12);                // 0..3
		int q = static_cast<int>(idx & 4095);                      // 0..4095

		int64_t lo, hi;
		bool negate = (quadrant >= 2);
		if (quadrant == 0 || quadrant == 2) {
			lo = kSinQuarter[q];
			hi = kSinQuarter[q + 1];
		} else {
			lo = kSinQuarter[4096 - q];
			hi = kSinQuarter[4096 - q - 1];
		}
		int64_t value = lo + (((hi - lo) * frac) >> kFractionBits);
		return Fixed::FromRaw(negate ? -value : value);
	}

	/// cos of a Fixed angle in radians.
	constexpr Fixed Cos(Fixed angle) { return Sin(angle + Fixed::FromRaw(kHalfPiRaw)); }

	/// CORDIC rotation constants — atan(2^-i) in Q24.
	inline constexpr std::array<int64_t, 28> kCordicAtan = {
	    13176795, 7778716, 4110060, 2086331, 1047214, 524117, 262123,
	    131069, 65536, 32768, 16384, 8192, 4096, 2048,
	    1024, 512, 256, 128, 64, 32, 16,
	    8, 4, 2, 1, 0, 0, 0};

	/// atan2(y, x) in radians, via CORDIC vectoring. Result in (-pi, pi].
	constexpr Fixed Atan2(Fixed y, Fixed x) {
		int64_t xr = x.Raw();
		int64_t yr = y.Raw();
		if (xr == 0 && yr == 0) { return Fixed::FromRaw(0); }

		// CORDIC grows x by ~1.65x; scale both down (angle-preserving) if large.
		while (xr > (int64_t(1) << 40) || xr < -(int64_t(1) << 40) ||
		       yr > (int64_t(1) << 40) || yr < -(int64_t(1) << 40)) {
			xr >>= 1;
			yr >>= 1;
		}
		// Fold to the right half-plane.
		int64_t base = 0;
		if (xr < 0) {
			base = (yr >= 0) ? kPiRaw : -kPiRaw;
			xr = -xr;
			yr = -yr;
		}
		int64_t angle = 0;
		for (int i = 0; i < 28; ++i) {
			int64_t nx, ny;
			if (yr > 0) {
				nx = xr + (yr >> i);
				ny = yr - (xr >> i);
				angle += kCordicAtan[i];
			} else {
				nx = xr - (yr >> i);
				ny = yr + (xr >> i);
				angle -= kCordicAtan[i];
			}
			xr = nx;
			yr = ny;
		}
		return Fixed::FromRaw(angle + base);
	}

#pragma endregion

#pragma region FixedVector

	/// A 2D vector of two Fixed. Parallels `Vector` (Source/System/Vector.h) on the
	/// determinism island; `Vector` itself stays float for mod-API compatibility.
	class FixedVector {
	public:
		Fixed m_X;
		Fixed m_Y;

		constexpr FixedVector() = default;
		constexpr FixedVector(Fixed x, Fixed y) :
		    m_X(x), m_Y(y) {}
		FixedVector(float x, float y) :
		    m_X(Fixed::FromFloat(x)), m_Y(Fixed::FromFloat(y)) {}

		/// Convert from anything with public float `m_X`/`m_Y` (e.g. `Vector`) without
		/// this header depending on the engine's Vector.h.
		template <class V> static FixedVector FromVectorLike(const V& v) {
			return FixedVector(Fixed::FromFloat(v.m_X), Fixed::FromFloat(v.m_Y));
		}
		template <class V> V ToVectorLike() const {
			V v;
			v.m_X = m_X.ToFloat();
			v.m_Y = m_Y.ToFloat();
			return v;
		}

		constexpr Fixed GetX() const { return m_X; }
		constexpr Fixed GetY() const { return m_Y; }

		constexpr FixedWide GetSqrMagnitude() const { return FixedWide::Product(m_X, m_X) + FixedWide::Product(m_Y, m_Y); }
		constexpr Fixed GetMagnitude() const { return Sqrt(GetSqrMagnitude()); }

		constexpr bool MagnitudeIsGreaterThan(Fixed mag) const { return GetSqrMagnitude() > FixedWide::Product(mag, mag); }
		constexpr bool MagnitudeIsLessThan(Fixed mag) const { return GetSqrMagnitude() < FixedWide::Product(mag, mag); }

		constexpr FixedWide Dot(const FixedVector& rhs) const { return FixedWide::Product(m_X, rhs.m_X) + FixedWide::Product(m_Y, rhs.m_Y); }
		constexpr Fixed Cross(const FixedVector& rhs) const { return (FixedWide::Product(m_X, rhs.m_Y) - FixedWide::Product(rhs.m_X, m_Y)).ToFixed(); }

		constexpr FixedVector GetPerpendicular() const { return FixedVector(m_Y, -m_X); }

		/// Indexed component access (0 = X, 1 = Y) — mirrors Vector::operator[].
		constexpr const Fixed& operator[](int axis) const { return (axis == 0) ? m_X : m_Y; }
		constexpr Fixed& operator[](int axis) { return (axis == 0) ? m_X : m_Y; }

		FixedVector& SetMagnitude(Fixed newMag) {
			Fixed mag = GetMagnitude();
			if (mag.Raw() == 0) {
				m_X = newMag;
				m_Y = Fixed::FromRaw(0);
			} else {
				Fixed scale = newMag / mag;
				m_X = m_X * scale;
				m_Y = m_Y * scale;
			}
			return *this;
		}
		FixedVector GetNormalized() const {
			FixedVector v = *this;
			v.SetMagnitude(Fixed(1));
			return v;
		}
		FixedVector& CapMagnitude(Fixed capMag) {
			if (capMag.Raw() == 0) {
				m_X = Fixed::FromRaw(0);
				m_Y = Fixed::FromRaw(0);
			} else if (MagnitudeIsGreaterThan(capMag)) {
				SetMagnitude(capMag);
			}
			return *this;
		}

		/// Rotate by an angle in radians — mirrors Vector::GetRadRotatedCopy's convention.
		FixedVector GetRadRotatedCopy(Fixed angle) const {
			Fixed adjusted = -angle;
			Fixed c = Cos(adjusted);
			Fixed s = Sin(adjusted);
			return FixedVector(m_X * c - m_Y * s, m_X * s + m_Y * c);
		}

		/// Absolute angle in radians, in [-pi/2, 1.5pi) — mirrors Vector::GetAbsRadAngle.
		Fixed GetAbsRadAngle() const {
			Fixed radAngle = -Atan2(m_Y, m_X);
			return (radAngle < Fixed::FromRaw(-kHalfPiRaw)) ? radAngle + Fixed::FromRaw(kTwoPiRaw) : radAngle;
		}

		constexpr FixedVector operator-() const { return FixedVector(-m_X, -m_Y); }
		constexpr FixedVector operator+(const FixedVector& o) const { return FixedVector(m_X + o.m_X, m_Y + o.m_Y); }
		constexpr FixedVector operator-(const FixedVector& o) const { return FixedVector(m_X - o.m_X, m_Y - o.m_Y); }
		constexpr FixedVector operator*(Fixed s) const { return FixedVector(m_X * s, m_Y * s); }
		constexpr FixedVector operator/(Fixed s) const { return FixedVector(m_X / s, m_Y / s); }
		constexpr FixedVector& operator+=(const FixedVector& o) { return *this = *this + o; }
		constexpr FixedVector& operator-=(const FixedVector& o) { return *this = *this - o; }

		constexpr bool operator==(const FixedVector& o) const { return m_X == o.m_X && m_Y == o.m_Y; }
		constexpr bool operator!=(const FixedVector& o) const { return !(*this == o); }
	};

#pragma endregion

	/// The fixed sim timestep as an exact Fixed, derived by integer math from
	/// TimerMan's tick rate — never a `FromFloat` of a possibly-drifting float.
	/// See M3_PLAN.md §3.8.
	constexpr Fixed SecondsFromTicks(int64_t deltaTicks, int64_t ticksPerSecond) {
		if (ticksPerSecond == 0) { return Fixed::FromRaw(0); }
		return Fixed::FromRatio(deltaTicks, ticksPerSecond);
	}

} // namespace RTE
