# TSan known-races filter list

Companion to the `determinism-tsan-linux` job in `.github/workflows/determinism.yml`.
Lists data-race reports the TSan gate is allowed to ignore, with the justification
and the change that would remove each entry.

## Policy

- **Every entry needs a justification + a removal path.** "Add to the filter list" is
  never the answer on its own — it comes with what would change to take the entry back off.
- **No accumulation.** A list that grows past a handful of entries means the SyncedUpdate
  contract is being papered over. Filter-list size is a signal, not a budget.
- **Reviewer expectation.** Any PR adding an entry points at the call pattern it covers.
  Empty justification = bounce.

## Where the actual suppression list lives

This .md is the human record. The machine-readable list TSan consumes is written inline by
the `Write TSan suppressions` step in `.github/workflows/determinism.yml` (heredoc →
`tsan-suppressions.txt`). Adding or removing an entry means editing **both** — keep them in
step.

## Entries

The set carries the threaded-sim's known-noise classes so the first CI run reports signal,
not noise. Unknown races still surface — only the listed classes are filtered.

### lj_*  (LuaJIT runtime)

LuaJIT emits machine code at runtime; TSan's shadow-memory model can't track JIT-generated
frames, so reports rooted in `lj_*` are phantom.

**Removal path:** filter `-fsanitize=thread` out of the LuaJIT subproject's compile flags, or
move to a non-JIT Lua runtime. Either lets us drop `race:^lj_` and surface real Lua-bridge races.

### libtbb.so / libtbbmalloc.so  (uninstrumented system libs)

`std::execution::par_unseq` dispatches through the distro's pre-compiled `libtbb`, which is not
built under `-fsanitize=thread`. TSan can't see TBB's internal join sync, so every par-unseq site
looks racy. The sim's own thread pool *is* instrumented, so real sim-side races still surface.
The patterns name each `.so` explicitly — a `called_from_lib` pattern that matches more than one
loaded library is fatal to TSan (a bare `libtbb` matches `libtbbmalloc.so` too and kills the run).
Because the racy frame of a par-unseq report usually lands in the dispatch templates compiled into
our own binary, the set also suppresses `tbb::detail::d1::start_for` and
`__pstl::__internal::__brick_walk1` — the price is that a real race whose frames inline fully into
a par_unseq body is masked too, which the removal path below fixes properly.

**Removal path:** build TBB from source under `-fsanitize=thread`, or drop `par_unseq` from the
hot paths that use it (`SLTerrain`, `PathFinder`, `ActivityMan`).

### AdjacentCost ↔ UpdateNodeCosts  (PathFinder grid)

The actual synchronisation is the `par_unseq` join on the node-cost update, which TSan can't see
(same blind spot as `libtbb`), so the post-join read of `AdjacentCost` looks racy. Not a real race.

**Removal path:** same as `libtbb` — drop par_unseq from the node-cost update, or instrument TBB.

### UpdateDrawMOIDs ↔ MOSRotating::Draw  (render-side MOID race)

A real race, but render-side only: the deferred MOID-draw task runs concurrent with the main-thread
`Draw`. The `actors`/`particles`/`scene` checksum subsystems are fed earlier in the tick, before the
task is submitted, so the race does not perturb the sim hash. This is the pre-existing async
MOID-rebuild/draw race, tracked for its own fix.

**Removal path:** join the async MOID rebuild before the draw/AI passes, or serialize
`Draw` against `UpdateDrawMOIDs`. Either unblocks removing these lines.

## Promoting the gate to required

The gate is advisory at landing — the job runs, posts a `::warning::` on any race, but does not
fail the workflow (`continue-on-error: true` at job level) and the summarise step does not exit
non-zero on race count. To promote:

1. Set `TSAN_REQUIRED: '1'` in the `env:` block of `determinism-tsan-linux`. The summarise step
   then `exit 1`s on any race count > 0.
2. Drop `continue-on-error: true` from the same job so step failures fail the job.
3. Add the job to the branch-protection required set (repo-admin change, not a workflow edit).

Step 1 alone is the soak hatch — flipping just `TSAN_REQUIRED=1` upgrades the step's failure
semantics while leaving the job advisory at the workflow level. New filter entries land in the
workflow heredoc + here, in step.
