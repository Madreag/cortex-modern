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
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Any, Optional
from urllib.parse import parse_qs, urlparse
from urllib.request import Request, urlopen

# Cloudflare refuses urllib's default agent (403, error code 1010), so the relay request names the product.
USER_AGENT = "cccp-session-directory/1"

LOGGER = logging.getLogger("session_directory")

MAX_ROWS = 4096
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
QUEUE_IDLE_S = 120.0
HANDLER_TIMEOUT_S = 10
HANDSHAKE_TIMEOUT_S = 5
MAX_SIGNAL_WAIT_S = 25.0
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
    text = re.sub(r"(?i)(token|token_or_join_nonce|join_nonce|nonce)=[^&\s\"#]*", r"\1=redacted", text)
    return re.sub(r"peer=(?!host(?:[&\s\"#]|$))[^&\s\"#]*", "peer=redacted", text)


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
    ) -> None:
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
        self._owner_state = Path(owner_state) if owner_state is not None else None
        self._world_owners: dict[str, dict[str, Any]] = {}
        if self._owner_state is not None and self._owner_state.exists():
            owners = json.loads(self._owner_state.read_text(encoding="utf-8"))
            if not isinstance(owners, dict):
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
                normalized_owners[canonical_id] = owner
            self._world_owners = normalized_owners
        self.limiter = DualRateLimiter()
        self.turn_provider = TurnCredentialProvider(turn_config)
        self.turn_limiter = RateLimiter(TURN_REQUESTS_PER_MIN, TURN_REQUESTS_PER_MIN)
        self._stop = threading.Event()
        self._pruner = threading.Thread(
            target=self._prune_loop, name="session-prune", daemon=True
        )

    def start_pruner(self) -> None:
        if not self._pruner.is_alive():
            self._pruner.start()

    def _remember_world_owner(self, sid: str, token: str, generation: int, presented: str = "", fingerprint: str = "",
                              install_key: str = "", world_boot: int = 0) -> None:
        owner = {"token_sha256": hashlib.sha256(token.encode()).hexdigest(), "migration_gen": generation}
        if install_key:
            owner.update(install_sha256=hashlib.sha256(install_key.encode()).hexdigest(), world_boot=world_boot)
        if fingerprint:
            owner.update(retry_token_sha256=hashlib.sha256(presented.encode()).hexdigest(),
                         retry_fingerprint=fingerprint, retry_until_unix=0)
        self._save_world_owner(sid, owner)

    def _save_world_owner(self, sid: str, owner: dict[str, Any]) -> None:
        if self._world_owners.get(sid) == owner:
            return
        owners = dict(self._world_owners, **{sid: owner})
        if self._owner_state is not None:
            self._owner_state.parent.mkdir(parents=True, exist_ok=True)
            temporary = self._owner_state.with_name(self._owner_state.name + "." + secrets.token_hex(8) + ".tmp")
            try:
                fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as stream:
                    json.dump(owners, stream, sort_keys=True)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(temporary, self._owner_state)
                if os.name != "nt":
                    directory_fd = os.open(self._owner_state.parent, os.O_RDONLY)
                    try:
                        os.fsync(directory_fd)
                    finally:
                        os.close(directory_fd)
            finally:
                temporary.unlink(missing_ok=True)
        self._world_owners = owners

    def _ack_world_register(self, sess: Session, now: float) -> None:
        owner = self._world_owners.get(sess.session_id)
        if owner is None or owner.get("retry_until_unix", -1) != 0:
            return
        self._save_world_owner(sess.session_id, dict(owner, retry_until_unix=time.time() + RESUME_GRACE_S))
        sess.register_retry_until = now + RESUME_GRACE_S

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
            self.turn_limiter.prune_idle(now)
            dead = [
                sid
                for sid, sess in self._sessions.items()
                if now - sess.last_beat >= self.expiry_s
            ]
            for sid in dead:
                sess = self._sessions[sid]
                self._resume_tokens[sid] = (sess.token, sess.last_beat + self.expiry_s + RESUME_GRACE_S, sess.migration_gen)
                del self._sessions[sid]
            for sid, (_, deadline, _) in list(self._resume_tokens.items()):
                if now >= deadline:
                    del self._resume_tokens[sid]
            while len(self._resume_tokens) > MAX_ROWS:
                del self._resume_tokens[next(iter(self._resume_tokens))]
            for sid, replay in list(self._register_replays.items()):
                if replay.register_retry_until != 0 and now >= replay.register_retry_until:
                    del self._register_replays[sid]
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
            if not resuming and len(self._sessions) >= MAX_ROWS:
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
            if resume is not None:
                session_id = require_str(data, "resume_session_id")
                try:
                    uuid.UUID(session_id)
                except ValueError:
                    raise FieldError("invalid_field", "resume_session_id")
                presented = data.get("resume_token")
                previous = self._sessions.get(session_id)
                fingerprint = hashlib.sha256(json.dumps([session_id, fields.get("world_boot", 0), claimed or 0, presented or "", install_key]).encode()).hexdigest()
                replay = self._register_replays.get(session_id)
                owner = self._world_owners.get(session_id)
                if (replay and fields.get("persistent_world") is True and valid_install_key(install_key)
                        and replay.register_fingerprint == fingerprint and (replay.register_retry_until == 0 or now < replay.register_retry_until)
                        and owner and tokens_equal(owner["token_sha256"], hashlib.sha256(replay.token.encode()).hexdigest())):
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
                # A replacement proof is kept until its host acknowledges it, then for one retry window.
                replayed_owner = (world and owner is not None and previous is None and retained is None
                                  and (presented is None or isinstance(presented, str))
                                  and valid_install_key(install_key)
                                  and tokens_equal(hashlib.sha256(install_key.encode()).hexdigest(), owner.get("install_sha256", ""))
                                  and fields.get("world_boot", 0) >= owner.get("world_boot", 0)
                                  and (owner.get("retry_until_unix", -1) == 0 or time.time() < owner.get("retry_until_unix", 0))
                                  and tokens_equal(hashlib.sha256((presented or "").encode()).hexdigest(), owner.get("retry_token_sha256", "")))
                proven = (owner is not None and isinstance(presented, str)
                          and tokens_equal(hashlib.sha256(presented.encode()).hexdigest(), owner["token_sha256"]))
                same_host = (world and proven and valid_install_key(install_key)
                             and (previous is None
                                  or tokens_equal(hashlib.sha256(install_key.encode()).hexdigest(), owner.get("install_sha256", ""))))
                first_world = world and not token and owner is None and presented in (None, "")
                if not first_world and not replayed_owner and not (proven if owner is not None else token and isinstance(presented, str) and tokens_equal(presented, token)):
                    raise PermissionError("forbidden")
                # One host per handover generation: the first successor's claim takes the row, any later claim at that generation
                # or below is told the match already went on; a resume that names no generation is a host reopening its own.
                held = previous.migration_gen if previous else retained[2] if retained else owner["migration_gen"] if owner else 0
                if same_host and fields.get("world_boot", 0) > owner.get("world_boot", 0):
                    held = generation
                if claimed is not None and claimed <= held and not ((replayed_owner or same_host) and claimed == held):
                    raise Superseded("already_migrated", held)
                if claimed is None and held > 0 and not (replayed_owner or same_host):
                    raise Superseded("already_migrated", held)
                if claimed is None:
                    generation = held
                if world:
                    token = secrets.token_urlsafe(24)
                elif proven:
                    token = presented
            else:
                if "resume_token" in data:
                    raise PermissionError("forbidden")
                session_id = str(uuid.uuid4())
                token = secrets.token_urlsafe(24)
            sess = Session(session_id, token, fields, observed_ip, now)
            sess.install_key = install_key
            sess.migration_gen = generation
            if fields.get("persistent_world") is True or session_id in self._world_owners:
                self._remember_world_owner(session_id, token, generation,
                                           presented if resume is not None and isinstance(presented, str) else "",
                                           fingerprint if resume is not None and fields.get("persistent_world") is True else "",
                                           install_key, fields.get("world_boot", 0))
            if fields.get("persistent_world") is True and resume is not None:
                sess.register_fingerprint = fingerprint
                sess.register_retry_until = 0
                self._register_replays[session_id] = sess
            if resume is not None:
                if not first_world:
                    sess.state = "running"
                self._resume_tokens.pop(session_id, None)
            self._sessions[session_id] = sess
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
            self._ack_world_register(sess, now)
            # A host the match left behind no longer keeps the row its successor holds.
            if generation is not None and generation < sess.migration_gen:
                raise Superseded("superseded", sess.migration_gen)
            if valid_install_key(install_key):
                sess.install_key = install_key
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
            listed_now = sess.listed
            held = sess.migration_gen
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
            self._register_replays.pop(session_id, None)
            self._resume_tokens.pop(session_id, None)
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
                self._ack_world_register(sess, now)
            deadline = now + wait_s
            # Long-poll: hold the request until this peer's queue gains a signal past
            # `after`, the session goes away, or the wait elapses.
            while wait_s > 0:
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
            now = time.monotonic()
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
    parser.add_argument("--owner-state", type=Path, default=None,
                        help="durable world owner hashes; defaults to world-owners.json beside the log, or in the working directory")
    parser.add_argument("--turn-config", type=Path, default=None)
    parser.add_argument("--turn-max-ttl", type=int, default=TURN_MAX_TTL,
                        help=f"the longest relay credential minted, {TURN_MIN_TTL}-{TURN_MAX_TTL} s")
    args = parser.parse_args(argv)
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
            return super().parse_request()

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
            # The Cloudflare tunnel reaches the service from loopback and names each client in CF-Connecting-IP; nothing
            # but a loopback peer may name another address, so a remote client cannot pick its own bucket.
            peer = str(self.client_address[0])
            named = (self.headers.get("CF-Connecting-IP") or "").strip()
            if named:
                try:
                    if ipaddress.ip_address(peer).is_loopback:
                        return str(ipaddress.ip_address(named))
                except ValueError:
                    pass
            return peer

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
                    self._send(
                        200,
                        store.get_signals(sid, peer, after, token, now, wait_s),
                    )
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
    turn_config: Optional[dict[str, Any]] = None,
    turn_max_ttl: int = TURN_MAX_TTL,
    owner_state: Optional[Path] = None,
) -> RunningServer:
    configure_logging(log_file)
    if cert is None or key is None:
        if not insecure_http:
            raise SystemExit("pass --cert and --key, or --insecure-http")
        cert = None
        key = None
    store = SessionDirectory(
        expiry_s=expiry_s, heartbeat_s=heartbeat_s, queue_idle_s=queue_idle_s, turn_config=turn_config, turn_max_ttl=turn_max_ttl,
        owner_state=owner_state if owner_state is not None else log_file.parent / "world-owners.json" if log_file else None,
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
