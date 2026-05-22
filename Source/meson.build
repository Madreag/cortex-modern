pch = meson.current_source_dir()/'System/StandardIncludes.h'
sources = []
source_libs = []
source_inc_dirs += [include_directories(
  '.',
  'Activities',
  'AI',
  'CI',
  'Entities',
  'GUI',
  'GUI/Wrappers',
  'Lua',
  'Managers',
  'Menus',
  'Network',
  'Renderer',
  'System')]

# BLAKE3 and nlohmann_json are pulled in via proper subproject deps in the top-level
# meson.build (so Meson >=1.6's sandbox check is satisfied on macOS-arm64). Their
# include dirs ride along on the dependency object — no manual include_directories
# crossing into external/sources is needed.

subdir('Activities')
subdir('AI')
subdir('CI')
subdir('Entities')
subdir('GUI')
subdir('Lua')
subdir('Managers')
subdir('Menus')
subdir('Network')
subdir('Renderer')
subdir('System')
