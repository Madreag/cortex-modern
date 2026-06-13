// Vendored musl transcendentals, bit-identical on every platform.
// sqrt/fabs/fmod are IEEE-exact already and stay on the CRT.
#ifndef DETMATH_H
#define DETMATH_H

#ifdef __cplusplus
extern "C" {
#endif

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
double detmath_scalbn(double x, int n);

#ifdef __cplusplus
}
#endif

#endif
