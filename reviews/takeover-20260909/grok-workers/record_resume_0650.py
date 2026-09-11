import io

P = r"D:\Projects\RESUME.md"
L = io.open(P, encoding="utf-8").read().split("\n")

entry = (
    "**2026-09-11 06:50 UTC - LIVE STATE FOR ANY RESUMING AGENT (read this, then `D:/Projects/STATUS.md`, then "
    "`reviews/takeover-20260909/grok-workers/FIX_GROUP_1_DESIGN.md`).** The lead is a Cursor agent (Fable 5.1); every worker is a "
    "Grok 4.6 xhigh-fast subagent (Cursor Task tool on Windows; Cursor CLI in a launchd GUI job on the Mac, see AGENTS.md 'Mac workers'). "
    "Worker reports live under `reviews/takeover-20260909/grok-workers/w<N>-*/REPORT.md`; a resuming agent cannot resume a running "
    "subagent, it reads the reports and the branches instead. "
    "WHAT IS DONE AND PUSHED: main `stage2/p4b-interp-lockstep` = `9751a90e29` (Source41 + the comparator repair); "
    "`stage2/takeover-next` = `0e2aa3812f` (positive20 as 12 per-concern commits; Mac clean-clone build byte-identical to the positive20 "
    "binary, 13/13 selftests, W15); `stage2/takeover-msvc` = `takeover-next` + MSVC porting commits `6fa0fd6898`, `be0a2672a9` (W12 loop "
    "continues there in `D:/Projects/takeover-build`; the Windows build of the group is NOT yet linked: last stop was C2888 in LuaMan.cpp, "
    "fixed; W21 observed two native selftest names failing on a Windows build of the group, `unreferenced_owner_reclaimed` and "
    "`native_runtime_checkpoint_values`, which the Mac passes: a Windows-only regression to pin when W12 links); "
    "`stage2/h4-secondary` = `0520bbc4f7` (W21: five item-6 defects, design B, detecting tests, ten network selftests + controller-frame "
    "PASS; pushed; lead reviewed the diff). Source41 breadth CLOSED (W13 independent review accepted the three oracle changes; notes: "
    "15 ms fake-lag band wider than the measured +0-2 ms, `removed=0` after `AddSound` is a both-rung quirk). "
    "RUNNING WHEN THIS WAS WRITTEN (results land as reports; if a report is missing the work was cut and must be redone from its brief "
    "in the transcript or re-briefed from the design): W12 MSVC build loop (`takeover-build`, `stage2/takeover-msvc`); W18 pause-the-match "
    "(`D:/Projects/hold-pause`, `stage2/hold-pause`, design A); W19 audio owner self-containment + restored-world-only resolution "
    "(`D:/Projects/audio-owner-registry`, `stage2/audio-owner-registry`, design C); W20 AI value maps as settled observations "
    "(`D:/Projects/value-observations`, `stage2/value-observations`, design D); Mac W22 (lane `/Users/erol/cortex-workers/takeover-next-clean`, "
    "`W22_REPORT.md`): build+13 selftests+A7 group of `h4-secondary` on arm64, then compile-check of the ~650-line journal/observer WIP "
    "(`handoff-20260910/post-positive20.patch`, applied in `repo-wip`). All four fix branches are cut from `stage2/takeover-msvc` at "
    "`be0a2672a9`. "
    "DECISIONS TAKEN TODAY: reclaim hold PAUSES THE MATCH (user, section J); no scheduled tasks, watchers or background loops on the PC "
    "(user); firewall: allow rules exist for every Cortex executable found today plus one inbound rule `Cortex harness UDP 43000-47000` "
    "(any program) so no firewall dialog appears for this project; other projects' rules untouched; the listen notification stays ON. "
    "Every task must move a STATUS.md number; a task that lands without moving one is reported as a negative delta. "
    "NEXT, IN ORDER: (1) when W12 links: run the ten network selftests + controller-frame + native suite on the Windows exe; pin the two "
    "native failures (Windows-only) and fix on `takeover-msvc`; (2) as W21/W19/W18/W20 land: lead reads each diff, pushes the branch, "
    "then merges in the order B (`h4-secondary`), C (`audio-owner-registry`), A (`hold-pause`), D (`value-observations`) into "
    "`stage2/takeover-msvc` (resolve conflicts by design intent, re-run the selftests after each merge); (3) Source42 = the full Windows "
    "family on the merged tree (`chain_build_gate.py --label source42-1 --source 42`, `run_remaining_gates.py --source 42`, the 81-case "
    "breadth via `reviews/takeover-20260909/run_breadth.py` with its Source42 pin, the 106-job matrix from `D:/mx/s42`) plus the Mac chain "
    "and A7 group on the merged tree from a clean clone; the H4/B1 gate oracles were rewritten to the pause semantics by W18 (pre-change "
    "copies in `w18-hold-pause/`); (4) when green: merge `stage2/takeover-msvc` into main, push, record here, STATUS items 1/2/5/6 move; "
    "(5) then items 4 (measured 100-200 ms feel), 7 (session UX), 8 (discovery/internet). The journal/observer WIP becomes positive21 "
    "only if W22 part 2 compiles and the selftests pass; otherwise it stays parked as the handoff patch. "
    "Worktrees: see section I (9). Build lanes: at most two `/MP6` workers at once; the lead's own chain builds alone with `/MP12`.**"
)
j = next(k for k, l in enumerate(L) if l.startswith("**2026-09-11 06:32 UTC - MAC VERIFIES"))
L[j:j] = [entry, ""]

# NEXT ACTIONS state sentence refresh
n = next(k for k, l in enumerate(L) if l.startswith("**2026-09-11 00:29 UTC - NEXT ACTIONS"))
old_start = L[n].index("State (refreshed 05:30 UTC")
old_end = L[n].index("Order of work:")
L[n] = L[n][:old_start] + ("State (refreshed 06:50 UTC; the LIVE STATE entry above is authoritative): items 1-3 of this list are DONE, item 4 "
                           "(`AI_StuckForTime`) is being fixed as design D, item 5 (WIP compile check) runs on the Mac as W22 part 2, item 6 "
                           "(Windows candidate) is the W12 loop + Source42, item 7 is the fix group A/B in flight. ") + L[n][old_end:]

# worktree table
w = next(k for k, l in enumerate(L) if l.startswith("## Worktrees ("))
L[w] = "## Worktrees (9 — reconciled 2026-09-11 06:50 UTC)"
row_tb = next(k for k in range(w, w + 14) if "takeover-build" in L[k] and L[k].startswith("|"))
L[row_tb] = "| `D:\\Projects\\takeover-build` | `stage2/takeover-msvc` @ `be0a2672a9`+ (W12 loop) | Lead-owned build tree: MSVC port of the positive20 group; the four fix branches are cut from this branch |"
new_rows = [
    "| `D:\\Projects\\hold-pause` | `stage2/hold-pause` | Lead-owned worker tree: design A, reclaim hold pauses the match (W18) |",
    "| `D:\\Projects\\h4-secondary` | `stage2/h4-secondary` @ `0520bbc4f7` (pushed) | Lead-owned worker tree: design B, five item-6 defects (W21, DONE, reviewed) |",
    "| `D:\\Projects\\value-observations` | `stage2/value-observations` | Lead-owned worker tree: design D, AI value maps as settled observations (W20) |",
]
L[row_tb + 1:row_tb + 1] = new_rows
row_ar = next(k for k in range(w, w + 16) if "audio-owner-registry" in L[k] and L[k].startswith("|"))
L[row_ar] = "| `D:\\Projects\\audio-owner-registry` | `stage2/audio-owner-registry` (cut from `takeover-msvc` `be0a2672a9`) | Lead-owned worker tree: design C, audio owner self-containment (W19) |"

# G Immediate pointer
g = next(k for k, l in enumerate(L) if l.startswith("## Immediate (2026-09-10"))
L[g] = "## Immediate (2026-09-11; the LIVE STATE entry at the top of §B and `D:\\Projects\\STATUS.md` are the live order)"

io.open(P, "w", encoding="utf-8", newline="\n").write("\n".join(L))
print("ok", j + 1, n + 1, w + 1, g + 1)
