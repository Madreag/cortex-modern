#!/usr/bin/python3
"""Tests for session_directory.py (loopback HTTP, optional TLS)."""

from __future__ import annotations

import base64
import http.client
import json
import logging
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
import uuid
from pathlib import Path
from typing import Any, Optional
from urllib.parse import quote

from session_directory import LOGGER, RunningServer, spawn_server

INSTALL_KEY = "0123456789abcdef"
HEX64_A = "a" * 64
HEX64_B = "b" * 64
HEX64_C = "c" * 64

REGISTER_RESP_KEYS = {
    "session_id",
    "token",
    "expires_in_s",
    "heartbeat_s",
    "observed_ip",
}
LIST_KEYS = {"sessions"}
LIST_ROW_KEYS = {
    "name",
    "activity",
    "scene",
    "mode",
    "peer_count",
    "seats_free",
    "game_version",
    "build_id",
    "network_protocol_version",
    "lockstep_codec_version",
    "controller_frame_version",
    "match_config_hash",
    "session_identity_hash",
    "module_manifest_hash",
    "listen_port",
    "listen_addrs",
    "join_mode",
    "session_id",
    "age_s",
    "observed_ip",
    "state",
}
HEARTBEAT_KEYS = {"expires_in_s", "heartbeat_s"}
DELETE_KEYS = {"ok"}
SIGNAL_POST_KEYS = {"ok", "seq"}
SIGNAL_GET_KEYS = {"signals"}
SIGNAL_ITEM_KEYS = {"seq", "from", "to", "payload_b64"}
RATE_KEYS = {"error", "retry_after_s"}
ERROR_KEYS = {"error"}
FIELD_ERROR_KEYS = {"error", "field"}


def sample_register(**overrides: object) -> dict[str, Any]:
    row: dict[str, Any] = {
        "name": "Erol",
        "activity": "P4 Alpha Duel",
        "scene": "Grasslands",
        "mode": "pvp-skirmish",
        "peer_count": 2,
        "seats_free": 1,
        "game_version": "7.0.0",
        "build_id": "stage2-p2d-local",
        "network_protocol_version": 1,
        "lockstep_codec_version": 20,
        "controller_frame_version": 6,
        "match_config_hash": HEX64_A,
        "session_identity_hash": HEX64_B,
        "module_manifest_hash": HEX64_C,
        "listen_port": 41010,
        "listen_addrs": ["192.168.1.20"],
        "join_mode": "ice",
        "ignored_extra": "drop-me",
    }
    row.update(overrides)
    return row


class DirectoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.server: Optional[RunningServer] = None
        self.tls_dir: Optional[tempfile.TemporaryDirectory[str]] = None
        self.use_tls = False

    def tearDown(self) -> None:
        if self.server is not None:
            self.server.stop()
            self.server = None
        if self.tls_dir is not None:
            self.tls_dir.cleanup()
            self.tls_dir = None

    def start(
        self,
        expiry_s: float = 15,
        heartbeat_s: float = 5,
        cert: Optional[Path] = None,
        key: Optional[Path] = None,
        queue_idle_s: Optional[float] = None,
    ) -> None:
        self.use_tls = cert is not None and key is not None
        kwargs: dict[str, Any] = {
            "bind": "127.0.0.1",
            "port": 0,
            "expiry_s": expiry_s,
            "heartbeat_s": heartbeat_s,
            "insecure_http": not self.use_tls,
            "cert": cert,
            "key": key,
        }
        if queue_idle_s is not None:
            kwargs["queue_idle_s"] = queue_idle_s
        try:
            self.server = spawn_server(**kwargs)
        except TypeError:
            kwargs.pop("queue_idle_s", None)
            self.server = spawn_server(**kwargs)
        self._wait_port()

    @property
    def port(self) -> int:
        assert self.server is not None
        return self.server.port

    def _wait_port(self) -> None:
        deadline = time.monotonic() + 2.0
        last: Optional[Exception] = None
        while time.monotonic() < deadline:
            try:
                if self.use_tls:
                    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    ctx.check_hostname = False
                    ctx.verify_mode = ssl.CERT_NONE
                    conn: http.client.HTTPConnection = http.client.HTTPSConnection(
                        "127.0.0.1", self.port, context=ctx, timeout=0.3
                    )
                else:
                    conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=0.3)
                conn.request(
                    "GET", "/v1/sessions", headers={"X-Install-Key": INSTALL_KEY}
                )
                conn.getresponse().read()
                conn.close()
                return
            except Exception as exc:
                last = exc
                time.sleep(0.05)
        raise RuntimeError(f"server did not accept on 127.0.0.1:{self.port}: {last}")

    def call(
        self,
        method: str,
        path: str,
        body: Optional[dict[str, Any]] = None,
        headers: Optional[dict[str, str]] = None,
        raw: Optional[bytes] = None,
        use_tls: bool = False,
        install_key: Optional[str] = INSTALL_KEY,
    ) -> tuple[int, Any]:
        hdrs = {"Content-Type": "application/json"}
        if install_key:
            hdrs["X-Install-Key"] = install_key
        if headers:
            hdrs.update(headers)
        payload: Optional[bytes] = None
        if raw is not None:
            payload = raw
        elif body is not None:
            payload = json.dumps(body).encode("utf-8")
        if use_tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            conn: http.client.HTTPConnection = http.client.HTTPSConnection(
                "127.0.0.1", self.port, context=ctx, timeout=5
            )
        else:
            conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            conn.request(method, path, body=payload, headers=hdrs)
            resp = conn.getresponse()
            data = resp.read()
            parsed = json.loads(data.decode("utf-8"))
            return resp.status, parsed
        finally:
            conn.close()

    def register(
        self,
        key: str = INSTALL_KEY,
        **overrides: object,
    ) -> tuple[int, dict[str, Any]]:
        status, body = self.call(
            "POST",
            "/v1/sessions",
            sample_register(**overrides),
            headers={"X-Install-Key": key},
        )
        self.assertIsInstance(body, dict)
        return status, body

    def list_sessions(
        self, query: str = "", headers: Optional[dict[str, str]] = None
    ) -> tuple[int, dict[str, Any]]:
        path = "/v1/sessions" if not query else f"/v1/sessions?{query}"
        status, body = self.call("GET", path, headers=headers)
        self.assertIsInstance(body, dict)
        return status, body

    def assert_keys(self, body: dict[str, Any], keys: set[str]) -> None:
        self.assertEqual(set(body.keys()), keys)

    def test_register_then_list(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        self.assert_keys(created, REGISTER_RESP_KEYS)
        self.assertEqual(created["expires_in_s"], 15)
        self.assertEqual(created["heartbeat_s"], 5)
        self.assertEqual(created["observed_ip"], "127.0.0.1")
        uuid.UUID(created["session_id"])
        self.assertIsInstance(created["token"], str)
        self.assertGreaterEqual(len(created["token"]), 16)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assert_keys(listed, LIST_KEYS)
        self.assertEqual(len(listed["sessions"]), 1)
        row = listed["sessions"][0]
        self.assert_keys(row, LIST_ROW_KEYS)
        self.assertNotIn("token", row)
        self.assertEqual(row["session_id"], created["session_id"])
        self.assertEqual(row["name"], "Erol")
        self.assertEqual(row["activity"], "P4 Alpha Duel")
        self.assertEqual(row["scene"], "Grasslands")
        self.assertEqual(row["mode"], "pvp-skirmish")
        self.assertEqual(row["peer_count"], 2)
        self.assertEqual(row["seats_free"], 1)
        self.assertEqual(row["game_version"], "7.0.0")
        self.assertEqual(row["build_id"], "stage2-p2d-local")
        self.assertEqual(row["network_protocol_version"], 1)
        self.assertEqual(row["lockstep_codec_version"], 20)
        self.assertEqual(row["controller_frame_version"], 6)
        self.assertEqual(row["match_config_hash"], HEX64_A)
        self.assertEqual(row["session_identity_hash"], HEX64_B)
        self.assertEqual(row["module_manifest_hash"], HEX64_C)
        self.assertEqual(row["listen_port"], 41010)
        self.assertEqual(row["listen_addrs"], ["192.168.1.20"])
        self.assertEqual(row["join_mode"], "ice")
        self.assertEqual(row["state"], "lobby")
        self.assertEqual(row["observed_ip"], "127.0.0.1")
        self.assertIsInstance(row["age_s"], int)
        self.assertGreaterEqual(row["age_s"], 0)
        self.assertNotIn("ignored_extra", row)

    def test_heartbeat_refreshes_expiry_and_seats(self) -> None:
        self.start(expiry_s=2, heartbeat_s=5)
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        time.sleep(1.2)
        status, beat = self.call(
            "POST",
            f"/v1/sessions/{sid}/heartbeat",
            {
                "token": created["token"],
                "peer_count": 3,
                "seats_free": 0,
                "listen_addrs": ["10.0.0.8"],
            },
        )
        self.assertEqual(status, 200)
        self.assert_keys(beat, HEARTBEAT_KEYS)
        self.assertEqual(beat["expires_in_s"], 2)
        self.assertEqual(beat["heartbeat_s"], 5)
        time.sleep(1.2)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assert_keys(listed, LIST_KEYS)
        self.assertEqual(len(listed["sessions"]), 1)
        row = listed["sessions"][0]
        self.assert_keys(row, LIST_ROW_KEYS)
        self.assertEqual(row["seats_free"], 0)
        self.assertEqual(row["peer_count"], 3)
        self.assertEqual(row["listen_addrs"], ["10.0.0.8"])

    def test_expiry_without_heartbeat(self) -> None:
        self.start(expiry_s=2)
        status, created = self.register()
        self.assertEqual(status, 200)
        self.assert_keys(created, REGISTER_RESP_KEYS)
        time.sleep(3.1)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assert_keys(listed, LIST_KEYS)
        self.assertEqual(listed["sessions"], [])

    def test_delete(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        status, deleted = self.call(
            "DELETE",
            f"/v1/sessions/{sid}",
            {"token": created["token"]},
        )
        self.assertEqual(status, 200)
        self.assert_keys(deleted, DELETE_KEYS)
        self.assertTrue(deleted["ok"])
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assert_keys(listed, LIST_KEYS)
        self.assertEqual(listed["sessions"], [])

    def test_bad_token_forbidden(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        status, err = self.call(
            "POST",
            f"/v1/sessions/{sid}/heartbeat",
            {"token": "not-the-session-token-value", "peer_count": 9, "seats_free": 9},
        )
        self.assertEqual(status, 403)
        self.assert_keys(err, ERROR_KEYS)
        self.assertEqual(err["error"], "forbidden")
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        row = listed["sessions"][0]
        self.assert_keys(row, LIST_ROW_KEYS)
        self.assertEqual(row["peer_count"], 2)
        self.assertEqual(row["seats_free"], 1)

    def test_wrong_install_key_format(self) -> None:
        self.start()
        status, err = self.register(key="short")
        self.assertEqual(status, 400)
        self.assert_keys(err, ERROR_KEYS)
        self.assertEqual(err["error"], "invalid_install_key")
        status, err = self.register(key="has spaces and!!")
        self.assertEqual(status, 400)
        self.assert_keys(err, ERROR_KEYS)
        self.assertEqual(err["error"], "invalid_install_key")
        status, err = self.call(
            "POST", "/v1/sessions", sample_register(), install_key=""
        )
        self.assertEqual(status, 400)
        self.assert_keys(err, ERROR_KEYS)
        self.assertEqual(err["error"], "invalid_install_key")

    def test_rate_limit(self) -> None:
        self.start()
        key_a = "aaaaaaaaaaaaaaaa"
        last_ok: Optional[dict[str, Any]] = None
        for _ in range(10):
            status, body = self.register(key=key_a)
            self.assertEqual(status, 200)
            self.assert_keys(body, REGISTER_RESP_KEYS)
            last_ok = body
        self.assertIsNotNone(last_ok)
        status, limited = self.register(key=key_a)
        self.assertEqual(status, 429)
        self.assert_keys(limited, RATE_KEYS)
        self.assertEqual(limited["error"], "rate_limited")
        self.assertIsInstance(limited["retry_after_s"], int)
        self.assertGreaterEqual(limited["retry_after_s"], 1)
        key_b = "bbbbbbbbbbbbbbbb"
        for _ in range(120):
            status, body = self.list_sessions(headers={"X-Install-Key": key_b})
            self.assertEqual(status, 200)
            self.assert_keys(body, LIST_KEYS)
        status, limited = self.list_sessions(headers={"X-Install-Key": key_b})
        self.assertEqual(status, 429)
        self.assert_keys(limited, RATE_KEYS)
        self.assertEqual(limited["error"], "rate_limited")
        self.assertIsInstance(limited["retry_after_s"], int)
        self.assertGreaterEqual(limited["retry_after_s"], 1)

    def test_filters(self) -> None:
        self.start()
        status, one = self.register(key="cccccccccccccccc", mode="pvp-skirmish", activity="Duel")
        self.assertEqual(status, 200)
        status, two = self.register(key="dddddddddddddddd", mode="coop", activity="Siege")
        self.assertEqual(status, 200)
        status, beat = self.call(
            "POST",
            f"/v1/sessions/{two['session_id']}/heartbeat",
            {"token": two["token"], "peer_count": 1, "seats_free": 3, "state": "running"},
        )
        self.assertEqual(status, 200)
        self.assert_keys(beat, HEARTBEAT_KEYS)
        status, by_mode = self.list_sessions("mode=pvp-skirmish")
        self.assertEqual(status, 200)
        self.assert_keys(by_mode, LIST_KEYS)
        self.assertEqual([row["session_id"] for row in by_mode["sessions"]], [one["session_id"]])
        self.assert_keys(by_mode["sessions"][0], LIST_ROW_KEYS)
        status, by_act = self.list_sessions(f"activity={quote('Siege')}")
        self.assertEqual(status, 200)
        self.assert_keys(by_act, LIST_KEYS)
        self.assertEqual([row["session_id"] for row in by_act["sessions"]], [two["session_id"]])
        status, by_state = self.list_sessions("state=running")
        self.assertEqual(status, 200)
        self.assert_keys(by_state, LIST_KEYS)
        self.assertEqual([row["session_id"] for row in by_state["sessions"]], [two["session_id"]])
        self.assertEqual(by_state["sessions"][0]["state"], "running")
        status, compat = self.list_sessions("compatible=1")
        self.assertEqual(status, 200)
        self.assert_keys(compat, LIST_KEYS)
        self.assertEqual(len(compat["sessions"]), 2)

    def test_running_row_stays_listed(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        status, beat = self.call(
            "POST",
            f"/v1/sessions/{created['session_id']}/heartbeat",
            {
                "token": created["token"],
                "peer_count": 4,
                "seats_free": 2,
                "state": "running",
            },
        )
        self.assertEqual(status, 200)
        self.assert_keys(beat, HEARTBEAT_KEYS)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assert_keys(listed, LIST_KEYS)
        self.assertEqual(len(listed["sessions"]), 1)
        row = listed["sessions"][0]
        self.assert_keys(row, LIST_ROW_KEYS)
        self.assertEqual(row["state"], "running")
        self.assertEqual(row["session_id"], created["session_id"])
        self.assertEqual(row["seats_free"], 2)

    def test_signal_round_trip_and_after(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        nonce = "joinNonce1"
        client_peer = f"client:{nonce}"
        up = base64.b64encode(b"client-hello").decode("ascii")
        down = base64.b64encode(b"host-reply").decode("ascii")
        status, posted = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": nonce,
                "from": client_peer,
                "to": "host",
                "payload_b64": up,
            },
        )
        self.assertEqual(status, 200)
        self.assert_keys(posted, SIGNAL_POST_KEYS)
        self.assertTrue(posted["ok"])
        self.assertEqual(posted["seq"], 1)
        status, host_q = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer=host&after=0&token={quote(token)}",
        )
        self.assertEqual(status, 200)
        self.assert_keys(host_q, SIGNAL_GET_KEYS)
        self.assertEqual(len(host_q["signals"]), 1)
        self.assert_keys(host_q["signals"][0], SIGNAL_ITEM_KEYS)
        self.assertEqual(host_q["signals"][0]["seq"], 1)
        self.assertEqual(host_q["signals"][0]["from"], client_peer)
        self.assertEqual(host_q["signals"][0]["to"], "host")
        self.assertEqual(host_q["signals"][0]["payload_b64"], up)
        status, again = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer=host&after=1&token={quote(token)}",
        )
        self.assertEqual(status, 200)
        self.assert_keys(again, SIGNAL_GET_KEYS)
        self.assertEqual(again["signals"], [])
        status, reply = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": token,
                "from": "host",
                "to": client_peer,
                "payload_b64": down,
            },
        )
        self.assertEqual(status, 200)
        self.assert_keys(reply, SIGNAL_POST_KEYS)
        self.assertEqual(reply["seq"], 1)
        status, client_q = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer={quote(client_peer, safe=':')}&after=0",
        )
        self.assertEqual(status, 200)
        self.assert_keys(client_q, SIGNAL_GET_KEYS)
        self.assertEqual(len(client_q["signals"]), 1)
        self.assert_keys(client_q["signals"][0], SIGNAL_ITEM_KEYS)
        self.assertEqual(client_q["signals"][0]["from"], "host")
        self.assertEqual(client_q["signals"][0]["to"], client_peer)
        self.assertEqual(client_q["signals"][0]["payload_b64"], down)
        status, drained = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer={quote(client_peer, safe=':')}&after=1",
        )
        self.assertEqual(status, 200)
        self.assert_keys(drained, SIGNAL_GET_KEYS)
        self.assertEqual(drained["signals"], [])

    def test_signal_queue_and_payload_caps(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        tiny = base64.b64encode(b"x").decode("ascii")
        last_seq = 0
        for i in range(256):
            status, posted = self.call(
                "POST",
                f"/v1/sessions/{sid}/signal",
                {
                    "token_or_join_nonce": token,
                    "from": "host",
                    "to": "client:cap",
                    "payload_b64": tiny,
                },
                install_key=f"q{i // 100:015d}",
            )
            self.assertEqual(status, 200, msg=f"signal {i+1}")
            self.assert_keys(posted, SIGNAL_POST_KEYS)
            last_seq = posted["seq"]
        self.assertEqual(last_seq, 256)
        status, full = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": token,
                "from": "host",
                "to": "client:cap",
                "payload_b64": tiny,
            },
        )
        self.assertEqual(status, 400)
        self.assert_keys(full, ERROR_KEYS)
        self.assertEqual(full["error"], "queue_full")
        ok_payload = base64.b64encode(b"z" * (64 * 1024)).decode("ascii")
        status, ok_body = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": token,
                "from": "host",
                "to": "client:roomy",
                "payload_b64": ok_payload,
            },
        )
        self.assertEqual(status, 200)
        self.assert_keys(ok_body, SIGNAL_POST_KEYS)
        over = base64.b64encode(b"z" * (64 * 1024 + 1)).decode("ascii")
        status, too_big = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": token,
                "from": "host",
                "to": "client:roomy2",
                "payload_b64": over,
            },
        )
        self.assertEqual(status, 413)
        self.assert_keys(too_big, ERROR_KEYS)
        self.assertEqual(too_big["error"], "payload_too_large")

    def test_malformed_json(self) -> None:
        self.start()
        status, err = self.call(
            "POST",
            "/v1/sessions",
            headers={"X-Install-Key": INSTALL_KEY},
            raw=b"{not-json",
        )
        self.assertEqual(status, 400)
        self.assert_keys(err, ERROR_KEYS)
        self.assertEqual(err["error"], "malformed_json")
        status, missing = self.call(
            "POST",
            "/v1/sessions",
            {"name": "Erol"},
            headers={"X-Install-Key": INSTALL_KEY},
        )
        self.assertEqual(status, 400)
        self.assert_keys(missing, FIELD_ERROR_KEYS)
        self.assertEqual(missing["error"], "missing_field")
        self.assertIsInstance(missing["field"], str)
        self.assertTrue(missing["field"])

    def test_tls_smoke(self) -> None:
        openssl = shutil.which("openssl")
        if openssl is None:
            self.skipTest("openssl not on PATH")
        self.tls_dir = tempfile.TemporaryDirectory()
        root = Path(self.tls_dir.name)
        cert = root / "cert.pem"
        key = root / "key.pem"
        proc = subprocess.run(
            [
                openssl,
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-keyout",
                str(key),
                "-out",
                str(cert),
                "-days",
                "1",
                "-nodes",
                "-subj",
                "/CN=127.0.0.1",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            self.skipTest(f"openssl failed: {proc.stderr.strip() or proc.stdout.strip()}")
        self.start(cert=cert, key=key)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        probe = http.client.HTTPSConnection(
            "127.0.0.1", self.port, context=ctx, timeout=5
        )
        try:
            probe.connect()
            sock = probe.sock
            self.assertIsNotNone(sock)
            assert sock is not None
            version = sock.version()
            self.assertIn(version, {"TLSv1.2", "TLSv1.3"})
            probe.request(
                "GET", "/v1/sessions", headers={"X-Install-Key": INSTALL_KEY}
            )
            probe.getresponse().read()
        finally:
            probe.close()
        status, created = self.call(
            "POST",
            "/v1/sessions",
            sample_register(),
            headers={"X-Install-Key": INSTALL_KEY},
            use_tls=True,
        )
        self.assertEqual(status, 200)
        self.assert_keys(created, REGISTER_RESP_KEYS)
        self.assertEqual(created["observed_ip"], "127.0.0.1")
        status, listed = self.call("GET", "/v1/sessions", use_tls=True)
        self.assertEqual(status, 200)
        self.assert_keys(listed, LIST_KEYS)
        self.assertEqual(listed["sessions"][0]["session_id"], created["session_id"])
        self.assert_keys(listed["sessions"][0], LIST_ROW_KEYS)

    def _self_signed_cert(self) -> tuple[Path, Path]:
        openssl = shutil.which("openssl")
        if openssl is None:
            self.skipTest("openssl not on PATH")
        self.tls_dir = tempfile.TemporaryDirectory()
        root = Path(self.tls_dir.name)
        cert = root / "cert.pem"
        key = root / "key.pem"
        proc = subprocess.run(
            [
                openssl,
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-keyout",
                str(key),
                "-out",
                str(cert),
                "-days",
                "1",
                "-nodes",
                "-subj",
                "/CN=127.0.0.1",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            self.skipTest(f"openssl failed: {proc.stderr.strip() or proc.stdout.strip()}")
        return cert, key

    def _tls_get_sessions(self, timeout: float) -> int:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        conn = http.client.HTTPSConnection(
            "127.0.0.1", self.port, context=ctx, timeout=timeout
        )
        try:
            conn.request(
                "GET", "/v1/sessions", headers={"X-Install-Key": INSTALL_KEY}
            )
            resp = conn.getresponse()
            resp.read()
            return resp.status
        finally:
            conn.close()

    def test_silent_tls_client_does_not_block(self) -> None:
        import session_directory as sd

        cert, key = self._self_signed_cert()
        self.start(cert=cert, key=key)
        silent = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        try:
            t0 = time.monotonic()
            try:
                status = self._tls_get_sessions(timeout=3.0)
            except OSError as exc:
                self.fail(f"second client blocked by silent connection: {exc}")
            self.assertEqual(status, 200)
            self.assertLess(time.monotonic() - t0, 1.0)
            handshake_timeout = getattr(sd, "HANDSHAKE_TIMEOUT_S", 5)
            silent.settimeout(handshake_timeout + 1.0)
            try:
                closed = silent.recv(1) == b""
            except (ConnectionResetError, ConnectionAbortedError):
                closed = True
            except OSError:
                closed = False
            self.assertTrue(closed, "silent connection not closed by server")
        finally:
            silent.close()

    def test_many_silent_tls_clients_do_not_block(self) -> None:
        cert, key = self._self_signed_cert()
        self.start(cert=cert, key=key)
        silents = [
            socket.create_connection(("127.0.0.1", self.port), timeout=5)
            for _ in range(16)
        ]
        try:
            t0 = time.monotonic()
            try:
                status = self._tls_get_sessions(timeout=3.0)
            except OSError as exc:
                self.fail(f"real request blocked by silent connections: {exc}")
            self.assertEqual(status, 200)
            self.assertLess(time.monotonic() - t0, 1.0)
        finally:
            for sock in silents:
                sock.close()

    def test_install_key_required_on_every_v1_endpoint(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        tiny = base64.b64encode(b"x").decode("ascii")
        cases: list[tuple[str, str, str, Optional[dict[str, Any]]]] = [
            ("GET", "/v1/sessions", "list", None),
            (
                "POST",
                f"/v1/sessions/{sid}/heartbeat",
                "heartbeat",
                {"token": token, "peer_count": 1, "seats_free": 1},
            ),
            ("DELETE", f"/v1/sessions/{sid}", "delete", {"token": token}),
            (
                "POST",
                f"/v1/sessions/{sid}/signal",
                "signal_post",
                {
                    "token_or_join_nonce": token,
                    "from": "host",
                    "to": "host",
                    "payload_b64": tiny,
                },
            ),
            (
                "GET",
                f"/v1/sessions/{sid}/signals?peer=host&after=0&token={quote(token)}",
                "signal_get",
                None,
            ),
        ]
        for method, path, label, body in cases:
            with self.subTest(endpoint=label, kind="missing"):
                status, err = self.call(method, path, body, install_key="")
                self.assertEqual(status, 400)
                self.assert_keys(err, ERROR_KEYS)
                self.assertEqual(err["error"], "invalid_install_key")
            with self.subTest(endpoint=label, kind="malformed"):
                status, err = self.call(method, path, body, install_key="bad key!!")
                self.assertEqual(status, 400)
                self.assert_keys(err, ERROR_KEYS)
                self.assertEqual(err["error"], "invalid_install_key")

    def test_per_ip_rate_limit(self) -> None:
        self.start()
        for i in range(30):
            status, body = self.register(key=f"{i:016d}")
            self.assertEqual(status, 200, msg=f"register {i+1}")
            self.assert_keys(body, REGISTER_RESP_KEYS)
        status, limited = self.register(key=f"{30:016d}")
        self.assertEqual(status, 429)
        self.assert_keys(limited, RATE_KEYS)
        self.assertEqual(limited["error"], "rate_limited")
        self.assertIsInstance(limited["retry_after_s"], int)
        self.assertGreaterEqual(limited["retry_after_s"], 1)
        assert self.server is not None
        self.server.stop()
        self.server = None
        self.start()
        for i in range(299):
            status, body = self.list_sessions(headers={"X-Install-Key": f"L{i:015d}"})
            self.assertEqual(status, 200, msg=f"list {i+1}")
            self.assert_keys(body, LIST_KEYS)
        status, limited = self.list_sessions(headers={"X-Install-Key": "L" + "x" * 15})
        self.assertEqual(status, 429)
        self.assert_keys(limited, RATE_KEYS)
        self.assertEqual(limited["error"], "rate_limited")
        self.assertIsInstance(limited["retry_after_s"], int)
        self.assertGreaterEqual(limited["retry_after_s"], 1)

    def test_signal_session_queue_caps_and_idle_drop(self) -> None:
        self.start(queue_idle_s=1.5)
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        tiny = base64.b64encode(b"x").decode("ascii")

        def post_to(dest: str, payload: str) -> tuple[int, dict[str, Any]]:
            status_i, body_i = self.call(
                "POST",
                f"/v1/sessions/{sid}/signal",
                {
                    "token_or_join_nonce": token,
                    "from": "host",
                    "to": dest,
                    "payload_b64": payload,
                },
            )
            self.assertIsInstance(body_i, dict)
            return status_i, body_i

        with self.subTest("dest_queue_cap"):
            for i in range(16):
                status, posted = post_to(f"client:q{i:02d}", tiny)
                self.assertEqual(status, 200, msg=f"dest {i+1}")
                self.assert_keys(posted, SIGNAL_POST_KEYS)
            status, full = post_to("client:q16", tiny)
            self.assertEqual(status, 400)
            self.assert_keys(full, ERROR_KEYS)
            self.assertEqual(full["error"], "queue_full")

        assert self.server is not None
        self.server.stop()
        self.server = None
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        chunk = base64.b64encode(b"z" * (64 * 1024)).decode("ascii")
        with self.subTest("session_payload_cap"):
            for i in range(16):
                status, posted = post_to(f"client:big", chunk)
                self.assertEqual(status, 200, msg=f"chunk {i+1}")
                self.assert_keys(posted, SIGNAL_POST_KEYS)
            status, full = post_to("client:big", tiny)
            self.assertEqual(status, 400)
            self.assert_keys(full, ERROR_KEYS)
            self.assertEqual(full["error"], "queue_full")

        assert self.server is not None
        self.server.stop()
        self.server = None
        self.start(expiry_s=30, queue_idle_s=1.5)
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        up = base64.b64encode(b"idle-payload").decode("ascii")
        status, posted = post_to("client:idle", up)
        self.assertEqual(status, 200)
        status, posted = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": "joinIdle1",
                "from": "client:joinIdle1",
                "to": "host",
                "payload_b64": up,
            },
        )
        self.assertEqual(status, 200)
        time.sleep(2.6)
        with self.subTest("idle_client_queue_dropped"):
            status, client_q = self.call(
                "GET",
                f"/v1/sessions/{sid}/signals?peer=client:idle&after=0",
            )
            self.assertEqual(status, 200)
            self.assert_keys(client_q, SIGNAL_GET_KEYS)
            self.assertEqual(client_q["signals"], [])
        with self.subTest("host_queue_kept"):
            status, host_q = self.call(
                "GET",
                f"/v1/sessions/{sid}/signals?peer=host&after=0&token={quote(token)}",
            )
            self.assertEqual(status, 200)
            self.assert_keys(host_q, SIGNAL_GET_KEYS)
            self.assertEqual(len(host_q["signals"]), 1)
            self.assertEqual(host_q["signals"][0]["payload_b64"], up)

    def test_slow_client_timeout_keeps_serving(self) -> None:
        self.start()
        finished = threading.Event()
        elapsed_s: list[float] = []
        recv_err: list[str] = []

        def stall() -> None:
            sock = socket.create_connection(("127.0.0.1", self.port), timeout=20)
            try:
                payload = (
                    "POST /v1/sessions HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    f"X-Install-Key: {INSTALL_KEY}\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: 50\r\n"
                    "\r\n"
                ).encode("ascii")
                sock.sendall(payload)
                sock.settimeout(15.0)
                t0 = time.monotonic()
                try:
                    sock.recv(4096)
                    elapsed_s.append(time.monotonic() - t0)
                except socket.timeout:
                    elapsed_s.append(time.monotonic() - t0)
                    recv_err.append("timeout")
            finally:
                sock.close()
                finished.set()

        worker = threading.Thread(target=stall, name="stall-client", daemon=True)
        worker.start()
        time.sleep(0.3)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assert_keys(listed, LIST_KEYS)
        worker.join(20.0)
        self.assertTrue(finished.is_set())
        self.assertEqual(recv_err, [])
        self.assertEqual(len(elapsed_s), 1)
        self.assertLessEqual(elapsed_s[0], 12.0)

    def test_tls_context_hardened(self) -> None:
        import session_directory as sd

        self.assertTrue(hasattr(sd, "make_server_ssl_context"))
        ctx = sd.make_server_ssl_context()
        self.assertEqual(ctx.minimum_version, ssl.TLSVersion.TLSv1_2)
        self.assertTrue(ctx.options & ssl.OP_NO_COMPRESSION)

    def test_signal_heartbeat_logs_id_and_client_not_body(self) -> None:
        records: list[str] = []

        class Capture(logging.Handler):
            def emit(self, record: logging.LogRecord) -> None:
                records.append(record.getMessage())

        handler = Capture()
        handler.setLevel(logging.INFO)
        LOGGER.addHandler(handler)
        try:
            self.start()
            status, created = self.register()
            self.assertEqual(status, 200)
            sid = created["session_id"]
            token = created["token"]
            marker = "BODY_MARKER_DO_NOT_LOG_9f3c"
            payload = base64.b64encode(marker.encode("ascii")).decode("ascii")
            status, posted = self.call(
                "POST",
                f"/v1/sessions/{sid}/signal",
                {
                    "token_or_join_nonce": token,
                    "from": "host",
                    "to": "client:log",
                    "payload_b64": payload,
                },
            )
            self.assertEqual(status, 200)
            status, beat = self.call(
                "POST",
                f"/v1/sessions/{sid}/heartbeat",
                {
                    "token": token,
                    "peer_count": 2,
                    "seats_free": 1,
                    "listen_addrs": [marker],
                },
            )
            self.assertEqual(status, 200)
            status, _host_q = self.call(
                "GET",
                f"/v1/sessions/{sid}/signals?peer=host&after=0&token={quote(token)}",
            )
            self.assertEqual(status, 200)
            joined = "\n".join(records)
            self.assertNotIn(marker, joined)
            hb = [line for line in records if line.startswith("heartbeat ")]
            sig = [line for line in records if line.startswith("signal ")]
            self.assertGreaterEqual(len(hb), 1)
            self.assertGreaterEqual(len(sig), 1)
            self.assertIn(sid, hb[0])
            self.assertIn("127.0.0.1", hb[0])
            self.assertIn(sid, sig[0])
            self.assertIn("127.0.0.1", sig[0])
            self.assertNotIn(token, hb[0])
            self.assertNotIn(token, sig[0])
            redacted = [line for line in records if "token=redacted" in line]
            self.assertGreaterEqual(len(redacted), 1)
            raw_token_hits = [line for line in records if token in line]
            self.assertEqual(raw_token_hits, [])
        finally:
            LOGGER.removeHandler(handler)

    def capture_log(self) -> list[str]:
        records: list[str] = []

        class Capture(logging.Handler):
            def emit(self, record: logging.LogRecord) -> None:
                records.append(record.getMessage())

        handler = Capture()
        handler.setLevel(logging.INFO)
        LOGGER.addHandler(handler)
        self.addCleanup(LOGGER.removeHandler, handler)
        return records

    def test_signal_peer_header_alternative(self) -> None:
        records = self.capture_log()
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        nonce = "headerNonce1"
        client_peer = f"client:{nonce}"
        down = base64.b64encode(b"host-reply").decode("ascii")
        up = base64.b64encode(b"client-hello").decode("ascii")
        for body in (
            {"token_or_join_nonce": token, "from": "host", "to": client_peer, "payload_b64": down},
            {"token_or_join_nonce": nonce, "from": client_peer, "to": "host", "payload_b64": up},
        ):
            status, _posted = self.call("POST", f"/v1/sessions/{sid}/signal", body)
            self.assertEqual(status, 200)
        signals = f"/v1/sessions/{sid}/signals"
        with self.subTest("client_header_only"):
            status, got = self.call(
                "GET", f"{signals}?after=0", headers={"X-Signal-Peer": client_peer}
            )
            self.assertEqual(status, 200)
            self.assert_keys(got, SIGNAL_GET_KEYS)
            self.assertEqual([item["payload_b64"] for item in got["signals"]], [down])
        with self.subTest("host_header_only"):
            status, got = self.call(
                "GET",
                f"{signals}?after=0",
                headers={"X-Signal-Peer": "host", "X-Session-Token": token},
            )
            self.assertEqual(status, 200)
            self.assertEqual([item["payload_b64"] for item in got["signals"]], [up])
        with self.subTest("header_and_query_agree"):
            status, got = self.call(
                "GET",
                f"{signals}?peer={quote(client_peer, safe=':')}&after=1",
                headers={"X-Signal-Peer": client_peer},
            )
            self.assertEqual(status, 200)
            self.assertEqual(got["signals"], [])
        with self.subTest("query_only_still_accepted"):
            status, got = self.call(
                "GET", f"{signals}?peer={quote(client_peer, safe=':')}&after=1"
            )
            self.assertEqual(status, 200)
            self.assertEqual(got["signals"], [])
        with self.subTest("header_and_query_disagree"):
            status, err = self.call(
                "GET",
                f"{signals}?peer=client:other&after=0",
                headers={"X-Signal-Peer": client_peer},
            )
            self.assertEqual(status, 400)
            self.assertEqual(err, {"error": "invalid_field", "field": "peer"})
        with self.subTest("header_peer_validated"):
            status, err = self.call(
                "GET", f"{signals}?after=0", headers={"X-Signal-Peer": "client:no spaces"}
            )
            self.assertEqual(status, 400)
            self.assertEqual(err, {"error": "invalid_field", "field": "peer"})
        with self.subTest("header_host_still_needs_token"):
            status, err = self.call(
                "GET", f"{signals}?after=0", headers={"X-Signal-Peer": "host"}
            )
            self.assertEqual(status, 403)
            self.assertEqual(err, {"error": "forbidden"})
        with self.subTest("no_peer_at_all"):
            status, err = self.call("GET", f"{signals}?after=0")
            self.assertEqual(status, 400)
            self.assertEqual(err, {"error": "missing_field", "field": "peer"})
        with self.subTest("log_names_the_path"):
            via = "\n".join(
                line for line in records if line.startswith("signal ") and "peer_via=" in line
            )
            self.assertIn("peer_via=header", via)
            self.assertIn("peer_via=query", via)
            self.assertNotIn(nonce, "\n".join(records))

    def test_client_peer_redacted_in_log(self) -> None:
        records = self.capture_log()
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        nonce = "RedactMe0123456789abcdefABCDEF_-"
        client_peer = f"client:{nonce}"
        tiny = base64.b64encode(b"x").decode("ascii")
        status, _posted = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {"token_or_join_nonce": token, "from": "host", "to": client_peer, "payload_b64": tiny},
        )
        self.assertEqual(status, 200)
        signals = f"/v1/sessions/{sid}/signals"
        status, got = self.call("GET", f"{signals}?peer={quote(client_peer, safe=':')}&after=0")
        self.assertEqual(status, 200)
        self.assertEqual(len(got["signals"]), 1)
        status, got = self.call("GET", f"{signals}?peer={quote(client_peer, safe='')}&after=1")
        self.assertEqual(status, 200)
        self.assertEqual(got["signals"], [])
        status, _host_q = self.call("GET", f"{signals}?peer=host&after=0&token={quote(token)}")
        self.assertEqual(status, 200)
        joined = "\n".join(records)
        self.assertNotIn(nonce, joined)
        self.assertNotIn(token, joined)
        polls = [line for line in records if "/signals?" in line]
        self.assertEqual(len(polls), 3)
        self.assertEqual(sum("peer=redacted&after=" in line for line in polls), 2)
        self.assertEqual(sum("peer=host&after=0&token=redacted" in line for line in polls), 1)

    def test_readme_documents_install_key_and_caps(self) -> None:
        text = Path(__file__).with_name("README.md").read_text(encoding="utf-8")
        self.assertIn("What the install key is", text)
        self.assertIn("rate-limit identity, not a secret", text)
        self.assertIn("10 registers/min", text)
        self.assertIn("120 requests/min", text)
        self.assertIn("30 registers/min", text)
        self.assertIn("300 requests/min", text)
        self.assertIn("16 destination queues", text)
        self.assertIn("1 MiB", text)
        self.assertIn("120 s", text)
        self.assertIn("host queue is never dropped", text.lower())


if __name__ == "__main__":
    unittest.main(verbosity=2)
