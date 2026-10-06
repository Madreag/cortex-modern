"""The join list's rows are what a join uses, and its words stay true when the online list goes out of reach.

A host lists its lobby in a loopback session directory; a joiner's Join a Game screen reads that directory through a small
proxy this script controls, so the list the joiner sees changes exactly when the script says:

  reopen       the host closes its lobby and opens the same-looking game on another port. The joiner's list goes from the
               first listing to the second with no empty list between (the proxy holds the last list while the host moves).
               The row must follow the game to its new port, and Join must connect there.
  unreachable  the online list fails after the joiner has seen a game from it. The screen must not call that game one
               "on this network".

  python tools/test_join_list_reopen.py --check reopen --out <output-dir> --port 49832
"""

from __future__ import annotations

import argparse
import http.server
import json
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run, engine_executable  # noqa: E402
import test_directory_ice_join as directory  # noqa: E402
from edith_cross import make_cert  # noqa: E402
from test_menu_readback import spread, managed_case  # noqa: E402

DISCOVERY_PORT = 42115

LANDING = "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\n"


class ListProxy(http.server.ThreadingHTTPServer):
    """Forwards every request to the directory; the session list it can hold at its last answer or refuse."""

    daemon_threads = True

    def __init__(self, port, upstream, context):
        super().__init__(("127.0.0.1", port), ProxyHandler)
        self.upstream, self.mode, self.last_list, self.lists_served = upstream, "pass", None, 0
        self.socket = context.wrap_socket(self.socket, server_side=True)


class ProxyHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def answer(self, status, body, headers=()):
        self.send_response(status)
        for name, value in headers:
            if name.lower() not in ("content-length", "transfer-encoding", "connection"):
                self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def forward(self):
        server = self.server
        listing = self.command == "GET" and self.path.startswith("/v1/sessions?")
        if listing and server.mode == "fail":
            return self.answer(503, b'{"error":"unavailable"}', (("Content-Type", "application/json"),))
        if listing and server.mode == "hold" and server.last_list is not None:
            server.lists_served += 1
            return self.answer(200, server.last_list, (("Content-Type", "application/json"),))
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        request = urllib.request.Request(f"https://127.0.0.1:{server.upstream}{self.path}", data=body, method=self.command,
                                         headers={k: v for k, v in self.headers.items() if k.lower() not in ("host", "connection", "content-length")})
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        try:
            with urllib.request.urlopen(request, timeout=10, context=context) as reply:
                status, data, headers = reply.status, reply.read(), reply.getheaders()
        except urllib.error.HTTPError as error:
            status, data, headers = error.code, error.read(), error.headers.items()
        if listing and status == 200:
            server.last_list = data
            server.lists_served += 1
        self.answer(status, data, headers)

    do_GET = do_POST = do_PUT = do_DELETE = forward


@managed_case
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--check", choices=("reopen", "unreachable"), required=True)
    parser.add_argument("--port", type=int, default=49832, help="the first of four lane ports: directory, proxy, two game ports")
    if spread:
        spread.add_arguments(parser)
    options = parser.parse_args()
    if not spread:
        parser.error("join-list gates require the shared spread executor")
    options.spread = True
    spread.configure(options)
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    dir_port, proxy_port, first, second = options.port, options.port + 1, options.port + 2, options.port + 3
    marks = {name: root / f"{name}.mark" for name in ("hosted", "seen", "failing", "read", "swap", "rehosted", "swapped", "joined")}
    cert, key, pin = make_cert(root)
    service = directory.start_service(root, dir_port, cert, key)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(cert), str(key))
    proxy = ListProxy(proxy_port, dir_port, context)
    threading.Thread(target=proxy.serve_forever, daemon=True).start()
    common = {"SessionDirectoryCertSha256": pin, "NetworkIceEnable": "1", "NetworkStunServers": ""}
    host = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\nassert_substate HostSetup\n" + f"setup_host_port {first}\n"
            f"activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\ntouch_file {marks['hosted']}\n")
    client = (LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\nassert_substate JoinSetup\n"
              f"wait_row GameRowPort{first} 90\ntouch_file {marks['seen']}\n")
    if options.check == "unreachable":
        host += f"wait_file {marks['read']} 120\nexit\n"
        client += (f"wait_file {marks['failing']} 60\nwait_label LabelLanGames The online game list is unavailable\n"
                   "assert_label_absent LabelLanGames on this network\nassert_text_fits LabelLanGames\ndump_host_options\n"
                   f"touch_file {marks['read']}\nexit\n")
    else:
        host += (f"wait_file {marks['swap']} 120\nactivate ButtonMultiplayerLeave\nwait 10\nassert_substate Landing\n"
                 "activate ButtonMultiplayerHostGame\nwait_ms 400\nassert_substate HostSetup\n" + f"setup_host_port {second}\n"
                 f"activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\ntouch_file {marks['rehosted']}\n"
                 f"wait_connected 2 120\nwait_file {marks['joined']} 60\nexit\n")
        # Two list polls after the swap, then the row the player sees - the same-looking game - and Join.
        client += (f"wait_file {marks['swapped']} 180\nwait_ms 12000\nclick_row GameRow0\nwait 4\n"
                   "activate ButtonMultiplayerConnect\nwait_connected 2 60\nwait_substate Lobby 30\n"
                   f"touch_file {marks['joined']}\nexit\n")
    result = {"pass": False, "check": options.check, "exe_sha256": directory.sha256(engine_executable(options.repo)), "steps": []}
    runs = {}
    execution = None
    try:
        # Hold the same discovery port on the joiner's native machine, so every listed row remains the directory's.
        execution = spread.prepare_case(options.repo, root,
            [spread.Peer("host", os="any"),
             spread.Peer("client", os="windows", reviewed=True, block_udp=(DISCOVERY_PORT,))],
            spread.Match(first, dir_port, parameters={"lane": "menus",
                "directory": {"DIRECTORY_URL": f"127.0.0.1:{dir_port}", "DIRECTORY_PIN": pin, "DIRECTORY_ROOT": root},
                "peer_directory_urls": {"client": f"127.0.0.1:{proxy_port}"}, "join_by_session": False}))
        for who, text, url in (("host", host, f"127.0.0.1:{dir_port}"), ("client", client, f"127.0.0.1:{proxy_port}")):
            script = root / f"{who}-menu.txt"
            script.write_text(text, encoding="utf-8")
            run = execution.make_run(options.repo, ["-menu-script", str(script)], root / who, 600, env={"CCCP_HEADLESS": "1"})
            directory.patch_settings(Path(run.cwd), {**common, "SessionDirectoryUrl": url, "SessionDirectoryInstallKey": f"join-reopen-{who}-key"})
            runs[who] = run.start()
            time.sleep(2)

        def wait_mark(name, seconds):
            deadline = time.monotonic() + seconds
            while not marks[name].exists():
                if time.monotonic() > deadline or any(run.poll() is not None for run in runs.values()):
                    raise RuntimeError(f"never reached {name}")
                time.sleep(0.2)
            result["steps"].append(f"{name} {time.strftime('%I:%M:%S %p')}")

        wait_mark("seen", 150)
        if options.check == "unreachable":
            proxy.mode = "fail"
            marks["failing"].write_text("failing\n", encoding="utf-8")
        else:
            # The list stays at the host's first game while it moves, then shows its second game alone: never an empty list between.
            proxy.mode = "hold"
            marks["swap"].write_text("swap\n", encoding="utf-8")
            wait_mark("rehosted", 120)
            deadline = time.monotonic() + 60
            while True:
                ports = sorted(int(row.get("listen_port", 0)) for row in directory.list_sessions(dir_port))
                if ports == [second]:
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(f"the directory never listed the second game alone: {ports}")
                time.sleep(0.5)
            served = proxy.lists_served
            while proxy.lists_served < served + 1:
                time.sleep(0.2)
            proxy.mode = "pass"
            result["steps"].append(f"directory lists only port {second}; the proxy passes it on")
            marks["swapped"].write_text("swapped\n", encoding="utf-8")
        records = {who: run.finish() for who, run in runs.items()}
        result["records"] = {who: {"exit_code": record.get("exit_code"), "timed_out": record.get("timed_out")} for who, record in records.items()}
        result["pass"] = all(record.get("exit_code") == 0 and not record.get("timed_out") for record in records.values())
    except Exception as error:
        result["error"] = repr(error)
        for run in runs.values():
            if run.poll() is None:
                run.terminate()
    finally:
        proxy.shutdown()
        service.terminate()
        for run in runs.values():
            run.close()
    for who, run in runs.items():
        log = run.out / "stdout.log"
        if log.exists():
            result.setdefault("failures", {})[who] = [line for line in log.read_text(errors="replace").splitlines() if "[menu-script] FAILED" in line][-3:]
    receipt = execution.result() if execution else spread.read_json(root / "spread-result.json", {})
    result.update(topology="spread", peer_boxes=receipt.get("peer_boxes", {}), spread=receipt, proof=result["pass"])
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[join-list] {options.check} {'PASS' if result['pass'] else 'FAIL'} {root / 'result.json'}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
