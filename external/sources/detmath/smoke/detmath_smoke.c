// Prints bit patterns over a probe grid; diff the output across toolchains.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "detmath.h"

static uint64_t bits(double v) {
	uint64_t u;
	memcpy(&u, &v, sizeof(u));
	return u;
}

typedef double (*fn1)(double);

int main(void) {
	static const struct { const char* name; fn1 f; } fns[] = {
	    {"sin", detmath_sin}, {"cos", detmath_cos}, {"tan", detmath_tan},
	    {"asin", detmath_asin}, {"acos", detmath_acos}, {"atan", detmath_atan},
	    {"exp", detmath_exp}, {"log", detmath_log}, {"log10", detmath_log10},
	    {"sinh", detmath_sinh}, {"cosh", detmath_cosh}, {"tanh", detmath_tanh},
	    {"expm1", detmath_expm1},
	};

	// Varied magnitudes incl. huge args so the rem_pio2_large path runs
	static const double xs[] = {
	    0.0, 1e-300, 1e-9, 0.1, 0.5, 0.999, 1.0, 1.5707963267948966, 3.141592653589793,
	    10.0, 123.456, 1e4, 1e9, 1e16, 1e300, -0.7, -3.0, -1e5,
	};

	for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); ++i) {
		for (size_t j = 0; j < sizeof(xs) / sizeof(xs[0]); ++j) {
			printf("%s(%.17g) = %016llx\n", fns[i].name, xs[j],
			       (unsigned long long)bits(fns[i].f(xs[j])));
		}
	}
	for (size_t j = 0; j < sizeof(xs) / sizeof(xs[0]); ++j) {
		printf("atan2(%.17g,0.3) = %016llx\n", xs[j],
		       (unsigned long long)bits(detmath_atan2(xs[j], 0.3)));
		printf("pow(1.7,%.17g) = %016llx\n", xs[j],
		       (unsigned long long)bits(detmath_pow(1.7, xs[j])));
	}
	return 0;
}
