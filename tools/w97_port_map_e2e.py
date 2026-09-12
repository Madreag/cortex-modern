"""W97 copy of the w85 directory e2e driver, repointed at this lane and its port-map fakes.

Declared differences from w85-identity-lua-states/w85_e2e.py (nothing else changed in shape):
  * REPO defaults to this worktree and RUN_ROOT to this lane's scratch (D:/mx/w97/e2e).
  * SERVICE_PORT 8469 (the directory), GAME_PORT 47603, and the lane's loopback fakes:
    tools/net_port_map_fake.py on UDP 47601 (NAT-PMP/PCP) and HTTP 8467 (the fake IGD).
  * Arms are "on" / "off" for NetworkPortMapEnable: the host's argv gains
    "-net-port-map on -net-port-map-gateway 127.0.0.1:47601 -net-port-map-igd
    http://127.0.0.1:8467/rootDesc.xml" in the on arm and "-net-port-map off" in the off arm,
    so no packet leaves loopback in either.
  * sample_loop records listen_addrs / join_mode / observed_ip per row, and the verdict quotes
    the last registered row of each arm plus the host's report.service.port_map section.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import re
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path


def _early(flag: str, default: str) -> str:
    return sys.argv[sys.argv.index(flag) + 1] if flag in sys.argv else default


REPO = Path(_early("--repo", "D:/Projects/h4-secondary"))
RUN_ROOT = Path(_early("--run-root", "D:/mx/w97/e2e"))
CERT = Path("D:/mx/w75/cert.pem")
KEY = Path("D:/mx/w75/key.pem")
SERVICE_PORT = 8469
FAKE_UDP_PORT = 47601
FAKE_IGD_PORT = 8467
GAME_PORT = 47603
TICKS = 600
TIMEOUT_S = 420.0
POLL_KEY = "w97pollkey-0123456789abcdef"  # the driver's own GETs; never written into a runtime
IGD_URL = f"http://127.0.0.1:{FAKE_IGD_PORT}/rootDesc.xml"

sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run  # noqa: E402
from compare_sim_traces import strict_compare  # noqa: E402

REFUSAL = re.compile(r"session_identity_hash|deterministic_config_hash|reject|refus|mismatch|incompatible", re.I)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cert_pin() -> str:
    der = subprocess.run(["openssl", "x509", "-in", str(CERT), "-outform", "DER"], capture_output=True, check=True).stdout
    return hashlib.sha256(der).hexdigest()


def write_directory_settings(runtime: Path, url: str, pin: str, install_key: str | None) -> str:
    settings = runtime / "Userdata" / "Settings.ini"
    text = settings.read_text(encoding="utf-8")
    text += f"\n\tSessionDirectoryUrl = {url}\n\tSessionDirectoryCertSha256 = {pin}\n"
    if install_key:
        text += f"\tSessionDirectoryInstallKey = {install_key}\n"
    settings.write_text(text, encoding="utf-8")
    return sha256_file(settings)


def settings_after(runtime: Path) -> dict:
    settings = runtime / "Userdata" / "Settings.ini"
    text = settings.read_text(encoding="utf-8", errors="replace")
    key = re.search(r"SessionDirectoryInstallKey = ([0-9a-f]{32})\s*$", text, re.M)
    return {"sha256": sha256_file(settings), "lines": text.count("\n"), "install_key": key.group(1) if key else None}


def peer_args(root: Path, who: str, port_map_on: bool) -> list[str]:
    args = [
        "-net-match-service-e2e", "-net-port", str(GAME_PORT), "-net-match-ticks", str(TICKS),
        "-net-reconnect-ticket", str(root / f"{who}.ticket"), "-net-match-peers", "2", "-tick-hashes",
        "-max-ticks", str(TICKS), "-out", str(root / f"{who}_trace.json"), "-net-match-report", str(root / f"{who}_report.json"),
    ]
    if who == "host":
        if port_map_on:
            args += ["-net-port-map", "on", "-net-port-map-gateway", f"127.0.0.1:{FAKE_UDP_PORT}", "-net-port-map-igd", IGD_URL]
        else:
            args += ["-net-port-map", "off"]
    return args


def directory_sessions() -> list[dict]:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    conn = http.client.HTTPSConnection("127.0.0.1", SERVICE_PORT, context=ctx, timeout=5)
    try:
        conn.request("GET", "/v1/sessions", headers={"X-Install-Key": POLL_KEY})
        reply = conn.getresponse()
        if reply.status != 200:
            return []
        return json.loads(reply.read().decode()).get("sessions", [])
    except Exception:
        return []
    finally:
        conn.close()


def sample_loop(out_path: Path, stop: threading.Event) -> None:
    seen: set[tuple] = set()
    with out_path.open("w", encoding="utf-8") as handle:
        while not stop.is_set():
            for row in directory_sessions():
                key = (row.get("session_id"), row.get("state"), row.get("peer_count"), row.get("seats_free"),
                       json.dumps(row.get("listen_addrs")), row.get("join_mode"))
                if key not in seen:
                    seen.add(key)
                    handle.write(json.dumps({"t": round(time.time(), 2), "name": row.get("name"), "state": row.get("state"),
                                             "peer_count": row.get("peer_count"), "seats_free": row.get("seats_free"),
                                             "session_id": row.get("session_id"), "listen_port": row.get("listen_port"),
                                             "listen_addrs": row.get("listen_addrs"), "join_mode": row.get("join_mode"),
                                             "observed_ip": row.get("observed_ip")}) + "\n")
                    handle.flush()
            time.sleep(0.5)


def run_list(root: Path, url: str, pin: str) -> dict:
    out = root / "list_run"
    argv = ["-net-directory-list"]
    run = make_run(REPO, argv, out, 60)
    staged = write_directory_settings(run.cwd, url, pin, None)
    try:
        record = run.start().finish()
    finally:
        run.close()
    record["argv"] = argv
    record["stdout_lines"] = [l for l in (out / "stdout.log").read_text(encoding="utf-8", errors="replace").splitlines() if "[net-directory-list]" in l]
    record["settings_staged_sha256"] = staged
    record["settings_after"] = settings_after(run.cwd)
    return {k: record[k] for k in ("exit_code", "timed_out", "argv", "stdout_lines", "settings_staged_sha256", "settings_after") if k in record}


def run_arm(root: Path, url: str, pin: str, port_map_on: bool, with_list: bool) -> dict:
    root.mkdir(parents=True, exist_ok=True)
    argvs = {"host": [*peer_args(root, "host", port_map_on), "-net-host"],
             "client": [*peer_args(root, "client", port_map_on), "-net-join", "127.0.0.1"]}
    runs = {who: make_run(REPO, argv, root / who, TIMEOUT_S) for who, argv in argvs.items()}
    staged = {"host": write_directory_settings(runs["host"].cwd, url, pin, None), "client": sha256_file(runs["client"].cwd / "Userdata" / "Settings.ini")}
    records: dict = {}

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as exc:  # noqa: BLE001
            records[who] = {"error": repr(exc)}

    stop = threading.Event()
    samples = root / "samples.jsonl"
    sampler = threading.Thread(target=sample_loop, args=(samples, stop), daemon=True)
    sampler.start()
    host = threading.Thread(target=drive, args=("host",), daemon=True)
    host.start()
    list_record = None
    if with_list:
        deadline = time.time() + 120
        while time.time() < deadline:
            if samples.exists() and '"state": "lobby"' in samples.read_text(encoding="utf-8"):
                break
            time.sleep(0.5)
        else:
            print("WARN: never saw a lobby-state row", flush=True)
        list_record = run_list(root, url, pin)
    time.sleep(0.5)
    client = threading.Thread(target=drive, args=("client",), daemon=True)
    client.start()
    host.join()
    client.join()
    stop.set()
    sampler.join(timeout=10)
    after = {who: settings_after(run.cwd) for who, run in runs.items()}
    for run in runs.values():
        run.close()

    verdict: dict = {"url": url, "port_map": "on" if port_map_on else "off", "argv": argvs,
                     "records": {k: {"exit_code": v.get("exit_code"), "timed_out": v.get("timed_out"), "error": v.get("error")} for k, v in records.items()}}
    reports = {}
    for who in ("host", "client"):
        p = root / f"{who}_report.json"
        reports[who] = json.loads(p.read_text()) if p.exists() else {}
    verdict["host_service_directory"] = reports["host"].get("service", {}).get("directory")
    verdict["client_service_directory"] = reports["client"].get("service", {}).get("directory")
    verdict["host_service_port_map"] = reports["host"].get("service", {}).get("port_map")
    verdict["running_ticks"] = {who: reports[who].get("running_ticks") for who in reports}
    for who in ("host", "client"):
        log = root / who / "stdout.log"
        text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        err = root / who / "stderr.log"
        if err.exists():
            text += err.read_text(encoding="utf-8", errors="replace")
        verdict[f"{who}_port_map_lines"] = [l for l in text.splitlines() if "[net-port-map]" in l] if text else None
        verdict[f"{who}_directory_lines"] = [l for l in text.splitlines() if "[net-directory]" in l] if text else None
        verdict[f"{who}_refusal_lines"] = [l for l in text.splitlines() if REFUSAL.search(l)] if text else None
    if samples.exists():
        rows = [json.loads(l) for l in samples.read_text(encoding="utf-8").splitlines() if l.strip()]
        verdict["registered_rows"] = rows
    verdict["settings"] = {who: {"staged_sha256": staged[who], **after[who], "unchanged": staged[who] == after[who]["sha256"]} for who in ("host", "client")}
    ht, ct = root / "host_trace.json", root / "client_trace.json"
    verdict["trace_compare"] = strict_compare(str(ht), str(ct)) if ht.exists() and ct.exists() else "missing traces"
    if list_record is not None:
        verdict["list"] = list_record
    (root / "verdict.json").write_text(json.dumps(verdict, indent=2, default=str), encoding="utf-8")
    return verdict


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arms", default="on,off")
    parser.add_argument("--repo", default=str(REPO))
    parser.add_argument("--run-root", default=str(RUN_ROOT))
    parser.add_argument("--natpmp-mode", default="ok")
    parser.add_argument("--pcp-mode", default="ok")
    options = parser.parse_args()
    RUN_ROOT.mkdir(parents=True, exist_ok=True)
    print("repo", REPO, "run root", RUN_ROOT, flush=True)
    pin = cert_pin()
    print("cert pin", pin, "(file says", Path("D:/mx/w75/cert-sha256.txt").read_text().strip() == pin, ")")
    log = RUN_ROOT / "service.log"
    fake_log = RUN_ROOT / "fake.log"
    for path in (log, fake_log):
        if path.exists():
            path.unlink()
    svc = subprocess.Popen([sys.executable, str(REPO / "tools/session_directory/session_directory.py"), "--bind", "127.0.0.1", "--port", str(SERVICE_PORT),
                            "--cert", str(CERT), "--key", str(KEY), "--log-file", str(log), "--heartbeat-s", "2", "--expiry-s", "20"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fake = subprocess.Popen([sys.executable, str(REPO / "tools/net_port_map_fake.py"), "--natpmp-port", str(FAKE_UDP_PORT),
                             "--igd-port", str(FAKE_IGD_PORT), "--natpmp-mode", options.natpmp_mode, "--pcp-mode", options.pcp_mode,
                             "--log-file", str(fake_log)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(40):
            time.sleep(0.5)
            ready = log.exists() and "listening" in log.read_text(encoding="utf-8", errors="replace")
            ready = ready and fake_log.exists() and "igd http listening" in fake_log.read_text(encoding="utf-8", errors="replace")
            if ready:
                break
        else:
            print("service or fakes never listened")
            return 2
        results = {}
        for arm in options.arms.split(","):
            on = arm == "on"
            url = f"127.0.0.1:{SERVICE_PORT}"
            print(f"=== arm {arm} url={url!r} port_map={on}", flush=True)
            results[arm] = run_arm(RUN_ROOT / f"e2e-{arm}", url, pin, port_map_on=on, with_list=on)
            print(json.dumps(results[arm], indent=2, default=str), flush=True)
        (RUN_ROOT / "w97_verdict.json").write_text(json.dumps(results, indent=2, default=str), encoding="utf-8")
        print("service log lines:", len(log.read_text(encoding="utf-8", errors="replace").splitlines()))
        print("fake log lines:", len(fake_log.read_text(encoding="utf-8", errors="replace").splitlines()))
    finally:
        for proc in (svc, fake):
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
