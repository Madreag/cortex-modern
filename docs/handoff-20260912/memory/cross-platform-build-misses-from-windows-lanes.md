---
name: cross-platform-build-misses-from-windows-lanes
description: Windows lanes break the Mac/Linux build in two recurring ways (a new .cpp added to RTEA.vcxproj but not meson.build; clang-rejected default arguments built from nested structs); check both before merging
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T19:52:40.443Z
---

On 2026-09-12 the Mac clean clone of the fixgroup-6 candidate failed to build for two reasons no Windows gate catches: W89-2 added Source/Managers/PreviewScriptSelfTest.cpp to RTEA.vcxproj only (Source/Managers/meson.build never listed it), and W97's NetPortMap.h declared `Request(..., const Options& options = Options())` where Options is a nested struct with default member initializers, which clang 17 refuses ("default member initializer ... required before the end of its enclosing class") while MSVC accepts it.

**Why:** the milestone must build on arm64/macOS (the second worker host and the cross-arch determinism reference); a Windows-only battery cannot see either break, so they surface late, after a push.

**How to apply:** before merging any lane branch, run `git diff --diff-filter=A --name-only <base>..<tip> -- 'Source/**/*.cpp'` and check each basename appears in its directory's meson.build; grep new headers for default arguments of nested-class type (`= Options()`, `= Config()`) and replace them with an overload. Put both checks in worker briefs that add files. Related: [[mac-job-model-gate-needs-dashed-output-name]].
