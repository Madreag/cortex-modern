import io

PATH = r"D:\Projects\RESUME.md"
lines = io.open(PATH, encoding="utf-8").read().split("\n")

# 1. drop the transient detached-tree line (and its blank spacer)
i = next(k for k, l in enumerate(lines) if l.startswith("**2026-09-11 05:12 UTC - TRANSIENT STATE"))
assert lines[i + 1] == ""
del lines[i:i + 2]

entry = (
    "**2026-09-11 05:30 UTC - PHASE 1 PROGRESS (Grok workers W1-W7; reports under `reviews/takeover-20260909/grok-workers/`; "
    "every verdict re-derived by the lead).** "
    "MAIN `stage2/p4b-interp-lockstep` = `9751a90e29`, pushed: the tools-only comparator repair (`compare_snapshots.py` accepts "
    "Activity1-3 and maps `actor_links[player][1]`, the controlled actor, not `[0]` the brain, per `Activity.cpp:985-987`; 61/61 "
    "detecting tests, 36 fail on the old file; the retained `mp_snapshot_p5` pair is FAIL on the old comparator and PASS on the "
    "new one; no mask or tolerance added). The approved executable is still `bb3cf264` built from `c8f8188ae0`; `Pin.verify` "
    "requires HEAD == pinned head, so reruns on that executable are done with the approved tree checked out detached at "
    "`c8f8188ae0` and returned to the tip afterwards (done for `s41b4`; tree is back on `9751a90e29`). "
    "WORKER BRANCH `stage2/takeover-next` = `0e2aa3812f`, pushed (backup, not a checkpoint): `60cb698146` + 12 per-concern commits "
    "holding exactly the Mac-verified positive20 working set (78 files; handoff manifest 0 mismatches after the WIP restore; tip tree "
    "identical to the first split attempt, tagged `w1-split-attempt1`; every new compile unit's vcxproj/meson lines in its own "
    "commit; the branch's comparator commit is blob-identical to main's). The `takeover-fixes` working tree now holds only the "
    "~650-line unbuilt journal/observer WIP (`handoff-20260910/post-positive20.patch`) and the two vendor libs. MAC PROVENANCE "
    "CLOSED: the 91 base-file mismatches between the Mac positive20 manifest and Windows are line endings only (LF-normalised, "
    "byte-equal; lead-checked `Actor.cpp`); the Mac `positive`/`control` repos are detached at `316e963757` (274 behind) with 300+ "
    "dirty paths built by layered patches, so future Mac work starts from a clean clone of `origin/stage2/takeover-next`. "
    "SOURCE41 BREADTH TRIAGE (`w2-breadth-triage/LEAD_VERDICTS.md`): no engine regression among the 13 non-pass cases. `fl200` was "
    "an ill-conditioned oracle (integer pin exactly on a tick boundary; the delay formula last changed 2026-07-08; 600-tick identity "
    "and 0 resyncs held). The four long-red H4 gates and the two B1 cases are known residue with several Source40 reds now green; "
    "the host `Rejected a AIOrder command` line is `MovableMan.cpp:415` from `79af0bb826`, present identically on Source40, and "
    "`9b84ddc7df` never touched it (suspicion withdrawn; the rejection itself is to be explained in the H4 work). HARNESS REPAIRS "
    "(`w4-breadth-harness/harness.diff`, not in the repo; `run_breadth.py` and the compat fixture): heal child puts `stage2_p4` on "
    "`sys.path`; the compat_deferral pin excludes ONLY the `spawn_child uid` value (evidence `source33-lanes/report.md:1531-1534, "
    ":1574`: the same executable printed 1049494 then 1049517), line shape, `ok=1`, count and order still exact; "
    "`compat_review_extra.lua:97` `HasAnySounds(true)` (luabind needs the bool; the fixture had been broken on every rung since it "
    "was written, so `shared_soundset_structural` had never run); fake-lag oracle = the engine formula `ceil(rtt/33.33)+1` "
    "(`NetMatchRunner.cpp:80-86`) on the measured RTT, RTT within [2*lag, 2*lag+15], both peers agree; a `--cases` filter. 47/47 "
    "old guards pass, 19 new detecting tests pass and 18 fail on the pre-fix copies. RERUNS `D:/mx/s41b4` on the unchanged "
    "executable (pin verified): fl100 PASS 202 ms -> 8, fl200 PASS 400 ms -> 13, compat_deferral both rungs PASS, compat_extra "
    "both rungs print 19 identical lines / 15 identical cases with no ERROR so the extra pin was re-captured from the Source22 rung "
    "(new line `shared_soundset_structural before=1 removed=0 any=1`; `s41b4-extra2` both PASS), heal: `resync_heal PASS []` but the "
    "wrapper demands a check `prediction_executed` the result lacks - UNDER TRIAGE (producer vs requirement). "
    "OPEN ENGINE RESIDUAL (`w5-audio-checkpoint-residual/REPORT.md`): `peers_3_4_regression` repeat 3 (`s41b3/j40`) - the fresh-"
    "process returner failed to launch from the received 5,409,186-byte snapshot (`AudioMan.cpp:1521` `voice 36 has no registered "
    "owner 9310`) while the running client applied the same bytes cleanly, and the returner's own pre-restore capture already "
    "carried a live voice owned by identity 9124 above its cursor 9067 (`AudioMan.cpp:1419`): a container gets an identity without "
    "registration, and a fresh launch restores differently from a resync. The merged `audio-checkpoint-flake` fix touched neither "
    "check. 1 of 10 gate runs; the snapshot blob was not retained. The two Source41 matrix hits with the same string are the "
    "music-checkpoint selftest's negative control (followed by PASS), not residuals. W7 is tracing the registry through both restore "
    "paths (read-only); next: retention-enabled reproduction, detecting native test, fix on the worker branch. Nothing waived, no "
    "mask widened; every exclusion above carries its evidence and awaits the independent review before the family is called closed.**"
)

j = next(k for k, l in enumerate(lines) if l.startswith("**2026-09-11 04:58 UTC - CHECKPOINT PUSH"))
lines[j:j] = [entry, ""]

# 2. refresh the NEXT ACTIONS state sentence
n = next(k for k, l in enumerate(lines) if l.startswith("**2026-09-11 00:29 UTC - NEXT ACTIONS"))
old = "State: main `c8f8188ae0` (Source41), pushed to origin 2026-09-11 04:58 UTC (see the entry above);"
new = ("State (refreshed 05:30 UTC, see PHASE 1 PROGRESS above): main `9751a90e29` pushed; `stage2/takeover-next` `0e2aa3812f` "
       "pushed with positive20 committed (item 1 DONE); item 2 triaged and repaired, reruns green except heal `prediction_executed` "
       "under triage; item 3 comparator landed, P5/peer_roundtrip engine reruns still to schedule; new item: the audio owner-registry "
       "restore residual (W5/W7);")
assert old in lines[n], lines[n][:200]
lines[n] = lines[n].replace(old, new)

io.open(PATH, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
print("ok: entry at", j + 1, "; next-actions at", n + 1)
