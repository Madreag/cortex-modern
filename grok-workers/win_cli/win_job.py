"""Run a Grok worker through the Cursor CLI on Windows behind a hard model gate.

The Cursor Task tool does not honour the reasoning level: W36 and W38 were spawned with the slug
cursor-grok-4.6-xhigh-fast and ran as "Cursor Grok 4.6 High Fast". The CLI's first stream-json line names
the model that actually runs, so Grok work goes through the CLI only. The gate is one read of that first line:
if it does not contain the required string the job is killed before it does any work. After that the runner
only waits for the process to exit, bounded by --timeout; nothing polls and nothing runs forever.

The CLI runs with a private profile (grok-workers/cli_home): no user MCP servers, hooks or Claude-compat
settings are loaded, its config pins commit/PR attribution OFF, and git still reads the user's global config.

usage: python win_job.py <name> <workspace> <prompt.txt> [--task TEXT] [--timeout SEC]
                         [--model cursor-grok-4.6-xhigh-fast] [--require-model "Extra High Fast"]
                         [--mode agent|ask|plan] [--init-wait SEC] [--startup-lock-wait SEC]
  name        run name (w40 ...); outputs land in grok-workers/cli_runs/<name>/
  workspace   the worker's worktree (the CLI's --workspace and cwd)
  prompt.txt  the brief (UTF-8); passed verbatim as the prompt argument, CRs stripped

Exit codes: 0 worker finished (its own exit code is in exit.txt); 2 timeout; 3 CLI ended before naming a
model (login, network, startup lock timeout); 4 REFUSED, wrong model.
"""
import argparse
import datetime as dt
import json
import msvcrt
import os
import pathlib
import re
import subprocess
import sys
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
RUNS = HERE.parent / "cli_runs"
CLI_HOME = HERE.parent / "cli_home"
SPAWN_LOG = HERE.parent / "SPAWN_LOG.md"


def fail(code, msg):
    print(msg, file=sys.stderr, flush=True)
    sys.exit(code)


def cli_command():
    root = pathlib.Path(os.environ["LOCALAPPDATA"]) / "cursor-agent" / "versions"
    pat = re.compile(r"^(\d{4})\.(\d{1,2})\.(\d{1,2})(?:-\d{2}-\d{2}-\d{2})?-[a-f0-9]+$")
    versions = []
    for d in root.iterdir() if root.is_dir() else []:
        m = pat.match(d.name)
        if m and (d / "node.exe").is_file() and (d / "index.js").is_file():
            versions.append((tuple(int(x) for x in m.groups()), d))
    if not versions:
        fail(3, f"Cursor CLI not installed under {root}; run: irm 'https://cursor.com/install?win32=true' | iex")
    d = max(versions)[1]
    return [str(d / "node.exe"), str(d / "index.js")], d.name


def pin_cli_config():
    """The CLI persists the last --model as its default and ships with attribution ON; pin both."""
    cfg_path = CLI_HOME / ".cursor" / "cli-config.json"
    cfg_path.parent.mkdir(parents=True, exist_ok=True)
    cfg = {}
    if cfg_path.is_file():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        except ValueError:
            cfg = {}
    params = [{"id": "effort", "value": "xhigh"}, {"id": "fast", "value": "true"}]
    cfg["version"] = cfg.get("version", 1)
    cfg["attribution"] = {"attributeCommitsToAgent": False, "attributePRsToAgent": False}
    cfg["model"] = {"modelId": "grok-4.6", "displayModelId": "grok-4.6", "displayName": "Cursor Grok 4.6 Extra High Fast",
                    "displayNameShort": "Cursor Grok 4.6 Extra High Fast", "aliases": [], "maxMode": False}
    cfg["hasChangedDefaultModel"] = True
    cfg["modelParameters"] = {"grok-4.6": params}
    cfg["selectedModel"] = {"modelId": "grok-4.6", "parameters": params}
    cfg.setdefault("sandbox", {"mode": "disabled", "networkAccess": "user_config_with_defaults"})
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def worker_env():
    env = dict(os.environ)
    env["CCCP_HEADLESS"] = "1"  # a direct engine launch from a worker shell creates its window hidden
    env["USERPROFILE"] = str(CLI_HOME)
    env["HOME"] = str(CLI_HOME)
    env["CURSOR_INVOKED_AS"] = "agent"
    env.setdefault("NODE_COMPILE_CACHE", str(pathlib.Path(os.environ["LOCALAPPDATA"]) / "cursor-compile-cache"))
    gitconfig = pathlib.Path(os.environ["USERPROFILE"]) / ".gitconfig"
    if gitconfig.is_file():
        env.setdefault("GIT_CONFIG_GLOBAL", str(gitconfig))
    return env


def kill_tree(proc):
    subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)


def model_of(line):
    m = re.search(r'"model"\s*:\s*"([^"]*)"', line)
    return m.group(1) if m else None


def log_spawn(name, task, level, evidence):
    stamp = dt.datetime.now(dt.timezone(dt.timedelta(hours=-7))).strftime("%Y-%m-%d %H:%M MST")
    with SPAWN_LOG.open("a", encoding="utf-8") as f:
        f.write(f"| {stamp} (CLI) | cli:{name} | {task} | {level} | {evidence} |\n")


def startup_lock_path():
    return CLI_HOME / ".cursor" / "startup.lock"


def try_acquire_startup_lock(timeout_s):
    """Process-owned one-byte msvcrt lock; return fd or None on timeout."""
    path = startup_lock_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_RDWR | os.O_CREAT | os.O_BINARY)
    try:
        if os.lseek(fd, 0, os.SEEK_END) == 0:
            os.write(fd, b"\0")
        deadline = time.monotonic() + max(0.0, float(timeout_s))
        while True:
            os.lseek(fd, 0, os.SEEK_SET)
            try:
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                return fd
            except OSError:
                if time.monotonic() >= deadline:
                    os.close(fd)
                    return None
                time.sleep(0.05)
    except Exception:
        os.close(fd)
        raise


def release_startup_lock(fd):
    if fd is None:
        return
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
    except OSError:
        pass
    finally:
        os.close(fd)


class Pump(threading.Thread):
    """Copies the CLI's stdout to the output file and hands the first line to the main thread."""

    def __init__(self, pipe, path):
        super().__init__(daemon=True)
        self.pipe, self.path = pipe, path
        self.first = None
        self.got_first = threading.Event()

    def run(self):
        with self.path.open("wb") as out:
            for raw in self.pipe:
                out.write(raw)
                out.flush()
                if self.first is None:
                    self.first = raw.decode("utf-8", "replace")
                    self.got_first.set()
        self.got_first.set()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("workspace")
    ap.add_argument("prompt")
    ap.add_argument("--task", default="")
    ap.add_argument("--timeout", type=int, default=3600, help="hard cap on the whole run; the job is killed at the cap")
    ap.add_argument("--init-wait", type=int, default=120, help="seconds allowed for the CLI to print its first line")
    ap.add_argument("--startup-lock-wait", type=int, default=180,
                    help="seconds allowed to acquire the private-profile startup lock; fail closed")
    ap.add_argument("--model", default="cursor-grok-4.6-xhigh-fast")
    ap.add_argument("--resume", default=None, help="chat id of a finished lane to continue (the CLI keeps its context)")
    ap.add_argument("--require-model", default="Extra High Fast",
                    help="substring the CLI's model line must contain, else the job is killed")
    ap.add_argument("--mode", default="agent", choices=["agent", "ask", "plan"])
    a = ap.parse_args()

    workspace = pathlib.Path(a.workspace).resolve()
    if not workspace.is_dir():
        fail(1, f"workspace does not exist: {workspace}")
    prompt = pathlib.Path(a.prompt).read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "")
    if not prompt.strip():
        fail(1, "empty prompt")

    run_dir = RUNS / a.name
    if run_dir.exists() and any(run_dir.iterdir()):
        fail(1, f"run dir already used: {run_dir} (fresh spawn per name; pick a new name)")
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "prompt-used.txt").write_text(prompt, encoding="utf-8")
    out_path = run_dir / "agent-output.jsonl"
    err_path = run_dir / "agent-stderr.txt"
    exit_path = run_dir / "exit.txt"

    lock_fd = try_acquire_startup_lock(a.startup_lock_wait)
    if lock_fd is None:
        exit_path.write_text("STARTUP_LOCK_TIMEOUT\n", encoding="utf-8")
        fail(3, f"STARTUP_LOCK_TIMEOUT after {a.startup_lock_wait}s; {startup_lock_path()}")

    try:
        with err_path.open("wb") as err:
            try:
                pin_cli_config()
                cmd, version = cli_command()
                cmd += ["-p", "--force", "--trust", "--sandbox", "disabled", "--output-format", "stream-json",
                        "--model", a.model, "--workspace", str(workspace)]
                if a.resume:
                    cmd += ["--resume", a.resume]
                if a.mode != "agent":
                    cmd += ["--mode", a.mode]
                cmd.append(prompt)
                (run_dir / "command.txt").write_text(f"cli {version}\nmodel slug {a.model}\nrequire {a.require_model!r}\n"
                                                     f"workspace {workspace}\ncli home {CLI_HOME}\nargs (prompt omitted): {cmd[:-1]}\n",
                                                     encoding="utf-8")

                t0 = time.time()
                proc = subprocess.Popen(cmd, cwd=str(workspace), stdout=subprocess.PIPE, stderr=err, stdin=subprocess.DEVNULL,
                                        env=worker_env())
                pump = Pump(proc.stdout, out_path)
                pump.start()

                # The gate: one bounded wait for the first line, one decision.
                pump.got_first.wait(a.init_wait)
                first = pump.first
                if first is None:
                    kill_tree(proc)
                    proc.wait()
                    exit_path.write_text("NO_INIT\n", encoding="utf-8")
                    fail(3, f"CLI printed nothing in {a.init_wait}s (exit {proc.returncode}); stderr: "
                            f"{err_path.read_text(encoding='utf-8', errors='replace')[-2000:]}")
                model = model_of(first)
                evidence = f"{out_path}:1 -> {first.strip()[:160]}"
                if model is None or a.require_model.lower() not in model.lower():
                    kill_tree(proc)
                    proc.wait()
                    exit_path.write_text("REFUSED\n", encoding="utf-8")
                    log_spawn(a.name, a.task, f"REFUSED ({model})", evidence)
                    fail(4, f"REFUSED: model line is {model!r}; required {a.require_model!r}\n{evidence}")
                print(f"model verified: {model} ({evidence})", flush=True)
                log_spawn(a.name, a.task, model, evidence)
            finally:
                release_startup_lock(lock_fd)
                lock_fd = None

            try:
                rc = proc.wait(timeout=max(1, a.timeout - (time.time() - t0)))
            except subprocess.TimeoutExpired:
                kill_tree(proc)
                proc.wait()
                exit_path.write_text("TIMEOUT\n", encoding="utf-8")
                fail(2, f"TIMEOUT after {a.timeout}s; killed {a.name}")
            pump.join(30)
    finally:
        release_startup_lock(lock_fd)

    exit_path.write_text(f"{rc}\n", encoding="utf-8")
    print(f"name={a.name} model={model} exit={rc} elapsed={int(time.time() - t0)}s out={out_path}")
    for line in reversed(out_path.read_text(encoding="utf-8", errors="replace").splitlines()):
        if '"type":"result"' in line.replace(" ", ""):
            print("----- result")
            print(line[:6000])
            break


if __name__ == "__main__":
    main()
