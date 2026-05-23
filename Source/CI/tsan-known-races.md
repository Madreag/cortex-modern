# TSan known-races filter list

Companion to the `determinism-tsan-linux` job in `.github/workflows/determinism.yml`.
Lists data-race reports that the TSan gate is allowed to ignore, with the
justification + the change that would remove each entry.

## Policy

- **Every entry needs an explicit justification + a removal path.** "Add to the
  filter list" is never the answer on its own — it has to come with what would
  change to take the entry back off.
- **No accumulation.** If the list grows past a handful of entries, that means
  the SyncedUpdate contract is being papered over and the audit (`Source/CI/path-e-audit-inventory.md`) needs to expand. Filter-list size is a
  signal, not a budget.
- **Reviewer expectation.** Any PR that adds an entry must also point at the
  audit row or open issue tracking its removal. Empty justification = bounce.

## Entry format

Each entry is three labelled blocks:

````markdown
### <short-name>

**Race signature:**
```
WARNING: ThreadSanitizer: data race ...
  <one or two frames from the TSan stack — enough to identify the call site>
```

**Justification:**
One paragraph. Why this race is benign or out-of-scope for the gate today. Cite
the audit row, ADR, or issue that explains the call pattern.

**Removal path:**
What concrete change closes this entry — relocate caller X to SyncedUpdate, land
fix Y, finish audit row Z. One short sentence.
````

## Where the actual suppression list lives

This .md is the human record. The machine-readable list TSan consumes is
written inline by the `Write TSan suppressions` step in
`.github/workflows/determinism.yml` (heredoc → `tsan-suppressions.txt`). Adding
or removing an entry means editing **both** — keep them in lockstep.

## Entries

The v1 set carries forward the Linux bring-up's known-noise classes
(`Source/CI/m5-linux-x64.md:154, 243-258`) so the first CI run reports signal,
not noise. Unknown races still surface — only the listed classes are filtered.

### lj_*  (LuaJIT runtime)

**Race signature:**
```
WARNING: ThreadSanitizer: data race
  #0 lj_BC_*  external/sources/LuaJIT-2.1/src/...
  #1 lj_trace_* ...
```

**Justification:**
LuaJIT emits machine code at runtime; TSan's shadow-memory model cannot track
JIT-generated frames, so reports rooted in `lj_*` are phantom. The Linux
bring-up (`m5-linux-x64.md:154`) handled this by excluding LuaJIT from
instrumentation at the meson level — out of scope for the M4A Block E lane,
which is workflow-only. A symbol-prefix suppression is the equivalent dynamic
analogue.

**Removal path:**
Patch `external/sources/LuaJIT-2.1/src/meson.build` to filter `-fsanitize=thread`
out of the LuaJIT compile flags (the bring-up's approach), or move the engine
to a non-JIT Lua runtime. Either lets us drop the `race:^lj_` line and surface
real Lua-bridge races.

### libtbb (uninstrumented system lib)

**Race signature:**
```
WARNING: ThreadSanitizer: data race
  #0 tbb::detail::r1::*    (in libtbb.so.*)
  #1 std::execution::__par_unseq::* ...
```

**Justification:**
`std::execution::par_unseq` on Linux dispatches through Ubuntu's pre-compiled
`libtbb`, which is not built with `-fsanitize=thread`. TSan cannot see TBB's
internal join synchronisation, so every par-unseq site looks like a race
(`m5-linux-x64.md:243-248`). The sim's own `BS::thread_pool` *is* instrumented,
so real sim-side races still surface — only the TBB-internal noise is filtered.

**Removal path:**
Build TBB from source under `-fsanitize=thread`, or drop `par_unseq` from the
hot paths that need it (the codebase has a few; `_LIBCPP_PSTL_BACKEND_SERIAL` on
macOS already does this).

### AdjacentCost ↔ UpdateNodeCosts  (PathFinder grid)

**Race signature:**
```
WARNING: ThreadSanitizer: data race
  Read:  PathFinder::AdjacentCost  Source/System/PathFinder.cpp:...
  Write: PathFinder::UpdateNodeCosts  Source/System/PathFinder.cpp:...
```

**Justification:**
Persists after the bring-up's PathFinder async-counter fix
(`m5-linux-x64.md:174-176`). The actual synchronisation is the `par_unseq` join
on `UpdateNodeList`, which TSan can't see (same blind spot as `libtbb` above) —
so the post-join read looks racy. Not a real race; covered by the par-unseq
join that TSan is blind to.

**Removal path:**
Same as `libtbb` — drop par_unseq from `UpdateNodeList`, or instrument TBB.
Closing either also closes this entry.

### UpdateDrawMOIDs ↔ MOSRotating::Draw  (cosmetic render race)

**Race signature:**
```
WARNING: ThreadSanitizer: data race
  Write: MovableMan::UpdateDrawMOIDs  Source/Managers/MovableMan.cpp:1899
  Read:  MOSRotating::Draw  Source/Entities/MOSRotating.cpp:...
```

**Justification:**
A real race, but render-side only: the deferred MOID-draw task runs concurrent
with the main-thread `Draw`. The `actors` / `particles` / `scene` checksum
subsystems are fed earlier in the tick, before the task is submitted, so the
race does not perturb the sim hash (`m5-linux-x64.md:251-258`). Filtering it
keeps the TSan gate focused on the determinism island.

**Removal path:**
Serialize `Draw` against `UpdateDrawMOIDs` (one mutex on MOID state), or
acknowledge the render path as out of the determinism scope. Either change
unblocks removing the line.

## Promoting the gate to required

The gate is advisory at landing — the job runs, posts a `::warning::` if races
are detected, but does not fail the workflow (`continue-on-error: true` at the
job level). To promote it to required, drop the `continue-on-error` flag from
`.github/workflows/determinism.yml` and add the job to the GitHub
branch-protection required-status set for the target branches. New filter
entries land in the workflow heredoc + here in lockstep.
