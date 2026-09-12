"""Two-process loopback e2e: a host registers an ICE-capable row, a client joins it by session id.

The session directory runs on 127.0.0.1 with a local certificate; the host advertises a row whose
join_mode is "either" and the client is given only `-net-join-session <id>`. Nothing here opens a
socket itself and no STUN or TURN server is contacted: the only outbound traffic is to the loopback
directory. Every engine process goes through tools/run_sim_test.py's make_run, which gives it a
private writable runtime, a muted private desktop and its own Temp.

  python tools/test_directory_ice_join.py --out D:/mx/w123/ice-e2e
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import ssl
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run  # noqa: E402

SERVICE = REPO / "tools" / "session_directory" / "session_directory.py"


def sha256(path: Path) -> str:
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def patch_settings(runtime: Path, values: dict[str, str]) -> None:
    """Adds or replaces settings keys in a runtime prepared by make_run, before the process starts."""
    ini = runtime / "Userdata" / "Settings.ini"
    text = ini.read_text(encoding="utf-8-sig")
    for name, value in values.items():
        pattern = rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*"
        text, count = re.subn(pattern, lambda m: m[1] + value, text)
        if count == 0:
            text += f"\n\t{name} = {value}\n"
    ini.write_text(text, encoding="utf-8")


def wait_engine_free(budget_s: float = 7200) -> None:
    start = time.time()
    while time.time() - start < budget_s:
        for lock in (Path(r"D:\mx\LEAD_BATTERY.lock"), Path(r"D:\mx\LEAD_EXCLUSIVE.lock")):
            if lock.exists():
                break
        else:
            out = subprocess.run(
                ["pwsh", "-NoProfile", "-Command",
                 "@(Get-Process 'Cortex Command*' -ErrorAction SilentlyContinue).Count"],
                capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
            if out.stdout.strip() in ("0", ""):
                return
        print("[ice-e2e] waiting for the shared engine", flush=True)
        time.sleep(30)
    raise SystemExit("gave up waiting for a free engine")


def start_service(out: Path, port: int, cert: Path, key: Path) -> subprocess.Popen:
    log = out / "service.log"
    handle = (out / "service-stdout.log").open("w", encoding="utf-8")
    proc = subprocess.Popen(
        [sys.executable, str(SERVICE), "--bind", "127.0.0.1", "--port", str(port),
         "--cert", str(cert), "--key", str(key), "--log-file", str(log),
         "--expiry-s", "120", "--heartbeat-s", "5"],
        stdout=handle, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    last = ""
    for _ in range(100):
        try:
            request = urllib.request.Request(f"https://127.0.0.1:{port}/v1/sessions",
                                             headers={"X-Install-Key": "w123icee2ekey000"})
            with urllib.request.urlopen(request, timeout=2, context=context) as reply:
                if reply.status == 200:
                    return proc
                last = f"status {reply.status}"
        except Exception as exc:
            last = f"{type(exc).__name__}: {exc}"
        time.sleep(0.2)
    proc.terminate()
    raise SystemExit(f"the session directory did not come up on 127.0.0.1: {last}")


def list_sessions(port: int) -> list[dict]:
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    request = urllib.request.Request(f"https://127.0.0.1:{port}/v1/sessions",
                                     headers={"X-Install-Key": "w123icee2ekey000"})
    with urllib.request.urlopen(request, timeout=5, context=context) as reply:
        return json.loads(reply.read().decode("utf-8")).get("sessions", [])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=0, help="0 picks a free loopback port")
    parser.add_argument("--game-port", type=int, default=41210)
    parser.add_argument("--ticks", type=int, default=600)
    parser.add_argument("--cert", type=Path, default=Path(r"D:\mx\w75\cert.pem"))
    parser.add_argument("--key", type=Path, default=Path(r"D:\mx\w75\key.pem"))
    parser.add_argument("--timeout", type=float, default=420)
    options = parser.parse_args()

    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if options.port == 0:
        import socket
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            options.port = probe.getsockname()[1]
        print(f"[ice-e2e] picked loopback port {options.port}", flush=True)
    pin = sha256(options.cert.with_suffix(".der")) if options.cert.with_suffix(".der").exists() else ""
    # The settings reader treats "//" as a line comment, so the value is written without a scheme;
    # NetDirectoryClient puts https:// back on.
    directory_url = f"127.0.0.1:{options.port}"
    settings = {
        "SessionDirectoryUrl": directory_url,
        "SessionDirectoryCertSha256": pin,
        "NetworkIceEnable": "1",
        "NetworkStunServers": "",
    }
    verdict = {"directory_url": directory_url, "cert_pin": pin, "ticks": options.ticks,
               "executable_sha256": sha256(REPO / "Cortex Command.exe"), "checks": []}

    def check(name, ok, detail=""):
        verdict["checks"].append({"check": name, "ok": bool(ok), "detail": str(detail)})
        print(f"[ice-e2e] {'OK  ' if ok else 'FAIL'} {name} {detail}", flush=True)
        return bool(ok)

    service = start_service(out, options.port, options.cert, options.key)
    host = client = None
    try:
        wait_engine_free()
        common = ["-net-match-service-e2e", "-net-port", str(options.game_port),
                  "-net-match-ticks", str(options.ticks), "-net-ice", "on",
                  "-tick-hashes", "-seed", "42"]
        host = make_run(REPO, [*common, "-net-host",
                               "-net-match-report", str(out / "host_report.json"),
                               "-out", str(out / "host_trace.json")],
                        out / "host", options.timeout)
        patch_settings(Path(host.cwd), settings)
        host.start()

        session_id = ""
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and not session_id:
            for row in list_sessions(options.port):
                if row.get("session_id"):
                    session_id = row["session_id"]
                    verdict["row"] = row
                    break
            if host.poll() is not None:
                break
            time.sleep(0.5)

        check("host_registered_a_row", bool(session_id), session_id)
        check("row_join_mode_either", verdict.get("row", {}).get("join_mode") == "either",
              verdict.get("row", {}).get("join_mode"))
        if not session_id:
            raise SystemExit(1)

        client = make_run(REPO, [*common, "-net-join-session", session_id,
                                 "-net-match-report", str(out / "client_report.json"),
                                 "-out", str(out / "client_trace.json")],
                          out / "client", options.timeout)
        patch_settings(Path(client.cwd), settings)
        client.start()
        client_record = client.finish()
        host_record = host.finish()
        verdict["host_exit"] = host_record.get("exit_code")
        verdict["client_exit"] = client_record.get("exit_code")
    finally:
        for run in (client, host):
            if run is not None:
                try:
                    run.close()
                except Exception:
                    pass
        service.terminate()
        try:
            service.wait(timeout=10)
        except subprocess.TimeoutExpired:
            service.kill()

    host_log = (out / "host" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    client_log = (out / "client" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    (out / "host_ice_lines.txt").write_text(
        "\n".join(l for l in host_log.splitlines() if "[net-ice]" in l), encoding="utf-8")
    (out / "client_ice_lines.txt").write_text(
        "\n".join(l for l in client_log.splitlines() if "[net-ice]" in l), encoding="utf-8")

    for side, path in (("host", out / "host_report.json"), ("client", out / "client_report.json")):
        if path.exists():
            report = json.loads(path.read_text(encoding="utf-8"))
            verdict[side + "_ice"] = report.get("service", {}).get("ice")
            verdict[side + "_p2p"] = report.get("p2p")

    check("host_ice_line", "[net-ice] host session" in host_log,
          next((l for l in host_log.splitlines() if "[net-ice]" in l), ""))
    check("client_resolved_the_session", "[net-ice] session" in client_log,
          next((l for l in client_log.splitlines() if "[net-ice]" in l), ""))
    check("host_exit_zero", verdict.get("host_exit") == 0, verdict.get("host_exit"))
    check("client_exit_zero", verdict.get("client_exit") == 0, verdict.get("client_exit"))

    traces = [out / "host_trace.json", out / "client_trace.json"]
    if all(p.exists() for p in traces):
        # The round runs the cap plus the start frame, so the tick count is ticks + 1.
        compare = subprocess.run(
            [sys.executable, str(REPO / "tools" / "compare_sim_traces.py"),
             str(traces[0]), str(traces[1]), "--expected-ticks", str(options.ticks + 1),
             "--json", str(out / "compare.json")],
            capture_output=True, text=True)
        text = compare.stdout + compare.stderr
        (out / "compare.txt").write_text(text, encoding="utf-8")
        check("traces_identical", compare.returncode == 0 and text.startswith("PASS"),
              text.strip().splitlines()[-1:] or "")
    else:
        check("traces_written", False, "a trace is missing")

    verdict["pass"] = all(c["ok"] for c in verdict["checks"])
    (out / "verdict.json").write_text(json.dumps(verdict, indent=2), encoding="utf-8")
    print("GATE ice-session-join " + ("PASS" if verdict["pass"] else "FAIL") + " " + str(out / "verdict.json"))
    return 0 if verdict["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
