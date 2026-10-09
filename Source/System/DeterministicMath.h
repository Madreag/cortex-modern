#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "Shared simulation math requires precise floating-point compilation"
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#elif !defined(__aarch64__)
#error "Deterministic square root requires a hardware implementation"
#endif

namespace RTE {
	inline double DeterministicSqrt(double value) {
		if (value < 0.0 || std::isnan(value)) { return std::numeric_limits<double>::quiet_NaN(); }
#if defined(__aarch64__)
		double result;
		asm("fsqrt %d0, %d1" : "=w"(result) : "w"(value));
		return result;
#else
		return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(value)));
#endif
	}
	inline float DeterministicSqrt(float value) {
		if (value < 0.0F || std::isnan(value)) { return std::numeric_limits<float>::quiet_NaN(); }
#if defined(__aarch64__)
		float result;
		asm("fsqrt %s0, %s1" : "=w"(result) : "w"(value));
		return result;
#else
		return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(value)));
#endif
	}
	template<class Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
	inline double DeterministicSqrt(Integer value) { return DeterministicSqrt(static_cast<double>(value)); }

	inline double DeterministicFmod(double value, double divisor) {
		if (!std::isfinite(value) || std::isnan(divisor) || divisor == 0.0) { return std::numeric_limits<double>::quiet_NaN(); }
		const double numerator = std::abs(value), denominator = std::abs(divisor);
		if (numerator < denominator || numerator == 0.0) { return value; }
		if (numerator == denominator) { return std::copysign(0.0, value); }
		int exponent, divisorExponent;
		double remainder = std::frexp(numerator, &exponent);
		const double mantissa = std::frexp(denominator, &divisorExponent);
		// Normalized dyadic subtraction and scaling are exact.
		while (exponent > divisorExponent) {
			if (remainder >= mantissa) { remainder -= mantissa; }
			remainder += remainder;
			--exponent;
		}
		if (remainder >= mantissa) { remainder -= mantissa; }
		return std::copysign(std::ldexp(remainder, divisorExponent), value);
	}
	inline float DeterministicFmod(float value, float divisor) { return static_cast<float>(DeterministicFmod(static_cast<double>(value), static_cast<double>(divisor))); }

	// Shared simulation math.
	inline void DeterministicSinCos(double angle, double& sinOut, double& cosOut) {
		if (!std::isfinite(angle)) { sinOut = cosOut = std::numeric_limits<double>::quiet_NaN(); return; }
		if (std::abs(angle) > 1.0e9) { angle = DeterministicFmod(angle, 6.28318530717958647692); }
		const double twoOverPi = 0.63661977236758134308;
		const double halfPi = 1.57079632679489661923;
		const double q = angle * twoOverPi;
		const int64_t k = static_cast<int64_t>(q >= 0.0 ? q + 0.5 : q - 0.5);
		const double r = angle - static_cast<double>(k) * halfPi;
		const double r2 = r * r;
		const double sinR = r * (1.0 + r2 * (-1.0 / 6.0 + r2 * (1.0 / 120.0 + r2 * (-1.0 / 5040.0 + r2 * (1.0 / 362880.0)))));
		const double cosR = 1.0 + r2 * (-1.0 / 2.0 + r2 * (1.0 / 24.0 + r2 * (-1.0 / 720.0 + r2 * (1.0 / 40320.0 + r2 * (-1.0 / 3628800.0)))));
		switch (k & 3) {
			case 1: sinOut = cosR; cosOut = -sinR; break;
			case 2: sinOut = -sinR; cosOut = -cosR; break;
			case 3: sinOut = -cosR; cosOut = sinR; break;
			default: sinOut = sinR; cosOut = cosR; break;
		}
	}

	// Shared simulation math.
	inline double DeterministicSin(double angle) { double s, c; DeterministicSinCos(angle, s, c); return s; }
	inline double DeterministicCos(double angle) { double s, c; DeterministicSinCos(angle, s, c); return c; }

	// Shared simulation math.
	inline double DeterministicAtan(double x) {
		static const double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00};
		static const double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17};
		static const double aT[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01, -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02, 6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02, -3.65315727442169155270e-02, 1.62858201153657823623e-02};
		const bool sign = x < 0.0;
		double ax = sign ? -x : x;
		if (ax >= 7.3786976294838206464e+19) { // |x| >= 2^66: saturates to +-pi/2
			const double z = atanhi[3] + atanlo[3];
			return sign ? -z : z;
		}
		int id;
		if (ax < 0.4375) {
			if (ax < 7.450580596923828125e-09) { return x; } // |x| < 2^-27: atan(x) == x
			id = -1;
		} else if (ax < 1.1875) {
			if (ax < 0.6875) { id = 0; ax = (2.0 * ax - 1.0) / (2.0 + ax); }
			else { id = 1; ax = (ax - 1.0) / (ax + 1.0); }
		} else if (ax < 2.4375) {
			id = 2; ax = (ax - 1.5) / (1.0 + 1.5 * ax);
		} else {
			id = 3; ax = -1.0 / ax;
		}
		const double z = ax * ax;
		const double w = z * z;
		const double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
		const double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
		if (id < 0) {
			const double r = ax - ax * (s1 + s2);
			return sign ? -r : r;
		}
		const double r = atanhi[id] - ((ax * (s1 + s2) - atanlo[id]) - ax);
		return sign ? -r : r;
	}

	// Shared simulation math.
	inline double DeterministicAtan2(double y, double x) {
		const double pi = 3.14159265358979311600e+00;
		const double piLo = 1.22464679914735317720e-16;
		const double halfPi = 1.57079632679489655800e+00;
		if (std::isnan(x) || std::isnan(y)) { return std::numeric_limits<double>::quiet_NaN(); }
		if (std::isinf(x) && std::isinf(y)) { x = std::copysign(1.0, x); y = std::copysign(1.0, y); }
		if (x == 0.0 && y == 0.0) { return 0.0; }
		if (x == 0.0) { return y > 0.0 ? halfPi : -halfPi; }
		if (y == 0.0) { return x > 0.0 ? 0.0 : pi; }
		const double ay = y < 0.0 ? -y : y;
		const double axx = x < 0.0 ? -x : x;
		const double z = DeterministicAtan(ay / axx); // atan(|y/x|), in [0, pi/2)
		if (x > 0.0) { return y < 0.0 ? -z : z; }
		return y < 0.0 ? (z - piLo) - pi : pi - (z - piLo);
	}

	// Shared simulation math.
	inline double DeterministicExp(double x) {
		if (std::isnan(x)) { return std::numeric_limits<double>::quiet_NaN(); }
		const double halF[2] = {0.5, -0.5};
		const double ln2HI[2] = {6.93147180369123816490e-01, -6.93147180369123816490e-01};
		const double ln2LO[2] = {1.90821492927058770002e-10, -1.90821492927058770002e-10};
		const double invln2 = 1.44269504088896338700e+00;
		const double P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;
		if (x >= 7.09782712893383973096e+02) { const double huge = 1.0e300; return huge * huge; } // overflow -> +inf
		if (x <= -7.45133219101941108420e+02) { return 0.0; } // underflow -> 0
		const int xsb = x < 0.0 ? 1 : 0;
		const double ax = x < 0.0 ? -x : x;
		double hi, lo;
		int k;
		if (ax > 0.34657359027997264311) { // |x| > 0.5*ln2
			if (ax < 1.03972077083991796313) { // |x| < 1.5*ln2
				hi = x - ln2HI[xsb];
				lo = ln2LO[xsb];
				k = 1 - xsb - xsb;
			} else {
				k = static_cast<int>(invln2 * x + halF[xsb]);
				const double t = static_cast<double>(k);
				hi = x - t * ln2HI[0];
				lo = t * ln2LO[0];
			}
			x = hi - lo;
		} else if (ax < 3.7252902984619140625e-09) { // |x| < 2^-28: exp(x) == 1+x
			return 1.0 + x;
		} else {
			hi = 0.0;
			lo = 0.0;
			k = 0;
		}
		const double t = x * x;
		const double c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
		if (k == 0) {
			return 1.0 - ((x * c) / (c - 2.0) - x);
		}
		const double y = 1.0 - ((lo - (x * c) / (2.0 - c)) - hi);
		return std::ldexp(y, k);
	}

	// Shared simulation math.
	inline double DeterministicLog(double x) {
		if (std::isnan(x)) { return std::numeric_limits<double>::quiet_NaN(); }
		if (x == std::numeric_limits<double>::infinity()) { return x; }
		const double ln2HI = 6.93147180369123816490e-01;
		const double ln2LO = 1.90821492927058770002e-10;
		const double Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01, Lg7 = 1.479819860511658591e-01;
		if (x <= 0.0) { const double huge = 1.0e300; const double inf = huge * huge; return x < 0.0 ? inf - inf : -inf; }
		int k;
		double f = std::frexp(x, &k); // x = f * 2^k, f in [0.5, 1)
		if (f < 0.70710678118654752440) { f += f; --k; } // center the mantissa around 1
		f -= 1.0;
		const double s = f / (2.0 + f);
		const double z = s * s;
		const double w = z * z;
		const double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
		const double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
		const double R = t2 + t1;
		const double hfsq = 0.5 * f * f;
		const double dk = static_cast<double>(k);
		return dk * ln2HI - ((hfsq - (s * (hfsq + R) + dk * ln2LO)) - f);
	}

	// Shared simulation math.
	inline double DeterministicPow(double base, double exponent) {
		if (exponent == 0.0 || base == 1.0) { return 1.0; }
		if (std::isnan(base) || std::isnan(exponent)) { return std::numeric_limits<double>::quiet_NaN(); }
		const double magnitude = std::abs(base);
		if (std::isinf(exponent)) {
			if (magnitude == 1.0) { return 1.0; }
			return (magnitude > 1.0) == (exponent > 0.0) ? std::numeric_limits<double>::infinity() : 0.0;
		}
		const bool integral = std::floor(exponent) == exponent;
		const bool odd = integral && DeterministicFmod(std::abs(exponent), 2.0) == 1.0;
		if (magnitude == 0.0 || std::isinf(magnitude)) {
			double result = (magnitude == 0.0) == (exponent > 0.0) ? 0.0 : std::numeric_limits<double>::infinity();
			return std::signbit(base) && odd ? -result : result;
		}
		if (integral && exponent >= -1024.0 && exponent <= 1024.0) {
			const long long intExponent = static_cast<long long>(exponent);
			double result = 1.0;
			const long long count = intExponent < 0 ? -intExponent : intExponent;
			for (long long i = 0; i < count; ++i) { result *= base; }
			if (intExponent < 0 && (!std::isnormal(result))) {
				int baseScale, scale = 0;
				const double mantissa = std::frexp(base, &baseScale);
				result = 1.0;
				for (long long i = 0; i < count; ++i) {
					int productScale;
					result = std::frexp(result * mantissa, &productScale);
					scale += baseScale + productScale;
				}
				return std::ldexp(1.0 / result, -scale);
			}
			return intExponent < 0 ? 1.0 / result : result;
		}
		if (base < 0.0 && !integral) { return std::numeric_limits<double>::quiet_NaN(); }
		const double result = DeterministicExp(exponent * DeterministicLog(magnitude));
		return base < 0.0 && odd ? -result : result;
	}

	inline double DeterministicLog2(double value) {
		if (value > 0.0 && std::isfinite(value)) {
			int exponent;
			if (std::frexp(value, &exponent) == 0.5) { return static_cast<double>(exponent - 1); }
		}
		return DeterministicLog(value) * 1.4426950408889634074;
	}
	inline double DeterministicLog10(double value) { return DeterministicLog(value) * 0.43429448190325182765; }
	inline double DeterministicTan(double value) { double sine, cosine; DeterministicSinCos(value, sine, cosine); return sine / cosine; }
	inline double DeterministicAsin(double value) {
		if (std::abs(value) > 1.0) { return std::numeric_limits<double>::quiet_NaN(); }
		return DeterministicAtan2(value, DeterministicSqrt(1.0 - value * value));
	}
	inline double DeterministicAcos(double value) {
		if (std::abs(value) > 1.0) { return std::numeric_limits<double>::quiet_NaN(); }
		return DeterministicAtan2(DeterministicSqrt(1.0 - value * value), value);
	}
	inline double DeterministicSinh(double value) { return (DeterministicExp(value) - DeterministicExp(-value)) * 0.5; }
	inline double DeterministicCosh(double value) { return (DeterministicExp(value) + DeterministicExp(-value)) * 0.5; }
	inline double DeterministicTanh(double value) {
		const double exponential = DeterministicExp(-2.0 * (value < 0.0 ? -value : value));
		const double result = (1.0 - exponential) / (1.0 + exponential);
		return value < 0.0 ? -result : result;
	}
}
