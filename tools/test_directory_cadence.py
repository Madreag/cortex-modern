"""Three local directory rows; each invocation runs one pair of game engines."""

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import os
import sys
import json
from pathlib import Path
import re
import subprocess
import threading
import time
from unittest import mock

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from run_sim_test import engine_executable, make_run, seed_settings
from session_directory import session_directory as directory
from test_lobby_lifecycle import wait_for_log

if os.environ.get("CC_TEST_DIRECTORY_SOURCE"):
    directory_source = Path(os.environ["CC_TEST_DIRECTORY_SOURCE"])
    sys.path.insert(0, str(directory_source))
    directory_spec = importlib.util.spec_from_file_location("row_directory", directory_source / "session_directory.py")
    directory = importlib.util.module_from_spec(directory_spec)
    directory_spec.loader.exec_module(directory)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stage_activity(repo, run):
    module = Path(run.cwd) / "Mods" / "DirectoryCadence.rte"
    module.mkdir()
    source = (repo / "Data/Tests.rte/Activities.ini").read_text()
    activity = source[source.index("AddActivity = GAScripted"):source.index("// Determinism baseline")]
    activity = activity.replace("PresetName = Net Lifecycle Test", "PresetName = Directory Cadence")
    activity = activity.replace("Tests.rte/Activities/NetLifecycleTest.lua", "DirectoryCadence.rte/DirectoryCadence.lua")
    activity = activity.replace("IsTestActivity = 1", "IsTestActivity = 0")
    (module / "Index.ini").write_text("DataModule\n\tModuleName = Directory Cadence\n\tSupportedGameVersion = 7.0.0\n\tIncludeFile = DirectoryCadence.rte/Activities.ini\n")
    (module / "Activities.ini").write_text(activity)
    (module / "DirectoryCadence.lua").write_bytes((repo / "Data/Tests.rte/Activities/NetLifecycleTest.lua").read_bytes())


class Requests:
    def __init__(self, case):
        self.case = case
        self.lock = threading.Lock()
        self.rows = []
        self.wait_from = None
        self.wait_until = None
        self.host_key = None
        self.seats = {}
        self.alive_samples = []

    def describe(self, handler):
        path = handler.path.split("?")[0]
        kind = path.rsplit("/", 1)[-1]
        body = getattr(handler, "row_body", {})
        if path.endswith("/connections"):
            kind = body.get("operation", "bootstrap")
        key = handler.headers.get("X-Install-Key", "")
        seat = None
        if "participant" in body:
            seat = self.seats.get(body["participant"])
        if "signed_request" in body:
            try:
                json.loads(base64.b64decode(body["signed_request"]))
                # The request's player maps to the seat issued by this local service.
                seat = self.seats.get(body.get("participant"))
            except (ValueError, TypeError):
                pass
        return key, kind, seat

    def refusal(self, handler):
        key, kind, seat = self.describe(handler)
        now = time.monotonic()
        with self.lock:
            if handler.command == "POST" and handler.path == "/v1/sessions":
                self.host_key = key
            if self.case == "liveness" and kind == "heartbeat" and self.wait_from is None:
                self.wait_from, self.wait_until = now, now + 20
            if self.case == "admission" and kind == "issue" and handler.row_body.get("seat") == 1 and self.wait_from is None:
                self.wait_from, self.wait_until = now, now + 40
            waiting = self.wait_until is not None and now < self.wait_until
            if waiting and key == self.host_key:
                if self.case == "liveness" and kind == "heartbeat":
                    return 429, {"error": "rate_limited_key", "retry_after_s": 1}
                if self.case == "admission" and kind in ("signals", "signal", "issue"):
                    return 429, {"error": "rate_limited_key", "retry_after_s": max(1, int(self.wait_until - now + .999))}
        return None

    def record(self, handler, status, body):
        now = time.monotonic()
        key, kind, seat = self.describe(handler)
        with self.lock:
            if kind == "issue" and status == 200:
                self.seats[handler.row_body["participant"]] = handler.row_body["seat"]
            contents = json.loads(base64.b64decode(handler.row_body["signed_request"])) if "signed_request" in handler.row_body else {}
            query_only = contents.get("frame_tie", {}).get("query_only", False)
            self.rows.append({"method": handler.command, "query_only": query_only, "at_ms": handler.row_arrived_ms, "reply_at_ms": int(now * 1000), "role": "host" if key == self.host_key else "guest",
                              "path": re.sub(r"/[0-9a-f-]{36}(?=/|$)", "/<session>", handler.path.split("?")[0]),
                              "kind": kind, "seat": seat, "status": status,
                              "error": body.get("error", ""), "retry_after_s": body.get("retry_after_s", 0),
                              "retry_wait_s": body.get("retry_after_s", 0)})
            handler.row_event = self.rows[-1]


@contextmanager
def local_directory(root, case, port):
    root.mkdir()
    records = Requests(case)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.now(timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                   .serial_number(x509.random_serial_number()).not_valid_before(now - timedelta(minutes=1))
                   .not_valid_after(now + timedelta(days=1)).add_extension(x509.SubjectAlternativeName([
                       x509.DNSName("localhost"), x509.IPAddress(__import__("ipaddress").ip_address("127.0.0.1"))]), False)
                   .sign(key, hashes.SHA256()))
    cert, key_path = root / "cert.pem", root / "key.pem"
    cert.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                         serialization.NoEncryption()))
    pin = hashlib.sha256(certificate.public_bytes(serialization.Encoding.DER)).hexdigest()
    original = directory.make_handler

    def observed_handler(store):
        if case != "flood":
            store.limiter._by_key.req_per_min = 10000
            store.limiter._by_ip.req_per_min = 20000
        parent = original(store)

        class Handler(parent):
            def parse_request(self):
                if not super().parse_request():
                    return False
                self.row_arrived_ms = int(time.monotonic() * 1000)
                self.row_body = {}
                self.row_raw = b""
                length = int(self.headers.get("Content-Length", "0"))
                if length:
                    self.row_raw = self.rfile.read(length)
                    self._body_read = True
                    self.server.body_complete()
                    self.row_body = json.loads(self.row_raw)
                return True

            def _read_json(self):
                return self.row_body

            def _install_gate(self, *args, **kwargs):
                return records.refusal(self) or super()._install_gate(*args, **kwargs)

            def send_header(self, name, value):
                if name.lower() == "retry-after":
                    if case == "admission" and self.row_status == 429:
                        value = str(max(1, int(records.wait_until - time.monotonic() + .999)))
                    self.row_header = True
                    self.row_event["header_retry_after_s"] = int(value)
                    self.row_event["retry_wait_s"] = max(self.row_event["retry_after_s"], int(value))
                super().send_header(name, value)

            def end_headers(self):
                if case == "admission" and self.row_status == 429 and not self.row_header:
                    self.send_header("Retry-After", "1")
                super().end_headers()

            def _send(self, status, body):
                self.row_status, self.row_header = status, False
                if case == "admission" and status == 429:
                    body = dict(body, retry_after_s=1)
                records.record(self, status, body)
                super()._send(status, body)

        return Handler

    previous = set(directory.LOGGER.handlers)
    with mock.patch.object(directory, "make_handler", observed_handler):
        server = directory.spawn_server(port=port, cert=cert, key=key_path, insecure_http=False,
                                        expiry_s=15, heartbeat_s=5, log_file=root / "service.log",
                                        create_owner_key=True, caller_mode="direct")
    try:
        yield server, records, pin
    finally:
        server.stop()
        (root / "requests.json").write_text(json.dumps(records.rows, indent=2) + "\n")
        for handler in set(directory.LOGGER.handlers) - previous:
            directory.LOGGER.removeHandler(handler)
            handler.close()


def menus(root, port, case):
    common = "in_play\nwait_ms 1000\nactivate ButtonMainToMultiplayer\nwait_ms 300\n"
    host = common + (
        "settext TextMultiplayerName Host\nactivate ButtonMultiplayerHostGame\nwait_ms 300\n"
        "combo_select ComboHostActivity Directory Cadence\nwait_ms 300\n"
        f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\nactivate ButtonMultiplayerCreate\n"
        "wait_connected 2 80\nwait_remote_ready 80\nactivate ButtonMultiplayerStart\n"
        "wait_lockstep 180 60\n" + ("wait_lockstep 1200 110\n" if case == "flood" else "wait_lockstep 2100 80\n" if case == "liveness" else "wait_lockstep 240 15\n") +
        "open_local_pause\nactivate ButtonEndMatch\nactivate ButtonLeaveConfirm\nwait_round_end 10\nwait_ms 1000\nexit\n")
    guest = common + (
        "settext TextMultiplayerName Guest\nactivate ButtonMultiplayerJoinGame\nwait_ms 300\n"
        "activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\n"
        f"settext TextJoinPort {port}\nactivate ButtonJoinAddressGo\nwait_connected 2 80\n"
        "activate ButtonMultiplayerReady\nwait_lockstep 180 60\n" +
        ("wait_lockstep 1200 110\n" if case == "flood" else "wait_lockstep 2100 80\n" if case == "liveness" else "wait_lockstep 240 15\n") + "wait_round_end 10\nexit\n")
    paths = {}
    for role, content in (("host", host), ("guest", guest)):
        paths[role] = root / (role + ".menu.txt")
        paths[role].write_text(content)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=("flood", "liveness", "admission"), required=True)
    parser.add_argument("--port", type=int, default=41841)
    parser.add_argument("--source-tip", default="")
    args = parser.parse_args()
    root, repo = args.out.resolve(), args.repo.resolve()
    root.mkdir(exist_ok=False)
    paths = menus(root, args.port, args.case)
    runs, records, checks, details = {}, {}, {}, {}
    result = {"case": args.case, "source_tip": args.source_tip or subprocess.check_output(
                  ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
              "binary_sha256": digest(engine_executable(repo)), "directory_sha256": digest(directory.__file__), "driver_sha256": digest(__file__),
              "topology": "single-box", "public_directory": False}
    requests = None
    try:
        with local_directory(root / "directory", args.case, args.port + 1) as (server, requests, pin):
            for role in ("host", "guest"):
                if role == "guest":
                    deadline = time.monotonic() + 20
                    while not server.store._sessions and time.monotonic() < deadline:
                        time.sleep(.05)
                    with server.store._lock:
                        session = next(iter(server.store._sessions))
                    paths[role].write_text(paths[role].read_text().replace("TextJoinAddress 127.0.0.1", "TextJoinAddress session:" + session))
                out = root / role
                env = {"CC_TEST_LINK_BLACKOUT": "2:180:60000"} if args.case == "flood" and role == "guest" else {}
                run = make_run(repo, ["-module", "DirectoryCadence.rte", "-menu-script", paths[role], "-num-lua-states", "4", "-net-match-ticks", "0",
                                     "-net-match-report", out / "match-report.json", "-net-live-tick-hashes", out / "live.jsonl"],
                               out, timeout=145, env=env)
                seed_settings(run, {"SessionDirectoryUrl": f"127.0.0.1:{args.port + 1}",
                                    "SessionDirectoryCertSha256": pin, "NetworkHostVisibility": 1,
                                    "NetworkIceEnable": 0 if args.case == "liveness" else 1,
                                    "NetworkConnectionMode": 1, "NetworkHostRelayMode": 0,
                                    "NetworkStunServers": "", "NetworkInputDelayFrames": 3})
                stage_activity(repo, run)
                runs[role] = run.start()
                if role == "host":
                    wait_for_log(run, "activate ButtonMultiplayerCreate click ButtonMultiplayerCreate PASS", 45)
            with ThreadPoolExecutor(max_workers=2) as pool:
                finished = {role: pool.submit(run.finish) for role, run in runs.items()}
                while not all(future.done() for future in finished.values()):
                    with server.store._lock:
                        requests.alive_samples.append({"at_ms": int(time.monotonic() * 1000),
                                                       "live_rows": len(server.store._sessions)})
                    time.sleep(.2)
                records = {role: future.result() for role, future in finished.items()}
        text = {role: (root / role / "stdout.log").read_text(errors="replace") for role in runs}
        reports = {role: json.loads((root / role / "match-report.json").read_text()) for role in runs
                   if (root / role / "match-report.json").is_file()}
        events = requests.rows
        checks["engines_exit"] = all(record["exit_code"] == 0 and not record.get("timed_out") for record in records.values())
        waits, early = {}, []
        for event in events:
            group = (event["role"], event["kind"])
            if event["at_ms"] < waits.get(group, 0):
                early.append(event)
            if event["status"] == 429:
                waits[group] = event.get("reply_at_ms", event["at_ms"]) + event["retry_wait_s"] * 1000
        checks["retry_deadlines"] = not early
        details.update(requests=len(events), inside_retry_wait=early[:20])
        if args.case == "flood":
            begin = re.search(r"\[net-link-blackout\] begin .*duration_ms=60000.*unix_ms=(\d+)", text["guest"])
            # A peer path row proves the engine was active while its acceptance stopped.
            waiting = "[input-acceptance-wait]" in text["guest"]
            seat_counts = {}
            for seat in (0, 1):
                stamps = [event["at_ms"] for event in events if event["kind"] in ("check-in", "host-change", "frame-tie") and event["seat"] == seat]
                peak = max((sum(start <= at < start + 60000 for at in stamps) for start in stamps), default=0)
                seat_counts[str(seat)] = peak
            busy = [event for event in events if event["error"] == "connection_busy"]
            ended = re.search(r"\[net-link-blackout\] end .*elapsed_ms=(\d+)", text["guest"])
            checks.update(acceptance_wait=bool(begin) and waiting,
                          blackout_completed=bool(ended) and 60000 <= int(ended[1]) <= 60050, bounded_seat_posts=max(seat_counts.values()) <= 12,
                          no_busy_seat=not busy)
            details.update(check_ins_per_60s=seat_counts, connection_busy=len(busy), blackout_elapsed_ms=int(ended[1]) if ended else None)
        elif args.case == "liveness":
            beats = [event for event in events if event["kind"] == "heartbeat" and event["status"] == 429]
            start, end = requests.wait_from * 1000, requests.wait_until * 1000
            samples = [sample for sample in requests.alive_samples if start <= sample["at_ms"] < end]
            registered = [event for event in events if event["role"] == "host" and event["method"] == "POST" and event["kind"] == "sessions" and event["status"] == 200]
            checks.update(refused_for_20s=bool(beats) and any(event["at_ms"] >= end for event in events), row_stayed_live=bool(samples) and all(sample["live_rows"] == 1 for sample in samples),
                          no_new_registration=len(registered) == 1,
                          no_missing_heartbeat=not any(event["kind"] == "heartbeat" and event["status"] == 404 for event in events))
            details.update(refused_heartbeats=len(beats), live_samples=samples, registers=len(registered))
        else:
            until = max((event.get("reply_at_ms", event["at_ms"]) + event["retry_wait_s"] * 1000 for event in events
                         if event["role"] == "host" and event["kind"] in ("issue", "signal", "signals") and event["status"] == 429), default=0)
            resumed = [event for event in events if event["role"] == "host" and event["at_ms"] >= until]
            checks.update(waited_40s=requests.wait_from is not None and any(
                              event["kind"] == "signals" and event["status"] == 429 for event in events),
                          admitted=all("[menu-script] connected:2 -> OK" in text[role] for role in runs),
                          no_premature_refusal=all("did not answer this seat request" not in value for value in text.values()),
                          truthful_wait="The directory is busy" in reports.get("guest", {}).get("directory_wait_line", ""),
                          admission_poll_first=bool(resumed) and resumed[0]["kind"] == "signals",
                          no_expired_seat=reports.get("host", {}).get("reconnect", {}).get("host_provisional_seats_expired") == 0)
            details.update(host_report=reports.get("host", {}))
    except Exception as error:
        checks["completed"] = False
        details["error"] = str(error)
    finally:
        for run in runs.values():
            run.close()
    result.update(pass_=bool(checks) and all(checks.values()), checks=checks, details=details, records=records)
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"pass": result["pass_"], "case": args.case, "checks": checks, "out": str(root)}), flush=True)
    return 0 if result["pass_"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
