import io

RULE = (
    "\n**Mac workers (binding, user's instruction 2026-09-11).** The Mac Mini is used as a second worker host: Grok 4.6 Extra High fast "
    "(`cursor-grok-4.6-xhigh-fast`) through the Cursor CLI (`/Users/erol/.local/bin/cursor-agent`, logged in as the user), orchestrated "
    "from the lead over `ssh Erol-Mac`. The CLI needs the unlocked login keychain, which a plain SSH session does not have (even `--help` "
    "fails with 'Your macOS login keychain is locked'), so every CLI run is a launchd job bootstrapped into the logged-in GUI session "
    "(`gui/501`): `reviews/takeover-20260909/grok-workers/mac_cli/mac_job.py <lane> <script.zsh>` copies the job, bootstraps it, polls "
    "`exit.txt`, boots it out and fetches outputs; the prompt travels by `scp` as `prompt.txt` (PowerShell has no `<` redirection) and the "
    "job runs `cursor-agent -p --force --trust --sandbox disabled --output-format text --model cursor-grok-4.6-xhigh-fast --workspace "
    "<lane> \"$(tr -d '\\r' < prompt.txt)\"`. Lanes live under `/Users/erol/cortex-workers/<lane>`; everything under "
    "`/Users/erol/Documents/Codex/` is retained evidence and read-only for workers; a Mac lane keeps under 12 GB; the same brief rules "
    "apply as on Windows (scope, no push, no attribution, evidence paths, 25-line summary).\n"
)

for name in ("AGENTS.md", "CLAUDE.md"):
    path = rf"D:\Projects\{name}"
    text = io.open(path, encoding="utf-8").read()
    marker = "**Status reporting (binding, user's rule 2026-09-11).**"
    assert marker in text and "Mac workers (binding" not in text, name
    text = text.replace(marker, RULE.lstrip("\n") + "\n" + marker, 1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
a = io.open(r"D:\Projects\AGENTS.md", encoding="utf-8").read()
c = io.open(r"D:\Projects\CLAUDE.md", encoding="utf-8").read()
print("twins identical:", a == c)

P = r"D:\Projects\RESUME.md"
L = io.open(P, encoding="utf-8").read().split("\n")
t = next(k for k, l in enumerate(L) if l.startswith("## Agent topology"))
L.insert(t + 1, "")
L.insert(t + 2, "**2026-09-11: the Mac is a worker host again.** Grok 4.6 Extra High fast via the Cursor CLI in the Mac's GUI session (launchd `gui/501`; see AGENTS.md 'Mac workers'); launcher `reviews/takeover-20260909/grok-workers/mac_cli/mac_job.py`; first lane `takeover-next-clean` = clean-clone build + selftests of `stage2/takeover-next` (W15). The old Claude-Code-over-SSH runbook remains valid for the launchd mechanics.")
io.open(P, "w", encoding="utf-8", newline="\n").write("\n".join(L))

S = r"D:\Projects\STATUS.md"
s = io.open(S, encoding="utf-8").read()
s = s.replace("| 1 | Controller boundary + wire/replay compat | 90 | 0 | First MSVC build of the positive20 group on `stage2/takeover-msvc` (W12; 1 compile fix landed, loop running) |",
              "| 1 | Controller boundary + wire/replay compat | 90 | 0 | MSVC build loop of the group on `stage2/takeover-msvc` (W12; 2 compile fixes landed); Mac clean-clone build + selftests of the committed group (W15, Cursor CLI on the Mac) |", 1)
s = s.replace("| 6 | H4 reconnect + host moderation | 55 | 0 | H4 four-gate + AIOrder causal trace (W10); B1 traced (W11 done: held \"Over\" fires when the seat is re-bound) | Fix design for B1 + H4 together when W10 lands |",
              "| 6 | H4 reconnect + host moderation | 55 | 0 | Root cause found (hostile duel kills the dropped brain during the hold); USER DECISION: pause the match while a seat is held; W14 tracing the coordinator hook points for the pause design | Fix group design when W14/W8/W9 land |", 1)
s = s.replace("Last update: 2026-09-11 05:45 UTC · Overall **55%** (Δ 0 since 05:25 UTC)", "Last update: 2026-09-11 05:55 UTC · Overall **55%** (Δ 0 since 05:25 UTC)", 1)
io.open(S, "w", encoding="utf-8", newline="\n").write(s)
print("ok")
