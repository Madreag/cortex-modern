#pragma once

// Header file for global utility methods.

#include "RTEError.h"
#include "Constants.h"

#include <random>
#include <memory>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <cmath>

namespace RTE {

	class Vector;
	class Matrix;

#pragma region Random Numbers
	class RandomGenerator {
		std::mt19937 m_RNG; //!< The random number generator used for all random functions.
		uint64_t m_Seed = 0; //!< The seed the generator was last seeded with.

		// One raw 32-bit draw. The mt19937 stream is portable; the std:: distributions are not,
		// so the mappings below are explicit.
		uint32_t DrawBits() {
			return static_cast<uint32_t>(m_RNG());
		}

		// Canonical [0, 1) from the top mantissa-width bits.
		template <typename floatType>
		floatType DrawCanonical() {
			if constexpr (sizeof(floatType) == 4) {
				return static_cast<floatType>(DrawBits() >> 8) * floatType(0x1.0p-24);
			} else {
				const uint64_t hi = DrawBits() >> 6;
				const uint64_t lo = DrawBits() >> 6;
				return static_cast<floatType>((hi << 26) | lo) * floatType(0x1.0p-52);
			}
		}

	public:
		/// Seed the random number generator.
		void Seed(uint64_t seed) {
			m_Seed = seed;
			m_RNG.seed(seed);
		};

		/// Gets the seed this generator was last seeded with.
		/// @return The last seed.
		uint64_t GetSeed() const { return m_Seed; }

		/// Gets a copy of the engine state, for rollback snapshots.
		/// @return The engine by value.
		std::mt19937 GetEngineState() const { return m_RNG; }

		/// Restores a previously captured engine state.
		/// @param state The engine state to restore.
		void SetEngineState(const std::mt19937& state) { m_RNG = state; }

		/// Serialize the generator's full internal state to a string for hashing — byte-identical
		/// across same-seed runs at the same tick once the determinism work has settled.
		std::string SerializeStateForHashing() const {
			// mt19937's operator<< text is implementation-defined; hash the next outputs of a copy instead (the stream is standard).
			std::mt19937 copy = m_RNG;
			std::ostringstream oss;
			for (int i = 0; i < std::mt19937::state_size; ++i) {
				oss << static_cast<uint32_t>(copy()) << ' ';
			}
			return oss.str();
		}

		/// Function template which returns a uniformly distributed random number in the range [-1, 1).
		/// @return Uniformly distributed random number in the range [-1, 1).
		template <typename floatType = float>
		typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNormalNum() {
			return DrawCanonical<floatType>() * floatType(2.0) - floatType(1.0);
		}

		/// Function template specialization for int types which returns a uniformly distributed random number in the range [-1, 1].
		/// @return Uniformly distributed random number in the range [-1, 1].
		template <typename intType>
		typename std::enable_if<std::is_integral<intType>::value, intType>::type RandomNormalNum() {
			return static_cast<intType>(DrawBits() % 3u) - intType(1);
		}

		/// Function template which returns a uniformly distributed random number in the range [0, 1).
		/// @return Uniformly distributed random number in the range [0, 1).
		template <typename floatType = float>
		typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNum() {
			return DrawCanonical<floatType>();
		}

		/// Function template specialization for int types which returns a uniformly distributed random number in the range [0, 1].
		/// @return Uniformly distributed random number in the range [0, 1].
		template <typename intType>
		typename std::enable_if<std::is_integral<intType>::value, intType>::type RandomNum() {
			return static_cast<intType>(DrawBits() & 1u);
		}

		/// Function template which returns a uniformly distributed random number in the range [min, max].
		/// @param min Lower boundary of the range to pick a number from.
		/// @param max Upper boundary of the range to pick a number from.
		/// @return Uniformly distributed random number in the range [min, max].
		template <typename floatType = float>
		typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNum(floatType min, floatType max) {
			if (max < min) {
				std::swap(min, max);
			}
			return min + (max - min) * DrawCanonical<floatType>();
		}

		/// Function template specialization for int types which returns a uniformly distributed random number in the range [min, max].
		/// @param min Lower boundary of the range to pick a number from.
		/// @param max Upper boundary of the range to pick a number from.
		/// @return Uniformly distributed random number in the range [min, max].
		template <typename intType>
		typename std::enable_if<std::is_integral<intType>::value, intType>::type RandomNum(intType min, intType max) {
			if (max < min) {
				std::swap(min, max);
			}
			// Two's-complement diff is correct modulo 2^64 even for signed full ranges
			const uint64_t spanMinusOne = static_cast<uint64_t>(max) - static_cast<uint64_t>(min);
			uint64_t draw;
			if (spanMinusOne >= 0xFFFFFFFFu) {
				const uint64_t hi = DrawBits();
				const uint64_t lo = DrawBits();
				draw = (hi << 32) | lo;
			} else {
				draw = DrawBits();
			}
			if (spanMinusOne != ~0ULL) {
				draw %= spanMinusOne + 1u;
			}
			return static_cast<intType>(min + static_cast<intType>(draw));
		}
	};

	// Sim/render RNG split: sim code draws g_SimRNG, cosmetic draws use g_RenderRNG,
	// so render-rate-dependent draws never advance the sim stream.
	extern RandomGenerator g_SimRNG;    //!< Sim RNG. Default for the RandomNum/RandomNormalNum free functions.
	extern RandomGenerator g_RenderRNG; //!< Cosmetic RNG. Used by visual jitter, audio variation, screen shake, etc.

	/// Deprecated: prefer g_SimRNG or g_RenderRNG directly. Aliases g_SimRNG so pre-split
	/// usage (including taking its address for Lua bindings) keeps the same behaviour.
	extern RandomGenerator& g_RandomGenerator;

	/// Seed both global RNGs to the deterministic constant.
	void SeedRNG();

	// Per-thread redirect for the sim free functions. Null on serial / main-thread code.
	extern thread_local RandomGenerator* t_simRNGOverride;

	/// Gets the sim RNG the free functions draw from: this thread's override if installed, else g_SimRNG.
	inline RandomGenerator& GetSimRNG() { return t_simRNGOverride ? *t_simRNGOverride : g_SimRNG; }

	// Free-function form routes to the sim RNG. Render-side call sites must reach for
	// g_RenderRNG.RandomNum<T>() / RandomNormalNum<T>() by name.
	template <typename floatType = float>
	typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNormalNum() {
		return GetSimRNG().RandomNormalNum<floatType>();
	}

	template <typename intType>
	typename std::enable_if<std::is_integral<intType>::value, intType>::type RandomNormalNum() {
		return GetSimRNG().RandomNormalNum<intType>();
	}

	template <typename floatType = float>
	typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNum() {
		return GetSimRNG().RandomNum<floatType>();
	}

	template <typename intType>
	typename std::enable_if<std::is_integral<intType>::value, intType>::type RandomNum() {
		return GetSimRNG().RandomNum<intType>();
	}

	template <typename floatType = float>
	typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNum(floatType min, floatType max) {
		return GetSimRNG().RandomNum<floatType>(min, max);
	}

	template <typename intType>
	typename std::enable_if<std::is_integral<intType>::value, intType>::type RandomNum(intType min, intType max) {
		return GetSimRNG().RandomNum<intType>(min, max);
	}
#pragma endregion

#pragma region Interpolation
	/// Simple Linear Interpolation, with an added bonus: scaleStart and scaleEnd let you define your scale, where 0 and 1 would be standard scale.
	/// This scale is used to normalize your progressScalar value and Lerp accordingly.
	/// @param scaleStart The start of the scale to Lerp along.
	/// @param scaleEnd The end of the scale to Lerp along.
	/// @param startValue The start value of your Lerp.
	/// @param endValue The end value of your Lerp.
	/// @param progressScalar How far your Lerp has progressed. Automatically normalized through use of scaleStart and scaleEnd.
	/// @return Interpolated value.
	float Lerp(float scaleStart, float scaleEnd, float startValue, float endValue, float progressScalar);

	/// Simple Linear Interpolation
	/// @param startValue The start value of your Lerp.
	/// @param endValue The end value of your Lerp.
	/// @param progressScalar How far your Lerp has progressed.
	/// @return Interpolated value.
	float Lerp(float startValue, float endValue, float progressScalar);

	/// Simple Linear Interpolation, with an added bonus: scaleStart and scaleEnd let you define your scale, where 0 and 1 would be standard scale.
	/// This scale is used to normalize your progressScalar value and Lerp accordingly.
	/// @param scaleStart The start of the scale to Lerp along.
	/// @param scaleEnd The end of the scale to Lerp along.
	/// @param startValue The start position of your Lerp.
	/// @param endValue The end position of your Lerp.
	/// @param progressScalar How far your Lerp has progressed. Automatically normalized through use of scaleStart and scaleEnd.
	/// @return Interpolated value.
	Vector Lerp(float scaleStart, float scaleEnd, Vector startPos, Vector endPos, float progressScalar);

	/// Simple Linear Interpolation
	/// This scale is used to normalize your progressScalar value and Lerp accordingly.
	/// @param startValue The start position of your Lerp.
	/// @param endValue The end position of your Lerp.
	/// @param progressScalar How far your Lerp has progressed.
	/// @return Interpolated value.
	Vector Lerp(Vector startPos, Vector endPos, float progressScalar);

	/// Simple Linear Interpolation, with an added bonus: scaleStart and scaleEnd let you define your scale, where 0 and 1 would be standard scale.
	/// This scale is used to normalize your progressScalar value and Lerp accordingly.
	/// @param scaleStart The start of the scale to Lerp along.
	/// @param scaleEnd The end of the scale to Lerp along.
	/// @param startRot The start rotation of your Lerp.
	/// @param endRot The end rotation of your Lerp.
	/// @param progressScalar How far your Lerp has progressed. Automatically normalized through use of scaleStart and scaleEnd.
	/// @return Interpolated value.
	Matrix Lerp(float scaleStart, float scaleEnd, const Matrix& startRot, const Matrix& endRot, float progressScalar);

	/// Simple Linear Interpolation
	/// @param startRot The start rotation of your Lerp.
	/// @param endRot The end rotation of your Lerp.
	/// @param progressScalar How far your Lerp has progressed.
	/// @return Interpolated value.
	Matrix Lerp(const Matrix& startRot, const Matrix& endRot, float progressScalar);

	/// Nonlinear ease-in interpolation. Starts slow.
	/// @param start Start value.
	/// @param end End value.
	/// @param progressScalar Normalized positive progress scalar (0 - 1.0).
	/// @return Interpolated value.
	float EaseIn(float start, float end, float progressScalar);

	/// Nonlinear ease-out interpolation. Slows down toward the end.
	/// @param start Start value.
	/// @param end End value.
	/// @param progressScalar Normalized positive progress scalar (0 - 1.0).
	/// @return Interpolated value.
	float EaseOut(float start, float end, float progressScalar);

	/// Nonlinear ease-in-out interpolation. Slows down in the start and end.
	/// @param start Start value.
	/// @param end End value.
	/// @param progressScalar Normalized positive progress scalar (0 - 1.0).
	/// @return Interpolated value.
	float EaseInOut(float start, float end, float progressScalar);
#pragma endregion

#pragma region Clamping
	/// Clamps a value between two limit values.
	/// @param value Value to clamp.
	/// @param upperLimit Upper limit of value.
	/// @param lowerLimit Lower limit of value.
	/// @return True if either limit is currently reached, False if not.
	bool Clamp(float& value, float upperLimit, float lowerLimit);

	/// Clamps a value between two limit values.
	/// @param value Value to clamp.
	/// @param upperLimit Upper limit of value.
	/// @param lowerLimit Lower limit of value.
	/// @return Upper/Lower limit value if limit is currently reached, value between limits if not.
	float Limit(float value, float upperLimit, float lowerLimit);
#pragma endregion

#pragma region Rounding
	/// Rounds a float to a set fixed point precision (digits after decimal point) with option to always ceil or always floor the remainder.
	/// @param inputFloat The input float to round.
	/// @param precision The precision to round to, i.e. the number of digits after the decimal points.
	/// @param roundingMode Method of rounding to use. 0 for system default, 1 for floored remainder, 2 for ceiled remainder.
	/// @return A string of the float, rounded and displayed to chosen precision.
	std::string RoundFloatToPrecision(float input, int precision, int roundingMode = 0);

	/// Rounds an integer to the specified nearest multiple.
	/// For example, if the arguments are 63 and 5, the returned value will be 65.
	/// @param num The number to round to the nearest multiple.
	/// @param multiple The multiple to round to.
	/// @return An integer rounded to the specified nearest multiple.
	inline int RoundToNearestMultiple(int num, int multiple) { return static_cast<int>(std::round(static_cast<float>(num) / static_cast<float>(multiple)) * static_cast<float>(multiple)); }
#pragma endregion

#pragma region Angle Helpers
	/// Returns a copy of the angle normalized so it's between 0 and 2PI.
	/// @param angle The angle to normalize, in radians.
	/// @return The angle, normalized so it's between 0 and 2PI
	float NormalizeAngleBetween0And2PI(float angle);

	/// Returns a copy of the angle normalized so it's between -PI and PI.
	/// @param angle The angle to normalize, in radians.
	/// @return The angle, normalized so it's between -PI and PI
	float NormalizeAngleBetweenNegativePIAndPI(float angle);

	/// Returns whether or not the angle to check is between the start and end angles. Note that, because of how angles work (when normalized), the start angle may be greater than the end angle.
	/// @param angleToCheck The angle to check, in radians.
	/// @param startAngle The starting angle for the range.
	/// @param endAngle The ending angle for the range.
	/// @return Whether or not the angle to check is between the start and end angle.
	bool AngleWithinRange(float angleToCheck, float startAngle, float endAngle);

	/// Clamps the passed in angle between the specified lower and upper limits, in a CCW direction.
	/// @param angleToClamp The angle to clamp.
	/// @param startAngle The lower limit for clamping.
	/// @param endAngle The upper limit for clamping.
	/// @return The angle, clamped between the start and end angle.
	float ClampAngle(float angleToClamp, float startAngle, float endAngle);
#pragma endregion

#pragma region Detection
	/// Tells whether a point is within a specified box.
	/// @param point Vector position of the point we're checking.
	/// @param boxPos Vector position of the box.
	/// @param width Width of the box.
	/// @param height Height of the box.
	/// @return True if point is inside box bounds.
	bool WithinBox(const Vector& point, const Vector& boxPos, float width, float height);

	/// Tells whether a point is within a specified box.
	/// @param point Vector position of the point we're checking.
	/// @param left Position of box left plane (X start).
	/// @param top Position of box top plane (Y start).
	/// @param right Position of box right plane (X end).
	/// @param bottom Position of box bottom plane (Y end).
	/// @return True if point is inside box bounds.
	bool WithinBox(const Vector& point, float left, float top, float right, float bottom);
#pragma endregion

#pragma region Conversion
	/// Returns a corrected angle value that can be used with Allegro fixed point math routines where 256 equals 360 degrees.
	/// @param angleDegrees The angle value to correct. In degrees.
	/// @return A float with the represented angle as full rotations being 256.
	inline float GetAllegroAngle(float angleDegrees) { return (angleDegrees / 360) * 256; }

	/// Returns the given angle converted from degrees to radians.
	/// @param angleDegrees The angle in degrees to be converted.
	/// @return The converted angle in radians.
	inline float DegreesToRadians(float angleDegrees) { return angleDegrees / 180.0F * c_PI; }

	/// Returns the given angle converted from radians to degrees.
	/// @param angleRadians The angle in radians to be converted.
	/// @return The converted angle in degrees.
	inline float RadiansToDegrees(float angleRadians) { return angleRadians / c_PI * 180.0F; }

	// Deterministic sin/cos for on-wire physics — platform libm differs in the last ULP cross-toolchain; this range-reduced polynomial in basic ops (FP contraction is off) is bit-identical on every toolchain.
	inline void DeterministicSinCos(double angle, double& sinOut, double& cosOut) {
		const double twoOverPi = 0.63661977236758134308;
		const double halfPi = 1.57079632679489661923;
		const double q = angle * twoOverPi;
		const long k = static_cast<long>(q >= 0.0 ? q + 0.5 : q - 0.5);
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

	// Deterministic sin/cos convenience wrappers over DeterministicSinCos.
	inline double DeterministicSin(double angle) { double s, c; DeterministicSinCos(angle, s, c); return s; }
	inline double DeterministicCos(double angle) { double s, c; DeterministicSinCos(angle, s, c); return c; }

	// Deterministic atan core (fdlibm s_atan, public domain) — platform libm atan differs in the last ULPs cross-toolchain; this rational reduction in basic ops (FP contraction is off) is bit-identical on every toolchain.
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

	// Deterministic atan2 — wraps DeterministicAtan with fdlibm quadrant handling.
	inline double DeterministicAtan2(double y, double x) {
		const double pi = 3.14159265358979311600e+00;
		const double piLo = 1.22464679914735317720e-16;
		const double halfPi = 1.57079632679489655800e+00;
		if (x == 0.0 && y == 0.0) { return 0.0; }
		if (x == 0.0) { return y > 0.0 ? halfPi : -halfPi; }
		if (y == 0.0) { return x > 0.0 ? 0.0 : pi; }
		const double ay = y < 0.0 ? -y : y;
		const double axx = x < 0.0 ? -x : x;
		const double z = DeterministicAtan(ay / axx); // atan(|y/x|), in [0, pi/2)
		if (x > 0.0) { return y < 0.0 ? -z : z; }
		return y < 0.0 ? (z - piLo) - pi : pi - (z - piLo);
	}

	// Deterministic exp (fdlibm e_exp, public domain) — platform libm exp differs cross-toolchain; this range-reduced rational in basic ops + an exact ldexp scale is bit-identical everywhere.
	inline double DeterministicExp(double x) {
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

	// Deterministic natural log (fdlibm e_log, public domain) via an exact frexp decomposition — platform libm log differs cross-toolchain; this is bit-identical everywhere.
	inline double DeterministicLog(double x) {
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

	// Deterministic pow — exact integer-exponent path via repeated multiply (the sim's powers are integral), general path through the exp/log polys. Platform libm pow routes through exp(y*log(x)) even for integer y and diverges cross-toolchain.
	inline double DeterministicPow(double base, double exponent) {
		const long long intExponent = static_cast<long long>(exponent);
		if (static_cast<double>(intExponent) == exponent && intExponent >= -1024 && intExponent <= 1024) {
			double result = 1.0;
			const long long count = intExponent < 0 ? -intExponent : intExponent;
			for (long long i = 0; i < count; ++i) { result *= base; }
			return intExponent < 0 ? 1.0 / result : result;
		}
		return DeterministicExp(exponent * DeterministicLog(base));
	}
#pragma endregion

#pragma region Strings
	/// Checks whether two strings are equal when the casing is disregarded.
	/// @param strA First string.
	/// @param strB Second string.
	/// @return Whether the two strings are equal case insensitively.
	inline bool StringsEqualCaseInsensitive(const std::string_view& strA, const std::string_view& strB) {
		return std::equal(strA.begin(), strA.end(), strB.begin(), strB.end(), [](char strAChar, char strBChar) { return std::tolower(strAChar) == std::tolower(strBChar); });
	}

	/// If a file "foo/Bar.txt" exists, and this method is passed "FOO/BAR.TXT", then this method will return "foo/Bar.txt".
	/// This method's purpose is to enable Linux to get the real path using a case-insensitive search.
	/// The real path is used by the Lua file I/O handling methods to ensure full Windows compatibility.
	/// @param fullPath Path to case-insensitively translate to a real path.
	/// @return The real path. If the path doesn't exist, it returns the fullPath argument with all the existing parent directories correctly capitalized.
	std::string GetCaseInsensitiveFullPath(const std::string& fullPath);

	/// Hashes a string in a cross-compiler/platform safe way (std::hash gives different results on different compilers).
	/// @param text Text string to hash.
	/// @return The hash result value.
	uint64_t Hash(const std::string& text);
#pragma endregion

#pragma region Misc
	/// Convenience method that takes in a double pointer array and returns a std::vector with its contents, because pointers-to-pointers are the devil. The passed in array is deleted in the process so no need to delete it manually.
	/// @param arrayOfType The double pointer to convert to a std::vector.
	/// @param arraySize The size of the double pointer array.
	template <typename Type> std::vector<Type*> ConvertDoublePointerToVectorOfPointers(Type** arrayOfType, size_t arraySize) {
		std::unique_ptr<Type*[]> doublePointerArray = std::unique_ptr<Type*[]>(arrayOfType);
		std::vector<Type*> outputVector;
		for (size_t i = 0; i < arraySize; ++i) {
			outputVector.emplace_back(doublePointerArray[i]);
		}
		return outputVector;
	}

	/// Returns the sign of the given input value.
	/// @return The sign as an integer -1, 0 or +1.
	template <typename Type> int Sign(const Type& value) {
		return (Type(0) < value) - (Type(0) > value);
	}

	/// Exponential decay function. Allows decaying a value from A to B at the same rate regardless of delta time. Always stable.
	///
	/// As an example, assuming variables like this:
	/// float current = 1.0f;
	/// float target = 0.0f;
	/// float decay = 10.0f;
	///
	/// Then running this:
	///
	/// current = ExpDecay(current, target, decay, 1.0f / 30.0f);
	///
	/// Preduces the same result (barring precision errors) as:
	/// 
	/// current = ExpDecay(current, target, decay, 1.0f / 60.0f);
	/// current = ExpDecay(current, target, decay, 1.0f / 60.0f);
	///
	/// In both cases, "current" will be equal to about 0.716531310573789.
	/// 
	/// Lecture on the topic: https://youtube.com/watch?v=LSNQuFEDOyQ
	///
	/// @param current The current value that we're decaying.
	/// @param target Target we're decaying to.
	/// @param decay Rate of decay. For example, with 0.7 the value will be decayed about halfway there in 1 second.
	/// @param deltaTime Amount of time of decay to simulate.
	/// @returns The decayed value.
	inline float ExpDecay(float current, float target, float decay, float deltaTime) {
		return target + (current - target) * static_cast<float>(DeterministicExp(static_cast<double>(-decay * deltaTime)));
	}
		
#pragma endregion
} // namespace RTE
