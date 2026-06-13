// Force-included into every vendored musl TU. Host <math.h> must come before the
// renames (glibc refuses math-name macros), then the renamed prototypes.
#ifndef DETMATH_PORT_H
#define DETMATH_PORT_H

#include <float.h>
#include <math.h>
#include <stdint.h>

// musl marks exported data `hidden`; meaningless here
#define hidden

// Rename everything these TUs define so nothing can collide with the platform CRT
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
#define expm1 detmath_expm1
#define scalbn detmath_scalbn

#define __sin detmath__sin
#define __cos detmath__cos
#define __tan detmath__tan
#define __rem_pio2 detmath__rem_pio2
#define __rem_pio2_large detmath__rem_pio2_large
#define __expo2 detmath__expo2

#define __math_oflow detmath__math_oflow
#define __math_uflow detmath__math_uflow
#define __math_xflow detmath__math_xflow
#define __math_divzero detmath__math_divzero
#define __math_invalid detmath__math_invalid

// Not compiled here, but libm.h declares these and glibc declares the same names
#define __rem_pio2f detmath__rem_pio2f
#define __sindf detmath__sindf
#define __cosdf detmath__cosdf
#define __tandf detmath__tandf
#define __expo2f detmath__expo2f
#define __rem_pio2l detmath__rem_pio2l
#define __sinl detmath__sinl
#define __cosl detmath__cosl
#define __tanl detmath__tanl
#define __polevll detmath__polevll
#define __p1evll detmath__p1evll
#define __lgamma_r detmath__lgamma_r
#define __lgammaf_r detmath__lgammaf_r
#define __math_xflowf detmath__math_xflowf
#define __math_oflowf detmath__math_oflowf
#define __math_uflowf detmath__math_uflowf
#define __math_divzerof detmath__math_divzerof
#define __math_invalidf detmath__math_invalidf

#define __exp_data detmath__exp_data
#define __log_data detmath__log_data
#define __pow_data detmath__pow_data

// Prototypes for the renamed entry points (sinh calls exp, etc.)
#include "detmath.h"

#endif
