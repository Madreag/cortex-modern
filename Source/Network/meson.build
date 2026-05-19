sources += files(
'SimChecksum.cpp',
'ReplayLog.cpp',
'NetworkSimulator.cpp',
)

blake3_sources = files(
'../../external/sources/BLAKE3-1.8.5/blake3.c',
'../../external/sources/BLAKE3-1.8.5/blake3_dispatch.c',
'../../external/sources/BLAKE3-1.8.5/blake3_portable.c',
)
blake3_cargs = [
'-DBLAKE3_NO_SSE2',
'-DBLAKE3_NO_SSE41',
'-DBLAKE3_NO_AVX2',
'-DBLAKE3_NO_AVX512',
'-DBLAKE3_USE_NEON=0',
]
blake3_lib = static_library(
'blake3',
blake3_sources,
c_args: blake3_cargs,
include_directories: include_directories('../../external/sources/BLAKE3-1.8.5'),
)
source_libs += blake3_lib
