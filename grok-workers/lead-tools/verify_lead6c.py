"""Verification set on the lead candidate after the five merges. Launches only through run_sim_test.make_run
or the lanes' drivers (which use it)."""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path("D:/Projects/control-build")
OUT = Path("D:/mx/lead-fg6/verify6-8418e6cbca-r2")
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run  # noqa: E402

env = dict(os.environ)
env["CCCP_HEADLESS"] = "1"
summary = {}


def note(name, ok, detail):
    summary[name] = {"pass": bool(ok), "detail": detail}
    print(f"[verify6] {'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2))


def engine(name, args, timeout, needle):
    out = OUT / name
    run = make_run(REPO, args, out, timeout)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (out / "stdout.log").read_text(errors="replace") if (out / "stdout.log").exists() else ""
    hit = needle in log
    fails = [l for l in log.splitlines() if " FAIL" in l or "FATAL" in l]
    note(name, record["exit_code"] == 0 and not record["timed_out"] and hit and not fails,
         f"exit={record['exit_code']} timed_out={record['timed_out']} needle={hit} fails={fails[:3]} sha={record['exe_sha256'][:12]}")


def shell(name, cmd, cwd=None, timeout=1800, needle=None):
    proc = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)
    text = proc.stdout + proc.stderr
    (OUT / f"{name}.log").write_text(text, encoding="utf-8", errors="replace")
    ok = proc.returncode == 0 and (needle is None or needle in text)
    tail = text.strip().splitlines()[-1][:200] if text.strip() else ""
    note(name, ok, f"rc={proc.returncode} tail={tail!r}")


t0 = time.time()
# 1. socket-free suite
shell("selftests", [sys.executable, str(REPO / "tools/run_selftests.py"), "--repo", str(REPO), "--out", str(OUT / "selftests"), "--timeout", "300"], needle='"passed": 12')
# 2. the merged selftests not in the suite
engine("port-map", ["-net-port-map-selftest"], 120, "[net-port-map-selftest] PASS")
engine("directory", ["-net-directory-selftest"], 120, "[net-directory-selftest] PASS")
engine("script-graph-s4", ["-script-graph-selftest", "-num-lua-states", 4], 300, "[script-graph-selftest] PASS")
# 3. global-callback driver (installs the fixture)
shell("global-callback", [sys.executable, str(REPO / "tools/test_global_callbacks.py"), "--repo", str(REPO), "--recording", "D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay", "--out", str(OUT / "gcb")], needle='"pass": true')
# 4. W89-2 crash-green argv on this candidate (event ledger at D=7) + lpinv matrix at tick 100
engine("preview-event-d7", ["-net-replay", "D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay", "-tick-hashes", "-max-ticks", 221, "-input-script", "D:/Projects/stage2_p4/fixtures/pickup_fire.txt", "-local-prediction-depth", 7, "-local-prediction-event-ledger", 153, "-out", str(OUT / "preview-event-d7/trace")], 240, "[preview-event-selftest] PASS")
engine("lpinv-100", ["-net-replay", "D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay", "-tick-hashes", "-max-ticks", 221, "-input-script", "D:/Projects/stage2_p4/fixtures/pickup_fire.txt", "-local-prediction-depth", 7, "-local-prediction-invariance", "100:1,4,7,12:1,3", "-out", str(OUT / "lpinv-100/trace")], 400, "[lpinv] PASS tick 100: 8/8")
# 5. W102 arm 1 (two-process switch-control e2e) through the lane's driver, on this candidate
shell("arm1-switch-control", [sys.executable, "D:/Projects/reviews/takeover-20260909/grok-workers/w102-fixtures-takeover-e2e/run_arm1.py", "--repo", str(REPO), "--out", str(OUT / "arm1"), "--port", "47641"], timeout=1500, needle="PASS switch_control:")
all_pass = all(v["pass"] for v in summary.values())
print(f"[verify6] done in {time.time() - t0:.0f} s; {sum(1 for v in summary.values() if v['pass'])}/{len(summary)} pass", flush=True)
sys.exit(0 if all_pass else 1)
