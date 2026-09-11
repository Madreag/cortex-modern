import io

PATH = r"D:\Projects\RESUME.md"
L = io.open(PATH, encoding="utf-8").read().split("\n")

# 1. transient line gone (approved tree is back on the tip)
i = next(k for k, l in enumerate(L) if l.startswith("**2026-09-11 05:40 UTC - TRANSIENT STATE"))
assert L[i + 1] == ""
del L[i:i + 2]

# 2. heal closure appended to the PHASE 1 PROGRESS entry
j = next(k for k, l in enumerate(L) if l.startswith("**2026-09-11 05:30 UTC - PHASE 1 PROGRESS"))
old = "heal: `resync_heal PASS []` but the wrapper demands a check `prediction_executed` the result lacks - UNDER TRIAGE (producer vs requirement)."
new = ("heal: the Source41 wrapper had forced the heal child to input delay 0 while Source39/40 ran it at `--input-delay 3` "
       "with prediction active, so the required `prediction_executed` check had no producer; the requirement stays and the child "
       "runs at delay 3 (invariance child stays at its Source40 baseline 0; required-check names equal the Source40 heal result "
       "23/23); rerun `D:/mx/s41b5` PASS with `prediction_executed previews=636`, `resync_heal PASS []`, `heal_mod_semantics` pass. "
       "SOURCE41 BREADTH IS NOW 81/81 ACCOUNTED FOR: 68 original passes + 7 harness-defect cases green on rerun + 6 known "
       "residues (4 H4 gates, 2 B1) + 0 engine regressions; one open intermittent engine residual (audio owner registry, below). "
       "Independent review of the three comparison changes (uid exclusion, fake-lag oracle, extra pin re-capture) still due before the family is called closed.")
assert old in L[j], "heal sentence not found"
L[j] = L[j].replace(old, new)

# 3. worktree table
w = next(k for k, l in enumerate(L) if l.startswith("## Worktrees ("))
L[w] = "## Worktrees (6 — reconciled 2026-09-11)"
row_main = next(k for k in range(w, w + 12) if "p4b-interp-validation" in L[k] and L[k].startswith("|"))
L[row_main] = L[row_main].replace("@ `c8f8188ae0`", "@ `9751a90e29` (pushed; approved exe `bb3cf264` built from `c8f8188ae0`)", 1)
row_tf = next(k for k in range(w, w + 12) if "takeover-fixes" in L[k] and L[k].startswith("|"))
L[row_tf] = L[row_tf].replace("@ `60cb698146` + uncommitted positive20 group",
                              "@ `0e2aa3812f` (positive20 committed and pushed) + uncommitted journal/observer WIP", 1)
new_rows = [
    "| `D:\\Projects\\audio-owner-registry` | `stage2/audio-owner-registry` @ `9751a90e29` | Lead-owned worker tree: audio owner-registry restore residual (W8 instrumentation, cut 2026-09-11) |",
    "| `D:\\Projects\\takeover-build` | detached @ `0e2aa3812f` | Lead-owned build tree: first MSVC build of the positive20 group (W12, cut 2026-09-11); delete after Source42 |",
]
L[row_tf + 1:row_tf + 1] = new_rows

io.open(PATH, "w", encoding="utf-8", newline="\n").write("\n".join(L))
print("ok")
for k in range(w, w + 10):
    print(L[k][:140])
