# Restoration and multiplayer contract audit

Investigation started 2026-09-07 16:50 UTC after the user required a complete contract investigation before further implementation. Engine source is frozen at the combined working state recorded by `manifest.json`: HEAD `d227a1a85`, executable SHA-256 `ef82285add68712534c6eb3165a010863e25752ed4f2901738df261c4ff03127`. The source patch, changed files, executable and symbols are retained here. Test-only instrumentation may form a separately identified audit build. No milestone is complete.

## Evidence rules

1. A copy-constructor member mention, writer/property-name match or passing fixture is an investigation lead. None establishes an untested class, owner, field or transition.
2. Invoke the production entry point without extra repairs from the harness. Compare independent observed state before and after the call, in addition to comparing serialized output.
3. Preserve complete per-peer state for restoration and prediction-isolation checks. For cross-peer equality, classify only the explicit Controller-sync boundary and proven local presentation state; shared gameplay, all other Lua values and aliases remain strict.
4. A negative must reach the intended operation with a valid initial state. A timeout, missing fixture or invalid reference is an invalid test, not proof of the intended defect.
5. Run the complete declared matrix on an unchanged identified build after grouped fixes. Keep prior failures and results attached to their original builds.

## Transition matrix

| ID | Production transition | Required observations | Existing coverage / gap |
|---|---|---|---|
| T01 | Create bare and preset native objects | Type, defaults, field mutation, ownership, alias identity | Lua-owned AHuman/HDFirearm fixture; remaining native types and ownership combinations open |
| T02 | Faithful native clone | Every mutable field; no accidental source mutation; owned tree independence; borrowed links | 898 members mentioned/classified by the old inventory; conditional copies, field values and file coverage are not proved |
| T03 | CaptureWorld / RestoreWorld | Residents, pending adds, alarms, order, aliases, clocks/RNG and native/Lua roots at each legal boundary | API currently assumes add queues drained; late globals can create valid pending objects at the capture boundary |
| T04 | SetAsideWorld / ReinstateWorld | Original identities and full state, pending work/lifetimes and queues; failure leaves originals usable | Broader cohort than WorldSnapshot; complete failure contract needs independent checks |
| T05 | SaveCurrentGame success | OnSave runs once; coherent post-callback scene/images/objects/Lua; complete global state; no additional mutation | Callback/image tests exist; whole-save omission checks are missing |
| T06 | SaveCurrentGame failure | Old file remains valid; failed async result reported; world reflects only intentional callback effects | I/O controls exist; full world and callback-state comparison needs audit |
| T07 | LoadGameToRestart staging | Valid input stages complete candidate; rejected input preserves live world, pending candidate and caches | 16 archive controls; late native/VM construction failures are not fully covered |
| T08 | RestartActivity snapshot success | Complete runtime is ready when the call returns; no probe repair; resume callbacks do not repeat startup | Probe previously repaired RNG, queues, controllers and hit layers after the call; ordinary loading therefore had weaker coverage |
| T09 | LoadAndLaunchGame | Same completion contract as T08 through the real ordinary-load entry | Fresh-process round-trip now exists; serialized-byte equality cannot detect fields absent from both saves |
| T10 | Any staged/late load failure | Atomic preservation of the prior live world, activity, VM graphs, allocator, pending work and usable continuation | Restart currently purges the old world before graph restoration; failure-family controls required |
| T11 | Desync / requested resync | Completed authoritative tick; full snapshot transferred; both peers restore correct state and continue | Real two-peer hash/global/orbit lanes; independent comparison with host state at capture is missing |
| T12 | Join / drop / rejoin | Admission isolation, coherent roster and ownership, no stale queues or commands | Menu rejection/replacement tests exist; authenticated H4 transaction is unfinished |
| T13 | Prediction / speculation | Every canonical value and original alias unchanged; speculative side effects isolated | Two 8/8 suites with Lua mutation control; queue counts omit queue contents, additional native families open |
| T14 | Pause / end / rematch / quit | Correct event counts, complete final tick, clean transition and lifetimes | Native completion controls, real stall/rematch and menu suites; larger multi-peer matrix open |
| T15 | Replay and cross-platform continuation | Same shared simulation per tick, metadata enforced, native and Lua RNG portable | Current Windows gameplay matrix running; latest complete Windows/Linux/macOS matrix not run |

## State-family matrix

| Family | Explicit state / ownership dimensions | Required independent oracle |
|---|---|---|
| Simulation globals | Engine RNG (seed/state/draw count), per-VM RNG, deterministic timestep, tick/time; manager options mutable by scripts | Direct generator checkpoints and manager values, not only save contents |
| World structure | Ordered live actors/items/particles, pending queues, delete/settle flags, alarms and join quarantine | Ordered UID lists plus complete pending object values and event payloads |
| Native values | Every mutable member of movable types, attachments, equipment, craft, doors, atoms, paths, timers, sounds, menus, scene/activity | Generated member/API inventory tied to direct value observations and typed mutation cases |
| Ownership and links | Resident, pending, Lua-owned, nested owned, borrowed, hidden owner, preset, renamed/bare, deleted/replaced owner | Identity/alias graph; lifetime and replacement controls; no pointer-address-only comparisons |
| Lua graph | Tables/metatables/global environment, loaded modules, closures/upvalue sharing, coroutine stacks, native and Lua iterators | Full graph plus independent behavior/alias assertions; non-AI global sharing must not be masked in peer comparisons |
| Script lifecycle | Start/update/late/pause/orbit/end/destroy/save, active and inactive globals, library/script caches, native callbacks | Exactly-once event ledger with mutations before and after each transition |
| Async work | Queued/running/completed/unpublished/published path work, callback IDs/order, world lifetime and cancellation | Controlled barriers at each phase and retained callback/event ledger |
| Input and transport | Committed frames, delay window, pending actor commands, Controller sim-facing fields vs local sampling/seat state | Per-peer wire frames and accepted/applied indices; future command/event continuation |
| Terrain and scene | Full material indices/images, modified areas/boxes, caches that affect collision/pathing, object masks | Raw layers and independently observed collision/path-query behavior |
| Activity and UI | Team funds/techs, player bindings, delivery and win state, enabled scripts, local presentation | Direct activity state plus actual handlers/screens; local fields still included in same-peer restoration |

## Findings collected so far

| ID | Finding | Evidence / classification |
|---|---|---|
| F01 | Probe-specific repair masked production load completion requirements | Source/Main.cpp post-load fidelity branch restores engine RNG, rewinds time, absorbs added MOs, clears quarantine, reapplies controllers and redraws hit layers |
| F02 | Full archive round-trip changes empty strings and unresolved native/controller state on the pre-audit build | `../snapshot-roundtrip-before-1`, both valid peer inputs fail; frozen audit build carries the already-in-progress repair and `roundtrip-existing-snapshots` passes both full archives |
| F03 | Late-update boundary is not covered by the normal global fixture | `late-restoration/20260907_165252_508619af`: valid reference; memory/file/ordinary capture 60 fail, all 70/100 controls pass. All source remains frozen; no repair attempted during this audit |
| F04 | Engine RNG is restored by the probe but absent from the file writer/reader | Source inspection of ActivityMan SaveCurrentGame/ReadSavedGame and Main fidelity branch. Per-VM RNG is separate. Needs independent runtime omission control; do not yet label a measured runtime result |
| F05 | WorldSnapshot, WorldSetAside and file capture cover different queue/event cohorts | WorldSnapshot only stores resident lists; WorldSetAside includes pending lists and alarms; file capture combines lists. Direct queue-content controls pending |
| F06 | Late graph failure occurs after destructive world restart | ActivityMan::RestartActivity purges before RestoreScriptGraphs; failure rollback controls and complete transaction ownership audit pending |
| F07 | Native test coverage is narrower than the API/state inventory | 1,540 APIs / 70 classes / 381 writable properties; named fixture exercises 100 writable properties on two principal Lua-owned classes. 909 methods remain unreviewed for mutations/aliases; old 898-member clone inventory is not a runtime gate |
| F08 | Cross-peer raw snapshot equality conflates local and shared state | Previous saves: four threaded Lua graphs identical; master graph differences below actor AI; native visual selections, real clock anchors and AI caches differ. Full restoration and strict shared-state projection must both be proved |

## Execution ledger

- Broad existing gameplay suite: `D:/Projects/stage2_p4/recovery_runs/20260907_165020_6fd61b52`, complete, 19/20 on the frozen build; raw peer snapshot comparison remains red.
- Existing peer archives loaded into fresh processes: `roundtrip-existing-snapshots`, 2/2 full serialized round-trips pass. This cannot prove preservation of fields the writer omits.
- Late global restoration: `late-restoration/20260907_165252_508619af`, complete, 7/10 including reference; three capture-60 failures retained.
- API inventory: `native-api-inventory/inventory.json`; copied historical fixture evidence is explicitly owner/transition limited.
- Clone inventory: `clone-inventory.txt`; 898 / 0 unclassified is an inventory result only.

Next investigation work: map all remaining test-path repairs; expand direct state observations beyond the serializer; exercise failure stages, queue contents and representative native ownership/type families; collect the complete failure set before grouped implementation.

## Direct observation checkpoint — 17:52 UTC

Engine behavior remains frozen. `generate_observer.py` adds read-only friend access and a generated `Source/System/ContractAudit.h`; `Source/Main.cpp` has an isolated diagnostic entry for observe/save/stage/file/memory/hold/preview/load transitions. The original headers are retained in `observer-originals`. This is test instrumentation, not a production repair. The latest audit build is `build-observer-7.log`, executable `f33ede8ac5b16e186e56d33b175e4ee76cc457760deb012bf9b0823e286261aa`.

The syntax inventory includes 237 original headers (subsequent runs also see the generated observer), roughly 4,700 declarations, and 31 explicit parse gaps. The direct observer covers 1,176 declarations in 62 selected native classes, with opaque types recorded per observation. These are discovery counts, not complete runtime coverage. `Actor::m_pHitBody` is never assigned or read anywhere in production and is deliberately not read by the observer; the original inventory's unused classification was verified against source. The first diagnostic controls failed when traversing stale registry entries and that uninitialized member, and do not constitute fidelity tests.

`direct-baseline-2` completes all seven observations on `8cbb698d...`: observe/save/preview have zero changes among 4,138,298 recorded values. File has 3,879 raw differences, memory 629, hold 1, and stage 142,018. These raw counts include bookkeeping, candidate objects and clocks; they are not bug counts. Unordered maps are observed by key, preserving values and aliases without comparing insertion order. The one hold difference is the cleared MOID index. The file result directly shows residents returned as pending adds, a cleared hit index and engine RNG not restored after a deliberate 73-draw perturbation. The newest observer adds raw float bits, real-clock context, direct terrain layer hashes and separate identity records to classify the remaining differences correctly.

**F09 — registry lifetime/identity:** before any save/load, 6,037 of 9,166 registry entries have keys different from the object's current unique ID (some now zero). Exact mismatches are retained in every `*.registry.txt`. The observer reports these and avoids reading the invalid entries' fields; it does not repair the registry. The first-fault dump and matching symbols are retained under `direct-observer-debug-2` / `observer-caf2d53e`.

**F10 — nullable native getter:** `native-fixture-control-2` crashes reproducibly in `Turret::GetFirstMountedDevice`, which indexes element zero of an empty vector. Both isolated processes fail. The dump/AbortLog localizes the getter. The combined property sweep explicitly retains this failure as a gap so the other types can run; it is not counted as passing coverage. `CC_CONTRACT_EMPTY_TURRET_CONTROL=1` re-enables it, optionally with `CC_CONTRACT_NATIVE_CLASS=Turret`.

`native-fixture-control-4` is a valid reference on `f33ede8a...`: ticks 5 and 50 both finish cleanly with zero observer differences, no console errors, 24 constructed native classes, 1,343 distinct inherited property cases, and 209 explicit gaps. The count includes the same inherited property on different owners and must not be presented as 1,343 unique API properties. Prior controls 1/3 had fixture errors (Lua userdata equality, then reassigning empty script paths); they remain invalid references. `generate_native_fixture.py` produces the inventory-driven fixture. External fixture copies and hashes are now retained in each run, along with complete source provenance; large state/gap files are losslessly compressed and verified before removing their uncompressed copy.

`direct-native-1` completed all six production transitions at tick 50 on this valid fixture and unchanged audit build. No native or checkpoint production repair has been applied during this audit.

## Direct transition and failure-stage results — 18:20 UTC

All runs in this section retained unchanged source and input hashes. `direct-native-1`,
`direct-late-1`, `direct-failures-1`, and `direct-references-1` use `f33ede8a...`;
the executable and matching PDB are preserved in `observer-f33ede8a`. Raw difference
counts are never a pass/fail oracle by themselves. `classify_observations.py` retains
unfiltered grouping, object identities and logical Lua-graph differences per case.

- `direct-native-1`: save and preview preserve all recorded values and Lua graphs; hold
  clears the MOID lookup. Memory has 49,729 raw differences and file has 53,299, of which
  47,512 / 47,904 are added registry-object fields. Existing mutable native values also
  revert: craft orbit/scuttle and dropship configuration, crab aim limits, arm configuration,
  emitter limits, throw configuration and turret rotation. Several of those fields are
  absent from both before/after INI payloads, demonstrating why byte round-trips missed them.
  `SaveSnapshotConfiguration` is implemented on only a subset of derived types;
  ordinary per-class `Save` support does not establish the snapshot path.
- The 1,343 distinct mutation cases map to **222 unique declared writable APIs**, with
  **159 not exercised by this sweep**. Exact mappings are in `native-mutation-coverage.json`.
  Earlier dedicated fixtures cover some of these remaining APIs; no combined completeness
  claim is made. Field-read and method/ownership contracts remain distinct dimensions.
- `direct-late-1`: all three observe controls at 60/70/100 have zero deltas. Memory capture
  refuses the valid pending spawn at 60 without changing the world. At 70/100 it captures
  and restores. All three ordinary file calls report success but return residents as pending
  additions and omit global state. All three set-aside/reinstate cases clear the MOID index.
  The logical Lua graph is equal in the successful memory/file cases: graph equality alone
  does not prove native/world completeness.
- `direct-failures-1`: a valid full archive saved at 70 loads through the ordinary restart
  path at 100. Nine controls return failure for graph/native/closure/coroutine/iterator/RNG
  problems, but the old simulation tick is replaced (100 becomes 70), along with the activity,
  scene and world. A bad final-VM header also leaves dangling Area/Box references: the next
  production graph serialization crashes in `ScriptGraphSceneBoxOwner` at LuaMan.cpp:2515.
  Its AbortCode and symbolized stack are retained. A truncated graph is accepted. The
  appended-unknown-native-property control is also accepted; its rejection expectation is
  not yet established because the reader supports unknown-property tolerance. An invalid
  open-upvalue control was unavailable in that saved state and is explicitly unexecuted.
- `direct-references-1`: 14 cases construct valid native references. Ten refuse saving:
  a Lua-owned Controller, the activity's player Controller, all three exposed activity
  Timers, BuyMenuGUI, SceneEditorGUI, red/yellow GUIBanner, and a scene SLBackground.
  The background iterator, a material, a bare GameActivity, and an owned SoundSet save
  without errors; restoration of these cases is still required. Six attempted constructors
  are not exposed in Lua and are invalid capture controls, not checkpoint failures.

### Additional failure families

| ID | Finding | Evidence / boundary |
|---|---|---|
| F11 | Snapshot configuration omits whole derived-class field groups | Direct native before/after values and absent graph INI properties; shared by ordinary file and Lua-owned reconstruction in memory restore |
| F12 | Native reconstruction clears a populated preset clone without releasing owned storage | `Graph.prepare` calls `Create<Class>(preset)`; `ScriptGraphNativeLoad` then calls `Reset`, whose derived `Clear` methods discard pointers/lists. Added valid registry objects are consistent with this lifetime defect. Requires direct ownership/lifetime validation in the repair gate |
| F13 | Valid retained native references are unsupported | Ten independently constructed references in `direct-references-1` refuse saving; no restriction on mods is authorized |
| F14 | Graph parsing does not validate complete input | `truncated_graph` reports successful ordinary load; the parser advances over tags and substrings without complete bounds/end checks |

`build-observer-8.log` / executable `65c51ea60dfa281d4c4449bd4b0b41b339ba5334b8d2659793c30a383926b8ef`
adds an optional diagnostic-only `_ContractAuditCheck` callback and prints graph problems.
The first native-check batch did not export its callback from the script's private environment;
its direct observations remain valid but it is **not** a getter-check gate. The first activity
fixture used a missing Lua overload for ReportDeath and is an invalid reference. Both test
issues were corrected in external fixtures; the second controls are running. These are test
instrumentation changes only. No production repair has been made during the audit.

### Grouped repair scope derived from the audit

1. Complete the snapshot state contract: derived native configuration/state, activity and
   owner state, engine RNG, ordered resident/pending cohorts, alarms and lookup readiness.
   Move completion responsibilities into production entry points and remove probe-only repairs.
2. Make staging and restart a transaction: candidate construction must not leak canonical
   registry/allocator/callback changes; any failed phase must restore the original scene,
   activity, Lua objects/aliases, async work and usable continuation.
3. Repair identity and native lifetime together: all UID transitions, preset reconstruction,
   abandoned candidates, held worlds and borrowed aliases must obey one ownership contract.
4. Complete native reference encoding and strict graph validation across all VMs, including
   activity/UI/background owners and exact alias relationships.
5. Gate full per-peer restoration separately from strict shared simulation equality, then
   run the declared restoration, failure, prediction and normal multiplayer matrix on one
   unchanged build. Fresh H4/cross-platform work and the full roadmap remain open.


18:26 UTC: `native-check-control-2` is valid: before/capture/save checks pass all 1,465 getter values on 24 native owners; memory and file restore each fail exactly 62 checks. Full mismatch lists are retained in each console/result. `activity-check-control-2` has no errors, zero direct deltas and 28/28 getter checks before/after. `direct-activity-1` is running its six transitions. The observer-8 executable and symbols are retained in `observer-65c51ea6`.

## Collected repair baseline — 18:39 UTC

Collection through the declared transition families is retained on unchanged audit builds.
Grouped implementation now follows the five repair groups above. This is not a claim that
every API method, platform or possible mod is covered. The full API/member inventories and
explicit gaps remain part of the repair and regression work, not implicit passing coverage.

- `direct-activity-1`: six transitions finish cleanly. Save/stage/hold/preview and the immediate
  memory world swap preserve all 28 checked values. Ordinary file restoration changes 17:
  activity timers, landing zones, game-over period, team deaths/skill, buy-menu constraints and
  owned items, and banner kerning. The memory API only captures the movable world; an immediate
  comparison does not prove that it captures the activity or manager state.
- `direct-fresh-activity-1`: a valid saved fixture is loaded with untouched initial manager
  defaults through the checked-in `run_audit.py`. It completes cleanly with **19/28 mismatches**:
  the same 17 plus MaxDroppedItems and particle settling. These two globals were inherited
  from the prior live state in the immediate-load case. No harness repair is applied.
- `typed-native-control-1`: the valid reference checks 1,523 values on 24 native classes with
  zero changes/errors. Nullable members now receive typed values; cloned sounds and other
  entities get distinct names. Both memory and file restoration report the same **151
  mismatches**, exposing additional borrowed targets and nested native identity loss within
  F11/F13. All targets and aliases stay strongly rooted. Earlier 62-mismatch results remain
  attached to their narrower fixture.
- `direct-reference-restore-1`: all eight memory/file cases for the four saveable variants
  pass their independent alias/value checks. The background iterator returns all five layers
  after restore. This does not change the ten unsupported-reference failures from
  `direct-references-1`.

**F15 — activity, UI and manager state:** ordinary save/start is configuration-oriented.
Activity::Start resets controllers, team deaths and player state; GameActivity::Start resets
its timers/UI/landing state, while their Save methods omit those live fields. The memory
probe's separate Activity::RollbackState carries only seven field groups. Use a complete
production checkpoint composition and apply runtime state after initialization. All exposed
manager options must be carried for fresh-process restoration and speculation isolation.

The native sweep also identifies a distinct API defect: ACDropShip::SetLateralControlSpeed
writes m_LateralControl, while its getter reads m_LateralControlSpeed. Its no-distinct-value
gap remains explicit; fix and gate the getter/setter contract with the grouped native work.

The local diagnostic commit is split from the built/run combined source, not independently
built. Production repair results must identify a new executable, and the complete collected
matrix must pass that unchanged build before any restoration or multiplayer sign-off.
