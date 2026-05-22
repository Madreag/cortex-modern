sources += files(
'SimChecksum.cpp',
'ReplayLog.cpp',
'NetworkSimulator.cpp',
)

# BLAKE3 is built as a proper subproject (external/sources/BLAKE3-1.8.5/meson.build)
# and linked via blake3_dep — meson rejects files() reaching into a subproject dir.
