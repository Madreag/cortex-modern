"""Host migration between networks on loopback: three peers play a service match, the host is dropped, and the survivors must
reach the successor through its ICE route, because CC_TEST_MIGRATION_ICE_ONLY makes a survivor dial none of its LAN entries.

  --arm ice (the gate): the clients join by the directory's session id, so the match runs through the session directory and
      every peer publishes its LAN address, then its ICE route. PASS needs a survivor's
      `[net-migration] dialing the successor's ICE route` line after its lever skipped the LAN entry, the successor answering as
      the directory's host end, the handover declared by both survivors with the same new host,
      boundary and round, both survivors exiting 0 and their live hashes equal on every tick both ran.
  --arm lan (the RED arm): the clients join by address, so the successor publishes its LAN entry only; with the lever on, a
      survivor has no route it may dial and the same verdict fails.

    python tools/test_migration_ice.py --out <dir> [--arm ice|lan] [--port 49700] [--directory-port 49719] [--kill-tick 600]

The session directory runs on 127.0.0.1 with a certificate made in the run directory; nothing contacts a STUN or TURN server.
Every engine goes through run_sim_test.make_run and the private-desktop runner with CCCP_HEADLESS=1.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run, engine_executable  # noqa: E402
from feel_measure import input_pattern, private_settings, stage_baseline  # noqa: E402
from feel.retained_resume import compare_live_hashes  # noqa: E402
from test_directory_ice_join import list_sessions, patch_settings, sha256, start_service  # noqa: E402

PORT_LO, PORT_HI = 49700, 49719
HOSTED = re.compile(r"(?m)^\[net-match\] Host left - (.+) is now hosting; boundary=(\d+) round=(\d+)")
ICE_DIAL = "[net-migration] dialing the successor's ICE route"
LAN_SKIPPED = "[net-test] the handover dial skips the successor's LAN entry"
HOST_END = "[net-migration] the successor answers ICE dials as the directory's host end"
NAMES = {"host": "Host", "clienta": "ClientA", "clientb": "ClientB"}


def make_certificate(folder: Path) -> tuple[Path, Path, str]:
    """A self-signed loopback certificate and the pin every peer carries."""
    folder.mkdir(parents=True, exist_ok=True)
    cert, key, der = folder / "cert.pem", folder / "key.pem", folder / "cert.der"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", str(key), "-out", str(cert), "-days", "2", "-nodes",
                    "-subj", "/CN=cortex-directory", "-addext", "subjectAltName=IP:127.0.0.1"], check=True, capture_output=True)
    subprocess.run(["openssl", "x509", "-in", str(cert), "-outform", "DER", "-out", str(der)], check=True, capture_output=True)
    return cert, key, sha256(der)


def last_live_tick(path: Path) -> int:
    """The highest tick the peer's live hash file holds so far; a line still being written is skipped."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return 0
    last = 0
    for line in lines:
        try:
            last = max(last, int(json.loads(line).get("tick", 0)))
        except (ValueError, AttributeError, TypeError):
            continue
    return last


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arm", choices=("ice", "lan"), default="ice")
    parser.add_argument("--port", type=int, default=PORT_LO)
    parser.add_argument("--directory-port", type=int, default=PORT_HI)
    parser.add_argument("--ticks", type=int, default=1800)
    parser.add_argument("--kill-tick", type=int, default=600)
    parser.add_argument("--timeout", type=float, default=300)
    options = parser.parse_args()
    if not (PORT_LO <= options.port <= PORT_HI - 4 and PORT_LO <= options.directory_port <= PORT_HI and options.directory_port != options.port):
        parser.error(f"--port and --directory-port stay inside {PORT_LO}-{PORT_HI}")
    if not 0 < options.kill_tick < options.ticks:
        parser.error("--kill-tick falls inside the match")
    if os.environ.get("CCCP_HEADLESS", "1") != "1":
        parser.error("CCCP_HEADLESS must stay 1")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, out = options.repo.resolve(), options.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    script = out / "input.txt"
    input_pattern(script)
    verdict = {"arm": options.arm, "executable_sha256": sha256(engine_executable(repo)), "ticks": options.ticks, "kill_tick": options.kill_tick,
               "port": options.port, "directory_port": options.directory_port, "checks": []}

    def check(name: str, ok: object, detail: object = "") -> bool:
        verdict["checks"].append({"check": name, "ok": bool(ok), "detail": str(detail)})
        print(f"[migration-ice] {'OK  ' if ok else 'FAIL'} {name} {detail}", flush=True)
        return bool(ok)

    settings = {"NetworkStunServers": ""}
    service = None
    if options.arm == "ice":
        cert, key, pin = make_certificate(out / "cert")
        settings.update({"SessionDirectoryUrl": f"127.0.0.1:{options.directory_port}", "SessionDirectoryCertSha256": pin, "NetworkIceEnable": "1"})
        service = start_service(out, options.directory_port, cert, key)
    else:
        settings["NetworkIceEnable"] = "0"
    env = {"CCCP_HEADLESS": "1", "CC_TEST_MIGRATION_ICE_ONLY": "1"}
    runs, records = {}, {}

    def launch(name: str, extra: list[str]):
        flags = ["-seed", "42", "-max-ticks", str(options.ticks), "-tick-hashes", "-out", str(out / f"{name}_trace.json"),
                 "-net-live-tick-hashes", str(out / f"{name}-live.jsonl"), "-input-script", str(script),
                 "-net-match-service-e2e", "-net-port", str(options.port), "-net-match-ticks", str(options.ticks),
                 "-net-match-humans", "3", "-net-match-peers", "3", "-net-match-cpu-slots", "0",
                 "-net-match-service-preset", "Determinism FeelBaseline", "-net-match-service-module", "UserScenes.rte",
                 "-net-match-auto-delay", "-net-match-report", str(out / f"{name}_report.json"),
                 *(["-net-ice", "on"] if options.arm == "ice" else []), *extra]
        run = make_run(repo, flags, out / name, timeout=options.timeout, env=env)
        private_settings(run, 60)
        stage_baseline(run, options.ticks, 2)
        patch_settings(Path(run.cwd), {**settings, "NetworkDisplayName": NAMES[name]})
        runs[name] = run
        run.start()
        return run

    try:
        host = launch("host", ["-net-host"])
        join = ["-net-join", "127.0.0.1"]
        if options.arm == "ice":
            session_id, deadline = "", time.monotonic() + 120
            while time.monotonic() < deadline and not session_id and host.poll() is None:
                session_id = next((row["session_id"] for row in list_sessions(options.directory_port) if row.get("session_id")), "")
                time.sleep(0.5)
            if not check("host_registered_a_row", bool(session_id), session_id):
                raise SystemExit(1)
            join = ["-net-join-session", session_id]
        for name in ("clienta", "clientb"):
            launch(name, join)
            time.sleep(0.75)
        # The host is dropped once its round has run to the kill tick.
        deadline = time.monotonic() + options.timeout
        while time.monotonic() < deadline and host.poll() is None and last_live_tick(out / "host-live.jsonl") < options.kill_tick:
            time.sleep(0.1)
        dropped_at = last_live_tick(out / "host-live.jsonl")
        check("host_dropped_at_the_kill_tick", host.poll() is None and dropped_at >= options.kill_tick, f"host live tick {dropped_at}")
        if host.poll() is None:
            host.terminate(reason=f"scenario host drop after live tick {dropped_at}")
        for name in ("clienta", "clientb", "host"):
            records[name] = runs[name].finish()
    finally:
        for run in runs.values():
            try:
                run.close()
            except Exception:
                pass
        if service is not None:
            service.terminate()
            try:
                service.wait(timeout=10)
            except subprocess.TimeoutExpired:
                service.kill()

    logs = {name: (out / name / "stdout.log").read_text(encoding="utf-8", errors="replace") if (out / name / "stdout.log").is_file() else ""
            for name in runs}
    declared = {name: HOSTED.findall(logs[name]) for name in ("clienta", "clientb")}
    verdict["declared"] = declared
    check("both_survivors_declare_one_handover", len(declared["clienta"]) == 1 and declared["clienta"] == declared["clientb"], declared)
    # The survivor that dialed is the one whose lever skipped a LAN entry; the other is the successor answering on the directory.
    skipped = [name for name in ("clienta", "clientb") if LAN_SKIPPED in logs[name]]
    dialer = skipped[0] if len(skipped) == 1 else ""
    successor = next((name for name in ("clienta", "clientb") if name != dialer), "") if dialer else ""
    dial_lines = {name: [line for line in logs[name].splitlines() if line.startswith(ICE_DIAL)] for name in ("clienta", "clientb")}
    verdict["ice_dials"] = dial_lines
    check("one_survivor_skipped_the_lan_entry", bool(dialer), skipped)
    check("the_survivor_dialed_the_successors_ice_route", bool(dialer and dial_lines[dialer]), (dialer, dial_lines.get(dialer, [])[:1]))
    check("the_successor_answered_on_the_directory", bool(successor and HOST_END in logs[successor]),
          (successor, next((line for line in logs.get(successor, "").splitlines() if HOST_END in line), "")))
    for name in ("clienta", "clientb"):
        record = records.get(name, {})
        check(f"{name}_exit_zero", record.get("exit_code") == 0 and not record.get("timed_out"), {k: record.get(k) for k in ("exit_code", "timed_out")})
    live = compare_live_hashes(out / "clienta-live.jsonl", out / "clientb-live.jsonl", 1)
    verdict["survivor_live_hashes"] = live
    check("survivor_hashes_equal", bool(live) and all(row["compared_ticks"] > 0 and row["mismatched_ticks"] == 0 and row["mismatched_applied_input_ticks"] == 0 for row in live),
          [{k: row.get(k) for k in ("compared_ticks", "mismatched_ticks", "last_tick")} for row in live])
    verdict["pass"] = all(row["ok"] for row in verdict["checks"])
    (out / "verdict.json").write_text(json.dumps(verdict, indent=2, default=str) + "\n", encoding="utf-8")
    print(f"GATE migration-ice arm={options.arm} {'PASS' if verdict['pass'] else 'FAIL'} {out / 'verdict.json'}", flush=True)
    return 0 if verdict["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
