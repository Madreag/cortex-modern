"""Detect the A7 connect-gate torn-read race. Runner-only launches."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from run_sim_test import make_run  # noqa: E402

GAP = "connect gate invalid or its bounded wait expired"
FAIL = "A7 connect gate invalid or timed out"
WAIT_S = 60
DONE_S = 30
PORTS = {"torn": 48340, "wrong-id": 48341}


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def events(path):
    if not path.is_file():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def matching(path, name):
    return [row for row in events(path) if row.get("event") == name]


def wait_event(path, name, deadline):
    while time.monotonic() < deadline:
        if matching(path, name):
            return True
        time.sleep(0.02)
    return False


def write_plain(path, run_id, peer):
    payload = json.dumps({"schema": 1, "run_id": run_id, "peers": [peer]}, ensure_ascii=False) + "\n"
    with path.open("w", encoding="utf-8") as stream:
        stream.write(payload)


def launch(repo, out, port, run_id, peer, gate, journal, report):
    exe = Path(repo) / "Cortex Command.exe"
    env = {
        "CCCP_HEADLESS": "1",
        "CC_A7_EVENT_LOG": str(journal),
        "CC_A7_RUN_ID": run_id,
        "CC_A7_PEER": peer,
        "CC_A7_BINARY_SHA256": digest(exe),
        "CC_A7_CONNECT_GATE": str(gate),
    }
    args = [
        "-net-match-service-e2e", "-net-join", "127.0.0.1", "-net-port", str(port),
        "-net-match-ticks", "60", "-net-match-report", str(report),
    ]
    return make_run(repo, args, out, timeout=90, env=env, expected=[journal])


def run_arm(repo, root, arm):
    peer = "client"
    run_id = "gate-torn" if arm == "torn" else "gate-wrong"
    out = Path(root) / arm
    journal = out / "events.jsonl"
    report = out / "report.json"
    gate = out / "gate.json"
    result = {
        "arm": arm,
        "mode": "-net-match-service-e2e -net-join (NetMatchService::Start -> WaitForConnectGate; no selftest hook)",
        "port": PORTS[arm],
        "journal": str(journal),
        "stdout": str(out / "stdout.log"),
        "exe": str(Path(repo) / "Cortex Command.exe"),
        "exe_sha256": digest(Path(repo) / "Cortex Command.exe"),
    }
    run = launch(repo, out, PORTS[arm], run_id, peer, gate, journal, report)
    try:
        run.start()
        if not wait_event(journal, "connect_waiting", time.monotonic() + WAIT_S):
            record = run.finish()
            result.update(error="no connect_waiting", exit_code=record.get("exit_code"),
                          events=[row.get("event") for row in events(journal)])
            return result
        if arm == "torn":
            gate.open("x").close()
            time.sleep(0.060)
            write_plain(gate, run_id, peer)
        else:
            payload = json.dumps({"schema": 1, "run_id": "other-run", "peers": [peer]}, ensure_ascii=False) + "\n"
            tmp = gate.with_suffix(gate.suffix + ".pub")
            with tmp.open("x", encoding="utf-8") as stream:
                stream.write(payload)
            tmp.replace(gate)
        deadline = time.monotonic() + DONE_S
        released = False
        while time.monotonic() < deadline:
            if matching(journal, "connect_released"):
                released = True
                break
            if run.poll() is not None:
                break
            time.sleep(0.02)
        if released and run.poll() is None:
            run.terminate(code=137, reason="A7 gate race observed connect_released")
        record = run.finish()
    except BaseException as error:
        run.close()
        result["error"] = f"{type(error).__name__}: {error}"
        return result
    finally:
        run.close()
    log = ""
    log_path = out / "stdout.log"
    if log_path.is_file():
        log = log_path.read_text(encoding="utf-8-sig", errors="replace")
    gaps = matching(journal, "evidence_gap")
    gap_text = gaps[0].get("reason", "") if gaps else ""
    fail_line = next((line for line in log.splitlines() if FAIL in line), "")
    result.update(
        exit_code=record.get("exit_code"),
        connect_waiting=bool(matching(journal, "connect_waiting")),
        connect_released=bool(matching(journal, "connect_released")),
        gap_reason=gap_text,
        fail_line=fail_line,
        has_gap_prefix=GAP in gap_text,
        has_fail_prefix=FAIL in log,
    )
    return result


def expected_ok(row, expect):
    if expect == "observe":
        return True
    if "error" in row:
        return False
    if expect == "red":
        return row.get("has_gap_prefix") and row.get("has_fail_prefix") and not row.get("connect_released")
    if expect == "green":
        return bool(row.get("connect_released")) and not row.get("has_gap_prefix")
    return row.get("has_gap_prefix") and row.get("has_fail_prefix") and not row.get("connect_released")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=TOOLS.parent)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arm", choices=("torn", "wrong-id", "both"), default="both")
    parser.add_argument("--expect", choices=("observe", "red", "green", "terminal"), default="observe")
    options = parser.parse_args()
    options.out.mkdir(parents=True, exist_ok=True)
    arms = ["torn", "wrong-id"] if options.arm == "both" else [options.arm]
    if options.expect == "red" and options.arm != "torn":
        parser.error("--expect red is the torn arm on the baseline exe")
    if options.expect == "green" and options.arm != "torn":
        parser.error("--expect green is the torn arm on the fixed exe")
    if options.expect == "terminal" and options.arm != "wrong-id":
        parser.error("--expect terminal is the wrong-id arm")
    rows = [run_arm(options.repo, options.out, arm) for arm in arms]
    ok = all(expected_ok(row, options.expect) for row in rows)
    summary = {"expect": options.expect, "ok": ok, "results": rows}
    (options.out / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
