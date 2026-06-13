#pragma once

// Declared directly: including the vendored detmath.h by name would resolve to this
// header itself on case-insensitive filesystems.
extern "C" {
double detmath_sin(double x);
double detmath_cos(double x);
double detmath_tan(double x);
double detmath_asin(double x);
double detmath_acos(double x);
double detmath_atan(double x);
double detmath_atan2(double y, double x);
double detmath_exp(double x);
double detmath_log(double x);
double detmath_log10(double x);
double detmath_pow(double x, double y);
double detmath_sinh(double x);
double detmath_cosh(double x);
double detmath_tanh(double x);
double detmath_expm1(double x);
}

// Deterministic transcendentals for sim code — same bits on every platform.
// Float overloads compute through double; sqrt/fabs/fmod are IEEE-exact and stay on std::.
namespace RTE::DetMath {

	inline double Sin(double x) { return detmath_sin(x); }
	inline float Sin(float x) { return static_cast<float>(detmath_sin(x)); }

	inline double Cos(double x) { return detmath_cos(x); }
	inline float Cos(float x) { return static_cast<float>(detmath_cos(x)); }

	inline double Tan(double x) { return detmath_tan(x); }
	inline float Tan(float x) { return static_cast<float>(detmath_tan(x)); }

	inline double ASin(double x) { return detmath_asin(x); }
	inline float ASin(float x) { return static_cast<float>(detmath_asin(x)); }

	inline double ACos(double x) { return detmath_acos(x); }
	inline float ACos(float x) { return static_cast<float>(detmath_acos(x)); }

	inline double ATan(double x) { return detmath_atan(x); }
	inline float ATan(float x) { return static_cast<float>(detmath_atan(x)); }

	inline double ATan2(double y, double x) { return detmath_atan2(y, x); }
	inline float ATan2(float y, float x) { return static_cast<float>(detmath_atan2(y, x)); }

	inline double Exp(double x) { return detmath_exp(x); }
	inline float Exp(float x) { return static_cast<float>(detmath_exp(x)); }

	inline double Log(double x) { return detmath_log(x); }
	inline float Log(float x) { return static_cast<float>(detmath_log(x)); }

	inline double Log10(double x) { return detmath_log10(x); }
	inline float Log10(float x) { return static_cast<float>(detmath_log10(x)); }

	inline double Pow(double x, double y) { return detmath_pow(x, y); }
	inline float Pow(float x, float y) { return static_cast<float>(detmath_pow(x, y)); }

	inline double Sinh(double x) { return detmath_sinh(x); }
	inline float Sinh(float x) { return static_cast<float>(detmath_sinh(x)); }

	inline double Cosh(double x) { return detmath_cosh(x); }
	inline float Cosh(float x) { return static_cast<float>(detmath_cosh(x)); }

	inline double Tanh(double x) { return detmath_tanh(x); }
	inline float Tanh(float x) { return static_cast<float>(detmath_tanh(x)); }
} // namespace RTE::DetMath
