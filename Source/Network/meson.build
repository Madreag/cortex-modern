sources += files(
'SimChecksum.cpp',
)

# BLAKE3 is built as a proper Meson subproject (external/sources/BLAKE3-1.8.5/meson.build).
# It is added to the top-level deps[] in meson.build and reaches this translation unit via
# the executable()'s dependencies list. No raw cross-subproject file references here.
