"""Run a GPT-6 Astra worker through the Codex CLI (`codex exec`) behind a hard model/effort gate.

usage: python codex_job.py <name> <workspace> <prompt.txt> [--task TEXT] [--timeout SEC] [--model gpt-6-astra]
                           [--effort max] [--require-model gpt-6-astra] [--require-effort max]
                           [--sandbox full|workspace-write|read-only] [--add-dir DIR ...] [--resume THREAD_ID]
                           [--init-wait SEC]
  name        run name; outputs land in grok-workers/cli_runs/<name>/
  workspace   the worker's worktree: the CLI's cwd (and, with --sandbox workspace-write, its writable root)
  prompt.txt  the brief (UTF-8); fed to the CLI on stdin as the '-' prompt, CRs stripped

Why this shape: `codex exec` hangs on an open non-TTY stdin pipe unless the prompt is read from it, so the brief
IS the stdin and stdin is closed after it; `--json` events name the thread but not the model, so the gate reads the
session rollout (~/.codex/sessions/<y>/<m>/<d>/rollout-*-<thread>.jsonl), whose turn_context line carries the model
and effort that actually run. The job is killed unless both match --require-model / --require-effort.
`--sandbox full` (default, the user's rule of 2026-09-13) is `--dangerously-bypass-approvals-and-sandbox`; the brief
is the fence. `--resume THREAD_ID` continues the same thread (`codex exec resume`), cwd taken from the session.

Exit codes: 0 worker finished (the CLI's own exit code is in exit.txt); 2 timeout; 3 the CLI ended or stayed silent
before a thread/rollout/turn_context appeared; 4 REFUSED (wrong model or effort).
"""
from __future__ import annotations

import argparse
import datetime as dt
import glob
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from typing import NoReturn

HERE = pathlib.Path(__file__).resolve().parent
RUNS = HERE.parent / "cli_runs"
SPAWN_LOG = HERE.parent / "SPAWN_LOG.md"
SESSIONS = pathlib.Path(os.environ.get("USERPROFILE", str(pathlib.Path.home()))) / ".codex" / "sessions"
MST = dt.timezone(dt.timedelta(hours=-7))


def stamp() -> str:
    return dt.datetime.now(MST).strftime("%Y-%m-%d %H:%M MST")


def fail(code: int, msg: str) -> NoReturn:
    print(msg, file=sys.stderr, flush=True)
    sys.exit(code)


def codex_exe() -> str:
    exe = shutil.which("codex") or shutil.which("codex.exe") or shutil.which("codex.cmd")
    return exe or "codex"


def worker_env(workspace: pathlib.Path) -> dict:
    env = dict(os.environ)
    env["CCCP_HEADLESS"] = "1"
    env.setdefault("CL", "/MP6")
    env.setdefault("GNS_ROOT", r"D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release")
    env.setdefault("GNS_DEP_ROOT", r"D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows")
    env["CODEX_WORKER_LANE"] = str(workspace)
    return env


def kill_tree(proc: subprocess.Popen) -> None:
    subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)


def read_events(path: pathlib.Path) -> list[dict]:
    out = []
    try:
        with path.open(encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    continue
    except OSError:
        pass
    return out


def find_rollout(thread: str) -> pathlib.Path | None:
    hits = glob.glob(str(SESSIONS / "**" / f"rollout-*{thread}*.jsonl"), recursive=True)
    return pathlib.Path(max(hits, key=os.path.getmtime)) if hits else None


def turn_context_line(rollout: pathlib.Path) -> str | None:
    try:
        with rollout.open(encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if '"turn_context"' in line and '"model"' in line:
                    return line.rstrip("\n")
    except OSError:
        return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("name")
    ap.add_argument("workspace")
    ap.add_argument("prompt")
    ap.add_argument("--task", default="")
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--model", default="gpt-6-astra")
    ap.add_argument("--effort", default="max")
    ap.add_argument("--require-model", default="gpt-6-astra")
    ap.add_argument("--require-effort", default="max")
    ap.add_argument("--sandbox", default="full", choices=["full", "workspace-write", "read-only"])
    ap.add_argument("--add-dir", action="append", default=[])
    ap.add_argument("--resume", default=None, help="thread id to continue (codex exec resume)")
    ap.add_argument("--init-wait", type=int, default=120)
    a = ap.parse_args()

    workspace = pathlib.Path(a.workspace).resolve()
    if not workspace.is_dir():
        fail(3, f"workspace missing: {workspace}")
    prompt_path = pathlib.Path(a.prompt)
    brief = prompt_path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
    if not brief.strip():
        fail(3, "empty prompt")

    run = RUNS / a.name
    run.mkdir(parents=True, exist_ok=True)
    (run / "prompt-used.txt").write_text(brief, encoding="utf-8")
    events_path = run / "events.jsonl"
    stderr_path = run / "stderr.txt"
    last_path = run / "last-message.md"
    for p in (events_path, stderr_path, last_path):
        if p.exists():
            p.unlink()

    cmd = [codex_exe(), "exec"]
    if a.resume:
        cmd += ["resume", a.resume]
    cmd += ["--json", "-o", str(last_path), "--skip-git-repo-check", "-m", a.model,
            "-c", f"model_reasoning_effort={a.effort}", "-c", "notify=[]"]
    if a.sandbox == "full":
        cmd += ["--dangerously-bypass-approvals-and-sandbox"]
    else:
        cmd += ["-s", a.sandbox]
        for d in a.add_dir:
            cmd += ["--add-dir", d]
    if not a.resume:
        cmd += ["-C", str(workspace)]
    cmd += ["-"]
    (run / "command.txt").write_text(" ".join(cmd) + f"\n# cwd {workspace}\n# spawned {stamp()}\n", encoding="utf-8")

    t0 = time.time()
    with events_path.open("wb") as out, stderr_path.open("wb") as err:
        proc = subprocess.Popen(cmd, cwd=str(workspace), env=worker_env(workspace), stdin=subprocess.PIPE,
                                stdout=out, stderr=err)
        assert proc.stdin is not None
        try:
            proc.stdin.write(brief.encode("utf-8"))
            proc.stdin.close()
        except OSError as exc:
            kill_tree(proc)
            fail(3, f"could not feed the brief on stdin: {exc}")

        # The gate: thread id from the --json stream, model and effort from the session rollout.
        thread = a.resume
        rollout = None
        evidence: str | None = None
        rc = -1
        while time.time() - t0 < a.init_wait:
            if thread is None:
                for ev in read_events(events_path):
                    if ev.get("type") == "thread.started" and ev.get("thread_id"):
                        thread = ev["thread_id"]
                        break
            if thread and rollout is None:
                rollout = find_rollout(thread)
            if rollout is not None:
                evidence = turn_context_line(rollout)
                if evidence:
                    break
            if proc.poll() is not None:
                break
            time.sleep(2)
        if not evidence:
            kill_tree(proc)
            (run / "exit.txt").write_text("NO_INIT\n", encoding="utf-8")
            fail(3, f"no turn_context within {a.init_wait}s (thread={thread}, rollout={rollout}, cli rc={proc.poll()})")
        assert evidence is not None
        (run / "thread.txt").write_text(f"{thread}\n{rollout}\n", encoding="utf-8")
        (run / "model-evidence.txt").write_text(evidence + "\n", encoding="utf-8")
        m_model = re.search(r'"model"\s*:\s*"([^"]*)"', evidence)
        m_effort = re.search(r'"(?:effort|reasoning_effort)"\s*:\s*"([^"]*)"', evidence)
        model = m_model.group(1) if m_model else None
        effort = m_effort.group(1) if m_effort else None
        if model != a.require_model or effort != a.require_effort:
            kill_tree(proc)
            (run / "exit.txt").write_text(f"REFUSED model={model} effort={effort}\n", encoding="utf-8")
            fail(4, f"REFUSED: model={model} effort={effort} (required {a.require_model}/{a.require_effort})")
        with SPAWN_LOG.open("a", encoding="utf-8") as f:
            f.write(f"| {stamp()} (Codex CLI) | codex:{a.name} | {a.task} | {model} effort={effort}"
                    f"{' resume ' + a.resume if a.resume else ''} | {events_path} |\n")
        print(f"gate ok: {model} effort={effort} thread={thread}", flush=True)

        try:
            rc = proc.wait(timeout=max(1, a.timeout - (time.time() - t0)))
        except subprocess.TimeoutExpired:
            kill_tree(proc)
            (run / "exit.txt").write_text(f"TIMEOUT {a.timeout}\n", encoding="utf-8")
            fail(2, f"timeout after {a.timeout}s")

    events = read_events(events_path)
    final = None
    usage = None
    for ev in events:
        item = ev.get("item") or {}
        if ev.get("type") == "item.completed" and item.get("type") == "agent_message":
            final = item.get("text")
        if ev.get("type") == "turn.completed" and ev.get("usage"):
            usage = ev["usage"]
    if final is None and last_path.exists():
        final = last_path.read_text(encoding="utf-8", errors="replace")
    (run / "REPORT-final-message.md").write_text((final or "") + "\n", encoding="utf-8")
    (run / "usage.json").write_text(json.dumps(usage or {}, indent=2), encoding="utf-8")
    (run / "exit.txt").write_text(f"{rc}\n", encoding="utf-8")
    print(f"finished rc={rc} elapsed={time.time() - t0:.0f}s usage={usage}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
