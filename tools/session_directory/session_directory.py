#!/usr/bin/python3
"""In-memory JSON session directory (HTTP or HTTPS)."""

from __future__ import annotations

import argparse
import base64
import binascii
import hmac
import json
import logging
import re
import secrets
import socket
import ssl
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Any, Optional
from urllib.parse import parse_qs, urlparse

LOGGER = logging.getLogger("session_directory")

MAX_ROWS = 4096
MAX_STR = 64
MAX_ARR = 8
MAX_QUEUE = 256
MAX_PAYLOAD = 64 * 1024
MAX_BODY = 128 * 1024
REG_PER_MIN = 10
REQ_PER_MIN = 120
IP_REG_PER_MIN = 30
IP_REQ_PER_MIN = 300
RATE_WINDOW_S = 60.0
MAX_DEST_QUEUES = 16
MAX_SESSION_PAYLOAD = 1024 * 1024
QUEUE_IDLE_S = 120.0
HANDLER_TIMEOUT_S = 10
HANDSHAKE_TIMEOUT_S = 5
INSTALL_KEY_MIN = 16
INSTALL_KEY_MAX = 32
INSTALL_KEY_CHARS = frozenset(
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_"
)
VALID_JOIN_MODES = frozenset({"ip", "ice", "either"})
VALID_STATES = frozenset({"lobby", "running"})
REGISTER_STR_FIELDS = (
    "name",
    "activity",
    "scene",
    "mode",
    "game_version",
    "build_id",
    "match_config_hash",
    "session_identity_hash",
    "module_manifest_hash",
    "join_mode",
)
REGISTER_INT_FIELDS = (
    "peer_count",
    "seats_free",
    "network_protocol_version",
    "lockstep_codec_version",
    "controller_frame_version",
    "listen_port",
)
LIST_ROW_FIELDS = REGISTER_STR_FIELDS + REGISTER_INT_FIELDS + ("listen_addrs",)


class FieldError(Exception):
    def __init__(self, error: str, field: str) -> None:
        super().__init__(error, field)
        self.error = error
        self.field = field

    def body(self) -> dict[str, str]:
        return {"error": self.error, "field": self.field}


def tokens_equal(left: str, right: str) -> bool:
    if len(left) != len(right):
        return False
    return hmac.compare_digest(left, right)


def valid_install_key(key: str) -> bool:
    if not INSTALL_KEY_MIN <= len(key) <= INSTALL_KEY_MAX:
        return False
    return all(ch in INSTALL_KEY_CHARS for ch in key)


def valid_peer(peer: str) -> bool:
    if peer == "host":
        return True
    if peer.startswith("client:") and 1 <= len(peer) - 7 <= MAX_STR:
        return all(ch in INSTALL_KEY_CHARS for ch in peer[7:])
    return False


def client_nonce(peer: str) -> str:
    return peer[7:]


def require_str(data: dict[str, Any], field: str) -> str:
    if field not in data:
        raise FieldError("missing_field", field)
    value = data[field]
    if not isinstance(value, str):
        raise FieldError("invalid_field", field)
    if len(value) > MAX_STR:
        raise FieldError("invalid_field", field)
    return value


def require_str_unbounded(data: dict[str, Any], field: str) -> str:
    if field not in data:
        raise FieldError("missing_field", field)
    value = data[field]
    if not isinstance(value, str):
        raise FieldError("invalid_field", field)
    return value


def require_int(data: dict[str, Any], field: str, min_v: int, max_v: int) -> int:
    if field not in data:
        raise FieldError("missing_field", field)
    value = data[field]
    if isinstance(value, bool) or not isinstance(value, int):
        raise FieldError("invalid_field", field)
    if value < min_v or value > max_v:
        raise FieldError("invalid_field", field)
    return value


def require_listen_addrs(data: dict[str, Any], field: str = "listen_addrs") -> list[str]:
    if field not in data:
        raise FieldError("missing_field", field)
    value = data[field]
    if not isinstance(value, list):
        raise FieldError("invalid_field", field)
    if len(value) > MAX_ARR:
        raise FieldError("invalid_field", field)
    addrs: list[str] = []
    for item in value:
        if not isinstance(item, str) or len(item) > MAX_STR:
            raise FieldError("invalid_field", field)
        addrs.append(item)
    return addrs


def as_json_int(value: float) -> int:
    return int(value)


class RateLimiter:
    def __init__(self, req_per_min: int, reg_per_min: int) -> None:
        self.req_per_min = req_per_min
        self.reg_per_min = reg_per_min
        self._requests: dict[str, list[float]] = {}
        self._registers: dict[str, list[float]] = {}

    def _trim(self, bucket: dict[str, list[float]], key: str, now: float) -> list[float]:
        kept = [ts for ts in bucket.get(key, []) if now - ts < RATE_WINDOW_S]
        bucket[key] = kept
        return kept

    def _retry_after(self, events: list[float], now: float) -> int:
        if not events:
            return 1
        wait = RATE_WINDOW_S - (now - min(events))
        return max(1, int(wait) if wait == int(wait) else int(wait) + 1)

    def probe(
        self, key: str, now: float, is_register: bool
    ) -> Optional[tuple[int, dict[str, Any]]]:
        reqs = self._trim(self._requests, key, now)
        if len(reqs) >= self.req_per_min:
            return (
                429,
                {
                    "error": "rate_limited",
                    "retry_after_s": self._retry_after(reqs, now),
                },
            )
        if is_register:
            regs = self._trim(self._registers, key, now)
            if len(regs) >= self.reg_per_min:
                return (
                    429,
                    {
                        "error": "rate_limited",
                        "retry_after_s": self._retry_after(regs, now),
                    },
                )
        return None

    def commit(self, key: str, now: float, is_register: bool) -> None:
        reqs = self._trim(self._requests, key, now)
        reqs.append(now)
        self._requests[key] = reqs
        if is_register:
            regs = self._trim(self._registers, key, now)
            regs.append(now)
            self._registers[key] = regs


class DualRateLimiter:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._by_key = RateLimiter(REQ_PER_MIN, REG_PER_MIN)
        self._by_ip = RateLimiter(IP_REQ_PER_MIN, IP_REG_PER_MIN)

    def _stricter(
        self,
        left: Optional[tuple[int, dict[str, Any]]],
        right: Optional[tuple[int, dict[str, Any]]],
    ) -> Optional[tuple[int, dict[str, Any]]]:
        if left is None:
            return right
        if right is None:
            return left
        left_wait = int(left[1]["retry_after_s"])
        right_wait = int(right[1]["retry_after_s"])
        return left if left_wait >= right_wait else right

    def check(
        self, install_key: str, client_ip: str, now: float, is_register: bool
    ) -> Optional[tuple[int, dict[str, Any]]]:
        with self._lock:
            key_hit = self._by_key.probe(install_key, now, is_register)
            ip_hit = self._by_ip.probe(client_ip, now, is_register)
            blocked = self._stricter(key_hit, ip_hit)
            if blocked is not None:
                return blocked
            self._by_key.commit(install_key, now, is_register)
            self._by_ip.commit(client_ip, now, is_register)
        return None


class Signal:
    def __init__(
        self,
        seq: int,
        from_peer: str,
        to_peer: str,
        payload_b64: str,
        payload_len: int,
    ) -> None:
        self.seq = seq
        self.from_peer = from_peer
        self.to_peer = to_peer
        self.payload_b64 = payload_b64
        self.payload_len = payload_len

    def as_json(self) -> dict[str, Any]:
        return {
            "seq": self.seq,
            "from": self.from_peer,
            "to": self.to_peer,
            "payload_b64": self.payload_b64,
        }


class Session:
    def __init__(
        self,
        session_id: str,
        token: str,
        fields: dict[str, Any],
        observed_ip: str,
        created_at: float,
    ) -> None:
        self.session_id = session_id
        self.token = token
        self.fields = fields
        self.observed_ip = observed_ip
        self.created_at = created_at
        self.last_beat = created_at
        self.state = "lobby"
        self.queues: dict[str, list[Signal]] = {}
        self.next_seq: dict[str, int] = {}
        self.queue_drain_at: dict[str, float] = {}
        self.undrained_bytes = 0

    def age_s(self, now: float) -> int:
        return max(0, int(now - self.created_at))

    def as_list_row(self, now: float) -> dict[str, Any]:
        row = {key: self.fields[key] for key in LIST_ROW_FIELDS}
        row["session_id"] = self.session_id
        row["age_s"] = self.age_s(now)
        row["observed_ip"] = self.observed_ip
        row["state"] = self.state
        return row


class SessionDirectory:
    def __init__(
        self, expiry_s: float, heartbeat_s: float, queue_idle_s: float = QUEUE_IDLE_S
    ) -> None:
        self.expiry_s = expiry_s
        self.heartbeat_s = heartbeat_s
        self.queue_idle_s = queue_idle_s
        self._lock = threading.RLock()
        self._sessions: dict[str, Session] = {}
        self.limiter = DualRateLimiter()
        self._stop = threading.Event()
        self._pruner = threading.Thread(
            target=self._prune_loop, name="session-prune", daemon=True
        )

    def start_pruner(self) -> None:
        if not self._pruner.is_alive():
            self._pruner.start()

    def stop(self) -> None:
        self._stop.set()

    def _prune_loop(self) -> None:
        while not self._stop.wait(1.0):
            self.prune(time.monotonic())

    def _prune_idle_queues(self, sess: Session, now: float) -> None:
        drop = [
            peer
            for peer, last in sess.queue_drain_at.items()
            if peer != "host" and now - last >= self.queue_idle_s
        ]
        for peer in drop:
            for item in sess.queues.get(peer, []):
                sess.undrained_bytes -= item.payload_len
            sess.queues.pop(peer, None)
            sess.queue_drain_at.pop(peer, None)
            sess.next_seq.pop(peer, None)

    def prune(self, now: float) -> None:
        with self._lock:
            dead = [
                sid
                for sid, sess in self._sessions.items()
                if now - sess.last_beat >= self.expiry_s
            ]
            for sid in dead:
                del self._sessions[sid]
            for sess in self._sessions.values():
                self._prune_idle_queues(sess, now)

    def _get(self, session_id: str, now: float) -> Optional[Session]:
        self.prune(now)
        return self._sessions.get(session_id)

    def register(self, data: dict[str, Any], observed_ip: str, now: float) -> dict[str, Any]:
        self.prune(now)
        with self._lock:
            if len(self._sessions) >= MAX_ROWS:
                raise OverflowError("full")
            fields: dict[str, Any] = {}
            for name in REGISTER_STR_FIELDS:
                fields[name] = require_str(data, name)
            if fields["join_mode"] not in VALID_JOIN_MODES:
                raise FieldError("invalid_field", "join_mode")
            for name in REGISTER_INT_FIELDS:
                if name == "listen_port":
                    fields[name] = require_int(data, name, 1, 65535)
                else:
                    fields[name] = require_int(data, name, 0, 10**9)
            fields["listen_addrs"] = require_listen_addrs(data)
            session_id = str(uuid.uuid4())
            token = secrets.token_urlsafe(24)
            sess = Session(session_id, token, fields, observed_ip, now)
            self._sessions[session_id] = sess
        return {
            "session_id": session_id,
            "token": token,
            "expires_in_s": as_json_int(self.expiry_s),
            "heartbeat_s": as_json_int(self.heartbeat_s),
            "observed_ip": observed_ip,
        }

    def heartbeat(
        self, session_id: str, data: dict[str, Any], now: float
    ) -> dict[str, Any]:
        token = require_str(data, "token")
        peer_count = require_int(data, "peer_count", 0, 10**9)
        seats_free = require_int(data, "seats_free", 0, 10**9)
        listen_addrs: Optional[list[str]] = None
        if "listen_addrs" in data:
            listen_addrs = require_listen_addrs(data)
        state: Optional[str] = None
        if "state" in data:
            state = require_str(data, "state")
            if state not in VALID_STATES:
                raise FieldError("invalid_field", "state")
        with self._lock:
            sess = self._get(session_id, now)
            if sess is None:
                raise KeyError("not_found")
            if not tokens_equal(token, sess.token):
                raise PermissionError("forbidden")
            sess.fields["peer_count"] = peer_count
            sess.fields["seats_free"] = seats_free
            if listen_addrs is not None:
                sess.fields["listen_addrs"] = listen_addrs
            if state is not None:
                sess.state = state
            sess.last_beat = now
        return {
            "expires_in_s": as_json_int(self.expiry_s),
            "heartbeat_s": as_json_int(self.heartbeat_s),
        }

    def delete(self, session_id: str, data: dict[str, Any], now: float) -> dict[str, Any]:
        token = require_str(data, "token")
        with self._lock:
            sess = self._get(session_id, now)
            if sess is None:
                raise KeyError("not_found")
            if not tokens_equal(token, sess.token):
                raise PermissionError("forbidden")
            del self._sessions[session_id]
        return {"ok": True}

    def list_sessions(
        self,
        now: float,
        mode: Optional[str],
        activity: Optional[str],
        state: Optional[str],
    ) -> dict[str, Any]:
        with self._lock:
            self.prune(now)
            rows: list[dict[str, Any]] = []
            for sess in self._sessions.values():
                if mode is not None and sess.fields["mode"] != mode:
                    continue
                if activity is not None and sess.fields["activity"] != activity:
                    continue
                if state is not None and sess.state != state:
                    continue
                rows.append(sess.as_list_row(now))
        return {"sessions": rows}

    def post_signal(
        self, session_id: str, data: dict[str, Any], now: float
    ) -> dict[str, Any]:
        token_or_nonce = require_str(data, "token_or_join_nonce")
        from_peer = require_str(data, "from")
        to_peer = require_str(data, "to")
        payload_b64 = require_str_unbounded(data, "payload_b64")
        if not valid_peer(from_peer):
            raise FieldError("invalid_field", "from")
        if not valid_peer(to_peer):
            raise FieldError("invalid_field", "to")
        try:
            raw = base64.b64decode(payload_b64, validate=True)
        except (ValueError, binascii.Error) as exc:
            raise FieldError("invalid_field", "payload_b64") from exc
        if len(raw) > MAX_PAYLOAD:
            raise ValueError("payload_too_large")
        with self._lock:
            sess = self._get(session_id, now)
            if sess is None:
                raise KeyError("not_found")
            if from_peer == "host":
                if not tokens_equal(token_or_nonce, sess.token):
                    raise PermissionError("forbidden")
            elif not tokens_equal(token_or_nonce, client_nonce(from_peer)):
                raise PermissionError("forbidden")
            if to_peer not in sess.queues and len(sess.queues) >= MAX_DEST_QUEUES:
                raise BufferError("queue_full")
            if sess.undrained_bytes + len(raw) > MAX_SESSION_PAYLOAD:
                raise BufferError("queue_full")
            queue = sess.queues.setdefault(to_peer, [])
            if len(queue) >= MAX_QUEUE:
                raise BufferError("queue_full")
            if to_peer not in sess.queue_drain_at:
                sess.queue_drain_at[to_peer] = now
            seq = sess.next_seq.get(to_peer, 1)
            sess.next_seq[to_peer] = seq + 1
            queue.append(Signal(seq, from_peer, to_peer, payload_b64, len(raw)))
            sess.undrained_bytes += len(raw)
        return {"ok": True, "seq": seq}

    def get_signals(
        self,
        session_id: str,
        peer: str,
        after: int,
        host_token: Optional[str],
        now: float,
    ) -> dict[str, Any]:
        if not valid_peer(peer):
            raise FieldError("invalid_field", "peer")
        with self._lock:
            sess = self._get(session_id, now)
            if sess is None:
                raise KeyError("not_found")
            if peer == "host":
                if host_token is None or not tokens_equal(host_token, sess.token):
                    raise PermissionError("forbidden")
            queue = sess.queues.get(peer)
            if queue is None:
                return {"signals": []}
            kept = [item for item in queue if item.seq > after]
            dropped = [item for item in queue if item.seq <= after]
            sess.undrained_bytes -= sum(item.payload_len for item in dropped)
            sess.queue_drain_at[peer] = now
            if kept:
                sess.queues[peer] = kept
            else:
                sess.queues.pop(peer, None)
                sess.queue_drain_at.pop(peer, None)
        return {"signals": [item.as_json() for item in kept]}


def parse_session_id(value: str) -> str:
    try:
        return str(uuid.UUID(value))
    except ValueError as exc:
        raise KeyError("not_found") from exc


class RunningServer:
    def __init__(
        self,
        httpd: ThreadingHTTPServer,
        store: SessionDirectory,
        thread: threading.Thread,
        use_tls: bool,
    ) -> None:
        self.httpd = httpd
        self.store = store
        self.thread = thread
        self.use_tls = use_tls

    @property
    def port(self) -> int:
        return int(self.httpd.server_address[1])

    def stop(self) -> None:
        self.store.stop()
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=3.0)


def configure_logging(log_file: Optional[Path]) -> None:
    LOGGER.setLevel(logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
    if not LOGGER.handlers:
        stream = logging.StreamHandler()
        stream.setFormatter(fmt)
        LOGGER.addHandler(stream)
    if log_file is not None and not any(
        isinstance(handler, RotatingFileHandler) for handler in LOGGER.handlers
    ):
        path = Path(log_file)
        path.parent.mkdir(parents=True, exist_ok=True)
        handler = RotatingFileHandler(
            path, maxBytes=5 * 1024 * 1024, backupCount=5, encoding="utf-8"
        )
        handler.setFormatter(fmt)
        LOGGER.addHandler(handler)


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Self-hosted session directory")
    parser.add_argument("--bind", default="0.0.0.0")
    # 0 = ephemeral; the bound port is printed at start. Game-port fixture
    # defaults live above 47600, each different:
    #   test_lobby_rejection.py            47611
    #   test_lobby_lifecycle.py            47612
    #   test_lobby_input_delay.py          47613
    #   test_hold_panel_probe.py           47614
    #   test_reconnect_menu_recovery.py    47615
    #   test_reconnect_startup_offer.py    47616
    #   test_substitute_application.py     47617
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--cert", type=Path, default=None)
    parser.add_argument("--key", type=Path, default=None)
    parser.add_argument("--insecure-http", action="store_true")
    parser.add_argument("--expiry-s", type=float, default=15)
    parser.add_argument("--heartbeat-s", type=float, default=5)
    parser.add_argument("--log-file", type=Path, default=None)
    return parser.parse_args(argv)


def check_tls_args(args: argparse.Namespace) -> None:
    has_cert = args.cert is not None
    has_key = args.key is not None
    if has_cert != has_key:
        raise SystemExit("both --cert and --key are required for TLS")
    if has_cert and has_key:
        return
    if not args.insecure_http:
        raise SystemExit("pass --cert and --key, or --insecure-http")


def make_handler(store: SessionDirectory) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "SessionDirectory/1"
        protocol_version = "HTTP/1.1"
        timeout = HANDLER_TIMEOUT_S

        def log_message(self, fmt: str, *args: object) -> None:
            text = fmt % args
            text = re.sub(r"token=[^&\s]+", "token=redacted", text)
            # A client peer is client:<join nonce>, the joiner's bearer credential; only "host" is public.
            text = re.sub(r"peer=(?!host(?:[&\s]|$))[^&\s]+", "peer=redacted", text)
            LOGGER.info("%s %s", self.address_string(), text)

        def _send(self, status: int, body: dict[str, Any]) -> None:
            raw = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(raw)

        def _read_json(self) -> dict[str, Any]:
            raw_len = self.headers.get("Content-Length", "0")
            try:
                length = int(raw_len)
            except ValueError as exc:
                raise ValueError("malformed_json") from exc
            if length < 0:
                raise ValueError("malformed_json")
            if length > MAX_BODY:
                raise OverflowError("payload_too_large")
            blob = self.rfile.read(length) if length else b""
            try:
                parsed = json.loads(blob.decode("utf-8"))
            except (ValueError, UnicodeDecodeError) as exc:
                raise ValueError("malformed_json") from exc
            if not isinstance(parsed, dict):
                raise ValueError("malformed_json")
            return parsed

        def _parts(self) -> tuple[list[str], dict[str, list[str]]]:
            parsed = urlparse(self.path)
            parts = [item for item in parsed.path.split("/") if item]
            return parts, parse_qs(parsed.query)

        def _observed_ip(self) -> str:
            return str(self.client_address[0])

        def _install_gate(
            self, is_register: bool
        ) -> Optional[tuple[int, dict[str, Any]]]:
            key = self.headers.get("X-Install-Key")
            if not key or not valid_install_key(key):
                return 400, {"error": "invalid_install_key"}
            return store.limiter.check(
                key, self._observed_ip(), time.monotonic(), is_register
            )

        def _q1(self, query: dict[str, list[str]], name: str) -> Optional[str]:
            values = query.get(name)
            if not values:
                return None
            return values[0]

        def _signal_peer(self, query: dict[str, list[str]]) -> tuple[str, str]:
            header = self.headers.get("X-Signal-Peer")
            in_query = self._q1(query, "peer")
            if header is None:
                if in_query is None:
                    raise FieldError("missing_field", "peer")
                return in_query, "query"
            if in_query is not None and in_query != header:
                raise FieldError("invalid_field", "peer")
            return header, "header"

        def _handle_error(self, exc: BaseException) -> None:
            if isinstance(exc, FieldError):
                self._send(400, exc.body())
            elif isinstance(exc, PermissionError):
                self._send(403, {"error": "forbidden"})
            elif isinstance(exc, KeyError):
                self._send(404, {"error": "not_found"})
            elif isinstance(exc, OverflowError) and str(exc) == "full":
                self._send(503, {"error": "full"})
            elif isinstance(exc, OverflowError):
                self._send(413, {"error": "payload_too_large"})
            elif isinstance(exc, BufferError):
                self._send(400, {"error": "queue_full"})
            elif isinstance(exc, ValueError) and str(exc) == "payload_too_large":
                self._send(413, {"error": "payload_too_large"})
            elif isinstance(exc, ValueError):
                self._send(400, {"error": "malformed_json"})
            else:
                LOGGER.exception("handler error")
                self._send(500, {"error": "internal"})

        def do_GET(self) -> None:
            try:
                parts, query = self._parts()
                gated = self._install_gate(is_register=False)
                if gated is not None:
                    self._send(gated[0], gated[1])
                    return
                now = time.monotonic()
                if parts == ["v1", "sessions"]:
                    self._send(
                        200,
                        store.list_sessions(
                            now,
                            self._q1(query, "mode"),
                            self._q1(query, "activity"),
                            self._q1(query, "state"),
                        ),
                    )
                    return
                if (
                    len(parts) == 4
                    and parts[0] == "v1"
                    and parts[1] == "sessions"
                    and parts[3] == "signals"
                ):
                    sid = parse_session_id(parts[2])
                    peer, peer_via = self._signal_peer(query)
                    after_raw = self._q1(query, "after")
                    after = 0
                    if after_raw is not None:
                        try:
                            after = int(after_raw)
                        except ValueError:
                            raise FieldError("invalid_field", "after") from None
                    token = self._q1(query, "token") or self.headers.get(
                        "X-Session-Token"
                    )
                    LOGGER.info(
                        "signal session_id=%s client=%s peer_via=%s",
                        sid,
                        self._observed_ip(),
                        peer_via,
                    )
                    self._send(200, store.get_signals(sid, peer, after, token, now))
                    return
                self._send(404, {"error": "not_found"})
            except TimeoutError:
                raise
            except Exception as exc:
                self._handle_error(exc)

        def do_POST(self) -> None:
            try:
                parts, _query = self._parts()
                now = time.monotonic()
                if parts == ["v1", "sessions"]:
                    gated = self._install_gate(is_register=True)
                    if gated is not None:
                        self._send(gated[0], gated[1])
                        return
                    body = self._read_json()
                    self._send(200, store.register(body, self._observed_ip(), now))
                    return
                gated = self._install_gate(is_register=False)
                if gated is not None:
                    self._send(gated[0], gated[1])
                    return
                body = self._read_json()
                if (
                    len(parts) == 4
                    and parts[0] == "v1"
                    and parts[1] == "sessions"
                    and parts[3] == "heartbeat"
                ):
                    sid = parse_session_id(parts[2])
                    LOGGER.info(
                        "heartbeat session_id=%s client=%s", sid, self._observed_ip()
                    )
                    self._send(200, store.heartbeat(sid, body, now))
                    return
                if (
                    len(parts) == 4
                    and parts[0] == "v1"
                    and parts[1] == "sessions"
                    and parts[3] == "signal"
                ):
                    sid = parse_session_id(parts[2])
                    LOGGER.info(
                        "signal session_id=%s client=%s", sid, self._observed_ip()
                    )
                    self._send(200, store.post_signal(sid, body, now))
                    return
                self._send(404, {"error": "not_found"})
            except TimeoutError:
                raise
            except Exception as exc:
                self._handle_error(exc)

        def do_DELETE(self) -> None:
            try:
                parts, _query = self._parts()
                gated = self._install_gate(is_register=False)
                if gated is not None:
                    self._send(gated[0], gated[1])
                    return
                if len(parts) == 3 and parts[0] == "v1" and parts[1] == "sessions":
                    sid = parse_session_id(parts[2])
                    body = self._read_json()
                    self._send(200, store.delete(sid, body, time.monotonic()))
                    return
                self._send(404, {"error": "not_found"})
            except TimeoutError:
                raise
            except Exception as exc:
                self._handle_error(exc)

    return Handler


class SessionHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer that wraps each accepted socket in TLS on its own handler thread."""

    tls_context: Optional[ssl.SSLContext] = None

    def process_request_thread(
        self, request: socket.socket, client_address: Any
    ) -> None:
        ctx = self.tls_context
        if ctx is not None:
            request.settimeout(HANDSHAKE_TIMEOUT_S)
            try:
                request = ctx.wrap_socket(request, server_side=True)
            except (socket.timeout, TimeoutError):
                LOGGER.info("tls handshake timeout from %s", client_address[0])
                self.shutdown_request(request)
                return
            except OSError as exc:
                LOGGER.info(
                    "tls handshake failed from %s: %s", client_address[0], exc
                )
                self.shutdown_request(request)
                return
        super().process_request_thread(request, client_address)


def make_server_ssl_context() -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.options |= ssl.OP_NO_COMPRESSION
    return ctx


def wrap_tls(httpd: SessionHTTPServer, cert: Path, key: Path) -> None:
    ctx = make_server_ssl_context()
    ctx.load_cert_chain(certfile=str(cert), keyfile=str(key))
    httpd.tls_context = ctx


def build_httpd(
    bind: str,
    port: int,
    store: SessionDirectory,
    cert: Optional[Path],
    key: Optional[Path],
) -> SessionHTTPServer:
    handler = make_handler(store)
    httpd = SessionHTTPServer((bind, port), handler)
    httpd.allow_reuse_address = True
    if cert is not None and key is not None:
        wrap_tls(httpd, cert, key)
    return httpd


def spawn_server(
    bind: str = "127.0.0.1",
    port: int = 0,
    expiry_s: float = 15,
    heartbeat_s: float = 5,
    insecure_http: bool = True,
    cert: Optional[Path] = None,
    key: Optional[Path] = None,
    log_file: Optional[Path] = None,
    queue_idle_s: float = QUEUE_IDLE_S,
) -> RunningServer:
    configure_logging(log_file)
    if cert is None or key is None:
        if not insecure_http:
            raise SystemExit("pass --cert and --key, or --insecure-http")
        cert = None
        key = None
    store = SessionDirectory(
        expiry_s=expiry_s, heartbeat_s=heartbeat_s, queue_idle_s=queue_idle_s
    )
    store.start_pruner()
    httpd = build_httpd(bind, port, store, cert, key)
    thread = threading.Thread(target=httpd.serve_forever, name="session-http", daemon=True)
    thread.start()
    LOGGER.info(
        "listening on %s:%s tls=%s",
        bind,
        httpd.server_address[1],
        cert is not None,
    )
    return RunningServer(httpd, store, thread, use_tls=cert is not None)


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    check_tls_args(args)
    configure_logging(args.log_file)
    use_tls = args.cert is not None and args.key is not None
    server = spawn_server(
        bind=args.bind,
        port=args.port,
        expiry_s=args.expiry_s,
        heartbeat_s=args.heartbeat_s,
        insecure_http=args.insecure_http and not use_tls,
        cert=args.cert if use_tls else None,
        key=args.key if use_tls else None,
        log_file=args.log_file,
    )
    print(f"session_directory listening on {args.bind}:{server.port}", flush=True)
    try:
        server.thread.join()
    except KeyboardInterrupt:
        LOGGER.info("stopping")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
