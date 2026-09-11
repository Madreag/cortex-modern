"""Run a zsh job on the Mac inside the logged-in GUI session (launchd gui/501) and fetch its outputs.

The Cursor CLI needs the unlocked login keychain, which plain SSH sessions do not have; a launchd
job bootstrapped into gui/501 runs with it (the route the 2026-09-08 Mac runbook verified).

usage: python mac_job.py <lane> <script.zsh> [--timeout SEC] [--fetch name ...]
  lane        job name; lives at /Users/erol/cortex-workers/<lane> on the Mac
  script.zsh  local zsh script (LF); it runs with cwd = the lane directory and must write exit.txt
"""
import argparse
import pathlib
import subprocess
import sys
import time
import uuid

HOST = "Erol-Mac"
ROOT = "/Users/erol/cortex-workers"


def ssh(cmd, check=True, input_bytes=None):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, cmd], capture_output=True, input=input_bytes)
    if check and r.returncode != 0:
        sys.exit(f"ssh failed ({r.returncode}): {cmd[:120]}\n{r.stderr.decode(errors='replace')}")
    return r.stdout.decode(errors="replace")


def put(remote_path, data: bytes):
    ssh(f"cat > '{remote_path}'", input_bytes=data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lane")
    ap.add_argument("script")
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--fetch", nargs="*", default=[])
    ap.add_argument("--poll", type=int, default=15)
    a = ap.parse_args()

    lane_dir = f"{ROOT}/{a.lane}"
    label = f"com.cortex.{a.lane}.{uuid.uuid4().hex[:8]}"
    script = pathlib.Path(a.script).read_bytes().replace(b"\r\n", b"\n")
    ssh(f"mkdir -p '{lane_dir}' && rm -f '{lane_dir}/exit.txt'")
    put(f"{lane_dir}/run.zsh", script)
    plist = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>{label}</string>
<key>ProgramArguments</key><array><string>/bin/zsh</string><string>-lc</string><string>cd '{lane_dir}' &amp;&amp; source '{lane_dir}/run.zsh'</string></array>
<key>WorkingDirectory</key><string>{lane_dir}</string>
<key>RunAtLoad</key><true/>
<key>StandardOutPath</key><string>{lane_dir}/launchd.out</string>
<key>StandardErrorPath</key><string>{lane_dir}/launchd.err</string>
</dict></plist>
"""
    put(f"{lane_dir}/{label}.plist", plist.encode())
    ssh(f"launchctl bootstrap gui/501 '{lane_dir}/{label}.plist'")
    t0 = time.time()
    try:
        while time.time() - t0 < a.timeout:
            state = ssh(
                f"test -f '{lane_dir}/exit.txt' && echo done || (launchctl print gui/501/{label} >/dev/null 2>&1 && echo wait || echo gone)",
                check=False).strip()
            if state == "done":
                break
            if state == "gone":
                print(f"job {label} disappeared without exit.txt (stopped externally)", file=sys.stderr)
                sys.exit(3)
            time.sleep(a.poll)
        else:
            print(f"TIMEOUT after {a.timeout}s; job {label} left running", file=sys.stderr)
            sys.exit(2)
    finally:
        ssh(f"launchctl bootout gui/501/{label} 2>/dev/null || true", check=False)
    code = ssh(f"cat '{lane_dir}/exit.txt'").strip()
    print(f"lane={a.lane} label={label} exit={code} elapsed={int(time.time() - t0)}s")
    for name in a.fetch:
        print(f"----- {name}")
        print(ssh(f"cat '{lane_dir}/{name}' 2>&1 | head -c 20000", check=False))


if __name__ == "__main__":
    main()
