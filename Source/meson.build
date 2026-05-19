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

external_inc_dirs += [include_directories(
  '../external/sources/BLAKE3-1.8.5',
  '../external/sources/nlohmann_json-3.12.0')]

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
