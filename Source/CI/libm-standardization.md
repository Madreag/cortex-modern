# Cross-platform floating-point policy

The shared simulation uses ordinary `float` and `double`. Its scalar primitives,
compiler rules and thread control registers define the numerical contract. The
engine does not replace the platform libm with a vendor library.

## Compiler and VM contract

- MSVC engine, LuaJIT and Allegro configurations use `/fp:precise`.
- GCC/Clang engine compilation disables contraction and fast math. LuaJIT and
  Allegro C compilation explicitly use `-ffp-contract=off` and `-fno-fast-math`.
- `lj_arch.h` requires `LUAJIT_NUMMODE=2`, the ARM64-compatible dual number mode,
  and Lua 5.1 semantics. `luaJIT_numeric_policy()` reports the compiled library's
  actual dual number, binary64, GC64 and compatibility flags. Generated VM assembly
  also reports its number mode; a disagreement with C returns an invalid policy.
  Windows DynASM generation passes `DUALNUM` for both x64 and x86. Engine VM startup
  rejects a mismatch before creating a state and prints the active runtime flags.
- Lua interpreter and JIT math use the same installed engine primitives.
  `jit.opt.start` cannot enable FMA. JIT/FFI remain available to mods; the user's
  JIT setting can still select interpreted execution.

## Runtime environment

`FloatingPointEnvironment` sets round-to-nearest with gradual underflow. On x64,
MXCSR FTZ and DAZ are clear. On ARM64, FPCR FZ and FZ16 are clear; alternate
handling, default NaN and exception traps are disabled. Sticky exception status
does not affect the policy check.

The main simulation thread initializes this policy before any engine work.
Engine thread-pool workers, owned threads and async jobs initialize it at entry.
Every tick and work boundary checks it. Lua/native callbacks, error unwinds,
interpreted and JIT FFI returns, pathfinding callbacks and capture callbacks check
it before simulation code resumes. Drift aborts with a diagnostic; it is not
silently repaired after a callback.

## Shared math

`Source/System/DeterministicMath.h` holds the engine's existing sin/cos, atan/atan2,
exp/log and pow primitives. The ordinary arithmetic paths remain unchanged.
Square root explicitly uses SSE or ARM64 `fsqrt` under the pinned environment.
Remainder uses normalized dyadic subtraction and exact binary scaling, avoiding
x87 `fprem`. `frexp`, `ldexp`, sign operations and integer rounding retain their
exact IEEE operations under this policy.

Physics, vector magnitude, angle normalization and Matrix interpolation call
these primitives. `RoundFloatToPrecision` also uses the common power primitive
because it is exported to Lua. LuaJIT's bytecode `^`, parser/constant folding,
fast math functions and JIT call table use the installed engine hooks. Table
overrides alone cannot cover these paths. `math.log(x, base)` follows one operation
sequence and `math.ldexp` uses one int32 exponent conversion on all targets.
The log2 primitive extracts the exact exponent of binary powers, including
subnormals, before the general logarithm path.

Allegro bitmap rotation remains shared: it stamps terrain and collision pixels.
Its existing `_AL_SINCOS` override in `alconfig.h` uses the same sin/cos polynomial.
Clean dependency rebuilds are required; frozen audit objects still contained
native trig calls. The C compiler contract applies to this polynomial too.

## Audit call-site disposition

Gate 3's 916 shared-risk rows contain repeated source, object, library and binary
references. Their disposition follows the callers, not the presence of an import.
The original gate findings and comparison rules remain unchanged.

| Call family | Disposition and source evidence |
| --- | --- |
| Lua VM math, `^`, folds, fast functions and optional-base log | Shared; routed through engine primitives in `lj_vmmath.c`, VM assembly, IR calls and recorders. |
| `RoundFloatToPrecision` | Shared Lua binding; power-of-ten scale uses the engine primitive. |
| `_rotate_scale_flip_coordinates` | Shared terrain/collision stamping; existing polynomial plus pinned C compilation, requiring a fresh dependency rebuild. |
| SDL stdlib wrappers | No direct simulation callers. SDL consumers are rendering, audio resampling, display metadata and local device input/haptics; input becomes controller frames. `SDL_fmod/fmodf` have no SDL runtime caller. SDL_image SVG is outside ContentFile's supported PNG/BMP image-info path. |
| PNG gamma tables | Unused transform paths. `IMG_png.c` reads raw palette/rows without setting gamma or alpha-mode transforms. `ContentFile::EncodeIndexedPNG` writes non-linear 8-bit colormap data with `PNG_IMAGE_FLAG_FAST`. Legacy Allegro loadpng is not used. |
| Allegro legacy 3D/quaternion, fixed hypot and AllegroGL | No engine calls or Lua bindings. Bitmap rotation is treated separately as shared. PSP mouse code is excluded on these targets; legacy Allegro joystick polling is not installed. |
| Allegro arc, Raylib and ImGui math | Screen geometry, font/UI meshes, texture transforms and GPU mipmaps. Collision/terrain use SceneMan and Allegro bitmaps. |
| SceneEditor circular planet position | New-scene GUI command writes a preset's planet-map location before a match; no shared fight update path. |
| LuaJIT host/minilua | Build artifact generator, not the game VM. |
| Transport `logf` and CRT import thunks | Transport diagnostics and import infrastructure; callers are classified separately. No networking behavior is changed. |

The Windows sites with missing function names were resolved against the exact
frozen EXE/PDB hashes, GUIDs and ages. Public symbols and unwind regions distinguish
VM calls, SDL wrappers, arc drawing, transport logging and import thunks. The
static optional-base log body has two log calls, a reciprocal and a multiply;
its nearest public symbol is not its owner. VM indirect dispatch and fold switch
tables require source correspondence rather than a linear instruction scan.

## Detecting checks

- `-lua-numeric-policy-selftest`: linked VM receipt plus overflow, signed zero,
  conversion and arithmetic checks in interpreted and JIT modes. The old x64
  single-number policy fails the required dual-number receipt.
- `-fp-environment-selftest`: rounding/denormal drift detection, inherited worker
  drift, gradual underflow and a clean native callback. `tools/test_fp_environment.py`
  additionally requires deliberate native-return, Lua-error and capture drift to abort
  with the FP diagnostic, rather than accepting a crash or nonzero exit alone.
- `-deterministic-math-selftest`: exact sqrt/remainder edge cases, VM and engine
  result-bit comparisons in both execution modes, bytecode/string/constant/JIT
  power, optional-base log, every binary64 power's exact log2, fractional ldexp and
  a request to enable FMA. It prints
  a digest that must match on Windows x64, Linux GCC x64 and macOS GCC ARM64.
- The existing `-rotate-primitive-selftest` must match across those rebuilt targets.

These are source changes awaiting queued platform builds and checks. A source
review or an old binary cannot establish a green runtime result.
