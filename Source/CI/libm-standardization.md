# Cross-platform FPU determinism

Determinism across platforms comes from the FPU + libm path, not fixed-point. The
engine runs ordinary `float`/`double` math; we pin the compiler and the math library
so the same source produces bit-identical results on same-arch builds.

## Compiler FP contract

- **MSVC**: `/fp:precise` + `/arch:SSE2` (`RTEA.vcxproj` `FloatingPointModel=Precise`
  on every config). Precise stops op reordering/contraction; SSE2 keeps Win32 off
  x87's 80-bit registers (x64 implies SSE2).
- **GCC/Clang**: `-ffp-contract=off` plus `-fno-fast-math`, `-fno-associative-math`,
  `-fno-reciprocal-math`, `-fno-unsafe-math-optimizations`, `-frounding-math`,
  `-fsignaling-nans`. `-ffp-contract=off` is required on ARM (FMA-on by default,
  which silently breaks cross-arch determinism) and harmless on x86.

## libm choice

- **Windows / x86 / x64**: SDL3's bundled `src/libm` provides the transcendental
  functions, so `sin`/`cos`/`pow`/etc. are identical regardless of the host CRT.
- **Linux / macOS**: same SDL3 libm path; do not link the platform libm for the
  determinism-sensitive transcendentals.

Standardizing the libm implementation removes the last source of cross-host drift
once the compiler FP contract is pinned.
