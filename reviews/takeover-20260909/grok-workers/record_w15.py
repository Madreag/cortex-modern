import io

S = r"D:\Projects\STATUS.md"
s = io.open(S, encoding="utf-8").read()
start = s.index("| 1 | Controller boundary")
end = s.index("\n", start)
new_row = ("| 1 | Controller boundary + wire/replay compat | 93 | +3 | Mac leg DONE (W15): clean clone of `0e2aa3812f` builds a "
           "binary byte-identical to positive20 (`238066fa01d9`), 13/13 selftests PASS. MSVC build loop of the group still running (W12) "
           "| Windows family Source42 after the fix group merges |")
s = s[:start] + new_row + s[end:]
s = s.replace("Last update: 2026-09-11 06:20 UTC · Overall **55%** (Δ 0 since the 05:37 UTC ask; item 5 +3)",
              "Last update: 2026-09-11 06:32 UTC · Overall **56%** (item 1 +3: Mac verification of the committed group)", 1)
s = s.rstrip("\n") + "\n- 2026-09-11 06:32 UTC · overall 56 · 1:93 · W15 landed: Mac clean-clone build byte-identical to positive20, 13/13 selftests\n"
io.open(S, "w", encoding="utf-8", newline="\n").write(s)

P = r"D:\Projects\RESUME.md"
L = io.open(P, encoding="utf-8").read().split("\n")
j = next(k for k, l in enumerate(L) if l.startswith("**2026-09-11 05:58 UTC - ITEM 6 ROOT CAUSE"))
entry = ("**2026-09-11 06:32 UTC - MAC VERIFIES THE COMMITTED GROUP FROM A CLEAN CLONE (W15, Cursor CLI Grok on the Mac).** "
         "`git clone --branch stage2/takeover-next` -> HEAD `0e2aa3812f`, clean; meson/ninja build with the positive20 configuration "
         "produced a binary whose sha256 equals the positive20 binary (`238066fa01d9...`, `run-build.json binary_sha256`); ten network CLI "
         "selftests, controller-frame and native-graph (diag off/on, 640/640) PASS with PASS lines identical to positive20 apart from lane "
         "path lengths and pointer text. Evidence on the Mac: `/Users/erol/cortex-workers/takeover-next-clean/` (REPORT.md, results.md, "
         "runs/, build.log). Mac CLI route lessons: the user-level MCP config (an OAuth remote server) and `--add-dir` both stall headless "
         "runs; workers run with an isolated HOME (`/Users/erol/cortex-workers/cli-home`, keychain symlinked, empty mcp.json) and read-only "
         "evidence copied into the lane.**")
L[j:j] = [entry, ""]
io.open(P, "w", encoding="utf-8", newline="\n").write("\n".join(L))
print("ok")
