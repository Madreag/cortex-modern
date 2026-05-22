project('BLAKE3', 'c', version: '1.8.5')

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
  include_directories: include_directories('.'),
)

blake3_dep = declare_dependency(
  link_with: blake3_lib,
  include_directories: include_directories('.'),
)

meson.override_dependency('blake3', blake3_dep)
