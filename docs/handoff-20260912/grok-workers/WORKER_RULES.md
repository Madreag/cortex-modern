# Worker rules (binding for every Grok worker brief; read fully before starting)

You are a worker. The lead reviews every line you produce and re-derives every verdict. You report evidence; you do not decide what is green.

## Scope
- Edit ONLY the paths your brief lists. Everything else is read-only.
- `D:\Projects\p4b-interp-validation` (the approved tree, branch `stage2/p4b-interp-lockstep`), `D:\Projects\cccp` and `D:\Projects\xarch-combat` are READ-ONLY for you, always.
- One task per worker. If the brief is wrong, ambiguous, or you are blocked: stop and report exactly what you saw. Do not improvise around it.

## Never
- Never launch `Cortex Command.exe` yourself in any way (`Start-Process`, `&`, `subprocess`, `System.Diagnostics.Process`). Engine launches exist only through `tools/win32_test_runner.py` / `tools/run_sim_test.py` / `tools/isolated_launch.py`, and only if your brief explicitly allows an engine launch.
- Never build the engine unless the brief says so; if it does, set `CL=/MP6`.
- Never move, copy, rename or delete a directory tree under `D:\mx` or any run root. Never `git worktree add/remove`, never create junctions or symlinks. Run directories contain junctions into live engine trees; a move or rmtree follows them and empties the repository.
- Never push, merge, rebase, squash, amend, `reset --hard`, `checkout -- .`, `clean`, or stash-drop. Never rewrite existing commits.
- Never widen a comparison mask, tolerance or exclusion; never patch a mod, fixture or test to make a failure disappear; never add an opt-in compatibility flag. Lua/native API behaviour is frozen (mod compatibility is binding).
- Never write attribution anywhere: no AI or model names, no `Co-Authored-By`, `Claude-Session`, `Generated-with` or similar lines in commits, code, docs or reports. Report headers carry no author/model line.
- Never launch an engine executable from a path that has no Windows Firewall allow rule, and never copy an executable to a new path to run it: the first socket bind of an unruled executable raises a firewall prompt on the user's desktop. `tools/win32_test_runner.py` refuses such a `-net*` launch with `firewall_allow_rule_present`; when you see that, stop and report the path - only the lead can add the rule (elevation).

## Style
- Commit messages: one plain imperative subject line under 72 chars, optional short body of plain sentences. Match `git log --oneline -30` of the tree you work in. No tags, ticket numbers or milestone labels.
- Code comments (if a brief lets you write code): at most one short line, present tense, only for non-obvious intent. Never narrate a fix or history.

## Scratch and disk
- Scratch only under the path the brief names; keep it under 2 GB. Never write into a repository tree except the files the brief lists.

## Output contract
- Write the report at the exact path the brief names. Every claim carries the command you ran and the path of the output file that proves it. Quote failing lines verbatim with file path and line number.
- Report EVERY change to an oracle, checker or driver you make between a failing run and a passing run, with both run directories and the diff in words; an undisclosed oracle edit is treated as a masked failure even when the new check is stricter (rule added 2026-09-12 after W58-2).
- A loopback service you start (the session directory, a TLS byte server, anything that listens) takes a lane-unique port you record in the report, never a default like 8443: Python's reuse-address sockets let two lanes bind the same port and the other lane's server answers your client (rule added 2026-09-12 after the fixgroup-3 battery's probe met another lane's service).
- Never open a GUI tool of any kind (no WinDbg/DbgX, no browser, no editor window, no headed game): everything runs in the shell; crash dumps are read with a dbghelp script or `cdb -c` (rule added 2026-09-12 after W45-2 opened WinDbg on the user's desktop).
- Your final message to the lead is at most 25 lines: what you did, exact hashes/paths, evidence file paths, what you could not do and why. No adjectives, no "should be fine", no verdicts stated as facts.
- The shell prints `[ERROR] - (starship::print): Under a 'dumb' terminal (TERM=dumb).` on every command. That line is noise; ignore it.
- Workspace shell is PowerShell. Prefer writing Python to a `.py` file and running it over long inline one-liners (quoting breaks).

**Engine launches (binding, restated 2026-09-12 after a worker opened a game window on the user's screen):** NEVER type the executable at a shell, not for a selftest, not for a quick check, not in a `for` loop: `"./Cortex Command.exe" -x-selftest` creates a REAL WINDOW on the user's desktop. Every launch, selftests included, goes through `python <tree>/tools/run_sim_test.py --repo <tree> --out <dir> --timeout <s> -- <engine flags>` (or `make_run` from Python, or `tools/isolated_launch.py` from PowerShell), which stages a private runtime, sets CCCP_HEADLESS and puts the process on a private desktop. A lane that launched the executable directly is stopped and its session ended.
