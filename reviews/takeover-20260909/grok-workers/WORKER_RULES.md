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

## Style
- Commit messages: one plain imperative subject line under 72 chars, optional short body of plain sentences. Match `git log --oneline -30` of the tree you work in. No tags, ticket numbers or milestone labels.
- Code comments (if a brief lets you write code): at most one short line, present tense, only for non-obvious intent. Never narrate a fix or history.

## Scratch and disk
- Scratch only under the path the brief names; keep it under 2 GB. Never write into a repository tree except the files the brief lists.

## Output contract
- Write the report at the exact path the brief names. Every claim carries the command you ran and the path of the output file that proves it. Quote failing lines verbatim with file path and line number.
- Your final message to the lead is at most 25 lines: what you did, exact hashes/paths, evidence file paths, what you could not do and why. No adjectives, no "should be fine", no verdicts stated as facts.
- The shell prints `[ERROR] - (starship::print): Under a 'dumb' terminal (TERM=dumb).` on every command. That line is noise; ignore it.
- Workspace shell is PowerShell. Prefer writing Python to a `.py` file and running it over long inline one-liners (quoting breaks).
