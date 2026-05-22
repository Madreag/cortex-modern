# Build glue for the vendored BLAKE3 (the per-tick determinism state hash).
# Every other compiled lib under external/sources/ has an in-tree meson.build
# loaded via subproject(); BLAKE3 was added without one, so Source/Network's
# meson.build reached in with files() — a subproject-sandbox violation that
# fails `meson setup`. Portable C path only (SSE/AVX/NEON dispatch disabled) so
# the hash is bit-identical across targets.

project('blake3', 'c')

blake3_lib = static_library(
  'blake3',
  files('blake3.c', 'blake3_dispatch.c', 'blake3_portable.c'),
  c_args: [
    '-DBLAKE3_NO_SSE2',
    '-DBLAKE3_NO_SSE41',
    '-DBLAKE3_NO_AVX2',
    '-DBLAKE3_NO_AVX512',
    '-DBLAKE3_USE_NEON=0',
  ],
)

blake3_dep = declare_dependency(
  link_with: blake3_lib,
  include_directories: include_directories('.'),
)
