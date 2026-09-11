# Fix group 1 — items 2 and 6 (design fixed by the lead, 2026-09-11)

All fixes land together on worker branches cut from `stage2/takeover-msvc` (positive20 + MSVC fixes), merge into one tree and are
verified by ONE Windows family (Source42) plus the Mac chain. Every fix carries detecting tests (red before, green after). No mask,
tolerance or exclusion is widened; Lua/native API behaviour is frozen (mod compatibility, AGENTS.md). Evidence base: the W7-W17
reports under `grok-workers/`.

## A. Reclaim hold pauses the match (item 6 core; user decision 2026-09-11 05:55 UTC)

Facts (W14): an unannounced drop calls `ApplyPeerLeave` (`NetLockstep.cpp:4182-4219`), which puts the seat in `m_DroppedSeats`,
makes the leaver not required (`IsRemoteRequiredForFrame :3273`) and immediately `AdvanceReadyFrames` (`:4303-4403`) keeps
committing; the hold is a 1200-frame window (`IsSeatHeldForReclaimAtFrame :4148`) plus the host admission wall-clock
(`NetReconnectHost::Tick :1326`, 20 s); the resync snapshot is taken at the CURRENT tick (`ResyncMatch :196-275`), long after the
drop; `WaitForLockstepControllerFrame` (`ScenarioRunner.cpp:1506-1599`) gives up after `timeoutMs+50` = 20 s.

Contract after the change:
1. While any seat in `m_DroppedSeats` is HELD (from its `leaveFrame` until its resolution), the coordinator commits NO frame:
   `AdvanceReadyFrames` returns without committing while `AnyDroppedSeatHeld()`; `nextFrame` stays at the leaver's
   `firstFrameWithout`. Every peer therefore stops at the same frame. `MissingFrameTimeout` (`:4400-4402`) does not fire while
   paused, and silence adjudication / unreachable-drop of OTHER peers keeps working from transport events and heartbeats: peers
   send a periodic lockstep heartbeat while paused (reuse the keepalive added for host saves in positive20) so
   `AdjudicateSilentPeers` sees them.
2. The hold ends only by a RESOLUTION the host decides and relays on the wire at the held frame, so every peer derives the same
   answer at the same frame (the B1 "told on the wire" pattern): `Reclaimed` (returner admitted: existing rejoin path), `Substituted`
   (moderation commit: existing reseat path), or `Expired` (host admission wall-clock `holdExpired`, `c_ProvisionalExpiryMs`).
   `Expired` marks the seat gone (leaves `m_PeerLeaveFrames` as is, removes the seat from the held set); commits resume with the
   leaver not required, and only then may the scripted outcome complete (`EndActivity` deferral `ActivityMan.cpp:1006-1024` keeps
   working unchanged: `IsLockstepHoldingSeatForReclaim` stays true while paused).
3. Reclaim/substitution resume FROM THE HELD FRAME: because frames stopped, `RequestResync` (`:3095`) stops at `nextFrame ==
   firstFrameWithout` and `ResyncMatch` saves the held state, so the returner relaunches exactly where the match paused. No change
   to the snapshot code is expected; a test pins `stop frame == leaveFrame`.
4. `WaitForLockstepControllerFrame` must not time out while the coordinator reports a held pause: it waits until the pause ends or
   the hold's wall-clock limit plus margin has passed; the stall overlay becomes a pause banner ("Match paused: waiting for <name>
   to return, Ns left" from `NetSeatPresence::Line :331`; `DrawLockstepStallOverlay :137`; F6 `NetModerationGUI::Refresh :122`).
5. Announced (clean) leaves are unchanged: no `m_DroppedSeats`, no pause, seat closed immediately (`TestAnnouncedLeaveHoldsNothing`).
6. A second unannounced drop during a pause adds its own held seat; the pause lasts until every held seat is resolved.
7. `c_ReclaimHoldFrames` (1200) no longer measures the hold (frames do not advance); the hold is the admission wall-clock. Keep the
   constant only where a frame-based UI estimate is still shown, or retire it with its tests rewritten (not deleted).

Detecting tests (coordinator selftests, red today): unannounced drop → no commit while held; `Expired` resolution → commits resume
with the seat not required; `Reclaimed` → resync stop frame == leaveFrame; heartbeats during pause keep the other peers un-adjudicated;
`WaitForLockstepControllerFrame` does not give up during a pause shorter than the hold limit; announced leave still closes at once.
Rewrite (never delete) the old-semantics tests W14 lists: `TestCoordinatorDroppedSeatHold`, `TestCoordinatorAdjudicatedPeerKeepsItsSeat`,
`TestCoordinatorHeldSeatKeepsPlaying`, `TestActivityGateAgreesAcrossPeers`, B1 `TestCoordinatorHeldSeatWithASurvivor`. Gate oracles
(`h4gates/*.py`, `tools/h4_substitution_gates.py`, A7 driver): after a drop `running_ticks` is frozen until the resolution, the
resume frame equals the drop frame, and the match CONTINUES after reclaim/substitution (no Over); their 900/1200-tick budgets become
wall-clock budgets that include the pause.

## B. Item 6 secondary defects (W10/W11), same branch group, own branch
1. `UsedStoredTicket` stays true after a refused reclaim falls back to a new join (`NetReconnectSession.cpp:1340-1373`): report the
   outcome distinctly (`reclaim_accepted` vs `new_join_after_refusal`); the gate `clean_old_ticket_refused` reads the outcome, not the
   sticky flag.
2. `ReportRuntimeError` (`NetMatchService.cpp:503-518`) tears down runner/session before the report is built (`:928-930`), so admission
   counters read `None`: the report keeps a copy of the admission and reconnect counters taken before teardown.
3. A resync requested against an `Over` activity (`ActivityMan.cpp:140-142` refusal) fails the whole session: the host answers the
   rejoin with a "match over" outcome (the joiner sees the result screen) and the session survives.
4. Sender-side authority filter: a peer never sends a `NetGameAIOrder`/economy command for a team it does not control (mirror
   `IsLockstepTeamCommandSender` at the send site, `Actor.cpp:1478` PopWaypoint path); the receiver's rejection stays.
5. The e2e rule `Over && running_ticks < 100` (`Main.cpp:2935-2938`) counts from the match start, not from a relaunch; with A this
   no longer trips on a hold, but the rule was wrong in principle.

## C. Audio owner registry (item 2; W5/W7/W8; exact lines from W16 when it lands)
Facts: the save carries every MO-owned voice owner in 1662 retained saves (W8); the failing owner 9310 (`Metal Impact Machinery`,
a wound emitter's `BurstSound`) was alive when the voice table was captured (`ActivityMan.cpp:161`) but not part of the written world
(`RetrieveSceneObjects(false)` `:219-225`); on running peers the apply "succeeds" only because `FindCheckpointSoundContainer` (`AudioMan.cpp:1069`)
finds the leftover pre-restore object kept alive by `SetAsideWorld` (`MovableMan.cpp:1411-1428`), binding a restored voice to a stale
object.
W16 (`w16-unsaved-owners/REPORT.md`): `RetrieveSceneObjects(false)` pulls the settled AND added actor/item/particle lists, does not
skip to-delete objects, and nested wounds on a written root carry their `BurstSound` identity; so the owners that are alive at line
161 but not written are the transient orphans: a wound popped or an attachable removed with `RemoveAttachable(..., false)` and not yet
deleted, a parent erased from its list before `delete`, a wound leaked by `AddWoundExt` onto a to-delete parent, off-list orphans,
Lua `copy`/`preset` SoundContainers or Lua-owned MOs (saved by name only, no identity), and roots with an empty/`None` preset whose
nested wounds are skipped. `BurstSound` starts on the emitter's first `Update`, not at `AddWound`. The host resync save runs after
`MovableMan::Update`, which can return early (`:3488`) before the absorb/delete pass.

Design, three parts, all in `AudioMan`/`SoundContainer`/`ActivityMan` (no Lua or API change):
1. Save self-containment. `SaveCurrentGame` gains a save scope in which `SoundContainer::Save` (the `SpecialBehaviour_SoundCheckpoint`
   write under `Writer::IsSnapshot()`, `SoundContainer.cpp:~274`) records every identity it writes into a "carried" set. The Lua GC step
   that `CaptureRuntimeGlobals` performs today runs first (unchanged semantics: a dropped Lua owner is not demanded); then the script
   graphs (`:235-248`) and the borrowed scene (`:219-251`) are serialised into buffers, collecting identities; then `CaptureRuntimeGlobals`
   runs with a containment predicate handed to `AudioMan::SaveCheckpoint`: a voice whose owner identity is neither carried by the world
   nor owned by GUISound/MusicMan (their checkpoints travel in the same RuntimeGlobals) is written with owner 0, exactly the state the
   voice would have after `DisownSoundContainerPlayingChannels`. The buffered texts are then emitted in the original property order, so
   the `.ccsave` layout is unchanged. A `[audio-checkpoint]` line names each disowned voice's preset once per save (diagnostic).
2. Apply resolves owners against the restored world only. During the snapshot `RestartActivity` path, `AudioMan` keeps a "restored
   registry view": the identities registered by the staged read (`m_PendingCheckpoint.soundRegistrations` /
   `ActivateCheckpointSoundRegistrations` `:1096`), the Start clones (`FaithfulCloneScope(true)`), `RestoreScriptGraphs`, and the GUI/Music
   apply. `AudioMan::LoadCheckpoint` apply (`:1519-1521`) resolves `voice.owner` in that view and never in leftover pre-restore
   registrations (`SetAsideWorld`'s kept objects), so a fresh process and a resyncing process apply identically; the refusal text names
   the owner's identity and preset. `FindCheckpointSoundContainer` keeps its current semantics for every other caller.
3. Invariant check in selftests: after each `SaveCurrentGame` the selftests run, every nonzero voice owner in the AudioRuntime text must be
   in the carried set (the checker W8 wrote offline, `check_save.py`, is the model; implement natively in the audio checkpoint selftest).

Detecting tests (native `[audio-checkpoint-selftest]`, red on `be0a2672a9`): (a) an actor with an updated wound (burst voice playing),
wound removed with `RemoveAttachable(..., false)` and kept alive → save → that voice's archived owner is 0 and the apply succeeds on a
clean registry; (b) a Lua-owned playing container saved as `copy` → owner 0 in the archive; (c) apply of one snapshot in a process with
leftover registrations versus a clean registry → identical outcome (today the leftover case binds the stale owner and the clean case
throws); (d) a voice whose owner IS written keeps its owner and resolves to the restored clone, not the leftover object (pointer
identity asserted); the existing staging/Activate contract tests keep passing.

## D. MO value maps written by per-machine AI (item 2; W9; template from W17 when it lands)
Facts: `SharedBehaviors.lua:547` sets `AI_StuckForTime` from `ThreadedUpdateAI` on the owning peer only; the map is one shared
`m_NumberValueMap` (`MovableObject.cpp:1465`) archived whole and read by shared code (`Controller.lua:1135-1148`); no local-AI
copy-on-write exists for values (`:1139-1141` covers sounds only).
Design principle (the sound solution, instantiated for Number/String/Object values): a write made inside the local-AI scope lands
on a per-machine control copy visible to that AI's own reads immediately; the controlling peer emits a value observation that every
peer (including the writer) applies to the shared map at one settled frame, so shared readers see identical values everywhere and
snapshots/hashes never carry per-machine state; unowned/activity-wide objects take the host's observation (settled authority policy).
Mod-visible behaviour preserved: read-after-write in the same AI script, cross-script visibility within the tick on the writer,
persistence across saves, removal semantics; only the multiplayer landing frame on non-writers changes (input-delay bounded), as
already accepted for sounds. Exact pieces (scope detection, store, record, transport, settle frame, checkpoint of pending
observations, authority, selftests) are copied from W17's template list.

## Branches, workers, build lanes
- `stage2/hold-pause` (A) — Windows worker, cut from `stage2/takeover-msvc`.
- `stage2/h4-secondary` (B) — Windows worker, cut from `stage2/takeover-msvc`.
- `stage2/audio-owner-registry` (C) — Windows worker, existing worktree `D:\Projects\audio-owner-registry` (rebase onto `takeover-msvc` first).
- `stage2/value-observations` (D) — Mac worker on the clean clone (GCC), Windows verification in Source42.
At most two Windows build lanes at once (`CL=/MP6`); a worker checks `Get-Process MSBuild` before building and waits. Each branch:
per-concern commits, plain messages, no attribution, detecting tests in the same commit as the fix, no push (the lead pushes after
reading the diff). Merge order into `takeover-msvc`: B, C, A, D; then Source42.
