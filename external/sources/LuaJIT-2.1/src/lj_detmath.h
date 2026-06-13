/*
** Redirect libm transcendentals to the engine's deterministic math (detmath),
** so Lua math is bit-identical across platforms. fmod/ldexp/sqrt are IEEE-exact
** and stay on the CRT. Resolved at final link against the detmath static lib.
*/
#ifndef _LJ_DETMATH_H
#define _LJ_DETMATH_H

extern double detmath_sin(double);
extern double detmath_cos(double);
extern double detmath_tan(double);
extern double detmath_asin(double);
extern double detmath_acos(double);
extern double detmath_atan(double);
extern double detmath_atan2(double, double);
extern double detmath_exp(double);
extern double detmath_log(double);
extern double detmath_log10(double);
extern double detmath_pow(double, double);
extern double detmath_sinh(double);
extern double detmath_cosh(double);
extern double detmath_tanh(double);

#define sin detmath_sin
#define cos detmath_cos
#define tan detmath_tan
#define asin detmath_asin
#define acos detmath_acos
#define atan detmath_atan
#define atan2 detmath_atan2
#define exp detmath_exp
#define log detmath_log
#define log10 detmath_log10
#define pow detmath_pow
#define sinh detmath_sinh
#define cosh detmath_cosh
#define tanh detmath_tanh

#endif
