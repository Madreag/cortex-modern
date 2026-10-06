#!/usr/bin/python3
"""Tests for session_directory.py (loopback HTTP, optional TLS)."""

from __future__ import annotations

import base64
import hashlib
import hmac
import http.client
import json
import logging
import os
import re
import select
import shutil
import socket
import struct
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from types import SimpleNamespace
from pathlib import Path
from typing import Any, Optional
from urllib.parse import quote

import session_directory
from world_ticks import compare_world_ticks, read_world_ticks
from session_directory import IP_REG_PER_MIN, IP_REQ_PER_MIN, DualRateLimiter, LOGGER, RunningServer, spawn_server
from unittest import mock

INSTALL_KEY = "0123456789abcdef"


def wire_version(header: str, owner: str, name: str = "c_Version") -> int:
    """One class-scoped constexpr from the engine sources, so a row carries the live layout and never a stale literal."""
    path = Path(__file__).resolve().parents[2] / header
    text = path.read_text(encoding="utf-8")
    opener = re.search(rf"(?m)^[ \t]*(?:class|struct)\s+{owner}\b[^;{{]*\{{", text)
    if not opener:
        raise RuntimeError(f"{path}: {owner} not found")
    start, depth = text.rindex("{", opener.start(), opener.end()), 0
    for index in range(start, len(text)):
        depth += (text[index] == "{") - (text[index] == "}")
        if depth == 0:
            body = text[start:index]
            break
    else:
        raise RuntimeError(f"{path}: {owner} is never closed")
    found = re.findall(rf"(?m)^[ \t]*static\s+constexpr\s+\w+\s+{name}\s*=\s*(\d+)\s*;", body)
    if len(found) != 1:
        raise RuntimeError(f"{path}: expected exactly one definition of {owner}::{name}, found {len(found)}")
    return int(found[0])


NETWORK_PROTOCOL_VERSION = wire_version("Source/Network/NetProtocol.h", "NetProtocol")
LOCKSTEP_CODEC_VERSION = wire_version("Source/Network/NetLockstep.h", "NetLockstepCodec")
CONTROLLER_FRAME_VERSION = wire_version("Source/Network/ControllerFrame.h", "ControllerFrame")
HEX64_A = "a" * 64
HEX64_B = "b" * 64
HEX64_C = "c" * 64

REGISTER_RESP_KEYS = {
    "session_id",
    "token",
    "expires_in_s",
    "heartbeat_s",
    "observed_ip",
    "supports_unlisted",
}
LIST_KEYS = {"sessions", "total"}
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
HEARTBEAT_KEYS = {"expires_in_s", "heartbeat_s", "listed"}
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
        "network_protocol_version": NETWORK_PROTOCOL_VERSION,
        "lockstep_codec_version": LOCKSTEP_CODEC_VERSION,
        "controller_frame_version": CONTROLLER_FRAME_VERSION,
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

    def test_coturn_offer_ttl_owner_rate_and_expiry(self) -> None:
        store = session_directory.SessionDirectory(300, 5, turn_config={
            "backend": "coturn", "static_auth_secret": "server-only-secret",
            "relay_urls": ["turn:relay.example:3478?transport=udp", "turn:relay.example:3478?transport=tcp", "turns:relay.example:5349?transport=tcp"],
        })
        row = store.register(sample_register(), "127.0.0.1", 10, INSTALL_KEY)
        data = {"token": row["token"], "match_id": "match:1", "ttl": 600}
        with mock.patch.object(session_directory.time, "time", return_value=1000):
            offer = store.mint_ice_servers(row["session_id"], data, INSTALL_KEY, 11)
            self.assertEqual(set(offer), {"match_id", "expires_at", "iceServers"})
            self.assertEqual(offer["expires_at"], 1600)
            server = offer["iceServers"][0]
            self.assertTrue(server["username"].startswith("1600:"))
            expected = base64.b64encode(hmac.new(b"server-only-secret", server["username"].encode(), hashlib.sha1).digest()).decode()
            self.assertEqual(server["credential"], expected)
            self.assertNotIn("server-only-secret", json.dumps(offer))
            self.assertEqual(store.get_ice_servers(row["session_id"], 11), offer)
            for _ in range(session_directory.TURN_REQUESTS_PER_MIN - 1):
                store.mint_ice_servers(row["session_id"], data, INSTALL_KEY, 11)
            with self.assertRaises(session_directory.TurnError) as limited:
                store.mint_ice_servers(row["session_id"], data, INSTALL_KEY, 11)
            self.assertEqual(limited.exception.status, 429)
        with mock.patch.object(session_directory.time, "time", return_value=1600):
            with self.assertRaises(session_directory.TurnError):
                store.get_ice_servers(row["session_id"], 12)
        for bad in ({**data, "token": "wrong"}, {**data, "ttl": 0}, {**data, "ttl": 86401}, {**data, "static_auth_secret": "never"}):
            with self.assertRaises((PermissionError, session_directory.FieldError)):
                store.mint_ice_servers(row["session_id"], bad, INSTALL_KEY, 12)
        with self.assertRaises(PermissionError):
            store.mint_ice_servers(row["session_id"], data, "fedcba9876543210", 12)
        store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 2, "seats_free": 1}, 13, "fedcba9876543210")
        with mock.patch.object(session_directory.time, "time", return_value=1601):
            replacement = store.mint_ice_servers(row["session_id"], data, "fedcba9876543210", 14)
        self.assertEqual(replacement["expires_at"], 2201)
        self.assertNotEqual(replacement["iceServers"][0]["username"], server["username"])
        with self.assertRaises(PermissionError):
            store.mint_ice_servers(row["session_id"], data, INSTALL_KEY, 14)

    def test_a_refused_mint_is_told_to_the_fetching_client(self) -> None:
        store = session_directory.SessionDirectory(300, 5, turn_config={"backend": "cloudflare", "turn_key_id": "key-id", "api_token": "backend-token"})
        row = store.register(sample_register(), "127.0.0.1", 10, INSTALL_KEY)
        with self.assertRaises(session_directory.TurnError) as none:
            store.get_ice_servers(row["session_id"], 11)
        self.assertEqual((none.exception.status, none.exception.body["error"]), (404, "relay_offer_unavailable"))
        data = {"token": row["token"], "match_id": "match:1", "ttl": 600}
        with mock.patch.object(session_directory, "urlopen", side_effect=OSError("backend-token")):
            with self.assertRaises(session_directory.TurnError):
                store.mint_ice_servers(row["session_id"], data, INSTALL_KEY, 11)
        with self.assertRaises(session_directory.TurnError) as refused:
            store.get_ice_servers(row["session_id"], 12)
        self.assertEqual((refused.exception.status, refused.exception.body["error"]), (502, "relay_provider_refused"))
        self.assertNotIn("backend-token", json.dumps(refused.exception.body))
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.status = 201
        response.read.return_value = json.dumps({"iceServers": [{"urls": ["turn:turn.cloudflare.com:3478?transport=udp"],
                                                                 "username": "u", "credential": "c"}]}).encode()
        with mock.patch.object(session_directory, "urlopen", return_value=response):
            store.mint_ice_servers(row["session_id"], data, INSTALL_KEY, 13)
        self.assertEqual(store.get_ice_servers(row["session_id"], 14)["iceServers"][0]["username"], "u")

    def test_turn_max_ttl_caps_the_minted_lifetime(self) -> None:
        store = session_directory.SessionDirectory(300, 5, turn_config={
            "backend": "coturn", "static_auth_secret": "server-only-secret", "relay_urls": ["turn:relay.example:3478?transport=udp"],
        }, turn_max_ttl=300)
        row = store.register(sample_register(), "127.0.0.1", 10, INSTALL_KEY)
        with mock.patch.object(session_directory.time, "time", return_value=1000):
            offer = store.mint_ice_servers(row["session_id"], {"token": row["token"], "match_id": "match:1", "ttl": 86400}, INSTALL_KEY, 11)
        self.assertEqual(offer["expires_at"], 1300)
        self.assertTrue(offer["iceServers"][0]["username"].startswith("1300:"))
        self.assertEqual(session_directory.SessionDirectory(300, 5).turn_max_ttl, session_directory.TURN_MAX_TTL)
        with self.assertRaises(SystemExit):
            session_directory.parse_args(["--caller-mode", "direct", "--turn-max-ttl", "299"])

    def test_main_hands_the_ttl_cap_to_its_server(self) -> None:
        seen = {}

        def spawn(**kwargs):
            seen.update(kwargs)
            raise SystemExit(0)
        with mock.patch.object(session_directory, "spawn_server", side_effect=spawn):
            with self.assertRaises(SystemExit):
                session_directory.main(["--caller-mode", "direct", "--insecure-http", "--port", "0", "--turn-max-ttl", "300"])
        self.assertEqual(seen.get("turn_max_ttl"), 300)

    def test_fixed_offer_and_secret_refusal(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        row = store.register(sample_register(), "127.0.0.1", 10, INSTALL_KEY)
        server = {"urls": ["turn:private.example:3478?transport=udp"], "username": "private-user", "credential": "private-password"}
        body = {"token": row["token"], "match_id": "fixed:1", "ttl": 600, "iceServers": [server]}
        with mock.patch.object(session_directory.time, "time", return_value=1000):
            offer = store.mint_ice_servers(row["session_id"], body, INSTALL_KEY, 11)
        self.assertEqual(offer, {"match_id": "fixed:1", "expires_at": 1600, "iceServers": [server]})
        server["static_auth_secret"] = "never-publish"
        with self.assertRaises(session_directory.FieldError):
            store.mint_ice_servers(row["session_id"], body, INSTALL_KEY, 12)

    def test_cloudflare_offer_mocks_the_http_boundary(self) -> None:
        provider = session_directory.TurnCredentialProvider({"backend": "cloudflare", "turn_key_id": "key-id", "api_token": "backend-token"})
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.status = 201
        response.read.return_value = json.dumps({"iceServers": [
            {"urls": ["stun:stun.cloudflare.com:3478"]},
            {"urls": ["turn:turn.cloudflare.com:3478?transport=udp", "turns:turn.cloudflare.com:5349?transport=tcp"], "username": "temporary-user", "credential": "temporary-password"},
        ]}).encode()
        with mock.patch.object(session_directory, "urlopen", return_value=response) as fetch:
            offer = provider.mint("match:2", 900, 1000)
        request = fetch.call_args.args[0]
        self.assertEqual(request.full_url, "https://rtc.live.cloudflare.com/v1/turn/keys/key-id/credentials/generate-ice-servers")
        self.assertEqual(json.loads(request.data), {"ttl": 900})
        self.assertEqual(request.get_header("Authorization"), "Bearer backend-token")
        # The request urlopen receives names the product, never urllib's default agent Cloudflare refuses.
        self.assertEqual(request.get_header("User-agent"), "cccp-session-directory/1")
        self.assertEqual(offer["expires_at"], 1900)
        self.assertEqual(offer["iceServers"][1]["credential"], "temporary-password")
        self.assertNotIn("backend-token", json.dumps(offer))
        with mock.patch.object(session_directory, "urlopen", side_effect=OSError("backend-token")):
            with self.assertRaises(session_directory.TurnError) as refused:
                provider.mint("match:2", 900, 1000)
        self.assertNotIn("backend-token", str(refused.exception))

    def test_ice_endpoint_refuses_missing_identity_and_host_token(self) -> None:
        self.start()
        status, row = self.call("POST", "/v1/sessions", sample_register())
        self.assertEqual(status, 200)
        path = "/v1/sessions/" + row["session_id"] + "/ice-servers"
        body = {"token": "wrong", "match_id": "match:3", "ttl": 600}
        status, _ = self.call("POST", path, body)
        self.assertEqual(status, 403)
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        connection.request("POST", path, json.dumps(body), {"Content-Type": "application/json"})
        response = connection.getresponse()
        self.assertEqual(response.status, 400)
        response.read()
        connection.close()

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
        port: int = 0,
    ) -> None:
        self.use_tls = cert is not None and key is not None
        kwargs: dict[str, Any] = {
            "bind": "127.0.0.1",
            "port": port,
            "expiry_s": expiry_s,
            "heartbeat_s": heartbeat_s,
            "insecure_http": not self.use_tls,
            "cert": cert,
            "key": key,
            "caller_mode": "direct",
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


    def test_T3_startup_requires_an_explicit_safe_caller_mode(self) -> None:
        with self.assertRaises(SystemExit, msg="T3: startup silently chose a caller mode"):
            session_directory.parse_args(["--insecure-http"])
        for mode in ("direct", "tunnel"):
            with self.assertRaises((ValueError, SystemExit), msg="T3: a public listener accepted an unsafe caller mode"):
                running = spawn_server(bind="0.0.0.0", port=47465, caller_mode=mode)
                running.stop()
            running = spawn_server(bind="127.0.0.1", port=47465, caller_mode=mode)
            running.stop()

    def test_T4_subnet_churn_cannot_claim_an_established_world(self) -> None:
        with mock.patch.object(session_directory, "MAX_WORLD_OWNERS", 64):
            store = session_directory.SessionDirectory(300, 5)
            self.addCleanup(store.stop)
            request = sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1)
            original = store.register(request, "192.0.2.200", 0, INSTALL_KEY)
            store.heartbeat(original["session_id"], {"token": original["token"], "peer_count": 1, "seats_free": 1}, 60, INSTALL_KEY)
            store.delete(original["session_id"], {"token": original["token"]}, 61)
            for index in range(65):
                key = f"{index:016x}"
                try:
                    row = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), f"192.0.2.{index + 1}", 61, key)
                    store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 1, "seats_free": 1}, 121, key)
                    store.delete(row["session_id"], {"token": row["token"]}, 122)
                except OverflowError:
                    pass
            stolen = False
            try:
                store.register(request, "192.0.2.70", 122, "0123456789abcdee")
                stolen = True
            except (PermissionError, OverflowError):
                pass
            recovered = store.register(dict(request, world_boot=2, resume_session_id=original["session_id"], resume_token=original["token"]), "192.0.2.200", 122, INSTALL_KEY)
            self.assertFalse(stolen, "T4: subnet churn let a stranger claim an established world")
            self.assertEqual(recovered["session_id"], original["session_id"], "T4: retained capacity refused an established owner's resume")
            self.assertIn(original["session_id"], store._world_owners, "T4: subnet churn evicted an established owner")

    def test_T4_established_capacity_refuses_new_worlds(self) -> None:
        with mock.patch.object(session_directory, "MAX_WORLD_OWNERS", 1):
            store = session_directory.SessionDirectory(300, 5)
            self.addCleanup(store.stop)
            request = sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1)
            row = store.register(request, "192.0.2.1", 0, INSTALL_KEY)
            store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 1, "seats_free": 1}, 60, INSTALL_KEY)
            store.delete(row["session_id"], {"token": row["token"]}, 61)
            with self.assertRaises(OverflowError, msg="T4: a table of established owners admitted a new world"):
                store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), "198.51.100.1", 61, INSTALL_KEY)
            resumed = store.register(dict(request, world_boot=2, resume_session_id=row["session_id"], resume_token=row["token"]), "192.0.2.1", 61, INSTALL_KEY)
            self.assertEqual(resumed["session_id"], row["session_id"])

    def test_T4_retained_shares_cover_ipv4_and_ipv6_subnets(self) -> None:
        for prefix in ("198.51.100.", "2001:db8:abcd:"):
            store = session_directory.SessionDirectory(300, 5)
            self.addCleanup(store.stop)
            for index in range(65):
                source = prefix + (str(index + 1) if prefix.endswith(".") else f"{index + 1:x}::1")
                row = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), source, 0, INSTALL_KEY)
                store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 1, "seats_free": 1}, 0, INSTALL_KEY)
                store.delete(row["session_id"], {"token": row["token"]}, 1)
            self.assertLessEqual(len(store._world_owners), session_directory.MAX_WORLD_OWNERS_PER_SOURCE,
                                 "T4: addresses in one subnet exceeded its retained-owner share")

    def test_T4_failed_retirement_preserves_the_old_owner(self) -> None:
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(session_directory, "MAX_WORLD_OWNERS", 1):
            store = session_directory.SessionDirectory(300, 5, owner_state=Path(directory) / "owners.json", create_owner_key=True)
            self.addCleanup(store.stop)
            original = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), "192.0.2.1", 0, INSTALL_KEY)
            store.heartbeat(original["session_id"], {"token": original["token"], "peer_count": 1, "seats_free": 1}, 0, INSTALL_KEY)
            store.delete(original["session_id"], {"token": original["token"]}, 1)
            incoming = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), "198.51.100.1", 1, INSTALL_KEY)
            before = {sid: dict(owner) for sid, owner in store._world_owners.items()}
            with mock.patch.object(store, "_write_owner_file", side_effect=OSError("storage unavailable")):
                with self.assertRaises(OSError):
                    store.heartbeat(incoming["session_id"], {"token": incoming["token"], "peer_count": 1, "seats_free": 1}, 2, INSTALL_KEY)
            self.assertTrue(store._world_owners == before, "T4: a refused storage write retired the old owner")

    def test_T4_failed_return_preserves_the_old_signals(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = session_directory.SessionDirectory(300, 5, owner_state=Path(directory) / "owners.json", create_owner_key=True)
            self.addCleanup(store.stop)
            request = sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1)
            row = store.register(request, "192.0.2.1", 0, INSTALL_KEY)
            store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 1, "seats_free": 1}, 0, INSTALL_KEY)
            store.post_signal(row["session_id"], {"from": "client:returning", "to": "host", "token_or_join_nonce": "returning", "payload_b64": "eA=="}, 1, "192.0.2.2")
            previous = store._sessions[row["session_id"]]
            before = store.get_signals(row["session_id"], "host", 0, row["token"], 1)
            with mock.patch.object(store, "_write_owner_file", side_effect=OSError("storage unavailable")):
                with self.assertRaises(OSError):
                    store.register(dict(request, world_boot=2, resume_session_id=row["session_id"], resume_token=row["token"]), "192.0.2.1", 1, INSTALL_KEY)
            self.assertTrue(store._sessions.get(row["session_id"]) is previous, "T4: a refused returning registration replaced its old lease")
            self.assertTrue(store.get_signals(row["session_id"], "host", 0, row["token"], 1) == before, "T4: a refused returning registration removed queued signaling")

    def test_T5_accept_deadline_closes_dripping_headers_and_bodies(self) -> None:
        self.start(port=47467)
        self.server.store.caller_mode = "tunnel"
        connections = [socket.create_connection(("127.0.0.1", self.port), timeout=1) for _ in range(2)]
        try:
            connections[0].sendall(b"POST /v1/sessions HTTP/1.1\r\nX-Test: ")
            connections[1].sendall((f"POST /v1/sessions HTTP/1.1\r\nX-Install-Key: {INSTALL_KEY}\r\nCF-Connecting-IP: 192.0.2.1\r\nContent-Length: 4096\r\n\r\n{{").encode())
            live = list(connections)
            deadline = time.monotonic() + 5
            while live and time.monotonic() < deadline:
                for connection in live:
                    try: connection.sendall(b" ")
                    except OSError: pass
                readable, _, _ = select.select(live, [], [], 0.1)
                for connection in readable:
                    try: ended = not connection.recv(4096)
                    except OSError: ended = True
                    if ended: live.remove(connection)
            self.assertFalse(live, "T5: a dripping header or body outlived the total accept deadline")
        finally:
            for connection in connections: connection.close()

    def test_T5_dripping_bodies_leave_an_honest_heartbeat_capacity(self) -> None:
        self.start(port=47466)
        self.server.store.caller_mode = "tunnel"
        honest = {"CF-Connecting-IP": "192.0.2.200"}
        status, row = self.call("POST", "/v1/sessions", sample_register(), headers=honest)
        self.assertEqual(status, 200)
        sockets = []
        stop = threading.Event()
        def drip():
            while not stop.wait(0.1):
                for connection in tuple(sockets):
                    try: connection.sendall(b" ")
                    except OSError: pass
        worker = threading.Thread(target=drip)
        try:
            for index in range(64):
                connection = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                sockets.append(connection)
                try:
                    connection.sendall((f"POST /v1/sessions HTTP/1.1\r\nHost: localhost\r\nX-Install-Key: {index:016x}\r\nCF-Connecting-IP: 192.0.2.1\r\nContent-Length: 4096\r\n\r\n{{").encode())
                except OSError:
                    pass
                time.sleep(0.015)
            worker.start()
            started = time.monotonic()
            try:
                status, _ = self.call("POST", f"/v1/sessions/{row['session_id']}/heartbeat", {"token": row["token"], "peer_count": 1, "seats_free": 1}, headers=honest)
            except (OSError, http.client.HTTPException):
                self.fail("T5: one caller's dripping bodies occupied every heartbeat handler")
            self.assertEqual(status, 200, "T5: dripping bodies blocked an honest heartbeat")
            self.assertLess(time.monotonic() - started, self.server.store.expiry_s, "T5: heartbeat answered after its lease")
        finally:
            stop.set()
            if worker.ident is not None: worker.join(2)
            for connection in sockets: connection.close()

    def test_R1_tunnel_requires_the_callers_address(self) -> None:
        self.start(port=47460)
        self.server.store.caller_mode = "tunnel"
        status, _ = self.call("GET", "/v1/sessions")
        self.assertEqual(status, 400, "R1: tunnel request without a caller address used the loopback bucket")
        for header in ("garbage", "127.0.0.1, 192.0.2.1", "", "192.0.2.1%fake"):
            status, _ = self.call("GET", "/v1/sessions", headers={"CF-Connecting-IP": header})
            self.assertEqual(status, 400, "R1: tunnel accepted an invalid caller address")
        for address in ("192.0.2.1", "192.0.2.2"):
            status, row = self.call("POST", "/v1/sessions", sample_register(), headers={"CF-Connecting-IP": address})
            self.assertEqual(status, 200)
            self.assertEqual(row["observed_ip"], address, "R1: valid forwarded address was not the pending registration source")
        self.assertEqual({row.observed_ip for row in self.server.store._sessions.values()}, {"192.0.2.1", "192.0.2.2"})

    def test_R2_waiters_leave_heartbeats_capacity(self) -> None:
        self.start(port=47461)
        self.server.store.caller_mode = "tunnel"
        address = {"CF-Connecting-IP": "192.0.2.250"}
        status, row = self.call("POST", "/v1/sessions", sample_register(), headers=address)
        self.assertEqual(status, 200)
        threads, answers = [], []
        def poll(index, source):
            conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
            try:
                conn.request("GET", f"/v1/sessions/{row['session_id']}/signals?peer=client:{index:016x}&wait=25",
                             headers={"X-Install-Key": f"{index % 2 if index < 1064 else index:016x}", "CF-Connecting-IP": source})
                response = conn.getresponse()
                answers.append((response.status, json.loads(response.read())))
            except (OSError, http.client.HTTPException) as error:
                answers.append((0, type(error).__name__))
            finally:
                conn.close()
        try:
            for count, shared in ((64, True), (200, False)):
                for index in range(count):
                    source = "192.0.2.1" if shared else f"198.51.{index // 250}.{index % 250 + 1}"
                    worker = threading.Thread(target=poll, args=(len(threads) + 1000, source))
                    threads.append(worker)
                    worker.start()
                    time.sleep(0.015)
                started = time.monotonic()
                try:
                    status, _ = self.call("POST", f"/v1/sessions/{row['session_id']}/heartbeat",
                                          {"token": row["token"], "peer_count": 2, "seats_free": 1}, headers=address)
                except (OSError, http.client.HTTPException):
                    self.fail("R2: waiting polls occupied the heartbeat handlers")
                self.assertEqual(status, 200, "R2: waiting polls prevented an honest heartbeat")
                self.assertLess(time.monotonic() - started, self.server.store.expiry_s, "R2: heartbeat answered after its lease")
                self.assertEqual(self.call("GET", "/v1/sessions", headers=address)[1]["total"], 1)
            self.assertTrue(any(status == 200 and body.get("retry_after_s") for status, body in answers),
                            "R2: excess waiters did not get an immediate empty answer and retry hint")
        finally:
            completed = list(answers)
            self.server.store.stop()
            for worker in threads:
                worker.join(3)
        self.assertTrue(all(status == 200 for status, _ in completed), "R2: bounded waiters dropped an honest polling request")

    def test_R3_one_source_cannot_exhaust_retained_owners(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        self.addCleanup(store.stop)
        now = time.monotonic()
        returning_request = sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1)
        returning = store.register(returning_request, "192.0.2.200", now, INSTALL_KEY)
        store.heartbeat(returning["session_id"], {"token": returning["token"], "peer_count": 1, "seats_free": 1}, now, INSTALL_KEY)
        store.delete(returning["session_id"], {"token": returning["token"]}, now)
        for index in range(session_directory.MAX_WORLD_OWNERS):
            request = sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1)
            try:
                row = store.register(request, "192.0.2.1", now, f"{index:016x}")
                store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 1, "seats_free": 1}, now, f"{index:016x}")
                store.delete(row["session_id"], {"token": row["token"]}, now)
            except OverflowError:
                self.fail("R3: one caller filled retained ownership before honest worlds could register")
        honest = sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1)
        try:
            fresh = store.register(honest, "192.0.2.201", now, INSTALL_KEY)
            recovered = store.register(dict(returning_request, world_boot=2, resume_session_id=returning["session_id"], resume_token=returning["token"]), "192.0.2.200", now, INSTALL_KEY)
        except OverflowError:
            self.fail("R3: strangers' retained owners refused an honest new or returning world")
        self.assertEqual(recovered["session_id"], returning["session_id"])
        self.assertNotEqual(fresh["session_id"], recovered["session_id"])
        self.assertLess(len(store._world_owners), session_directory.MAX_WORLD_OWNERS, "R3: one source kept the whole owner table")

    def test_R4_refused_offer_preserves_every_queued_offer(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        self.addCleanup(store.stop)
        now = time.monotonic()
        row = store.register(sample_register(), "192.0.2.200", now, INSTALL_KEY)
        def offer(nonce, size, source):
            return store.post_signal(row["session_id"], {"from": "client:" + nonce, "to": "host", "token_or_join_nonce": nonce,
                                                        "payload_b64": base64.b64encode(b"x" * size).decode()}, now, source)
        first = offer("honest", 1024, "192.0.2.200")
        offer("honest", 1024, "192.0.2.200")
        for index in range(7):
            offer(f"attack{index}", 65536, f"192.0.2.{index // 2 + 1}")
        before = store.get_signals(row["session_id"], "host", 0, row["token"], now)
        with self.assertRaises(BufferError):
            offer("attack7", 65536, "192.0.2.5")
        after = store.get_signals(row["session_id"], "host", 0, row["token"], now)
        self.assertEqual(after, before, "R4: a refused offer evicted an honest joiner's queued offer")
        self.assertIn(first["seq"], [item["seq"] for item in after["signals"]])

    def test_owner_pressure_retires_short_listings_before_established_worlds(self) -> None:
        with mock.patch.object(session_directory, "MAX_WORLD_OWNERS", 4):
            store = session_directory.SessionDirectory(300, 5)
            self.addCleanup(store.stop)
            request = sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1)
            original = store.register(request, "192.0.2.200", 0, INSTALL_KEY)
            store.heartbeat(original["session_id"], {"token": original["token"], "peer_count": 1, "seats_free": 1}, session_directory.OWNER_MIN_LISTED_S, INSTALL_KEY)
            store.delete(original["session_id"], {"token": original["token"]}, 61)
            for index in range(5):
                row = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), f"192.0.2.{index}", 61, INSTALL_KEY)
                store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 1, "seats_free": 1}, 62, INSTALL_KEY)
                store.delete(row["session_id"], {"token": row["token"]}, 62)
            self.assertEqual(len(store._world_owners), 4)
            self.assertIn(original["session_id"], store._world_owners, "R3: short listings displaced an established owner")
            expired_wall = time.time() + session_directory.OWNER_IDLE_S + 1
            with mock.patch.object(session_directory.time, "time", return_value=expired_wall):
                store.prune(62)
                for index in range(4):
                    row = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), f"198.51.100.{index}", 62, INSTALL_KEY)
                    store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 1, "seats_free": 1}, 62, INSTALL_KEY)
                    store.delete(row["session_id"], {"token": row["token"]}, 62)
                recovered = store.register(dict(request, world_boot=2, resume_session_id=original["session_id"], resume_token=original["token"]), "192.0.2.200", 62, INSTALL_KEY)
                store.heartbeat(recovered["session_id"], {"token": recovered["token"], "peer_count": 1, "seats_free": 1}, 62, INSTALL_KEY)
            self.assertEqual(recovered["session_id"], original["session_id"], "R3: a full table refused a signed owner whose record expired")
            self.assertEqual(len(store._world_owners), 4, "R3: recovery exceeded the retained owner cap")

    def test_R5_interrupted_key_creation_leaves_a_restartable_service(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "world-owners.json"
            real_fdopen = session_directory.os.fdopen
            class InterruptedWrite:
                def __init__(self, fd, mode):
                    self.stream = real_fdopen(fd, mode)
                def __enter__(self): return self
                def __exit__(self, *args): self.stream.close()
                def write(self, data):
                    self.stream.write(data[:7])
                    self.stream.flush()
                    raise OSError("interrupted first start")
            import inspect
            argument = {"create_owner_key": True} if "create_owner_key" in inspect.signature(session_directory.SessionDirectory).parameters else {"first_upgrade_worlds": 0}
            with mock.patch.object(session_directory.os, "fdopen", side_effect=InterruptedWrite):
                with self.assertRaises(OSError):
                    session_directory.SessionDirectory(15, 5, owner_state=state, **argument)
            try:
                resumed = session_directory.SessionDirectory(15, 5, owner_state=state,
                            **({} if state.with_suffix(".key").exists() else argument))
            except ValueError:
                self.fail("R5: interrupted first start left a partial signing key that prevents the next start")
            self.addCleanup(resumed.stop)
            self.assertEqual(resumed._world_owners, {})
            row = resumed.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), "192.0.2.1", 0, INSTALL_KEY)
            self.assertTrue(row["token"])

    def test_successor_resumes_row_only_with_its_sealed_token(self) -> None:
        self.start(port=45799)
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, _ = self.register(resume_session_id=sid, resume_token=sid)
        self.assertEqual(status, 403, "public session id resume was not refused")
        status, _ = self.register(resume_session_id=sid)
        self.assertEqual(status, 403, "a resume without the row token was accepted")
        status, moved = self.register(
            resume_session_id=sid, resume_token=token,
            listen_addrs=["203.0.113.42"], listen_port=45793,
        )
        self.assertEqual(status, 200)
        self.assertEqual(moved["session_id"], sid, "successor created another row instead of resuming the match")
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(len(listed["sessions"]), 1)
        row = listed["sessions"][0]
        self.assertEqual(row["listen_addrs"], ["203.0.113.42"])
        self.assertEqual(row["listen_port"], 45793)
        self.assertEqual(row["state"], "running")
        self.assertNotIn("resume_token", row)
        self.assertNotIn("token", row)

    def test_successor_token_outlives_the_discovery_lease(self) -> None:
        directory = session_directory.SessionDirectory(15, 5)
        created = directory.register(sample_register(), "192.0.2.1", 0)
        directory.heartbeat(created["session_id"], {"token": created["token"], "peer_count": 2, "seats_free": 1}, 0)
        directory.prune(20)
        resumed = directory.register(sample_register(
            resume_session_id=created["session_id"], resume_token=created["token"],
            listen_addrs=["192.0.2.2"], listen_port=45793,
        ), "192.0.2.2", 21)
        self.assertEqual(resumed["session_id"], created["session_id"], "lease expiry lost the authenticated migration row")
        directory.delete(created["session_id"], {"token": created["token"]}, 22)
        with self.assertRaises(PermissionError, msg="deleted match was resurrected by a retired token"):
            directory.register(sample_register(
                resume_session_id=created["session_id"], resume_token=created["token"],
            ), "192.0.2.2", 23)

    def test_one_host_claims_each_handover_generation(self) -> None:
        directory = session_directory.SessionDirectory(15, 5)
        created = directory.register(sample_register(), "192.0.2.1", 0)
        sid, token = created["session_id"], created["token"]
        first = directory.register(sample_register(
            resume_session_id=sid, resume_token=token, migration_gen=1, listen_addrs=["192.0.2.2"], listen_port=45793,
        ), "192.0.2.2", 1)
        self.assertEqual(first["session_id"], sid, "the first successor's claim did not take the row")
        with self.assertRaises(session_directory.Superseded) as again:
            directory.register(sample_register(
                resume_session_id=sid, resume_token=token, migration_gen=1, listen_addrs=["192.0.2.3"], listen_port=45794,
            ), "192.0.2.3", 2)
        self.assertEqual(again.exception.body, {"error": "already_migrated", "migration_gen": 1})
        with self.assertRaises(session_directory.Superseded):
            directory.register(sample_register(resume_session_id=sid, resume_token=token, listen_addrs=["192.0.2.4"], listen_port=45795), "192.0.2.4", 3)
        row = directory._sessions[sid]
        self.assertEqual((row.fields["listen_addrs"], row.migration_gen), (["192.0.2.2"], 1), "a later claim moved the row off the first successor")
        # The host the match left behind is refused its heartbeat and its delete; the successor keeps both.
        beat = {"token": token, "peer_count": 2, "seats_free": 0}
        with self.assertRaises(session_directory.Superseded) as stale:
            directory.heartbeat(sid, {**beat, "migration_gen": 0}, 4)
        self.assertEqual(stale.exception.body, {"error": "superseded", "migration_gen": 1})
        with self.assertRaises(session_directory.Superseded):
            directory.delete(sid, {"token": token, "migration_gen": 0}, 5)
        self.assertEqual(directory.heartbeat(sid, {**beat, "migration_gen": 1}, 6)["migration_gen"], 1)
        # A generation retained past the lease still decides the next claim.
        directory.prune(30)
        with self.assertRaises(session_directory.Superseded):
            directory.register(sample_register(resume_session_id=sid, resume_token=token, migration_gen=1), "192.0.2.5", 31)
        second = directory.register(sample_register(resume_session_id=sid, resume_token=token, migration_gen=2), "192.0.2.6", 32)
        self.assertEqual((second["session_id"], directory._sessions[sid].migration_gen), (sid, 2))
        directory.delete(sid, {"token": token, "migration_gen": 2}, 33)
        self.assertNotIn(sid, directory._sessions)

    def test_live_resume_wrong_token_leaves_the_row_unchanged(self) -> None:
        self.start(port=45810)
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        self.assertEqual(self.beat(sid, token, state="running")[0], 200)
        row = self._session(sid)
        before = (dict(row.fields), row.token, row.observed_ip, row.last_beat, row.state, row.listed)
        status, refused = self.register(
            resume_session_id=sid, resume_token="wrong-row-token",
            name="unauthorized successor", listen_addrs=["203.0.113.78"], listen_port=45810,
        )
        self.assertEqual((status, refused), (403, {"error": "forbidden"}))
        after = self._session(sid)
        self.assertEqual(
            (dict(after.fields), after.token, after.observed_ip, after.last_beat, after.state, after.listed),
            before, "a refused live resume mutated the directory row",
        )
        with mock.patch.object(session_directory, "MAX_ROWS", 1):
            status, resumed = self.register(resume_session_id=sid, resume_token=token)
        self.assertEqual((status, resumed.get("session_id"), resumed.get("token")), (200, sid, token))
        status, refused = self.register(resume_token=token)
        self.assertEqual((status, refused), (403, {"error": "forbidden"}))

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
        self.assertEqual(row["network_protocol_version"], NETWORK_PROTOCOL_VERSION)
        self.assertEqual(row["lockstep_codec_version"], LOCKSTEP_CODEC_VERSION)
        self.assertEqual(row["controller_frame_version"], CONTROLLER_FRAME_VERSION)
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

    def test_a_refusal_reaches_a_client_whose_body_arrives_late(self) -> None:
        # A loaded box sends the headers and the body apart; the install-key gate answers between them. Closing over the
        # unread body reset the connection and the client lost the answer (WinError 10053 on the Z13).
        self.start()
        body = json.dumps(dict(sample_register(), padding="x" * 2000)).encode("utf-8")
        head = (f"POST /v1/sessions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                f"X-Install-Key: short\r\nContent-Length: {len(body)}\r\n\r\n").encode("ascii")
        for attempt in range(3):
            with self.subTest(attempt=attempt), socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
                sock.sendall(head)
                time.sleep(0.2)
                sock.sendall(body)
                time.sleep(0.2)
                reply = sock.recv(65536)
                self.assertTrue(reply.startswith(b"HTTP/1.1 400 "), reply[:80])
                self.assertIn(b'"invalid_install_key"', reply)

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
            self.assertEqual(self.beat(body["session_id"], body["token"], key=key_a)[0], 200)
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

    def test_signal_long_poll_wait(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid = created["session_id"]
        token = created["token"]
        nonce = "longPollNonce"
        client_peer = f"client:{nonce}"
        up = base64.b64encode(b"held-for-you").decode("ascii")

        held: list[tuple[int, Any]] = []

        def poll() -> None:
            held.append(
                self.call(
                    "GET",
                    f"/v1/sessions/{sid}/signals?peer=host&after=0&wait=2&token={quote(token)}",
                )
            )

        thread = threading.Thread(target=poll, daemon=True)
        thread.start()
        time.sleep(0.4)
        # wait=2 must hold the GET instead of answering an empty queue at once.
        self.assertTrue(thread.is_alive())
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
        thread.join(timeout=3.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(held), 1)
        status, body = held[0]
        self.assertEqual(status, 200)
        self.assert_keys(body, SIGNAL_GET_KEYS)
        self.assertEqual([item["payload_b64"] for item in body["signals"]], [up])

        # An empty queue with wait elapses before answering, and the held request
        # counts once against the per-key budget.
        key_reqs = self.server.store.limiter._by_key._requests
        before = len(key_reqs.get(INSTALL_KEY, []))
        began = time.monotonic()
        status, empty = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer={quote(client_peer, safe=':')}&after=0&wait=0.5",
        )
        elapsed = time.monotonic() - began
        self.assertEqual(status, 200)
        self.assertEqual(empty["signals"], [])
        self.assertGreaterEqual(elapsed, 0.35)
        self.assertEqual(len(key_reqs.get(INSTALL_KEY, [])) - before, 1)

        # wait must be a number inside 0..25.
        for bad in ("abc", "-1", "30"):
            status, err = self.call(
                "GET",
                f"/v1/sessions/{sid}/signals?peer=host&after=0&wait={bad}&token={quote(token)}",
            )
            self.assertEqual(status, 400)
            self.assertEqual(err, {"error": "invalid_field", "field": "wait"})

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

    def test_one_host_and_three_joiners_keep_admission_budget(self) -> None:
        limiter = DualRateLimiter()
        for request in range(80):
            for peer in range(4):
                refused = limiter.check(f"peer{peer:012d}", "127.0.0.1", request * 0.7, False)
                self.assertIsNone(refused, f"four-peer signalling refused at request {request}, peer {peer}")

    def test_per_ip_rate_limit(self) -> None:
        self.start()
        for i in range(30):
            status, body = self.register(key=f"{i:016d}")
            self.assertEqual(status, 200, msg=f"register {i+1}")
            self.assert_keys(body, REGISTER_RESP_KEYS)
            self.assertEqual(self.beat(body["session_id"], body["token"], key=f"{i:016d}")[0], 200)
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
        for i in range(IP_REQ_PER_MIN - 1):
            status, body = self.list_sessions(headers={"X-Install-Key": f"L{i:015d}"})
            self.assertEqual(status, 200, msg=f"list {i+1}")
            self.assert_keys(body, LIST_KEYS)
        status, limited = self.list_sessions(headers={"X-Install-Key": "L" + "x" * 15})
        self.assertEqual(status, 429)
        self.assert_keys(limited, RATE_KEYS)
        self.assertEqual(limited["error"], "rate_limited")
        self.assertIsInstance(limited["retry_after_s"], int)
        self.assertGreaterEqual(limited["retry_after_s"], 1)

    def test_tunnel_clients_keep_their_own_ip(self) -> None:
        # Behind the Cloudflare tunnel every request reaches the service from loopback: each client is limited and
        # reported by the address the tunnel names, never pooled into one loopback bucket.
        self.start()
        self.server.store.caller_mode = "tunnel"

        def register_from(ip: str, i: int) -> tuple[int, dict[str, Any]]:
            status_i, body_i = self.call("POST", "/v1/sessions", sample_register(),
                                         headers={"X-Install-Key": f"T{ip.replace('.', '')}{i:04d}".ljust(16, "0")[:16], "CF-Connecting-IP": ip})
            self.assertIsInstance(body_i, dict)
            return status_i, body_i

        for i in range(IP_REG_PER_MIN):
            status, body = register_from("203.0.113.7", i)
            self.assertEqual(status, 200, msg=f"register {i+1} from the first client")
            self.assertEqual(body["observed_ip"], "203.0.113.7")
            headers = {"X-Install-Key": f"T20301137{i:04d}".ljust(16, "0")[:16], "CF-Connecting-IP": "203.0.113.7"}
            self.assertEqual(self.call("POST", f'/v1/sessions/{body["session_id"]}/heartbeat', {"token": body["token"], "peer_count": 2, "seats_free": 1}, headers=headers)[0], 200)
        status, _ = register_from("203.0.113.7", IP_REG_PER_MIN)
        self.assertEqual(status, 429)
        status, body = register_from("198.51.100.9", 0)
        self.assertEqual(status, 200, msg="a second client behind the tunnel was limited by the first one's registers")
        self.assertEqual(body["observed_ip"], "198.51.100.9")
        status, body = self.call("POST", "/v1/sessions", sample_register(),
                                 headers={"X-Install-Key": "Tbadheader000000", "CF-Connecting-IP": "not-an-address"})
        self.assertEqual(status, 400)
        self.assertEqual(body["error"], "invalid_caller_address")

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
        self.assertIn("limit", text)
        self.assertIn("cursor", text)
        self.assertIn("next_cursor", text)
        self.assertIn("total", text)

    def test_list_limit_and_cursor(self) -> None:
        self.start()
        assert self.server is not None
        base = time.monotonic()
        ids: list[str] = []
        for i in range(250):
            created = self.server.store.register(
                sample_register(name=f"n{i:03d}"), "127.0.0.1", base + i * 0.001
            )
            ids.append(created["session_id"])
            self.server.store.heartbeat(created["session_id"], {"token": created["token"], "peer_count": 2, "seats_free": 1}, base + i * 0.001)
        status, page1 = self.list_sessions("limit=100")
        self.assertEqual(status, 200)
        self.assertEqual(len(page1["sessions"]), 100)
        self.assertIn("next_cursor", page1)
        self.assertEqual(page1["total"], 250)
        page1_ids = [row["session_id"] for row in page1["sessions"]]
        self.assertEqual(page1_ids, ids[:100])
        status, page2 = self.list_sessions(
            "limit=100&cursor=" + quote(page1["next_cursor"], safe="")
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(page2["sessions"]), 100)
        self.assertIn("next_cursor", page2)
        self.assertEqual(page2["total"], 250)
        page2_ids = [row["session_id"] for row in page2["sessions"]]
        self.assertEqual(page2_ids, ids[100:200])
        status, page3 = self.list_sessions(
            "limit=100&cursor=" + quote(page2["next_cursor"], safe="")
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(page3["sessions"]), 50)
        self.assertNotIn("next_cursor", page3)
        self.assertEqual(page3["total"], 250)
        page3_ids = [row["session_id"] for row in page3["sessions"]]
        self.assertEqual(page3_ids, ids[200:])
        seen = page1_ids + page2_ids + page3_ids
        self.assertEqual(seen, ids)
        self.assertEqual(len(set(seen)), 250)

    def test_list_bad_cursor(self) -> None:
        self.start()
        status, err = self.list_sessions("cursor=not-a-cursor")
        self.assertEqual(status, 400)
        self.assertEqual(err, {"error": "invalid_field", "field": "cursor"})

    def test_limiter_prunes_idle_buckets(self) -> None:
        limiter = DualRateLimiter()
        now = 1.0
        later = now + 601.0
        key = "aaaaaaaaaaaaaaaa"
        ip = "10.0.0.1"
        self.assertIsNone(limiter.check(key, ip, now, True))
        self.assertIn(key, limiter._by_key._requests)
        for i in range(255):
            self.assertIsNone(
                limiter.check(f"{i:016d}", f"11.0.{i // 250}.{i % 250}", later, False)
            )
        self.assertNotIn(key, limiter._by_key._requests)
        self.assertNotIn(key, limiter._by_key._registers)
        for _ in range(10):
            self.assertIsNone(limiter.check(key, ip, later, True))
        blocked = limiter.check(key, ip, later, True)
        self.assertIsNotNone(blocked)
        assert blocked is not None
        self.assertEqual(blocked[0], 429)
        self.assertEqual(blocked[1]["error"], "rate_limited")

    def test_limiter_probe_does_not_create_buckets(self) -> None:
        limiter = DualRateLimiter()
        now = 1.0
        ip = "10.0.0.2"
        for i in range(IP_REQ_PER_MIN):
            self.assertIsNone(limiter.check(f"{i:016x}", ip, now, False))
        blocked = limiter.check("orphan0000000000", ip, now, True)
        self.assertIsNotNone(blocked)
        self.assertNotIn("orphan0000000000", limiter._by_key._requests)
        self.assertNotIn("orphan0000000000", limiter._by_key._registers)
        self.assertNotIn("orphan0000000000", limiter._by_key._last)
        for key in limiter._by_key._requests:
            self.assertIn(key, limiter._by_key._last)

    def beat(
        self,
        sid: str,
        token: str,
        **fields: object,
    ) -> tuple[int, dict[str, Any]]:
        body: dict[str, Any] = {
            "token": token,
            "peer_count": 2,
            "seats_free": 1,
        }
        body.update(fields)
        return self.call("POST", f"/v1/sessions/{sid}/heartbeat", body)

    def _session(self, sid: str):
        assert self.server is not None
        return self.server.store._sessions[sid]

    def test_register_reports_unlisted_capability(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        self.assertIs(created["supports_unlisted"], True)

    def test_heartbeat_reply_reports_actual_listed(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, reply = self.beat(sid, token)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], True)
        status, reply = self.beat(sid, token, listed=False)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], False)
        status, reply = self.beat(sid, token, listed=True)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], True)

    def test_hidden_disappears_but_signals_round_trip(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, reply = self.beat(sid, token, listed=False)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], False)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(listed["sessions"], [])
        self.assertEqual(listed["total"], 0)
        payload = base64.b64encode(b"offer").decode("ascii")
        status, posted = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": token,
                "from": "host",
                "to": "client:abc",
                "payload_b64": payload,
            },
        )
        self.assertEqual(status, 200)
        self.assertEqual(posted["seq"], 1)
        status, got = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer=client:abc&after=0",
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(got["signals"]), 1)
        self.assertEqual(got["signals"][0]["payload_b64"], payload)
        status, posted = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": "abc",
                "from": "client:abc",
                "to": "host",
                "payload_b64": base64.b64encode(b"answer").decode("ascii"),
            },
        )
        self.assertEqual(status, 200)
        status, got = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer=host&after=0&token={token}",
        )
        self.assertEqual(status, 200)
        self.assertEqual(len(got["signals"]), 1)

    def test_hidden_survives_ordinary_heartbeat(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, _ = self.beat(sid, token, listed=False)
        self.assertEqual(status, 200)
        status, reply = self.beat(sid, token, seats_free=0)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], False)
        status, listed = self.list_sessions()
        self.assertEqual(listed["sessions"], [])

    def test_relist_same_id_token_monotonic_seq(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, _ = self.beat(sid, token, listed=False)
        self.assertEqual(status, 200)
        status, posted = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": token,
                "from": "host",
                "to": "client:abc",
                "payload_b64": base64.b64encode(b"s1").decode("ascii"),
            },
        )
        self.assertEqual(status, 200)
        self.assertEqual(posted["seq"], 1)
        status, reply = self.beat(sid, token, listed=True)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], True)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(len(listed["sessions"]), 1)
        self.assertEqual(listed["sessions"][0]["session_id"], sid)
        status, posted = self.call(
            "POST",
            f"/v1/sessions/{sid}/signal",
            {
                "token_or_join_nonce": token,
                "from": "host",
                "to": "client:abc",
                "payload_b64": base64.b64encode(b"s2").decode("ascii"),
            },
        )
        self.assertEqual(status, 200)
        self.assertEqual(posted["seq"], 2)
        status, got = self.call(
            "GET",
            f"/v1/sessions/{sid}/signals?peer=client:abc&after=0",
        )
        self.assertEqual(status, 200)
        self.assertEqual([item["seq"] for item in got["signals"]], [1, 2])

    def test_delete_hidden_session(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, _ = self.beat(sid, token, listed=False)
        self.assertEqual(status, 200)
        status, deleted = self.call(
            "DELETE", f"/v1/sessions/{sid}", {"token": token}
        )
        self.assertEqual(status, 200)
        self.assertTrue(deleted["ok"])
        status, err = self.call(
            "GET", f"/v1/sessions/{sid}/signals?peer=host&token={token}"
        )
        self.assertEqual(status, 404)

    def test_hidden_session_expires_normally(self) -> None:
        self.start(expiry_s=1)
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, _ = self.beat(sid, token, listed=False)
        self.assertEqual(status, 200)
        time.sleep(2.0)
        status, _ = self.call(
            "GET", f"/v1/sessions/{sid}/signals?peer=host&token={token}"
        )
        self.assertEqual(status, 404)

    def test_wrong_token_cannot_hide_or_relist(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, err = self.beat(sid, "bad-token-000000", listed=False)
        self.assertEqual(status, 403)
        self.assertEqual(err, {"error": "forbidden"})
        self.assertTrue(self._session(sid).listed)
        status, _ = self.beat(sid, token, listed=False)
        self.assertEqual(status, 200)
        status, err = self.beat(sid, "bad-token-000000", listed=True)
        self.assertEqual(status, 403)
        self.assertFalse(self._session(sid).listed)
        status, listed = self.list_sessions()
        self.assertEqual(listed["sessions"], [])

    def test_malformed_listed_values_rejected_without_mutation(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        sess = self._session(sid)
        beat_before = sess.last_beat
        for bad in (None, "true", 0, 1, [True], {"x": True}):
            status, err = self.beat(sid, token, listed=bad, seats_free=9)
            self.assertEqual(status, 400)
            self.assertEqual(err, {"error": "invalid_field", "field": "listed"})
            self.assertEqual(sess.last_beat, beat_before)
            self.assertTrue(sess.listed)
            self.assertEqual(sess.fields["seats_free"], 1)
        status, reply = self.beat(sid, token)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], True)

    def test_mixed_listed_unlisted_pagination(self) -> None:
        self.start()
        assert self.server is not None
        base = time.monotonic()
        ids: list[str] = []
        tokens: list[str] = []
        for i in range(4):
            created = self.server.store.register(
                sample_register(name=f"m{i}"), "127.0.0.1", base + i * 0.001
            )
            ids.append(created["session_id"])
            tokens.append(created["token"])
        for i in (1, 3):
            status, _ = self.beat(ids[i], tokens[i], listed=False)
            self.assertEqual(status, 200)
        status, page = self.list_sessions("limit=1")
        self.assertEqual(status, 200)
        self.assertEqual(page["total"], 2)
        self.assertEqual([row["session_id"] for row in page["sessions"]], [ids[0]])
        self.assertIn("next_cursor", page)
        status, page2 = self.list_sessions(
            "limit=1&cursor=" + quote(page["next_cursor"], safe="")
        )
        self.assertEqual(status, 200)
        self.assertEqual(page2["total"], 2)
        self.assertEqual([row["session_id"] for row in page2["sessions"]], [ids[2]])
        self.assertNotIn("next_cursor", page2)
        for i in (0, 2):
            status, _ = self.beat(ids[i], tokens[i], listed=False)
            self.assertEqual(status, 200)
        status, empty = self.list_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(empty["sessions"], [])
        self.assertEqual(empty["total"], 0)

    def test_hidden_rows_still_consume_capacity(self) -> None:
        self.start()
        assert self.server is not None
        with mock.patch.object(session_directory, "MAX_ROWS", 3):
            for i in range(3):
                status, created = self.register(name=f"c{i}")
                self.assertEqual(status, 200)
                status, _ = self.beat(
                    created["session_id"], created["token"], listed=False
                )
                self.assertEqual(status, 200)
            status, err = self.register(name="c3")
            self.assertEqual(status, 503)
            self.assertEqual(err, {"error": "full"})

    def test_world_row_advertises_its_free_spectator_slots(self) -> None:
        self.start()
        world_id = str(uuid.uuid4())
        world = {
            "persistent_world": True,
            "world_id": world_id,
            "world_boot": 1,
            "resume_session_id": world_id,
            "seats_free": 0,
            "spectator_free": 3,
        }
        status, first = self.register(**world)
        self.assertEqual(status, 200, first)
        status, listed = self.list_sessions()
        row = listed["sessions"][0]
        self.assertEqual((row.get("seats_free"), row.get("spectator_free")), (0, 3), row)
        # A beat keeps the count current without re-registering the whole row.
        status, ok = self.beat(world_id, first["token"], seats_free=0, spectator_free=1)
        self.assertEqual(status, 200, ok)
        status, listed = self.list_sessions()
        row = listed["sessions"][0]
        self.assertEqual((row.get("seats_free"), row.get("spectator_free")), (0, 1), row)
        # A beat that names none leaves the row's count alone.
        status, ok = self.beat(world_id, first["token"], seats_free=0)
        self.assertEqual(status, 200, ok)
        status, listed = self.list_sessions()
        self.assertEqual(listed["sessions"][0].get("spectator_free"), 1, listed)
        # An ordinary row carries no spectator field at all.
        status, plain = self.register(name="plain")
        self.assertEqual(status, 200, plain)
        status, listed = self.list_sessions()
        rows = {row["session_id"]: row for row in listed["sessions"]}
        self.assertNotIn("spectator_free", rows[plain["session_id"]])

    def test_running_row_lists_the_seats_it_holds(self) -> None:
        self.start()
        status, first = self.register(name="held")
        self.assertEqual(status, 200, first)
        status, listed = self.list_sessions()
        self.assertNotIn("seats_held", listed["sessions"][0])
        # A running match whose player dropped is full, but a newcomer may apply for the held seat.
        status, ok = self.beat(first["session_id"], first["token"], seats_free=0, seats_held=1, state="running")
        self.assertEqual(status, 200, ok)
        status, listed = self.list_sessions()
        row = listed["sessions"][0]
        self.assertEqual((row.get("state"), row.get("seats_free"), row.get("seats_held")), ("running", 0, 1), row)
        # A beat that names none leaves the count alone; a reclaim's beat clears it.
        status, ok = self.beat(first["session_id"], first["token"], seats_free=0)
        status, listed = self.list_sessions()
        self.assertEqual(listed["sessions"][0].get("seats_held"), 1, listed)
        status, ok = self.beat(first["session_id"], first["token"], seats_free=0, seats_held=0)
        status, listed = self.list_sessions()
        self.assertEqual(listed["sessions"][0].get("seats_held"), 0, listed)
        status, err = self.beat(first["session_id"], first["token"], seats_held=-1)
        self.assertEqual((status, err), (400, {"error": "invalid_field", "field": "seats_held"}), err)

    def test_world_row_refuses_a_negative_spectator_count(self) -> None:
        self.start()
        world_id = str(uuid.uuid4())
        status, err = self.register(
            persistent_world=True,
            world_id=world_id,
            world_boot=1,
            resume_session_id=world_id,
            spectator_free=-1,
        )
        self.assertEqual(
            (status, err), (400, {"error": "invalid_field", "field": "spectator_free"}), err
        )
        status, created = self.register(
            persistent_world=True,
            world_id=world_id,
            world_boot=1,
            resume_session_id=world_id,
            spectator_free=0,
        )
        self.assertEqual(status, 200, created)
        status, err = self.beat(world_id, created["token"], spectator_free="many")
        self.assertEqual(
            (status, err), (400, {"error": "invalid_field", "field": "spectator_free"}), err
        )

    def test_world_resume_keeps_its_fields_and_rotates_the_token(self) -> None:
        self.start()
        world_id = str(uuid.uuid4())
        world = {
            "persistent_world": True,
            "world_id": world_id,
            "world_boot": 1,
            "resume_session_id": world_id,
        }
        status, first = self.register(**world)
        self.assertEqual((status, first["session_id"]), (200, world_id), first)
        status, second = self.register(
            **{**world, "world_boot": 2, "resume_token": first["token"]}
        )
        self.assertEqual((status, second["session_id"]), (200, world_id), second)
        self.assertNotEqual(second["token"], first["token"], second)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        rows = listed["sessions"]
        self.assertEqual(len(rows), 1, rows)
        self.assertEqual(
            (rows[0].get("persistent_world"), rows[0].get("world_id"), rows[0].get("world_boot")),
            (True, world_id, 2),
            rows[0],
        )
        # The rotated token is the live one: the old token no longer beats the row.
        status, refused = self.beat(world_id, first["token"])
        self.assertEqual(status, 403, refused)
        status, ok = self.beat(world_id, second["token"])
        self.assertEqual(status, 200, ok)

    def test_pending_public_registrations_are_bounded_and_expire(self) -> None:
        source_cap = getattr(session_directory, "MAX_PENDING_PER_SOURCE", 8)
        total_cap = getattr(session_directory, "MAX_PENDING_REGISTRATIONS", 256)
        store = session_directory.SessionDirectory(2, 1)
        payload = base64.b64encode(b"offer" * 200).decode()
        peak_source = 0
        for index in range(source_cap * 2 + total_cap * 2):
            source = "192.0.2.1" if index < source_cap * 2 else f"198.51.{index // 256}.{index % 256}"
            try:
                row = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), source, 0, INSTALL_KEY)
            except OverflowError:
                continue
            store.post_signal(row["session_id"], {"token_or_join_nonce": "joiner", "from": "client:joiner", "to": "host", "payload_b64": payload}, 0)
            peak_source = max(peak_source, sum(sess.observed_ip == "192.0.2.1" for sess in store._sessions.values()))
        peak_total = len(store._sessions)
        owners_before_ack = len(store._world_owners)
        store.prune(3)
        retained_payload = sum(sess.undrained_bytes for sess in store._register_replays.values())
        print(json.dumps({"case": "S1", "peak_source": peak_source, "peak_total": peak_total,
                          "owners_before_ack": owners_before_ack, "expired_replays": len(store._register_replays),
                          "retained_payload_bytes": retained_payload}))
        self.assertLessEqual(peak_source, source_cap, "S1: one source retained more pending registrations than its cap")
        self.assertLessEqual(peak_total, total_cap, "S1: many sources retained more pending registrations than the global cap")
        self.assertEqual(owners_before_ack, 0, "S1: unacknowledged registrations wrote durable owner records")
        self.assertEqual((len(store._register_replays), retained_payload), (0, 0), "S1: expired unacknowledged registrations retained their signal queues")
        with mock.patch.object(session_directory, "PRUNE_MAP_MAX", 8):
            limiter = DualRateLimiter()
            for index in range(24):
                limiter.check(f"{index:016d}", f"203.0.113.{index}", 0, False)
            self.assertLessEqual(limiter._map_size(), 8, "S1: sparse callers grew rate buckets beyond the map cap")

    def test_owner_writes_are_bounded_batched_and_outside_the_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(session_directory, "MAX_WORLD_OWNERS", 4):
            store = session_directory.SessionDirectory(15, 5, owner_state=Path(directory) / "world-owners.json", create_owner_key=True)
            self.addCleanup(store.stop)
            writes = []
            original = store._write_owner_file
            def observe(owners):
                writes.append((store._lock._is_owned(), time.monotonic(), len(owners), len(json.dumps(owners).encode())))
                return original(owners)
            with mock.patch.object(store, "_write_owner_file", side_effect=observe):
                for index in range(4):
                    row = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), f"192.0.2.{index}", index, INSTALL_KEY)
                    store.heartbeat(row["session_id"], {"token": row["token"], "peer_count": 2, "seats_free": 1}, index, INSTALL_KEY)
                replacement = store.register(sample_register(persistent_world=True, world_id=str(uuid.uuid4()), world_boot=1), "192.0.2.99", 5, INSTALL_KEY)
                store.heartbeat(replacement["session_id"], {"token": replacement["token"], "peer_count": 2, "seats_free": 1}, 5, INSTALL_KEY)
                self.assertEqual(len(store._world_owners), 4, "S1: durable ownership exceeded its count cap after retirement")
                last = max(owner["last_heartbeat_unix"] for owner in store._world_owners.values())
                with mock.patch.object(session_directory.time, "time", return_value=last + session_directory.OWNER_IDLE_S + 1):
                    store.prune(30)
                store._wait_owner_write(store._owner_revision)
            self.assertTrue(writes and all(not held for held, _when, _count, _bytes in writes), "S1: a durable owner write held the directory lock")
            self.assertTrue(all(count <= 4 and size <= session_directory.MAX_OWNER_STATE_BYTES for _held, _when, count, size in writes), "S1: an owner write exceeded its count or byte cap")
            self.assertTrue(all(right[1] - left[1] >= session_directory.OWNER_WRITE_INTERVAL_S for left, right in zip(writes, writes[1:])), "S1: owner writes bypassed the batch interval")
            self.assertEqual(len(store._world_owners), 0, "S1: ownership remained after thirty days without a heartbeat")

    def test_signaling_requires_host_proof_and_preserves_a_joiners_offer(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        row = store.register(sample_register(), "192.0.2.1", 0, INSTALL_KEY)
        sid = row["session_id"]
        payload = base64.b64encode(b"offer").decode()
        with self.assertRaises(PermissionError, msg="S2: an unauthenticated caller allocated a client-destination queue"):
            store.post_signal(sid, {"token_or_join_nonce": "stranger", "from": "client:stranger", "to": "client:invented", "payload_b64": payload}, 1)
        store.post_signal(sid, {"token_or_join_nonce": row["token"], "from": "host", "to": "client:legacy", "payload_b64": payload}, 1)
        self.assertEqual(len(store.get_signals(sid, "client:legacy", 0, None, 1)["signals"]), 1)
        store.post_signal(sid, {"token_or_join_nonce": "legitimate", "from": "client:legitimate", "to": "host", "payload_b64": payload}, 1, source_ip="198.51.100.1")
        for index in range(session_directory.MAX_QUEUE * 2):
            try:
                store.post_signal(sid, {"token_or_join_nonce": f"attack{index}", "from": f"client:attack{index}", "to": "host", "payload_b64": payload}, 1, source_ip="203.0.113.1")
            except BufferError:
                pass
        signals = store.get_signals(sid, "host", 0, row["token"], 2)["signals"]
        self.assertTrue(any(signal["from"] == "client:legitimate" for signal in signals), "S2: queue pressure removed a legitimate joiner's only offer")
        self.assertLessEqual(len(signals), session_directory.MAX_QUEUE, "S2: fair queue admission exceeded the queue bound")

    def test_capacity_refusal_preserves_the_active_lease_and_signals(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        world = str(uuid.uuid4())
        request = sample_register(persistent_world=True, world_id=world, world_boot=1)
        host = store.register(request, "192.0.2.1", 0, INSTALL_KEY)
        store.heartbeat(world, {"token": host["token"], "peer_count": 2, "seats_free": 1}, 1, INSTALL_KEY)
        store.post_signal(world, {"token_or_join_nonce": "joiner", "from": "client:joiner", "to": "host", "payload_b64": "b2ZmZXI="}, 1)
        with mock.patch.object(session_directory, "MAX_PENDING_REGISTRATIONS", 1):
            store.register(sample_register(), "198.51.100.1", 2, INSTALL_KEY)
            with self.assertRaises(OverflowError):
                store.register(dict(request, resume_session_id=world, resume_token=host["token"]), "192.0.2.1", 3, INSTALL_KEY)
        signals = store.get_signals(world, "host", 0, host["token"], 4)["signals"]
        self.assertEqual(len(signals), 1, "S2: a capacity-refused registration discarded the active joiner's offer")

    def test_relay_helper_preserves_the_source_and_creates_its_signing_key(self) -> None:
        tools = str(Path(__file__).resolve().parents[1])
        with mock.patch.object(sys, "path", [tools, *sys.path]):
            import relay_cloudflare_match as relay
            import edith_cross
            from relay_secrets import SecretBook
        server = mock.MagicMock()
        original_post = server.store.post_signal
        packet = dict(token_or_join_nonce="joiner", **{"from": "client:joiner", "to": "host", "payload_b64": "YQ=="})
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cert, key = root / "cert.pem", root / "key.pem"
            key.write_text("fixture")
            package = SimpleNamespace(session_directory=session_directory)
            with mock.patch.dict(sys.modules, {"session_directory": package}), \
                 mock.patch.object(edith_cross, "make_cert", return_value=(cert, key, "0" * 64)), \
                 mock.patch.object(session_directory, "spawn_server", return_value=server) as factory:
                helper = relay.Directory(root, 0, None, 600, SecretBook())
            try:
                try:
                    server.store.post_signal("session", packet, 10, "192.0.2.42")
                except TypeError:
                    self.fail("S2: relay helper rejected the handler's source address")
                original_post.assert_called_once_with("session", packet, 10, "192.0.2.42")
                self.assertEqual(factory.call_args.kwargs.get("create_owner_key"), True,
                                 "S4: fresh relay directory omitted its explicit signing-key creation")
            finally:
                helper.stop()

    def test_local_video_and_mint_helpers_create_their_signing_keys(self) -> None:
        tools = str(Path(__file__).resolve().parents[1])
        nested = "session_directory.session_directory"
        previous = sys.modules.get(nested)
        sys.modules[nested] = session_directory
        try:
            with mock.patch.object(sys, "path", [tools, *sys.path]):
                from e2e import directory as video
        finally:
            if previous is None:
                del sys.modules[nested]
            else:
                sys.modules[nested] = previous
        with mock.patch.object(sys, "path", [tools, *sys.path]):
            import relay_cloudflare_mint as mint
        class StartupCaptured(Exception):
            pass
        missing = []
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ("video", "mint"):
                arguments = {}
                def capture(**kwargs):
                    arguments.update(kwargs)
                    raise StartupCaptured()
                if name == "video":
                    with mock.patch.object(video, "spawn_server", side_effect=capture), self.assertRaises(StartupCaptured):
                        with video.serve(root / name, 47497, block=(47460, 47499)):
                            pass
                else:
                    with mock.patch.object(mint, "load_directory", return_value=session_directory), \
                         mock.patch.object(mint, "read_turn_config", return_value={}), \
                         mock.patch.object(mint, "record_provider"), \
                         mock.patch.object(session_directory, "spawn_server", side_effect=capture), self.assertRaises(StartupCaptured):
                        mint.main(["--turn-config", str(root / "unused.json"), "--out", str(root / name)])
                if arguments.get("create_owner_key") is not True:
                    missing.append(name)
        self.assertEqual(missing, [], "S4: fresh local helpers omitted their explicit signing-key creation: " + ",".join(missing))

    def test_world_tick_receipts_keep_live_and_private_replay_comparisons(self) -> None:
        host = {"live": {1: "a" * 64, 2: "b" * 64, 3: "c" * 64}, "catchup": {}}
        peer = {"live": {1: "a" * 64, 3: "c" * 64}, "catchup": {2: "b" * 64}}
        scored = compare_world_ticks(host, peer)
        self.assertTrue(scored["pass"], "world ticks: recorded private replay tick 2 was reported missing")
        self.assertEqual((scored["compared"], scored["compared_catchup"], scored["live_only_holes"]), (2, 1, 1))
        with self.subTest("missing_replay"):
            self.assertFalse(compare_world_ticks(host, dict(peer, catchup={}))["pass"], "world ticks: an unrecorded gap passed")
        with self.subTest("wrong_replay"):
            self.assertFalse(compare_world_ticks(host, dict(peer, catchup={2: "d" * 64}))["pass"], "world ticks: a mismatching replay passed")
        with self.subTest("wrong_live"):
            self.assertFalse(compare_world_ticks(host, dict(peer, live={1: "d" * 64, 3: "c" * 64}))["pass"], "world ticks: a mismatching live tick passed")
        with self.subTest("never_played"):
            self.assertFalse(compare_world_ticks(host, dict(peer, live={}))["pass"], "world ticks: a peer that never played passed")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "live.jsonl"
            rows = [{"tick": tick, "phase": phase, "sim_gated": value} for phase, ticks in peer.items() for tick, value in ticks.items()]
            path.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
            self.assertTrue(compare_world_ticks(host, read_world_ticks(path.parent))["pass"])
            with path.open("a") as stream:
                stream.write(json.dumps({"tick": 2, "phase": "live", "sim_gated": "d" * 64}) + "\n")
            with self.assertRaisesRegex(ValueError, "contradictory world tick", msg="world ticks: contradictory phases hid a mismatching tick"):
                read_world_ticks(path.parent)

    def test_rotated_lease_refuses_an_old_long_poll_before_draining(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        now = time.monotonic()
        world = str(uuid.uuid4())
        request = sample_register(persistent_world=True, world_id=world, world_boot=1)
        first = store.register(request, "192.0.2.1", now, INSTALL_KEY)
        waiting = threading.Event()
        original_wait = store._signals_changed.wait
        def observed_wait(timeout=None):
            waiting.set()
            return original_wait(timeout)
        answer = []
        def poll():
            try:
                answer.append(store.get_signals(world, "host", 10000, first["token"], time.monotonic(), 0.25))
            except Exception as error:
                answer.append(error)
        with mock.patch.object(store._signals_changed, "wait", side_effect=observed_wait):
            thread = threading.Thread(target=poll)
            thread.start()
            self.assertTrue(waiting.wait(2), "S3: authenticated long poll never reached its wait")
            replacement = store.register(dict(request, resume_session_id=world, resume_token=first["token"]), "192.0.2.1", time.monotonic(), INSTALL_KEY)
            store.post_signal(world, {"token_or_join_nonce": "newjoiner", "from": "client:newjoiner", "to": "host", "payload_b64": "b2ZmZXI="}, time.monotonic())
            thread.join(2)
        self.assertFalse(thread.is_alive(), "S3: rotated long poll did not finish with a refusal")
        self.assertTrue(answer and isinstance(answer[0], PermissionError), "S3: revoked long poll adopted the replacement lease and applied its cursor")
        self.assertEqual(len(store.get_signals(world, "host", 0, replacement["token"], time.monotonic())["signals"]), 1,
                         "S3: revoked long poll drained the replacement joiner's signal")

    def test_first_start_requires_create_key_and_keeps_signed_proofs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            owner_file = Path(directory) / "world-owners.json"
            with self.assertRaisesRegex(ValueError, "create-owner-key", msg="S4: first startup omitted explicit signing-key creation"):
                session_directory.SessionDirectory(15, 5, owner_state=owner_file)
            store = session_directory.SessionDirectory(15, 5, owner_state=owner_file, create_owner_key=True)
            world = str(uuid.uuid4())
            request = sample_register(persistent_world=True, world_id=world, world_boot=1)
            restored = store.register(request, "192.0.2.1", 0, INSTALL_KEY)
            store.heartbeat(world, {"token": restored["token"], "peer_count": 2, "seats_free": 1}, 1, INSTALL_KEY)
            store.stop()
            restarted = session_directory.SessionDirectory(15, 5, owner_state=owner_file)
            resumed = restarted.register(dict(request, world_boot=2, resume_session_id=world, resume_token=restored["token"]), "192.0.2.1", 2, INSTALL_KEY)
            self.assertEqual(resumed["session_id"], world, "S4: signed proof could not restore its world after restart")
            with self.assertRaises(PermissionError, msg="S4: stranger claimed a signed world after restart"):
                restarted.register(request, "203.0.113.1", 3, INSTALL_KEY)
            self.assertNotIn(restored["token"], owner_file.read_text(), "S4: owner file stored a plaintext issued proof")
            restarted.stop()

    def test_previous_proof_recovers_a_lost_reply_after_host_restart(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        world = str(uuid.uuid4())
        request = sample_register(persistent_world=True, world_id=world, world_boot=1)
        first = store.register(request, "192.0.2.1", 0, INSTALL_KEY)
        store.heartbeat(world, {"token": first["token"], "peer_count": 2, "seats_free": 1}, 1, INSTALL_KEY)
        lost = store.register(dict(request, resume_session_id=world, resume_token=first["token"]), "192.0.2.1", 2, INSTALL_KEY)
        try:
            recovered = store.register(dict(request, world_boot=2, resume_session_id=world, resume_token=first["token"]), "192.0.2.1", 3, INSTALL_KEY)
        except PermissionError:
            self.fail("S5: previous owner proof was refused after a lost resume reply and host boot change")
        self.assertEqual(recovered["session_id"], world, "S5: recovery registered a second world id")
        self.assertEqual(len(store._sessions), 1, "S5: recovery retained two live world rows")
        self.assertEqual(recovered["token"], lost["token"], "S5: an unacknowledged recovery rotated its replacement again")

    def test_aborted_get_redacts_token_and_client_nonce(self) -> None:
        self.server = spawn_server("127.0.0.1", 47493, expiry_s=15, heartbeat_s=5, caller_mode="direct")
        store = self.server.store
        row = store.register(sample_register(), "127.0.0.1", time.monotonic(), INSTALL_KEY)
        nonce = "nonce-that-must-not-be-logged"
        entered = threading.Event()
        aborted = threading.Event()
        records = []
        original = store.get_signals
        def observed_get(*args, **kwargs):
            entered.set()
            return original(*args, **kwargs)
        class Capture(logging.Handler):
            def emit(self, record):
                records.append(record.getMessage())
                if "client aborted" in records[-1]:
                    aborted.set()
        handler = Capture()
        previous_handlers = LOGGER.handlers[:]
        LOGGER.handlers = [handler]
        previous_level = LOGGER.level
        LOGGER.setLevel(logging.INFO)
        try:
            with mock.patch.object(store, "get_signals", side_effect=observed_get):
                for peer_key, token_key in (("peer", "token"), ("%70eer", "t%6fken")):
                    entered.clear(); aborted.clear()
                    connection = socket.create_connection(("127.0.0.1", self.server.port), timeout=3)
                    path = f'/v1/sessions/{row["session_id"]}/signals?{peer_key}=client:{nonce}&{token_key}={row["token"]}&wait=0.2'
                    connection.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\nX-Install-Key: {INSTALL_KEY}\r\nConnection: close\r\n\r\n".encode())
                    self.assertTrue(entered.wait(2), "B2: GET never entered the signal long poll")
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh" if os.name == "nt" else "ii", 1, 0))
                    connection.close()
                    self.assertTrue(aborted.wait(3), "B2: reset GET never reached the abort logging path")
        finally:
            LOGGER.handlers = previous_handlers
            LOGGER.setLevel(previous_level)
        text = "\n".join(records)
        self.assertTrue(all(secret not in text for secret in (row["token"], nonce)), "B2: aborted GET wrote a token or client nonce to the log")

    def test_lost_world_register_reply_replays_the_same_lease(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        world_id = str(uuid.uuid4())
        first_request = sample_register(persistent_world=True, world_id=world_id,
                                        world_boot=1, resume_session_id=world_id)
        first = store.register(first_request, "192.0.2.1", 10, INSTALL_KEY)
        resume = dict(first_request, world_boot=2, resume_token=first["token"])
        moved = store.register(resume, "192.0.2.1", 20, INSTALL_KEY)
        store.post_signal(world_id, {"from": "client:joiner", "to": "host",
                                     "token_or_join_nonce": "joiner", "payload_b64": "YQ=="}, 21)
        try:
            retried = store.register(resume, "192.0.2.1", 25, INSTALL_KEY)
        except PermissionError:
            self.fail("F2: a lost successful resume reply made the old-token retry forbidden")
        self.assertEqual(retried, moved, "F2: a repeated successful resume issued another lease")
        self.assertEqual(len(store.list_sessions(26, None, None, None)["sessions"]), 1, "F2: register retries duplicated the world")
        self.assertEqual(len(store.get_signals(world_id, "host", 0, moved["token"], 26)["signals"]), 1,
                         "F2: a repeated register discarded a joiner's pending signal")
        store.heartbeat(world_id, {"token": moved["token"], "peer_count": 1, "seats_free": 1}, 26, INSTALL_KEY)
        with self.assertRaises(PermissionError, msg="F2: another install replayed a tokenless world claim"):
            store.register(first_request, "192.0.2.1", 27, "fedcba9876543210")
        with self.assertRaises(PermissionError, msg="F2: an old register replay outlived its retry window"):
            store.register(resume, "192.0.2.1", 26 + session_directory.RESUME_GRACE_S + 1, INSTALL_KEY)
        first_request = dict(first_request, world_id=str(uuid.uuid4()))
        first_request["resume_session_id"] = first_request["world_id"]
        first = store.register(first_request, "192.0.2.1", 200, INSTALL_KEY)
        try:
            repeated = store.register(first_request, "192.0.2.1", 205, INSTALL_KEY)
        except PermissionError:
            self.fail("F2: a lost first world register reply made the identical retry forbidden")
        self.assertEqual(repeated, first, "F2: an identical first register retry changed its lease")
        short = session_directory.SessionDirectory(3, 1)
        first = short.register(first_request, "192.0.2.1", 10, INSTALL_KEY)
        self.assertEqual(short.register(first_request, "192.0.2.1", 15, INSTALL_KEY)["session_id"], first["session_id"],
                         "F2: a lost register reply changed the world id after its unacknowledged lease expired")

    def test_world_owner_survives_a_directory_restart(self) -> None:
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(session_directory, "configure_logging"):
            log_file = Path(folder) / "directory.log"
            world_id = str(uuid.uuid4())
            request = sample_register(persistent_world=True, world_id=world_id,
                                      world_boot=1, resume_session_id=world_id)
            first = spawn_server(log_file=log_file, create_owner_key=True, caller_mode="direct")
            try:
                now = time.monotonic()
                created = first.store.register(request, "192.0.2.1", now, INSTALL_KEY)
                first.store.heartbeat(world_id, {"token": created["token"], "peer_count": 1, "seats_free": 1}, now, INSTALL_KEY)
            finally:
                first.stop()
            second = spawn_server(log_file=log_file, caller_mode="direct")
            try:
                now = time.monotonic()
                try:
                    second.store.register(request, "192.0.2.2", now, "fedcba9876543210")
                except PermissionError:
                    pass
                else:
                    self.fail("F3: a tokenless host claimed a known world id after the directory restarted")
                resumed = second.store.register(dict(request, world_boot=2, resume_token=created["token"]),
                                                "192.0.2.1", now + 1, INSTALL_KEY)
                self.assertEqual(resumed["session_id"], world_id, "F3: the proven owner lost its world id after restart")
                second.store.heartbeat(world_id, {"token": resumed["token"], "peer_count": 1, "seats_free": 1}, now + 1, INSTALL_KEY)
                proof = Path(folder) / "world-owners.json"
                self.assertTrue(proof.is_file(), "F3: no durable world owner file was written")
                self.assertNotIn(created["token"], proof.read_text(), "F3: the owner file exposed the original token")
                self.assertNotIn(resumed["token"], proof.read_text(), "F3: the owner file exposed the resumed token")
            finally:
                second.stop()
            third = spawn_server(log_file=log_file, caller_mode="direct")
            try:
                with self.assertRaises(PermissionError, msg="F3: a rotated token recovered ownership after another restart"):
                    third.store.register(dict(request, resume_token=created["token"]), "192.0.2.1", time.monotonic(), INSTALL_KEY)
                third.store.register(dict(request, world_boot=3, resume_token=resumed["token"]),
                                     "192.0.2.1", time.monotonic(), INSTALL_KEY)
            finally:
                third.stop()

    def test_lost_world_reply_recovers_even_when_the_service_restarts(self) -> None:
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(session_directory, "configure_logging"):
            log = Path(folder) / "directory.log"
            world_id = str(uuid.uuid4())
            request = sample_register(persistent_world=True, world_id=world_id,
                                      world_boot=1, resume_session_id=world_id)
            first = spawn_server(log_file=log, create_owner_key=True, caller_mode="direct")
            try:
                created = first.store.register(request, "192.0.2.1", time.monotonic(), INSTALL_KEY)
            finally:
                first.stop()
            second = spawn_server(log_file=log, caller_mode="direct")
            try:
                with self.assertRaises(PermissionError, msg="F2: a different install replayed a lost first registration"):
                    second.store.register(request, "192.0.2.1", time.monotonic(), "fedcba9876543210")
                recovered = second.store.register(request, "192.0.2.1", time.monotonic(), INSTALL_KEY)
                self.assertEqual(recovered["session_id"], world_id, "F2: a lost first reply and restart changed the world id")
                resume = dict(request, world_boot=2, resume_token=recovered["token"])
                lost = second.store.register(resume, "192.0.2.1", time.monotonic(), INSTALL_KEY)
            finally:
                second.stop()
            third = spawn_server(log_file=log, caller_mode="direct")
            try:
                replayed = third.store.register(resume, "192.0.2.1", time.monotonic(), INSTALL_KEY)
                self.assertEqual(replayed["session_id"], lost["session_id"], "F2: a lost resume reply and restart changed the world id")
                third.store.heartbeat(world_id, {"token": replayed["token"], "peer_count": 1, "seats_free": 1}, time.monotonic(), INSTALL_KEY)
            finally:
                third.stop()

    def test_world_owner_cannot_be_bypassed_with_an_alias_or_same_generation(self) -> None:
        store = session_directory.SessionDirectory(300, 5)
        world_id = str(uuid.uuid4())
        request = sample_register(persistent_world=True, world_id=world_id, world_boot=1, resume_session_id=world_id)
        first = store.register(request, "192.0.2.1", 10, INSTALL_KEY)
        moved_request = dict(request, migration_gen=1, resume_token=first["token"])
        moved = store.register(moved_request, "192.0.2.2", 20, "fedcba9876543210")
        current = dict(moved_request, resume_token=moved["token"])
        own = store.register(current, "192.0.2.2", 21, "fedcba9876543210")
        store.heartbeat(world_id, {"token": own["token"], "peer_count": 1, "seats_free": 1}, 21, "fedcba9876543210")
        self.assertEqual(own["session_id"], world_id, "F3: the current world host could not refresh its own generation")
        with self.assertRaises(session_directory.Superseded, msg="F3: a different host refreshed the same world generation"):
            store.register(dict(current, resume_token=own["token"]), "192.0.2.3", 22, "cccccccccccccccc")
        alias = dict(request)
        alias.pop("resume_session_id")
        with self.assertRaises(PermissionError, msg="F3: an omitted resume id bypassed world ownership"):
            store.register(alias, "192.0.2.3", 23, "cccccccccccccccc")
        with self.assertRaises(PermissionError, msg="F3: a new resume id duplicated a known world"):
            store.register(dict(request, resume_session_id=str(uuid.uuid4())), "192.0.2.3", 24, "cccccccccccccccc")
        for alternate in (world_id.upper(), world_id.replace("-", "")):
            with self.assertRaises(PermissionError, msg="F3: an alternate UUID spelling duplicated a known world's owner"):
                store.register(dict(request, world_id=alternate, resume_session_id=alternate), "192.0.2.3", 25, "cccccccccccccccc")
        store.prune(321)
        try:
            own = store.register(dict(current, resume_token=own["token"]), "192.0.2.2", 322, "abababababababab")
        except session_directory.Superseded:
            self.fail("F3: stored owner proof could not resume an expired world when the install identity changed")
        store.heartbeat(world_id, {"token": own["token"], "peer_count": 1, "seats_free": 1}, 322, "abababababababab")
        with tempfile.TemporaryDirectory() as temporary:
            owner_file = Path(temporary) / "world-owners.json"
            owner_file.write_text(json.dumps({world_id.upper(): store._world_owners[world_id]}), encoding="utf-8")
            restarted = session_directory.SessionDirectory(300, 5, owner_state=owner_file, create_owner_key=True)
            self.addCleanup(restarted.stop)
            with self.assertRaises(PermissionError, msg="F3: a noncanonical saved owner allowed a tokenless claim"):
                restarted.register(request, "192.0.2.3", 26, "cccccccccccccccc")
            try:
                resumed = restarted.register(dict(current, world_id=world_id.upper(), resume_session_id=world_id.upper(), resume_token=own["token"]),
                                             "192.0.2.2", 27, "edededededededed")
            except session_directory.Superseded:
                self.fail("F3: stored owner proof could not resume after restart when the install identity changed")
            self.assertEqual(resumed["session_id"], world_id, "F3: a proved alternate spelling changed the world id")
            restarted.heartbeat(world_id, {"token": resumed["token"], "peer_count": 1, "seats_free": 1}, 27, "edededededededed")
            self.assertEqual(list(json.loads(owner_file.read_text(encoding="utf-8"))), [world_id], "F3: saved ownership retained duplicate spellings")

    def test_lost_register_reply_survives_live_updates_and_a_long_outage(self) -> None:
        store = session_directory.SessionDirectory(15, 5)
        world_id = str(uuid.uuid4())
        request = sample_register(persistent_world=True, world_id=world_id, world_boot=1, resume_session_id=world_id)
        created = store.register(request, "192.0.2.1", 10, INSTALL_KEY)
        resume = dict(request, world_boot=2, resume_token=created["token"])
        lost = store.register(resume, "192.0.2.1", 11, INSTALL_KEY)
        try:
            retried = store.register(dict(resume, seats_free=0, peer_count=3, listen_addrs=["192.0.2.9"]),
                                     "192.0.2.9", 600, INSTALL_KEY)
        except PermissionError:
            self.fail("F2: a live listing update or long outage invalidated the host's unacknowledged register proof")
        self.assertEqual((retried["session_id"], retried["token"]), (lost["session_id"], lost["token"]),
                         "F2: recovery after a long outage changed the world lease")
        self.assertEqual(store.list_sessions(601, None, None, None)["sessions"][0]["seats_free"], 0,
                         "F2: replaying a register lost the host's updated seat count")

    def test_world_resume_without_the_row_token_is_refused(self) -> None:
        self.start()
        world_id = str(uuid.uuid4())
        world = {
            "persistent_world": True,
            "world_id": world_id,
            "world_boot": 1,
            "resume_session_id": world_id,
        }
        status, first = self.register(**world)
        self.assertEqual(status, 200, first)
        status, seized = self.register(**{**world, "world_boot": 9})
        self.assertEqual((status, seized), (403, {"error": "forbidden"}), seized)
        status, wrong = self.register(
            **{**world, "world_boot": 9, "resume_token": "not-the-row-token"}
        )
        self.assertEqual((status, wrong), (403, {"error": "forbidden"}), wrong)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(listed["sessions"][0].get("world_boot"), 1, listed["sessions"][0])
        # The real holder still owns the row.
        status, ok = self.beat(world_id, first["token"])
        self.assertEqual(status, 200, ok)

    def test_full_precedes_field_validation(self) -> None:
        self.start()
        with mock.patch.object(session_directory, "MAX_ROWS", 1):
            status, created = self.register(name="only")
            self.assertEqual(status, 200, created)
            status, err = self.register(name="second", listen_port=0)
            self.assertEqual((status, err), (503, {"error": "full"}), err)

    def test_legacy_requests_default_visible(self) -> None:
        self.start()
        status, created = self.register()
        self.assertEqual(status, 200)
        sid, token = created["session_id"], created["token"]
        status, reply = self.beat(sid, token)
        self.assertEqual(status, 200)
        self.assertIs(reply["listed"], True)
        self.assertTrue(self._session(sid).listed)
        status, listed = self.list_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(len(listed["sessions"]), 1)
        self.assertNotIn("listed", listed["sessions"][0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
