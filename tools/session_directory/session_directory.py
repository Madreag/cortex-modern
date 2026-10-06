#!/usr/bin/python3
"""In-memory JSON session directory (HTTP or HTTPS)."""

from __future__ import annotations

import argparse
import base64
import binascii
import hmac
import hashlib
import ipaddress
import json
import logging
import os
import re
import secrets
import socket
import ssl
import struct
import threading
import time
import uuid
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Any, Optional
from urllib.parse import parse_qs, urlparse, unquote_plus
from urllib.request import Request, urlopen

# Cloudflare refuses urllib's default agent (403, error code 1010), so the relay request names the product.
USER_AGENT = "cccp-session-directory/1"

LOGGER = logging.getLogger("session_directory")

MAX_ROWS = 4096
# Unproved registrations cannot reserve the directory's whole live capacity.
MAX_PENDING_REGISTRATIONS = 256
MAX_PENDING_PER_SOURCE = 8
MAX_PENDING_BYTES = 8 * 1024 * 1024
SESSION_METADATA_BYTES = 4096
# A month covers a returning host without making abandoned ownership permanent.
OWNER_IDLE_S = 30 * 24 * 60 * 60
MAX_WORLD_OWNERS = MAX_ROWS
# One address keeps at most one sixty-fourth of retained ownership.
MAX_WORLD_OWNERS_PER_SOURCE = 64
# Neighbouring addresses share the same creation budget.
MAX_WORLD_OWNERS_PER_NETWORK = 64
# A minute distinguishes established worlds from register-heartbeat-delete churn.
OWNER_MIN_LISTED_S = 60.0
MAX_OWNER_STATE_BYTES = 8 * 1024 * 1024
# One batched write per second stays inside the five-second heartbeat cadence.
OWNER_WRITE_INTERVAL_S = 1.0
OWNER_WRITE_WAIT_S = 5.0
SERVICE_KEY_BYTES = 32
TOKEN_TAG_BYTES = 16
RESUME_GRACE_S = 120.0
MAX_STR = 64
MAX_ARR = 8
MAX_QUEUE = 256
MAX_PAYLOAD = 64 * 1024
MAX_BODY = 128 * 1024
REG_PER_MIN = 10
REQ_PER_MIN = 120
IP_REG_PER_MIN = 30
# Four seated installs keep their individual budgets behind one NAT.
IP_REQ_PER_MIN = 4 * REQ_PER_MIN
RATE_WINDOW_S = 60.0
RATE_IDLE_S = 600.0
PRUNE_EVERY_N = 256
PRUNE_MAP_MAX = 10000
LIST_LIMIT_DEFAULT = 100
LIST_LIMIT_MAX = 200
MAX_DEST_QUEUES = 16
MAX_SESSION_PAYLOAD = 1024 * 1024
# Four installs behind a NAT keep sixteen outstanding frames each.
MAX_SIGNAL_SOURCE_MESSAGES = 64
MAX_SIGNAL_NONCE_MESSAGES = 16
MAX_SIGNAL_SOURCE_BYTES = 256 * 1024
MAX_ANONYMOUS_SIGNAL_BYTES = MAX_SESSION_PAYLOAD // 2
# Encoded payloads and their metadata share a service-wide memory budget.
MAX_STORED_SIGNAL_BYTES = 64 * 1024 * 1024
SIGNAL_METADATA_BYTES = 256
QUEUE_IDLE_S = 120.0
HANDLER_TIMEOUT_S = 10
# Three seconds bounds body readers well inside a fifteen-second lease.
HANDLER_BODY_DEADLINE_S = 3.0
MAX_SHORT_CONNECTIONS_PER_SOURCE = 4
MAX_ACTIVE_HANDLERS = 64
# Waiters have their own capacity, leaving all short handlers available.
MAX_SIGNAL_WAITERS = 64
MAX_SIGNAL_WAITERS_PER_SOURCE = 4
SIGNAL_WAIT_RETRY_S = 1
HANDSHAKE_TIMEOUT_S = 5
MAX_SIGNAL_WAIT_S = 25.0
HANDLER_LIFETIME_S = MAX_SIGNAL_WAIT_S + 2 * HANDLER_TIMEOUT_S
INSTALL_KEY_MIN = 16
INSTALL_KEY_MAX = 32
INSTALL_KEY_CHARS = frozenset(
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_"
)
VALID_JOIN_MODES = frozenset({"ip", "ice", "either"})
VALID_STATES = frozenset({"lobby", "running"})
TURN_MIN_TTL = 300
TURN_MAX_TTL = 86400
TURN_REQUESTS_PER_MIN = 4
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


class Superseded(Exception):
    """A host handover generation the row has already passed: the match went on under a later host."""

    def __init__(self, code: str, generation: int) -> None:
        super().__init__(code)
        self.body = {"error": code, "migration_gen": generation}


def optional_generation(data: dict[str, Any]) -> Optional[int]:
    if "migration_gen" not in data:
        return None
    return require_int(data, "migration_gen", 0, 10**9)


class TurnError(Exception):
    def __init__(self, status: int, code: str, retry_after_s: int = 0) -> None:
        super().__init__(code)
        self.status = status
        self.body = {"error": code}
        if retry_after_s:
            self.body["retry_after_s"] = retry_after_s


def clean_ice_servers(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not 1 <= len(value) <= 8:
        raise TurnError(502, "invalid_relay_response")
    result = []
    has_relay = False
    for entry in value:
        if not isinstance(entry, dict) or set(entry) - {"urls", "username", "credential"}:
            raise TurnError(502, "invalid_relay_response")
        urls = entry.get("urls")
        if isinstance(urls, str):
            urls = [urls]
        if not isinstance(urls, list) or not 1 <= len(urls) <= 8:
            raise TurnError(502, "invalid_relay_response")
        for url in urls:
            if not isinstance(url, str) or len(url) > 256 or not re.fullmatch(
                r"(?:stun|turn|turns):[A-Za-z0-9.\-\[\]:]+(?::[0-9]+)?(?:\?transport=(?:udp|tcp))?", url
            ):
                raise TurnError(502, "invalid_relay_response")
        relay = any(url.startswith(("turn:", "turns:")) for url in urls)
        clean = {"urls": urls}
        if relay:
            has_relay = True
            for key in ("username", "credential"):
                text = entry.get(key)
                if not isinstance(text, str) or not 1 <= len(text) <= 1024 or any(ord(ch) < 32 or ch == "," for ch in text):
                    raise TurnError(502, "invalid_relay_response")
                clean[key] = text
        result.append(clean)
    if not has_relay:
        raise TurnError(502, "invalid_relay_response")
    return result


class TurnCredentialProvider:
    def __init__(self, config: Optional[dict[str, Any]] = None) -> None:
        self._config = dict(config or {})

    def mint(self, match_id: str, ttl: int, now: int) -> dict[str, Any]:
        backend = self._config.get("backend", "cloudflare")
        if backend == "coturn":
            secret = self._config.get("static_auth_secret")
            urls = self._config.get("relay_urls")
            if not isinstance(secret, str) or not secret or not urls:
                raise TurnError(503, "relay_not_configured")
            tag = hashlib.sha256((match_id + secrets.token_hex(16)).encode()).hexdigest()[:24]
            username = f"{now + ttl}:{tag}"
            credential = base64.b64encode(hmac.new(secret.encode(), username.encode(), hashlib.sha1).digest()).decode("ascii")
            servers = clean_ice_servers([{"urls": urls, "username": username, "credential": credential}])
        elif backend == "cloudflare":
            key_id = self._config.get("turn_key_id", "")
            token = self._config.get("api_token", "")
            if not isinstance(key_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", key_id) or not isinstance(token, str) or not token:
                raise TurnError(503, "relay_not_configured")
            request = Request(
                f"https://rtc.live.cloudflare.com/v1/turn/keys/{key_id}/credentials/generate-ice-servers",
                data=json.dumps({"ttl": ttl}).encode(),
                headers={"Authorization": "Bearer " + token, "Content-Type": "application/json", "User-Agent": USER_AGENT},
                method="POST",
            )
            try:
                with urlopen(request, timeout=5) as response:
                    if response.status not in (200, 201):
                        raise ValueError("status")
                    raw = response.read(MAX_BODY + 1)
                if len(raw) > MAX_BODY:
                    raise ValueError("size")
                servers = clean_ice_servers(json.loads(raw)["iceServers"])
            except Exception:
                raise TurnError(502, "relay_provider_unavailable") from None
        else:
            raise TurnError(503, "relay_not_configured")
        return {"match_id": match_id, "expires_at": now + ttl, "iceServers": servers}


def tokens_equal(left: str, right: str) -> bool:
    if len(left) != len(right):
        return False
    return hmac.compare_digest(left, right)


def redact_log_url(text: str) -> str:
    def redact(match):
        key = unquote_plus(match.group(2)).lower()
        if key in {"token", "token_or_join_nonce", "join_nonce", "nonce"} or (key == "peer" and unquote_plus(match.group(3)) != "host"):
            return match.group(1) + match.group(2) + "=redacted"
        return match.group(0)
    return re.sub(r'([?&])([^=&\s"#]+)=([^&\s"#]*)', redact, text)


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


def encode_list_cursor(created_at: float, session_id: str) -> str:
    raw = f"{created_at}:{session_id}"
    return base64.b64encode(raw.encode("utf-8")).decode("ascii")


def decode_list_cursor(cursor: str) -> tuple[float, str]:
    try:
        raw = base64.b64decode(cursor.encode("ascii"), validate=True).decode("utf-8")
    except (ValueError, binascii.Error, UnicodeDecodeError) as exc:
        raise FieldError("invalid_field", "cursor") from exc
    created_s, sep, session_id = raw.partition(":")
    if not sep or not created_s or not session_id:
        raise FieldError("invalid_field", "cursor")
    try:
        created_at = float(created_s)
    except ValueError as exc:
        raise FieldError("invalid_field", "cursor") from exc
    return created_at, session_id


class RateLimiter:
    def __init__(self, req_per_min: int, reg_per_min: int) -> None:
        self.req_per_min = req_per_min
        self.reg_per_min = reg_per_min
        self._requests: dict[str, list[float]] = {}
        self._registers: dict[str, list[float]] = {}
        self._last: dict[str, float] = {}

    def _trim(self, bucket: dict[str, list[float]], key: str, now: float) -> list[float]:
        kept = [ts for ts in bucket.get(key, []) if now - ts < RATE_WINDOW_S]
        if key in bucket:
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
        if key not in self._last and len(self._last) >= PRUNE_MAP_MAX:
            self.prune_idle(now)
            if len(self._last) >= PRUNE_MAP_MAX:
                return 429, {"error": "rate_limited", "retry_after_s": max(1, int(RATE_IDLE_S - (now - min(self._last.values()))) + 1)}
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
        self._last[key] = now
        if is_register:
            regs = self._trim(self._registers, key, now)
            regs.append(now)
            self._registers[key] = regs

    def prune_idle(self, now: float) -> None:
        cutoff = now - RATE_IDLE_S
        dead = [key for key, ts in self._last.items() if ts < cutoff]
        for key in dead:
            self._requests.pop(key, None)
            self._registers.pop(key, None)
            self._last.pop(key, None)


class DualRateLimiter:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._by_key = RateLimiter(REQ_PER_MIN, REG_PER_MIN)
        self._by_ip = RateLimiter(IP_REQ_PER_MIN, IP_REG_PER_MIN)
        self._checks = 0

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

    def _map_size(self) -> int:
        return max(
            len(self._by_key._requests),
            len(self._by_key._registers),
            len(self._by_ip._requests),
            len(self._by_ip._registers),
        )

    def check(
        self, install_key: str, client_ip: str, now: float, is_register: bool
    ) -> Optional[tuple[int, dict[str, Any]]]:
        with self._lock:
            self._checks += 1
            if self._checks % PRUNE_EVERY_N == 0 or self._map_size() > PRUNE_MAP_MAX:
                self._by_key.prune_idle(now)
                self._by_ip.prune_idle(now)
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
        source_ip: str = "",
    ) -> None:
        self.seq = seq
        self.from_peer = from_peer
        self.to_peer = to_peer
        self.payload_b64 = payload_b64
        self.payload_len = payload_len
        self.source_ip = source_ip
        self.stored_bytes = len(payload_b64) + SIGNAL_METADATA_BYTES

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
        self.listed = True
        self.queues: dict[str, list[Signal]] = {}
        self.next_seq: dict[str, int] = {}
        self.queue_drain_at: dict[str, float] = {}
        self.undrained_bytes = 0
        self.install_key = ""
        self.ice_offer: Optional[dict[str, Any]] = None
        self.ice_generation = 0
        self.ice_refused = False  # the host's last mint was refused by its relay backend
        self.migration_gen = 0  # the host handover generation that holds the row
        self.register_fingerprint = ""
        self.register_retry_until = 0.0
        self.register_previous_sha256 = ""
        self.acknowledged = False
        self.owner_listed_at = created_at
        self.stored_bytes = SESSION_METADATA_BYTES + len(json.dumps(fields, ensure_ascii=False).encode("utf-8"))

    def age_s(self, now: float) -> int:
        return max(0, int(now - self.created_at))

    def as_list_row(self, now: float) -> dict[str, Any]:
        row = {key: self.fields[key] for key in LIST_ROW_FIELDS}
        for key in ("persistent_world", "world_id", "world_boot", "spectator_free", "spectator_max", "seats_held"):
            if key in self.fields:
                row[key] = self.fields[key]
        row["session_id"] = self.session_id
        row["age_s"] = self.age_s(now)
        row["observed_ip"] = self.observed_ip
        row["state"] = self.state
        return row


class SessionDirectory:
    def __init__(
        self, expiry_s: float, heartbeat_s: float, queue_idle_s: float = QUEUE_IDLE_S,
        turn_config: Optional[dict[str, Any]] = None, turn_max_ttl: int = TURN_MAX_TTL,
        owner_state: Optional[Path] = None,
        owner_key: Optional[Path] = None, create_owner_key: bool = False,
        caller_mode: Optional[str] = None,
    ) -> None:
        if caller_mode not in (None, "direct", "tunnel"):
            raise ValueError("invalid caller mode")
        self.caller_mode = caller_mode
        self.expiry_s = expiry_s
        # The longest relay credential this directory mints; a client asking for longer gets this much.
        self.turn_max_ttl = max(TURN_MIN_TTL, min(TURN_MAX_TTL, int(turn_max_ttl)))
        self.heartbeat_s = heartbeat_s
        self.queue_idle_s = queue_idle_s
        self._lock = threading.RLock()
        self._signals_changed = threading.Condition(self._lock)
        self._sessions: dict[str, Session] = {}
        self._resume_tokens: dict[str, tuple[str, float, int]] = {}
        self._register_replays: dict[str, Session] = {}
        self._stored_signal_bytes = 0
        self._owner_state = Path(owner_state) if owner_state is not None else None
        self._pending_state = self._owner_state.with_suffix(".pending.json") if self._owner_state else None
        self._pending_worlds: dict[str, dict[str, Any]] = {}
        self._owner_key = Path(owner_key) if owner_key is not None else self._owner_state.with_suffix(".key") if self._owner_state else None
        self._service_era = 1
        self._next_owner = 0
        self._service_key = self._load_service_key(create_owner_key)
        self._world_owners: dict[str, dict[str, Any]] = {}
        self._owner_replacements: dict[str, tuple[str, dict[str, Any], int]] = {}
        self._registering_sessions: dict[str, Session] = {}
        if self._owner_state is not None and self._owner_state.exists():
            if self._owner_state.stat().st_size > MAX_OWNER_STATE_BYTES:
                raise ValueError("world owner state exceeds its byte bound")
            owners = json.loads(self._owner_state.read_text(encoding="utf-8"))
            if not isinstance(owners, dict) or len(owners) > MAX_WORLD_OWNERS:
                raise ValueError("invalid world owner state")
            normalized_owners: dict[str, dict[str, Any]] = {}
            for sid, owner in owners.items():
                canonical_id = str(uuid.UUID(sid))
                if (not isinstance(owner, dict) or not re.fullmatch(r"[0-9a-f]{64}", owner.get("token_sha256", ""))
                        or type(owner.get("migration_gen")) is not int or not 0 <= owner["migration_gen"] <= 10**9):
                    raise ValueError("invalid world owner state")
                if "retry_fingerprint" in owner and (not re.fullmatch(r"[0-9a-f]{64}", owner["retry_fingerprint"])
                        or not re.fullmatch(r"[0-9a-f]{64}", owner.get("retry_token_sha256", ""))
                        or type(owner.get("retry_until_unix")) not in (int, float) or not 0 <= owner["retry_until_unix"] < 10**12):
                    raise ValueError("invalid world owner state")
                if "install_sha256" in owner and (not re.fullmatch(r"[0-9a-f]{64}", owner["install_sha256"])
                        or type(owner.get("world_boot")) is not int or not 0 <= owner["world_boot"] <= 10**9):
                    raise ValueError("invalid world owner state")
                if canonical_id in normalized_owners and normalized_owners[canonical_id] != owner:
                    raise ValueError("conflicting world owner state")
                allowed = {"token_sha256", "migration_gen", "install_sha256", "world_boot", "retry_token_sha256", "retry_fingerprint", "retry_until_unix", "last_heartbeat_unix", "owner_nonce_sha256", "owner_birth", "acked_lease", "acked_era", "source_ip", "listed_s"}
                if set(owner) - allowed:
                    raise ValueError("invalid world owner state")
                if "source_ip" in owner:
                    try:
                        ipaddress.ip_address(owner["source_ip"])
                    except (ValueError, TypeError):
                        raise ValueError("invalid world owner source") from None
                if "listed_s" in owner and (type(owner["listed_s"]) not in (int, float) or not 0 <= owner["listed_s"] < 10**12):
                    raise ValueError("invalid world owner listed age")
                owner.setdefault("last_heartbeat_unix", self._owner_state.stat().st_mtime)
                if type(owner["last_heartbeat_unix"]) not in (int, float) or not 0 <= owner["last_heartbeat_unix"] < 10**12:
                    raise ValueError("invalid world owner heartbeat")
                if "owner_nonce_sha256" in owner and (not re.fullmatch(r"[0-9a-f]{64}", owner["owner_nonce_sha256"])
                        or type(owner.get("owner_birth")) is not int or not 0 <= owner["owner_birth"] < 2**64
                        or type(owner.get("acked_lease")) is not int or not 0 <= owner["acked_lease"] < 2**32
                        or type(owner.get("acked_era")) is not int or not 0 <= owner["acked_era"] < 2**32):
                    raise ValueError("invalid signed world owner state")
                if time.time() - owner["last_heartbeat_unix"] >= OWNER_IDLE_S:
                    continue
                normalized_owners[canonical_id] = owner
            self._world_owners = normalized_owners
        self.limiter = DualRateLimiter()
        self.turn_provider = TurnCredentialProvider(turn_config)
        self.turn_limiter = RateLimiter(TURN_REQUESTS_PER_MIN, TURN_REQUESTS_PER_MIN)
        self._stop = threading.Event()
        self._owner_changed = threading.Condition(self._lock)
        self._owner_revision = 0
        self._owner_written = 0
        self._owner_failure: Optional[BaseException] = None
        self._owner_writer = threading.Thread(target=self._write_owners_loop, name="world-owner-write", daemon=True)
        if self._owner_state is not None:
            self._owner_writer.start()
        if self._pending_state is not None and self._pending_state.exists():
            if self._pending_state.stat().st_size > MAX_PENDING_BYTES:
                raise ValueError("pending world state exceeds its byte bound")
            pending = json.loads(self._pending_state.read_text())
            if not isinstance(pending, dict) or len(pending) > MAX_PENDING_REGISTRATIONS:
                raise ValueError("invalid pending world state")
            now, wall = time.monotonic(), time.time()
            for sid, record in pending.items():
                sid = str(uuid.UUID(sid))
                if not isinstance(record, dict) or type(record.get("expires_unix")) not in (int, float):
                    raise ValueError("invalid pending world state")
                if record["expires_unix"] <= wall:
                    continue
                fields = record.get("fields")
                payload = base64.b64decode(record.get("token_payload", ""), validate=True)
                if (not isinstance(fields, dict) or len(payload) != 28
                        or not re.fullmatch(r"[0-9a-f]{64}", record.get("fingerprint", ""))
                        or not re.fullmatch(r"[0-9a-f]{64}", record.get("install_sha256", ""))
                        or not isinstance(record.get("source"), str) or len(record["source"]) > MAX_STR):
                    raise ValueError("invalid pending world state")
                tag = hmac.new(self._service_key, b"world-proof\0" + uuid.UUID(sid).bytes + payload, hashlib.sha256).digest()[:TOKEN_TAG_BYTES]
                token = "w1_" + base64.urlsafe_b64encode(payload + tag).decode().rstrip("=")
                sess = Session(sid, token, fields, record["source"], now - max(0, self.expiry_s - min(self.expiry_s, record["expires_unix"] - wall)))
                sess.register_fingerprint = record["fingerprint"]
                sess.register_previous_sha256 = record.get("previous_sha256", "")
                sess.install_key_sha256 = record["install_sha256"]
                sess.migration_gen = record["migration_gen"]
                self._sessions[sid] = sess
                self._register_replays[sid] = sess
                self._pending_worlds[sid] = record
        self._pruner = threading.Thread(
            target=self._prune_loop, name="session-prune", daemon=True
        )

    def start_pruner(self) -> None:
        if not self._pruner.is_alive():
            self._pruner.start()

    def _load_service_key(self, create_owner_key: bool) -> bytes:
        if self._owner_key is None:
            return secrets.token_bytes(SERVICE_KEY_BYTES)
        if self._owner_key.exists():
            if self._owner_key.stat().st_size not in (SERVICE_KEY_BYTES, SERVICE_KEY_BYTES + 4):
                raise ValueError("invalid service signing key")
            key = self._owner_key.read_bytes()
            self._service_era = (struct.unpack(">I", key[SERVICE_KEY_BYTES:])[0] if len(key) > SERVICE_KEY_BYTES else 0) + 1
            if self._service_era >= 2**32:
                raise ValueError("service signing era exhausted")
            self._replace_secret(self._owner_key, key[:SERVICE_KEY_BYTES] + struct.pack(">I", self._service_era))
            return key[:SERVICE_KEY_BYTES]
        if not create_owner_key:
            raise ValueError("create-owner-key is required before creating the service key")
        self._owner_key.parent.mkdir(parents=True, exist_ok=True)
        key = secrets.token_bytes(SERVICE_KEY_BYTES)
        self._replace_secret(self._owner_key, key + struct.pack(">I", self._service_era), create=True)
        return key

    @staticmethod
    def _replace_secret(path: Path, raw: bytes, create: bool = False) -> None:
        temporary = path.with_name(path.name + "." + secrets.token_hex(8) + ".tmp")
        try:
            descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(raw)
                stream.flush()
                os.fsync(stream.fileno())
            if create:
                os.link(temporary, path)
            else:
                os.replace(temporary, path)
            if os.name != "nt":
                descriptor = os.open(path.parent, os.O_RDONLY)
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
        finally:
            temporary.unlink(missing_ok=True)

    def _token_proof(self, sid: str, token: Any) -> Optional[tuple[int, bytes, int, int, int, bool]]:
        if not isinstance(token, str) or not token.startswith("w1_") or len(token) != 62:
            return None
        try:
            raw = base64.b64decode(token[3:] + "=" * (-len(token[3:]) % 4), altchars=b"-_", validate=True)
            payload, tag = raw[:-TOKEN_TAG_BYTES], raw[-TOKEN_TAG_BYTES:]
            expected = hmac.new(self._service_key, b"world-proof\0" + uuid.UUID(sid).bytes + payload, hashlib.sha256).digest()[:TOKEN_TAG_BYTES]
            if len(payload) != 28 or not hmac.compare_digest(tag, expected):
                return None
            birth, nonce, era, generation, lease = struct.unpack(">Q8sIII", payload)
            return birth, nonce, generation & 0x7fffffff, lease, era, bool(generation & 0x80000000)
        except (ValueError, binascii.Error, struct.error):
            return None

    def _issue_token(self, sid: str, generation: int, previous: str = "", owner: Optional[dict[str, Any]] = None, world: bool = False) -> str:
        proof = self._token_proof(sid, previous)
        if proof:
            birth, nonce, _generation, lease, _era, _world = proof
            if lease == 2**32 - 1:
                raise PermissionError("forbidden")
            lease += 1
        elif previous:
            birth = 0
            nonce = hmac.new(self._service_key, b"legacy-owner\0" + uuid.UUID(sid).bytes + previous.encode(), hashlib.sha256).digest()[:8]
            lease = 1
        else:
            self._next_owner += 1
            if self._next_owner >= 2**32:
                raise OverflowError("full")
            birth, nonce, lease = (self._service_era << 32) | self._next_owner, secrets.token_bytes(8), 0
        payload = struct.pack(">Q8sIII", birth, nonce, self._service_era, generation | (0x80000000 if world else 0), lease)
        tag = hmac.new(self._service_key, b"world-proof\0" + uuid.UUID(sid).bytes + payload, hashlib.sha256).digest()[:TOKEN_TAG_BYTES]
        return "w1_" + base64.urlsafe_b64encode(payload + tag).decode().rstrip("=")

    def _write_owners_loop(self) -> None:
        next_write = 0.0
        while True:
            with self._owner_changed:
                while self._owner_written >= self._owner_revision and not self._stop.is_set():
                    self._owner_changed.wait()
                if self._stop.is_set() and self._owner_written >= self._owner_revision:
                    return
                delay = next_write - time.monotonic()
                if delay > 0 and not self._stop.is_set():
                    self._owner_changed.wait(delay)
                    continue
                revision = self._owner_revision
                owners = {sid: dict(owner) for sid, owner in self._world_owners.items()}
                replacements = dict(self._owner_replacements)
                for sid, (victim, owner, _) in replacements.items():
                    owners.pop(victim, None)
                    owners[sid] = dict(owner)
                pending = {sid: dict(record) for sid, record in self._pending_worlds.items()}
            try:
                raw_pending = json.dumps(pending, sort_keys=True).encode()
                if len(raw_pending) > MAX_PENDING_BYTES:
                    raise ValueError("pending world state exceeds its byte bound")
                self._replace_secret(self._pending_state, raw_pending)
                self._write_owner_file(owners)
            except BaseException as error:
                with self._owner_changed:
                    self._owner_failure = error
                    self._owner_changed.notify_all()
                return
            with self._owner_changed:
                for sid, replacement in replacements.items():
                    victim, owner, _ = replacement
                    self._world_owners.pop(victim, None)
                    self._world_owners[sid] = owner
                    if self._owner_replacements.get(sid) is replacement:
                        del self._owner_replacements[sid]
                self._owner_written = revision
                self._owner_changed.notify_all()
            next_write = time.monotonic() + OWNER_WRITE_INTERVAL_S

    def _write_owner_file(self, owners: dict[str, dict[str, Any]]) -> None:
        raw = json.dumps(owners, sort_keys=True).encode("utf-8")
        if len(raw) > MAX_OWNER_STATE_BYTES:
            raise ValueError("world owner state exceeds its byte bound")
        self._owner_state.parent.mkdir(parents=True, exist_ok=True)
        temporary = self._owner_state.with_name(self._owner_state.name + "." + secrets.token_hex(8) + ".tmp")
        try:
            descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(raw)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self._owner_state)
            if os.name != "nt":
                descriptor = os.open(self._owner_state.parent, os.O_RDONLY)
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
        finally:
            temporary.unlink(missing_ok=True)

    def _wait_owner_write(self, revision: int) -> None:
        if self._owner_state is None or revision == 0:
            return
        deadline = time.monotonic() + OWNER_WRITE_WAIT_S
        with self._owner_changed:
            while self._owner_written < revision and self._owner_failure is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("world owner acknowledgement is still waiting for storage")
                self._owner_changed.wait(remaining)
            if self._owner_failure is not None:
                raise OSError("world owner acknowledgement could not be stored") from self._owner_failure

    def _remember_world_owner(self, sid: str, token: str, generation: int, presented: str = "", fingerprint: str = "",
                              install_key: str = "", world_boot: int = 0, source_ip: str = "", listed_s: float = 0,
                              previous_sha256: str = "") -> None:
        owner = {"token_sha256": hashlib.sha256(token.encode()).hexdigest(), "migration_gen": generation, "last_heartbeat_unix": time.time()}
        owner.update(source_ip=source_ip, listed_s=listed_s)
        proof = self._token_proof(sid, token)
        if proof:
            owner.update(owner_birth=proof[0], owner_nonce_sha256=hashlib.sha256(struct.pack(">Q", proof[0]) + proof[1]).hexdigest(), acked_lease=proof[3], acked_era=proof[4])
        if install_key:
            owner.update(install_sha256=hashlib.sha256(install_key.encode()).hexdigest(), world_boot=world_boot)
        if fingerprint:
            owner.update(retry_token_sha256=hashlib.sha256(presented.encode()).hexdigest(),
                         retry_fingerprint=fingerprint, retry_until_unix=time.time() + RESUME_GRACE_S)
        if previous_sha256:
            owner["retry_token_sha256"] = previous_sha256
        self._save_world_owner(sid, owner)

    @staticmethod
    def _owner_network(source: str) -> str:
        if not source:
            return ""
        address = ipaddress.ip_address(source)
        return str(ipaddress.ip_network(f"{address}/{24 if address.version == 4 else 48}", strict=False))

    def _projected_owners(self) -> dict[str, dict[str, Any]]:
        owners = dict(self._world_owners)
        for sid, (victim, owner, _) in self._owner_replacements.items():
            owners.pop(victim, None)
            owners[sid] = owner
        return owners

    def _owner_retirement_candidate(self, sid: str, source: str) -> Optional[str]:
        owners = self._projected_owners()
        if sid in owners:
            return None
        network = self._owner_network(source)
        same_source = [key for key, value in owners.items() if value.get("source_ip", "") == source]
        same_network = [key for key, value in owners.items() if self._owner_network(value.get("source_ip", "")) == network]
        candidates = (same_source if len(same_source) >= MAX_WORLD_OWNERS_PER_SOURCE else
                      same_network if len(same_network) >= MAX_WORLD_OWNERS_PER_NETWORK else
                      list(owners) if len(owners) >= MAX_WORLD_OWNERS else [])
        if not candidates:
            return None
        candidates = [key for key in candidates if owners[key].get("listed_s", 0) < OWNER_MIN_LISTED_S]
        if not candidates:
            raise OverflowError("full")
        return min(candidates, key=lambda key: (key in self._sessions, owners[key]["last_heartbeat_unix"]))

    def _save_world_owner(self, sid: str, owner: dict[str, Any]) -> None:
        projected = self._projected_owners()
        previous = projected.get(sid)
        if previous is not None:
            # A resume keeps the creation share even when its host changes address.
            owner["source_ip"] = previous.get("source_ip", owner.get("source_ip", ""))
        if previous == owner:
            return
        victim = self._owner_retirement_candidate(sid, owner.get("source_ip", ""))
        existing = self._owner_replacements.get(sid)
        self._owner_revision += 1
        if self._owner_state is not None and (victim is not None or existing is not None):
            self._owner_replacements[sid] = (victim if victim is not None else existing[0], owner, self._owner_revision)
        else:
            if victim is not None:
                self._world_owners.pop(victim, None)
            self._world_owners[sid] = owner
        self._owner_changed.notify_all()

    def _ack_world_register(self, sess: Session, now: float) -> int:
        if sess.fields.get("persistent_world") is True:
            owner = self._world_owners.get(sess.session_id)
            if not sess.acknowledged or owner is None or time.time() - owner["last_heartbeat_unix"] >= OWNER_WRITE_INTERVAL_S:
                self._remember_world_owner(sess.session_id, sess.token, sess.migration_gen,
                                           fingerprint=sess.register_fingerprint, install_key=sess.install_key,
                                           world_boot=sess.fields.get("world_boot", 0), source_ip=sess.observed_ip,
                                           listed_s=(owner or {}).get("listed_s", 0) + (max(0, now - sess.owner_listed_at) if sess.listed else 0),
                                           previous_sha256=sess.register_previous_sha256)
                sess.owner_listed_at = now
            if self._pending_worlds.pop(sess.session_id, None) is not None:
                self._owner_revision += 1
                self._owner_changed.notify_all()
        sess.acknowledged = True
        if sess.register_retry_until == 0:
            sess.register_retry_until = now + RESUME_GRACE_S
        return self._owner_revision

    def _remember_pending_world(self, sess: Session, now: float) -> int:
        if sess.fields.get("persistent_world") is not True or sess.acknowledged:
            return 0
        raw = base64.b64decode(sess.token[3:] + "=" * (-len(sess.token[3:]) % 4), altchars=b"-_", validate=True)
        self._pending_worlds[sess.session_id] = {
            "fields": dict(sess.fields), "source": sess.observed_ip, "fingerprint": sess.register_fingerprint,
            "install_sha256": sess.install_key_sha256, "migration_gen": sess.migration_gen,
            "previous_sha256": sess.register_previous_sha256, "token_payload": base64.b64encode(raw[:-TOKEN_TAG_BYTES]).decode(),
            "expires_unix": time.time() + max(0, self.expiry_s - (now - sess.created_at)),
        }
        self._owner_revision += 1
        self._owner_changed.notify_all()
        return self._owner_revision

    def stop(self) -> None:
        self._stop.set()
        with self._owner_changed:
            self._owner_changed.notify_all()
            self._signals_changed.notify_all()
        if self._owner_writer.is_alive():
            self._owner_writer.join(OWNER_WRITE_WAIT_S)

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
                self._stored_signal_bytes -= item.stored_bytes
            sess.queues.pop(peer, None)
            sess.queue_drain_at.pop(peer, None)
            sess.next_seq.pop(peer, None)

    def _clear_signals(self, sess: Session) -> None:
        self._stored_signal_bytes -= sum(item.stored_bytes for queue in sess.queues.values() for item in queue)
        sess.queues.clear()
        sess.next_seq.clear()
        sess.queue_drain_at.clear()
        sess.undrained_bytes = 0

    def prune(self, now: float) -> None:
        with self._lock:
            self.turn_limiter.prune_idle(now)
            dead = [
                sid
                for sid, sess in self._sessions.items()
                if now - (sess.last_beat if sess.acknowledged else sess.created_at) >= self.expiry_s
            ]
            for sid in dead:
                sess = self._sessions[sid]
                self._clear_signals(sess)
                if sess.acknowledged:
                    self._resume_tokens[sid] = (sess.token, sess.last_beat + self.expiry_s + RESUME_GRACE_S, sess.migration_gen)
                del self._sessions[sid]
                if self._pending_worlds.pop(sid, None) is not None:
                    self._owner_revision += 1
                    self._owner_changed.notify_all()
            for sid, (_, deadline, _) in list(self._resume_tokens.items()):
                if now >= deadline:
                    del self._resume_tokens[sid]
            while len(self._resume_tokens) > MAX_ROWS:
                del self._resume_tokens[next(iter(self._resume_tokens))]
            for sid, replay in list(self._register_replays.items()):
                if self._sessions.get(sid) is not replay or (replay.register_retry_until != 0 and now >= replay.register_retry_until):
                    del self._register_replays[sid]
            old_owners = [sid for sid, owner in self._world_owners.items() if time.time() - owner["last_heartbeat_unix"] >= OWNER_IDLE_S]
            if old_owners:
                for sid in old_owners:
                    del self._world_owners[sid]
                self._owner_revision += 1
                self._owner_changed.notify_all()
            for sess in self._sessions.values():
                self._prune_idle_queues(sess, now)

    def _get(self, session_id: str, now: float) -> Optional[Session]:
        self.prune(now)
        return self._sessions.get(session_id)

    def register(self, data: dict[str, Any], observed_ip: str, now: float, install_key: str = "") -> dict[str, Any]:
        self.prune(now)
        with self._lock:
            # Capacity is answered before any field work: a full directory must not spend parsing.
            resume_id = data.get("resume_session_id", data.get("world_id") if data.get("persistent_world") is True else None)
            resuming = isinstance(resume_id, str) and resume_id in self._sessions
            reserved_rows = sum(sid not in self._sessions for sid in self._registering_sessions)
            if not resuming and len(self._sessions) + reserved_rows >= MAX_ROWS:
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
            if "persistent_world" in data:
                if not isinstance(data["persistent_world"], bool):
                    raise FieldError("invalid_field", "persistent_world")
                fields["persistent_world"] = data["persistent_world"]
            if "world_id" in data:
                fields["world_id"] = require_str(data, "world_id")
            if "world_boot" in data:
                fields["world_boot"] = require_int(data, "world_boot", 1, 10**9)
            if "spectator_free" in data:
                fields["spectator_free"] = require_int(data, "spectator_free", 0, 10**9)
            if "spectator_max" in data:
                fields["spectator_max"] = require_int(data, "spectator_max", 0, 10**9)
            if "seats_held" in data:
                fields["seats_held"] = require_int(data, "seats_held", 0, 10**9)
            resume = data.get("resume_session_id")
            if fields.get("persistent_world") is True:
                world_id = fields.get("world_id", "")
                try:
                    world_id = str(uuid.UUID(world_id))
                except (ValueError, TypeError, AttributeError):
                    raise FieldError("invalid_field", "world_id") from None
                fields["world_id"] = world_id
                if resume is not None:
                    try:
                        resume = str(uuid.UUID(resume))
                    except (ValueError, TypeError, AttributeError):
                        raise PermissionError("forbidden") from None
                    if resume != world_id:
                        raise PermissionError("forbidden")
                # Equivalent UUID spellings share one owner proof.
                resume = world_id
                data = dict(data, resume_session_id=world_id)
            claimed = optional_generation(data)
            generation = claimed or 0
            world = fields.get("persistent_world") is True
            if resume is not None:
                session_id = require_str(data, "resume_session_id")
                try:
                    session_id = str(uuid.UUID(session_id))
                except ValueError:
                    raise FieldError("invalid_field", "resume_session_id")
                presented = data.get("resume_token")
                previous = self._sessions.get(session_id)
                fingerprint = hashlib.sha256(json.dumps([session_id, None if presented else fields.get("world_boot", 0), claimed or 0, presented or "", install_key]).encode()).hexdigest()
                replay = self._register_replays.get(session_id)
                owner = self._world_owners.get(session_id)
                if (replay and fields.get("persistent_world") is True and valid_install_key(install_key)
                        and replay.register_fingerprint == fingerprint and (replay.register_retry_until == 0 or now < replay.register_retry_until)
                        and fields.get("world_boot", 0) >= replay.fields.get("world_boot", 0)):
                    replay.last_beat = now
                    replay.fields = fields
                    replay.observed_ip = observed_ip
                    self._sessions[session_id] = replay
                    self._resume_tokens.pop(session_id, None)
                    return self._register_reply(replay)
                retained = self._resume_tokens.get(session_id)
                token = previous.token if previous else retained[0] if retained else ""
                world = fields.get("persistent_world") is True and fields.get("world_id") == session_id
                owner = self._world_owners.get(session_id)
                if owner is not None and not world:
                    raise PermissionError("forbidden")
                proof = self._token_proof(session_id, presented)
                if proof is not None and proof[5] != world:
                    raise PermissionError("forbidden")
                same_signed_owner = world and proof is not None and (owner is None or tokens_equal(hashlib.sha256(struct.pack(">Q", proof[0]) + proof[1]).hexdigest(), owner.get("owner_nonce_sha256", "")))
                original_signed_owner = proof is not None and owner is not None and proof[0] < owner.get("owner_birth", 0)
                # A replacement proof is kept until its host acknowledges it, then for one retry window.
                replayed_owner = (world and owner is not None and previous is None and retained is None
                                  and (presented is None or isinstance(presented, str))
                                  and valid_install_key(install_key)
                                  and tokens_equal(hashlib.sha256(install_key.encode()).hexdigest(), owner.get("install_sha256", ""))
                                  and fields.get("world_boot", 0) >= owner.get("world_boot", 0)
                                  and (owner.get("retry_until_unix", -1) == 0 or time.time() < owner.get("retry_until_unix", 0))
                                  and tokens_equal(hashlib.sha256((presented or "").encode()).hexdigest(), owner.get("retry_token_sha256", "")))
                proven = (isinstance(presented, str) and ((owner is not None and tokens_equal(hashlib.sha256(presented.encode()).hexdigest(), owner["token_sha256"]))
                          or (previous is not None and tokens_equal(presented, previous.token))
                          or (same_signed_owner and (owner is None or (proof[4], proof[3]) >= (owner.get("acked_era", 0), owner.get("acked_lease", 0))))
                          or original_signed_owner))
                same_host = (world and proven and valid_install_key(install_key)
                             and (previous is None or tokens_equal(hashlib.sha256(install_key.encode()).hexdigest(), getattr(previous, "install_key_sha256", hashlib.sha256(previous.install_key.encode()).hexdigest())) or original_signed_owner))
                first_world = world and not token and owner is None and presented in (None, "")
                if first_world:
                    self._owner_retirement_candidate(session_id, observed_ip)
                if previous is not None and not previous.acknowledged and same_host and presented != previous.token and (claimed is None or claimed == previous.migration_gen):
                    previous.fields = fields
                    previous.observed_ip = observed_ip
                    return self._register_reply(previous)
                if same_signed_owner and owner is not None and not proven and claimed is not None and proof[2] < owner["migration_gen"] and claimed <= owner["migration_gen"]:
                    raise Superseded("already_migrated", owner["migration_gen"])
                if not first_world and not replayed_owner and not (proven or (token and isinstance(presented, str) and tokens_equal(presented, token))):
                    raise PermissionError("forbidden")
                # One host per handover generation: the first successor's claim takes the row, any later claim at that generation
                # or below is told the match already went on; a resume that names no generation is a host reopening its own.
                held = previous.migration_gen if previous else retained[2] if retained else owner["migration_gen"] if owner else 0
                if original_signed_owner or (same_host and owner is not None and fields.get("world_boot", 0) > owner.get("world_boot", 0)):
                    held = generation
                if world and owner is None and previous is None and proof is not None:
                    held = proof[2]
                if claimed is not None and claimed <= held and not ((replayed_owner or same_host or first_world) and claimed == held):
                    raise Superseded("already_migrated", held)
                if claimed is None and held > 0 and not (replayed_owner or same_host):
                    raise Superseded("already_migrated", held)
                if claimed is None:
                    generation = held
                if world:
                    token = self._issue_token(session_id, generation, presented if isinstance(presented, str) else "", owner, True)
                elif proven:
                    token = presented
            else:
                if "resume_token" in data:
                    raise PermissionError("forbidden")
                session_id = str(uuid.uuid4())
                token = self._issue_token(session_id, generation)
            previous_session = self._sessions.get(session_id)
            if session_id in self._registering_sessions:
                raise OverflowError("full")
            pending = [item for sid, item in self._sessions.items() if sid != session_id and not item.acknowledged]
            pending += [item for sid, item in self._registering_sessions.items() if sid != session_id and sid not in self._sessions]
            if (len(pending) >= MAX_PENDING_REGISTRATIONS or sum(item.observed_ip == observed_ip for item in pending) >= MAX_PENDING_PER_SOURCE
                    or sum(item.stored_bytes for item in pending) + SESSION_METADATA_BYTES + len(json.dumps(fields).encode()) > MAX_PENDING_BYTES):
                raise OverflowError("full")
            sess = Session(session_id, token, fields, observed_ip, now)
            sess.install_key = install_key
            sess.install_key_sha256 = hashlib.sha256(install_key.encode()).hexdigest()
            sess.migration_gen = generation
            if fields.get("persistent_world") is True and resume is not None:
                sess.register_fingerprint = fingerprint
                sess.register_retry_until = 0
                sess.register_previous_sha256 = hashlib.sha256((presented or "").encode()).hexdigest()
            if resume is not None:
                if not first_world:
                    sess.state = "running"
            previous_pending = self._pending_worlds.get(session_id)
            self._registering_sessions[session_id] = sess
            revision = self._remember_pending_world(sess, now)
        try:
            self._wait_owner_write(revision)
        except BaseException:
            with self._lock:
                self._registering_sessions.pop(session_id, None)
                if previous_pending is None:
                    self._pending_worlds.pop(session_id, None)
                else:
                    self._pending_worlds[session_id] = previous_pending
            raise
        with self._lock:
            self._registering_sessions.pop(session_id, None)
            if previous_session is not None:
                self._clear_signals(previous_session)
            self._sessions[session_id] = sess
            if fields.get("persistent_world") is True:
                self._register_replays[session_id] = sess
            if resume is not None:
                self._resume_tokens.pop(session_id, None)
            self._signals_changed.notify_all()
        return self._register_reply(sess)

    def _register_reply(self, sess: Session) -> dict[str, Any]:
        return {
            "session_id": sess.session_id,
            "token": sess.token,
            "expires_in_s": as_json_int(self.expiry_s),
            "heartbeat_s": as_json_int(self.heartbeat_s),
            "observed_ip": sess.observed_ip,
            "supports_unlisted": True,
        }

    def mint_ice_servers(self, session_id: str, data: dict[str, Any], install_key: str, now: float) -> dict[str, Any]:
        if set(data) - {"token", "match_id", "ttl", "iceServers"}:
            raise FieldError("invalid_field", "ice_offer")
        token = require_str(data, "token")
        match_id = require_str_unbounded(data, "match_id")
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", match_id):
            raise FieldError("invalid_field", "match_id")
        ttl = min(require_int(data, "ttl", TURN_MIN_TTL, TURN_MAX_TTL), self.turn_max_ttl)
        with self._lock:
            sess = self._get(session_id, now)
            if not sess:
                raise KeyError(session_id)
            if not valid_install_key(install_key) or not tokens_equal(sess.token, token) or (sess.install_key and not tokens_equal(sess.install_key, install_key)):
                raise PermissionError("forbidden")
            limited = self.turn_limiter.probe(install_key, now, False)
            if limited:
                raise TurnError(429, "relay_rate_limited", limited[1]["retry_after_s"])
            self.turn_limiter.commit(install_key, now, False)
            sess.ice_generation += 1
            generation = sess.ice_generation
        wall = int(time.time())
        if "iceServers" in data:
            try:
                servers = clean_ice_servers(data["iceServers"])
            except TurnError:
                raise FieldError("invalid_field", "iceServers") from None
            offer = {"match_id": match_id, "expires_at": wall + ttl, "iceServers": servers}
        else:
            try:
                offer = self.turn_provider.mint(match_id, ttl, wall)
            except TurnError:
                with self._lock:
                    if self._sessions.get(session_id) is sess and generation == sess.ice_generation:
                        sess.ice_refused = True
                raise
        with self._lock:
            if self._sessions.get(session_id) is not sess or generation != sess.ice_generation:
                raise TurnError(409, "relay_request_superseded")
            if offer["expires_at"] <= int(time.time()):
                raise TurnError(503, "relay_credential_expired")
            sess.ice_offer = offer
            sess.ice_refused = False
            LOGGER.info('relay_offer_issued %s', json.dumps(dict(session_id=session_id, match_id=offer['match_id'],
                provider='fixed' if 'iceServers' in data else self.turn_provider._config.get('backend', 'cloudflare'),
                generation=generation, expires_at=offer['expires_at'], server_count=len(offer['iceServers'])), sort_keys=True))
            return offer

    def get_ice_servers(self, session_id: str, now: float) -> dict[str, Any]:
        with self._lock:
            sess = self._get(session_id, now)
            if not sess:
                raise KeyError(session_id)
            if not sess.ice_offer or sess.ice_offer["expires_at"] <= int(time.time()):
                # A client must tell a relay that refused the host from a match that has none.
                if sess.ice_refused:
                    raise TurnError(502, "relay_provider_refused")
                raise TurnError(404, "relay_offer_unavailable")
            return sess.ice_offer

    def heartbeat(
        self, session_id: str, data: dict[str, Any], now: float, install_key: str = ""
    ) -> dict[str, Any]:
        token = require_str(data, "token")
        peer_count = require_int(data, "peer_count", 0, 10**9)
        seats_free = require_int(data, "seats_free", 0, 10**9)
        # A full world still takes watchers, so the row keeps that count current between registers.
        spectator_free = (
            require_int(data, "spectator_free", 0, 10**9) if "spectator_free" in data else None
        )
        # A running match's seats held for players who are gone: a newcomer may apply to the host for one.
        seats_held = require_int(data, "seats_held", 0, 10**9) if "seats_held" in data else None
        listen_addrs: Optional[list[str]] = None
        if "listen_addrs" in data:
            listen_addrs = require_listen_addrs(data)
        state: Optional[str] = None
        if "state" in data:
            state = require_str(data, "state")
            if state not in VALID_STATES:
                raise FieldError("invalid_field", "state")
        listed: Optional[bool] = None
        if "listed" in data:
            value = data["listed"]
            if not isinstance(value, bool):
                raise FieldError("invalid_field", "listed")
            listed = value
        generation = optional_generation(data)
        with self._lock:
            sess = self._get(session_id, now)
            if sess is None:
                raise KeyError("not_found")
            if not tokens_equal(token, sess.token):
                raise PermissionError("forbidden")
            # A host the match left behind no longer keeps the row its successor holds.
            if generation is not None and generation < sess.migration_gen:
                raise Superseded("superseded", sess.migration_gen)
            if valid_install_key(install_key):
                sess.install_key = install_key
                sess.install_key_sha256 = hashlib.sha256(install_key.encode()).hexdigest()
            sess.fields["peer_count"] = peer_count
            sess.fields["seats_free"] = seats_free
            if spectator_free is not None:
                sess.fields["spectator_free"] = spectator_free
            if seats_held is not None:
                sess.fields["seats_held"] = seats_held
            if listen_addrs is not None:
                sess.fields["listen_addrs"] = listen_addrs
            if state is not None:
                sess.state = state
            if listed is not None:
                sess.listed = listed
            sess.last_beat = now
            revision = self._ack_world_register(sess, now)
            listed_now = sess.listed
            held = sess.migration_gen
        self._wait_owner_write(revision)
        answer: dict[str, Any] = {
            "expires_in_s": as_json_int(self.expiry_s),
            "heartbeat_s": as_json_int(self.heartbeat_s),
            "listed": listed_now,
        }
        # Only a host that names its generation is told the row's: an older build reads the answer it always did.
        if generation is not None:
            answer["migration_gen"] = held
        return answer

    def delete(self, session_id: str, data: dict[str, Any], now: float) -> dict[str, Any]:
        token = require_str(data, "token")
        generation = optional_generation(data)
        with self._lock:
            sess = self._get(session_id, now)
            if sess is None:
                raise KeyError("not_found")
            if not tokens_equal(token, sess.token):
                raise PermissionError("forbidden")
            if generation is not None and generation < sess.migration_gen:
                raise Superseded("superseded", sess.migration_gen)
            del self._sessions[session_id]
            self._clear_signals(sess)
            self._register_replays.pop(session_id, None)
            self._resume_tokens.pop(session_id, None)
            if self._pending_worlds.pop(session_id, None) is not None:
                self._owner_revision += 1
                self._owner_changed.notify_all()
            self._signals_changed.notify_all()
        return {"ok": True}

    def list_sessions(
        self,
        now: float,
        mode: Optional[str],
        activity: Optional[str],
        state: Optional[str],
        limit: int = LIST_LIMIT_DEFAULT,
        cursor: Optional[str] = None,
    ) -> dict[str, Any]:
        start_key: Optional[tuple[float, str]] = None
        if cursor is not None:
            start_key = decode_list_cursor(cursor)
        with self._lock:
            self.prune(now)
            matched: list[Session] = []
            for sess in self._sessions.values():
                if not sess.listed:
                    continue
                if mode is not None and sess.fields["mode"] != mode:
                    continue
                if activity is not None and sess.fields["activity"] != activity:
                    continue
                if state is not None and sess.state != state:
                    continue
                matched.append(sess)
            matched.sort(key=lambda item: (item.created_at, item.session_id))
            total = len(matched)
            if start_key is not None:
                matched = [
                    sess
                    for sess in matched
                    if (sess.created_at, sess.session_id) > start_key
                ]
            page = matched[:limit]
            body: dict[str, Any] = {
                "sessions": [sess.as_list_row(now) for sess in page],
                "total": total,
            }
            if len(matched) > limit:
                last = page[-1]
                body["next_cursor"] = encode_list_cursor(last.created_at, last.session_id)
        return body

    def post_signal(
        self, session_id: str, data: dict[str, Any], now: float, source_ip: str = ""
    ) -> dict[str, Any]:
        token_or_nonce = require_str(data, "token_or_join_nonce")
        from_peer = require_str(data, "from")
        to_peer = require_str(data, "to")
        payload_b64 = require_str_unbounded(data, "payload_b64")
        if not valid_peer(from_peer):
            raise FieldError("invalid_field", "from")
        if not valid_peer(to_peer):
            raise FieldError("invalid_field", "to")
        if len(payload_b64) > 4 * ((MAX_PAYLOAD + 2) // 3):
            raise ValueError("payload_too_large")
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
            elif to_peer != "host":
                raise PermissionError("forbidden")
            if to_peer not in sess.queues and len(sess.queues) >= MAX_DEST_QUEUES:
                raise BufferError("queue_full")
            queue = list(sess.queues.get(to_peer, []))
            victims = []
            anonymous = [item for item in sess.queues.get("host", []) if item.from_peer != "host"]
            projected_payload = sess.undrained_bytes
            projected_stored = self._stored_signal_bytes
            charge = len(payload_b64) + SIGNAL_METADATA_BYTES
            while True:
                source = [item for item in anonymous if item.source_ip == source_ip]
                nonce = [item for item in source if item.from_peer == from_peer]
                source_full = from_peer != "host" and (len(source) >= MAX_SIGNAL_SOURCE_MESSAGES or sum(item.stored_bytes for item in source) + charge > MAX_SIGNAL_SOURCE_BYTES)
                nonce_full = from_peer != "host" and len(nonce) >= MAX_SIGNAL_NONCE_MESSAGES
                anonymous_full = from_peer != "host" and sum(item.payload_len for item in anonymous) + len(raw) > MAX_ANONYMOUS_SIGNAL_BYTES
                if not (source_full or nonce_full or anonymous_full or len(queue) >= MAX_QUEUE or projected_payload + len(raw) > MAX_SESSION_PAYLOAD
                        or projected_stored + charge > MAX_STORED_SIGNAL_BYTES):
                    break
                counts = Counter((item.source_ip, item.from_peer) for item in anonymous)
                candidates = [item for item in anonymous if counts[item.source_ip, item.from_peer] > 1
                              and (not source_full or item.source_ip == source_ip)
                              and (not nonce_full or (item.source_ip == source_ip and item.from_peer == from_peer))]
                if not candidates:
                    raise BufferError("queue_full")
                weights = Counter()
                for item in anonymous:
                    weights[item.source_ip] += item.stored_bytes
                heaviest = min({item.source_ip for item in candidates}, key=lambda address: (-weights[address], next(item.seq for item in candidates if item.source_ip == address)))
                victim = next(item for item in candidates if item.source_ip == heaviest)
                victims.append(victim)
                anonymous.remove(victim)
                if victim in queue:
                    queue.remove(victim)
                projected_payload -= victim.payload_len
                projected_stored -= victim.stored_bytes
            for victim in victims:
                sess.queues["host"].remove(victim)
                sess.undrained_bytes -= victim.payload_len
                self._stored_signal_bytes -= victim.stored_bytes
            queue = sess.queues.setdefault(to_peer, [])
            if to_peer not in sess.queue_drain_at:
                sess.queue_drain_at[to_peer] = now
            seq = sess.next_seq.get(to_peer, 1)
            sess.next_seq[to_peer] = seq + 1
            queue.append(Signal(seq, from_peer, to_peer, payload_b64, len(raw), source_ip))
            sess.undrained_bytes += len(raw)
            self._stored_signal_bytes += charge
            self._signals_changed.notify_all()
        return {"ok": True, "seq": seq}

    def get_signals(
        self,
        session_id: str,
        peer: str,
        after: int,
        host_token: Optional[str],
        now: float,
        wait_s: float = 0.0,
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
            authenticated_lease = sess
            deadline = now + wait_s
            # Long-poll: hold the request until this peer's queue gains a signal past
            # `after`, the session goes away, or the wait elapses.
            while wait_s > 0:
                if self._stop.is_set():
                    raise KeyError("not_found")
                queue = sess.queues.get(peer)
                if queue is not None and any(item.seq > after for item in queue):
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._signals_changed.wait(remaining)
                sess = self._sessions.get(session_id)
                if sess is None:
                    raise KeyError("not_found")
                if peer == "host" and (sess is not authenticated_lease or not tokens_equal(host_token or "", sess.token)):
                    raise PermissionError("forbidden")
            now = time.monotonic()
            queue = sess.queues.get(peer)
            if queue is None:
                return {"signals": []}
            kept = [item for item in queue if item.seq > after]
            dropped = [item for item in queue if item.seq <= after]
            sess.undrained_bytes -= sum(item.payload_len for item in dropped)
            self._stored_signal_bytes -= sum(item.stored_bytes for item in dropped)
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


def check_listener_mode(bind: str, caller_mode: Optional[str]) -> None:
    if caller_mode not in ("direct", "tunnel"):
        raise ValueError("choose --caller-mode direct or tunnel explicitly")
    try:
        loopback = ipaddress.ip_address(bind).is_loopback
    except ValueError:
        loopback = False
    if not loopback:
        raise ValueError("the directory listener must be loopback; put the forwarding tunnel on 127.0.0.1")


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Self-hosted session directory")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--caller-mode", choices=("direct", "tunnel"), required=True,
                        help="direct uses the socket address; tunnel requires the edge-set CF-Connecting-IP")
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
    parser.add_argument("--owner-state", type=Path, default=None,
                        help="durable world owner hashes; defaults to world-owners.json beside the log, or in the working directory")
    parser.add_argument("--owner-key", type=Path, default=None, help="persistent service signing key; defaults beside owner-state with .key suffix")
    parser.add_argument("--create-owner-key", action="store_true", help="create the permanent signing key on the first start only")
    parser.add_argument("--turn-config", type=Path, default=None)
    parser.add_argument("--turn-max-ttl", type=int, default=TURN_MAX_TTL,
                        help=f"the longest relay credential minted, {TURN_MIN_TTL}-{TURN_MAX_TTL} s")
    args = parser.parse_args(argv)
    try:
        check_listener_mode(args.bind, args.caller_mode)
    except ValueError as error:
        parser.error(str(error))
    if not TURN_MIN_TTL <= args.turn_max_ttl <= TURN_MAX_TTL:
        parser.error(f"--turn-max-ttl must be {TURN_MIN_TTL}-{TURN_MAX_TTL}")
    return args


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
            LOGGER.info("%s %s", self.address_string(), redact_log_url(fmt % args))

        def parse_request(self) -> bool:
            self._body_read = False
            if not super().parse_request():
                return False
            try:
                source = self._observed_ip()
            except FieldError as error:
                self._handle_error(error)
                self.close_connection = True
                return False
            if not self.server.begin_short_request(source):
                self.close_connection = True
                return False
            try:
                body_length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                body_length = None
            if body_length is not None and body_length > MAX_BODY:
                self._handle_error(OverflowError("payload_too_large"))
                self.close_connection = True
                return False
            if body_length == 0:
                self.server.body_complete()
            return True

        def _drain_body(self) -> None:
            # A refusal answered before the body is read still takes it off the wire: closing over unread bytes resets
            # the connection, and a client whose body is still arriving loses the answer.
            self._body_read = True
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                return
            if 0 < length <= MAX_BODY:
                try:
                    self.rfile.read(length)
                except OSError:
                    pass
            self.server.body_complete()

        def _send(self, status: int, body: dict[str, Any]) -> None:
            if not getattr(self, "_body_read", True):
                self._drain_body()
            raw = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
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
            self._body_read = True
            blob = self.rfile.read(length) if length else b""
            self.server.body_complete()
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
            if store.caller_mode == "direct":
                return str(ipaddress.ip_address(self.client_address[0]))
            named = (self.headers.get("CF-Connecting-IP") or "").strip()
            try:
                if not named or "%" in named:
                    raise ValueError()
                return str(ipaddress.ip_address(named))
            except ValueError:
                raise FieldError("invalid_caller_address", "CF-Connecting-IP") from None

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
            if isinstance(exc, TurnError):
                self._send(exc.status, exc.body)
            elif isinstance(exc, Superseded):
                self._send(409, exc.body)
            elif isinstance(exc, FieldError):
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
            elif isinstance(exc, ConnectionError):
                # The peer drops a long poll it no longer needs; that is the client's call, not a server error.
                LOGGER.info("client aborted %s", redact_log_url(self.path))
                self.close_connection = True
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
                if len(parts) == 4 and parts[:2] == ["v1", "sessions"] and parts[3] == "ice-servers":
                    self._send(200, store.get_ice_servers(parse_session_id(parts[2]), now))
                    return
                if parts == ["v1", "sessions"]:
                    limit = LIST_LIMIT_DEFAULT
                    limit_raw = self._q1(query, "limit")
                    if limit_raw is not None:
                        try:
                            limit = int(limit_raw)
                        except ValueError:
                            raise FieldError("invalid_field", "limit") from None
                        if limit < 1 or limit > LIST_LIMIT_MAX:
                            raise FieldError("invalid_field", "limit")
                    self._send(
                        200,
                        store.list_sessions(
                            now,
                            self._q1(query, "mode"),
                            self._q1(query, "activity"),
                            self._q1(query, "state"),
                            limit,
                            self._q1(query, "cursor"),
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
                    wait_raw = self._q1(query, "wait")
                    wait_s = 0.0
                    if wait_raw is not None:
                        try:
                            wait_s = float(wait_raw)
                        except ValueError:
                            raise FieldError("invalid_field", "wait") from None
                        if not 0.0 <= wait_s <= MAX_SIGNAL_WAIT_S:
                            raise FieldError("invalid_field", "wait")
                    token = self._q1(query, "token") or self.headers.get(
                        "X-Session-Token"
                    )
                    LOGGER.info(
                        "signal session_id=%s client=%s peer_via=%s",
                        sid,
                        self._observed_ip(),
                        peer_via,
                    )
                    waiting = wait_s > 0 and self.server.begin_signal_wait(self._observed_ip())
                    try:
                        answer = store.get_signals(sid, peer, after, token, now, wait_s if waiting else 0)
                        if wait_s > 0 and not waiting:
                            answer["retry_after_s"] = SIGNAL_WAIT_RETRY_S
                        self._send(200, answer)
                    finally:
                        if waiting:
                            self.server.end_signal_wait(self._observed_ip())
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
                    self._send(200, store.register(body, self._observed_ip(), now, self.headers.get("X-Install-Key", "")))
                    return
                gated = self._install_gate(is_register=False)
                if gated is not None:
                    self._send(gated[0], gated[1])
                    return
                body = self._read_json()
                if len(parts) == 4 and parts[:2] == ["v1", "sessions"] and parts[3] == "ice-servers":
                    self._send(200, store.mint_ice_servers(parse_session_id(parts[2]), body, self.headers.get("X-Install-Key", ""), now))
                    return
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
                    self._send(200, store.heartbeat(sid, body, now, self.headers.get("X-Install-Key", "")))
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
                    self._send(200, store.post_signal(sid, body, now, self._observed_ip()))
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

    def __init__(self, *args, **kwargs) -> None:
        self._handler_slots = threading.BoundedSemaphore(MAX_ACTIVE_HANDLERS)
        self._handler_capacity = threading.local()
        self._connection_lock = threading.Lock()
        self._short_sources = Counter()
        self._request_started: dict[socket.socket, float] = {}
        self._waiter_lock = threading.Lock()
        self._waiter_sources = Counter()
        self._waiters = 0
        super().__init__(*args, **kwargs)

    def process_request(self, request: socket.socket, client_address: Any) -> None:
        if not self._handler_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        with self._connection_lock:
            self._request_started[request] = time.monotonic()
        try:
            super().process_request(request, client_address)
        except BaseException:
            with self._connection_lock:
                self._request_started.pop(request, None)
            self._handler_slots.release()
            raise

    def process_request_thread(
        self, request: socket.socket, client_address: Any
    ) -> None:
        self._handler_capacity.held = True
        self._handler_capacity.source = ""
        with self._connection_lock:
            accepted_at = self._request_started.pop(request)
        self._handler_capacity.body_deadline = accepted_at + HANDLER_BODY_DEADLINE_S
        current = [request]
        def expire():
            try:
                current[0].shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        timer = threading.Timer(HANDLER_LIFETIME_S, expire)
        body_timer = threading.Timer(max(0, self._handler_capacity.body_deadline - time.monotonic()), expire)
        body_timer.daemon = True
        self._handler_capacity.body_timer = body_timer
        body_timer.start()
        timer.daemon = True
        timer.start()
        try:
            self._serve_request(request, client_address, current)
        finally:
            timer.cancel()
            body_timer.cancel()
            self.end_short_request()
            if self._handler_capacity.held:
                self._handler_slots.release()

    def begin_short_request(self, source: str) -> bool:
        with self._connection_lock:
            if self._short_sources[source] >= MAX_SHORT_CONNECTIONS_PER_SOURCE:
                return False
            self._short_sources[source] += 1
            self._handler_capacity.source = source
            return True

    def end_short_request(self) -> None:
        source = self._handler_capacity.source
        if source:
            with self._connection_lock:
                self._short_sources[source] -= 1
                if not self._short_sources[source]:
                    del self._short_sources[source]
            self._handler_capacity.source = ""

    def body_complete(self) -> None:
        self._handler_capacity.body_timer.cancel()

    def begin_signal_wait(self, source: str) -> bool:
        with self._waiter_lock:
            if self._waiters >= MAX_SIGNAL_WAITERS or self._waiter_sources[source] >= MAX_SIGNAL_WAITERS_PER_SOURCE:
                return False
            self._waiters += 1
            self._waiter_sources[source] += 1
            self._handler_capacity.held = False
            self.end_short_request()
            self._handler_slots.release()
            return True

    def end_signal_wait(self, source: str) -> None:
        with self._waiter_lock:
            self._waiters -= 1
            self._waiter_sources[source] -= 1
            if not self._waiter_sources[source]:
                del self._waiter_sources[source]

    def _serve_request(self, request: socket.socket, client_address: Any, current: list[socket.socket]) -> None:
        ctx = self.tls_context
        if ctx is not None:
            remaining = self._handler_capacity.body_deadline - time.monotonic()
            if remaining <= 0:
                self.shutdown_request(request)
                return
            request.settimeout(min(HANDSHAKE_TIMEOUT_S, remaining))
            try:
                request = ctx.wrap_socket(request, server_side=True)
                current[0] = request
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
    check_listener_mode(bind, store.caller_mode)
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
    turn_config: Optional[dict[str, Any]] = None,
    turn_max_ttl: int = TURN_MAX_TTL,
    owner_state: Optional[Path] = None,
    owner_key: Optional[Path] = None,
    create_owner_key: bool = False,
    caller_mode: Optional[str] = None,
) -> RunningServer:
    check_listener_mode(bind, caller_mode)
    configure_logging(log_file)
    if cert is None or key is None:
        if not insecure_http:
            raise SystemExit("pass --cert and --key, or --insecure-http")
        cert = None
        key = None
    store = SessionDirectory(
        expiry_s=expiry_s, heartbeat_s=heartbeat_s, queue_idle_s=queue_idle_s, turn_config=turn_config, turn_max_ttl=turn_max_ttl,
        owner_state=owner_state if owner_state is not None else log_file.parent / "world-owners.json" if log_file else None,
        owner_key=owner_key, create_owner_key=create_owner_key, caller_mode=caller_mode,
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
    try:
        turn_config = json.loads(args.turn_config.read_text(encoding="utf-8")) if args.turn_config else None
        if turn_config is not None and not isinstance(turn_config, dict):
            raise ValueError()
    except Exception:
        raise SystemExit("invalid TURN configuration file") from None
    server = spawn_server(
        bind=args.bind,
        port=args.port,
        expiry_s=args.expiry_s,
        heartbeat_s=args.heartbeat_s,
        insecure_http=args.insecure_http and not use_tls,
        cert=args.cert if use_tls else None,
        key=args.key if use_tls else None,
        log_file=args.log_file,
        turn_config=turn_config,
        turn_max_ttl=args.turn_max_ttl,
        owner_state=args.owner_state or (args.log_file.parent if args.log_file else Path.cwd()) / "world-owners.json",
        owner_key=args.owner_key, create_owner_key=args.create_owner_key, caller_mode=args.caller_mode,
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
