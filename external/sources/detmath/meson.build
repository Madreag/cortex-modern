project('detmath', 'c')

cc = meson.get_compiler('c')

detmath_sources = files(
  'musl/sin.c', 'musl/cos.c', 'musl/tan.c', 'musl/asin.c', 'musl/acos.c',
  'musl/atan.c', 'musl/atan2.c', 'musl/exp.c', 'musl/log.c', 'musl/log10.c',
  'musl/pow.c', 'musl/sinh.c', 'musl/cosh.c', 'musl/tanh.c', 'musl/expm1.c',
  'musl/scalbn.c', 'musl/__sin.c', 'musl/__cos.c', 'musl/__tan.c',
  'musl/__rem_pio2.c', 'musl/__rem_pio2_large.c', 'musl/__expo2.c',
  'musl/__math_oflow.c', 'musl/__math_uflow.c', 'musl/__math_xflow.c',
  'musl/__math_divzero.c', 'musl/__math_invalid.c',
  'musl/exp_data.c', 'musl/log_data.c', 'musl/pow_data.c',
)

detmath_inc = [include_directories('include'), include_directories('musl')]
# Linux has the real <features.h>/<endian.h>; the shims are for toolchains without them
if host_machine.system() != 'linux'
  detmath_inc += include_directories('include_shim')
endif

detmath_args = ['-include', meson.current_source_dir() / 'include/detmath_port.h']
if cc.get_id() != 'msvc'
  detmath_args += ['-ffp-contract=off', '-fno-fast-math']
endif

detmath_lib = static_library('detmath', detmath_sources,
  include_directories: detmath_inc,
  c_args: detmath_args,
)

detmath_dep = declare_dependency(
  link_with: detmath_lib,
  include_directories: include_directories('include'),
)

meson.override_dependency('detmath', detmath_dep)
