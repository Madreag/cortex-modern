// Cross-platform Lua-math divergence probe.
// LuaJIT's math.sin/cos/tan/asin/acos/tanh/log wrap the C library (libm); the ^ operator
// uses pow. This dumps raw IEEE-754 bits of each over a dense domain sweep so arm64 (Apple)
// and Linux (glibc) outputs can be byte-diffed per op. Build: clang++/g++ -O2 -ffp-contract=off
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>

static uint64_t B(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

static void sweep(FILE* f, const char* name, double (*fn)(double), double lo, double hi, int n) {
	for (int i = 0; i <= n; ++i) {
		double x = lo + (hi - lo) * (static_cast<double>(i) / n);
		double y = fn(x);
		std::fprintf(f, "%s,%016llx,%016llx\n", name,
		             static_cast<unsigned long long>(B(x)), static_cast<unsigned long long>(B(y)));
	}
}

static void sweepPow(FILE* f, const char* name, double e, double lo, double hi, int n) {
	for (int i = 0; i <= n; ++i) {
		double b = lo + (hi - lo) * (static_cast<double>(i) / n);
		double y = std::pow(b, e);
		std::fprintf(f, "%s,%016llx:%016llx,%016llx\n", name,
		             static_cast<unsigned long long>(B(b)), static_cast<unsigned long long>(B(e)),
		             static_cast<unsigned long long>(B(y)));
	}
}

int main(int argc, char** argv) {
	const char* out = argc > 1 ? argv[1] : "luaprobe.csv";
	FILE* f = std::fopen(out, "w");
	const int N = 4000;
	sweep(f, "sin",  std::sin,  -12.566370614359172, 12.566370614359172, N); // [-4pi,4pi]
	sweep(f, "cos",  std::cos,  -12.566370614359172, 12.566370614359172, N);
	sweep(f, "tan",  std::tan,  -1.5, 1.5, N);
	sweep(f, "asin", std::asin, -1.0, 1.0, N);
	sweep(f, "acos", std::acos, -1.0, 1.0, N);
	sweep(f, "tanh", std::tanh, -10.0, 10.0, N);
	sweep(f, "sinh", std::sinh, -10.0, 10.0, N);
	sweep(f, "cosh", std::cosh, -10.0, 10.0, N);
	sweep(f, "log",  std::log,  1e-12, 1000.0, N);
	sweep(f, "exp",  std::exp,  -700.0, 700.0, N);
	sweepPow(f, "pow_int3", 3.0, -5.0, 5.0, N);   // x^3 (the SharedBehaviors aimSkill call)
	sweepPow(f, "pow_0.37", 0.37, 0.001, 10.0, N);
	sweepPow(f, "pow_2.5",  2.5, 0.001, 10.0, N);
	std::fclose(f);
	std::printf("wrote %s\n", out);
	return 0;
}
