"""Match-scoped connection leases. Gameplay and seat phases stay on the host.

The directory signs the Internet lease, while the engine's host signs the same
format for a LAN/typed-address match. A lease contains no address. Check-ins are
signed by the participant key, so possession of the lease alone grants nothing.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import struct
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Optional

from cryptography.exceptions import InvalidSignature, InvalidTag
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


CONNECTION_PROTOCOL = 1
SEAT_TOKEN_SECONDS = 300
MATCH_RETENTION_SECONDS = 24 * 60 * 60
CHECK_IN_SECONDS = 5
INSTANCE_GRACE_SECONDS = 20
REQUEST_WINDOW_SECONDS = 60
HOST_DISCONNECT_SECONDS = 15  # Keep aligned with the engine host-silence policy.
FRAME_TIE_COLLECT_SECONDS = 1.0
MAX_FRAME_TIES = 64
MAX_SEATS = 7 + 16 + 1  # Match slots, world watchers, and the world's unused seat zero.
MAX_MATCHES = 4096
MAX_RECORD_BYTES = 2 * 1024 * 1024
TOKEN_DOMAIN = b"CortexSeatToken1\0"
REQUEST_DOMAIN = b"CortexCheckIn1\0"
# version, network protocol, directory UUID (zero for host authority), epoch,
# host session, stable seat, holder generation, participant public key,
# issued/expiry wall seconds, and the existing reconnect proof credential.
TOKEN_FIELDS = struct.Struct("<HH16s16sQHI32sQQ32s")


class ConnectionErrorReply(Exception):
    """Only public, actionable words belong in this exception."""

    def __init__(self, status: int, code: str, message: str, **fields: Any) -> None:
        super().__init__(code)
        self.status = status
        self.body = {"error": code, "message": message, "connection_protocol": CONNECTION_PROTOCOL, **fields}


def connection_version(data: dict[str, Any]) -> None:
    version = data.get("connection_protocol", 0)
    if type(version) is not int or version != CONNECTION_PROTOCOL:
        shown = version if type(version) is int and 0 <= version <= 65535 else "unknown"
        raise ConnectionErrorReply(409, "connection_version", f"Your connection protocol is {shown}; the directory uses {CONNECTION_PROTOCOL}. Update the game or directory so the versions match.", client_version=shown, directory_version=CONNECTION_PROTOCOL)


def bounded_hex(data: dict[str, Any], field: str, size: int) -> bytes:
    value = data.get(field)
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{" + str(size * 2) + "}", value):
        raise ConnectionErrorReply(400, "connection_request", "The connection request was incomplete. Cancel and join again.")
    return bytes.fromhex(value)


def bounded_int(data: dict[str, Any], field: str, minimum: int, maximum: int) -> int:
    value = data.get(field)
    if type(value) is not int or not minimum <= value <= maximum:
        raise ConnectionErrorReply(400, "connection_request", "The connection request was incomplete. Cancel and join again.")
    return value


def player_name(value: Any) -> str:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 64 or any(ord(c) < 32 for c in value):
        raise ConnectionErrorReply(400, "connection_request", "Choose a player name and join again.")
    return value


class ConnectionAuthority:
    """Serialized by SessionDirectory's lock; disk records are authenticated.

    Each acknowledged mutation is a FULL SQLite transaction. The service key
    derives separate encryption and signing keys; none is a relay provider key.
    An unreadable record fails startup instead of silently forgetting seats.
    """

    def __init__(self, service_key: bytes, path: Optional[Path] = None) -> None:
        self._lock = threading.RLock()
        self._signer = Ed25519PrivateKey.from_private_bytes(hmac.new(service_key, b"connection-signing-v1", hashlib.sha256).digest())
        self.public_key = self._signer.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
        self._seal = AESGCM(hmac.new(service_key, b"connection-storage-v1", hashlib.sha256).digest())
        self._db = None
        self._closed = False
        self._records: dict[str, dict[str, Any]] = {}
        if path is not None:
            path = Path(path)
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                os.close(fd)
            os.chmod(path, 0o600)
            self._db = sqlite3.connect(path, check_same_thread=False)
            try:
                self._db.execute("PRAGMA journal_mode=DELETE")
                self._db.execute("PRAGMA synchronous=FULL")
                self._db.execute("CREATE TABLE IF NOT EXISTS connections (session TEXT PRIMARY KEY, record BLOB NOT NULL)")
                now = time.time()
                for sid, sealed in self._db.execute("SELECT session, record FROM connections"):
                    try:
                        if str(uuid.UUID(sid)) != sid or not 28 <= len(sealed) <= MAX_RECORD_BYTES + 28:
                            raise ValueError()
                        raw = self._seal.decrypt(sealed[:12], sealed[12:], sid.encode("ascii"))
                        record = json.loads(raw)
                        if not isinstance(record, dict) or record.get("version") != CONNECTION_PROTOCOL:
                            raise ValueError()
                        if record.get("retain_until", 0) > now:
                            self._records[sid] = record
                    except (ValueError, InvalidTag, TypeError):
                        raise ValueError("connection state could not be verified; restore the matching directory state and service key") from None
                self.prune(now)
            except BaseException:
                self.close()
                raise

    def close(self) -> None:
        with self._lock:
            self._closed = True
            if self._db is not None:
                self._db.close()
                self._db = None

    def _save(self, sid: str, record: dict[str, Any]) -> None:
        if self._closed:
            raise ConnectionErrorReply(503, "directory_restarting", "The directory is restarting. Your match can keep playing; reconnecting will retry.")
        if sid not in self._records and len(self._records) >= MAX_MATCHES:
            raise ConnectionErrorReply(503, "connection_capacity", "The directory is full. Wait a moment and host again.")
        raw = json.dumps(record, separators=(",", ":"), ensure_ascii=True).encode()
        if len(raw) > MAX_RECORD_BYTES:
            raise ConnectionErrorReply(503, "connection_capacity", "The directory is busy. Your match can keep playing; retry the connection shortly.")
        if self._db is not None:
            nonce = secrets.token_bytes(12)
            sealed = nonce + self._seal.encrypt(nonce, raw, sid.encode("ascii"))
            with self._db:
                self._db.execute("INSERT INTO connections VALUES (?, ?) ON CONFLICT(session) DO UPDATE SET record=excluded.record", (sid, sealed))
        self._records[sid] = record

    def prune(self, now: float) -> None:
        with self._lock:
            for sid in [sid for sid, record in self._records.items() if record.get("retain_until", 0) <= now]:
                self._records.pop(sid, None)
            if self._db is not None:
                with self._db:
                    for (sid,) in self._db.execute("SELECT session FROM connections").fetchall():
                        if sid not in self._records:
                            self._db.execute("DELETE FROM connections WHERE session=?", (sid,))

    def records(self) -> list[tuple[str, dict[str, Any]]]:
        with self._lock:
            return [(sid, json.loads(json.dumps(row))) for sid, row in self._records.items()]

    def retained_until(self, sid: str) -> float:
        with self._lock:
            record = self._records.get(sid)
            return record["retain_until"] if record and record.get("seats") and not record.get("ended") else 0

    def remember_session(self, sid: str, snapshot: dict[str, Any], now: float, reopen_world: bool = False) -> None:
        with self._lock:
            record = dict(self._records.get(sid, {"version": CONNECTION_PROTOCOL, "seats": {}, "removed": {}, "route_revision": 0}))
            if record.get("ended"):
                # Only an authenticated owner registration can open a later boot
                # of a persistent world. Old match seats are never carried over.
                if not (reopen_world and snapshot["fields"].get("persistent_world") is True
                        and snapshot["fields"].get("world_boot", 0) > record.get("world_boot", 0)):
                    raise ConnectionErrorReply(410, "match_ended", "The host ended this match. Choose another game.")
                record.update(ended=False, seats={})
            record["session"] = snapshot
            record["retain_until"] = now + (MATCH_RETENTION_SECONDS if record.get("seats") else 150)
            self._save(sid, record)

    def end(self, sid: str, now: float) -> None:
        with self._lock:
            if self._closed:
                raise ConnectionErrorReply(503, "directory_restarting", "The directory is restarting. Retry ending this match shortly.")
            record = dict(self._records.get(sid, {}))
            if not record.get("seats") and not record.get("removed"):
                # No participant ever obtained a seat token. Deleting an empty
                # listing must not reserve a day of connection-table capacity.
                if self._db is not None:
                    with self._db:
                        self._db.execute("DELETE FROM connections WHERE session=?", (sid,))
                self._records.pop(sid, None)
                return
            record.update(ended=True, seats={}, session=None, retain_until=now + MATCH_RETENTION_SECONDS,
                          world_boot=(record.get("session") or {}).get("fields", {}).get("world_boot", 0))
            self._save(sid, record)

    def _match(self, sid: str, now: float) -> dict[str, Any]:
        record = self._records.get(sid)
        if not record or record.get("retain_until", 0) <= now:
            raise ConnectionErrorReply(404, "match_unavailable", "The directory is waiting for this match. Reconnecting will retry; Cancel returns to Multiplayer.")
        if record.get("ended"):
            raise ConnectionErrorReply(410, "match_ended", "The host ended this match. Choose another game.")
        return json.loads(json.dumps(record))

    def _token(self, sid: str, seat: dict[str, Any], now: int) -> str:
        fields = TOKEN_FIELDS.pack(CONNECTION_PROTOCOL, seat["network_protocol"], uuid.UUID(sid).bytes, bytes.fromhex(seat["epoch"]), seat["host_session"], seat["seat"], seat["generation"], bytes.fromhex(seat["participant"]), now, now + SEAT_TOKEN_SECONDS, bytes.fromhex(seat["credential"]))
        return base64.b64encode(fields + self._signer.sign(TOKEN_DOMAIN + fields)).decode("ascii")

    def _read_token(self, sid: str, value: Any) -> dict[str, Any]:
        try:
            if not isinstance(value, str) or len(value) > 512:
                raise ValueError()
            raw = base64.b64decode(value, validate=True)
            if len(raw) != TOKEN_FIELDS.size + 64:
                raise ValueError()
            self._signer.public_key().verify(raw[-64:], TOKEN_DOMAIN + raw[:-64])
            version, protocol, session, epoch, host_session, seat, generation, participant, issued, expiry, credential = TOKEN_FIELDS.unpack(raw[:-64])
            if version != CONNECTION_PROTOCOL or session != uuid.UUID(sid).bytes:
                raise ValueError()
            return dict(network_protocol=protocol, epoch=epoch.hex(), host_session=host_session, seat=seat, generation=generation, participant=participant.hex(), issued=issued, expiry=expiry, credential=credential.hex())
        except (ValueError, InvalidSignature, TypeError):
            raise ConnectionErrorReply(403, "seat_unproven", "This seat token could not be verified. Rejoin from the device that owns the seat.") from None

    def issue(self, sid: str, data: dict[str, Any], now: float) -> dict[str, Any]:
        """Called only after SessionDirectory verifies the current host's token."""
        connection_version(data)
        participant = bounded_hex(data, "participant", 32).hex()
        epoch = bounded_hex(data, "epoch", 16).hex()
        credential = bounded_hex(data, "credential", 32).hex()
        seat_id = bounded_int(data, "seat", 0, MAX_SEATS - 1)
        generation = bounded_int(data, "generation", 1, 2**32 - 1)
        host_session = bounded_int(data, "host_session", 1, 2**64 - 1)
        protocol = bounded_int(data, "network_protocol", 1, 65535)
        name = player_name(data.get("name"))
        with self._lock:
            record = self._match(sid, now)
            if participant in record["removed"]:
                removal = record["removed"][participant]
                raise ConnectionErrorReply(403, "player_removed", f"{removal['name']} was {removal['action']} by the host. Choose another match.")
            seat = record["seats"].get(str(seat_id))
            if seat and seat["epoch"] == epoch:
                if seat["generation"] > generation or (seat["generation"] == generation and seat["participant"] != participant):
                    raise ConnectionErrorReply(409, "seat_taken", f"{seat['name']} owns this seat. Join another open seat.")
                if seat["generation"] == generation:
                    if not hmac.compare_digest(seat["credential"], credential):
                        raise ConnectionErrorReply(409, "seat_taken", f"{seat['name']} already owns this seat. Keep the current seat credential when renewing it.")
                    if now - seat["lease_issued"] >= SEAT_TOKEN_SECONDS // 2:
                        seat["lease_issued"] = int(now)
                        self._save(sid, record)
                    return self._reply(sid, record, seat, now)
            seat = dict(seat=seat_id, generation=generation, participant=participant, name=name, epoch=epoch, host_session=host_session, network_protocol=protocol, credential=credential, lease_issued=int(now), instance="", last_check_in=0, route={}, recent_requests={}, relay=None)
            record["seats"][str(seat_id)] = seat
            record["retain_until"] = now + MATCH_RETENTION_SECONDS
            self._save(sid, record)
            return self._reply(sid, record, seat, now)

    def remove(self, sid: str, data: dict[str, Any], now: float) -> dict[str, Any]:
        connection_version(data)
        participant = bounded_hex(data, "participant", 32).hex()
        name = player_name(data.get("name"))
        action = data.get("action")
        if action not in ("removed", "banned"):
            raise ConnectionErrorReply(400, "connection_request", "Choose Remove or Ban for this player.")
        with self._lock:
            record = self._match(sid, now)
            record["removed"][participant] = dict(name=name, action=action)
            record["route_revision"] += 1
            self._save(sid, record)
            return {"ok": True, "connection_protocol": CONNECTION_PROTOCOL}

    def check_in(self, sid: str, envelope: dict[str, Any], now: float) -> tuple[dict[str, Any], bool, Optional[dict[str, Any]]]:
        connection_version(envelope)
        participant = bounded_hex(envelope, "participant", 32)
        signature = bounded_hex(envelope, "signature", 64)
        try:
            encoded = envelope.get("signed_request")
            if not isinstance(encoded, str) or len(encoded) > 16384:
                raise ValueError()
            raw = base64.b64decode(encoded, validate=True)
            Ed25519PublicKey.from_public_bytes(participant).verify(signature, REQUEST_DOMAIN + raw)
            data = json.loads(raw)
            if not isinstance(data, dict) or data.get("session_id") != sid:
                raise ValueError()
        except (ValueError, InvalidSignature, TypeError):
            raise ConnectionErrorReply(403, "player_unproven", "This device could not prove it owns the seat. The original player keeps it; rejoin on that device.") from None
        nonce = bounded_hex(data, "nonce", 16).hex()
        instance = bounded_hex(data, "instance", 16).hex()
        sent = bounded_int(data, "sent_at", 1, 2**53 - 1)
        if abs(now - sent) > REQUEST_WINDOW_SECONDS:
            raise ConnectionErrorReply(409, "connection_clock", "This device's clock differs from the directory. Correct its clock and rejoin.")
        token = self._read_token(sid, data.get("seat_token"))
        route = data.get("route", {})
        if (not isinstance(route, dict) or set(route) - {"ice_identity", "ice_virtual_port", "listen_port", "listen_addrs", "generation", "state"}
                or len(json.dumps(route).encode()) > 4096):
            raise ConnectionErrorReply(400, "connection_request", "The new route was incomplete. Reconnecting will retry; Cancel returns to Multiplayer.")
        if route:
            identity = route.get("ice_identity", "")
            addresses = route.get("listen_addrs", [])
            if (not isinstance(identity, str) or len(identity) > 128 or not isinstance(addresses, list) or len(addresses) > 8
                    or any(not isinstance(address, str) or len(address) > 256 for address in addresses)
                    or route.get("state") not in ("connecting", "connected", "reconnecting", "held", "left")):
                raise ConnectionErrorReply(400, "connection_request", "The new route was incomplete. Reconnecting will retry; Cancel returns to Multiplayer.")
            bounded_int(route, "generation", 0, 2**32 - 1)
            bounded_int(route, "ice_virtual_port", 0, 65535)
            bounded_int(route, "listen_port", 0, 65535)
        host_claim = data.get("host")
        if host_claim is not None:
            if not isinstance(host_claim, dict) or not isinstance(host_claim.get("token"), str) or len(host_claim["token"]) > 256:
                raise ConnectionErrorReply(400, "connection_request", "The host check-in was incomplete. Reconnecting will retry; Cancel returns to Multiplayer.")
            bounded_int(host_claim, "generation", 0, 2**32 - 1)
        with self._lock:
            record = self._match(sid, now)
            seat = record["seats"].get(str(token["seat"]))
            if not seat or any(token[key] != seat[key] for key in ("epoch", "generation", "participant", "credential", "host_session", "network_protocol")):
                raise ConnectionErrorReply(403, "seat_reassigned", "The host gave this seat to another player. Join another open seat.")
            if not hmac.compare_digest(participant.hex(), seat["participant"]):
                raise ConnectionErrorReply(403, "seat_taken", f"{seat['name']} owns this seat. The original player keeps it; join another open seat.")
            if seat["participant"] in record["removed"]:
                removal = record["removed"][seat["participant"]]
                raise ConnectionErrorReply(403, "player_removed", f"{removal['name']} was {removal['action']} by the host. Choose another match.")
            # A stale signed lease can be renewed only by its own key while the
            # retained seat remains unchanged. It cannot admit a connection as-is.
            if token["issued"] > now + REQUEST_WINDOW_SECONDS:
                raise ConnectionErrorReply(403, "seat_expired", "This saved match has expired. Choose another game.")
            if seat["instance"] and seat["instance"] != instance and now - seat["last_check_in"] < INSTANCE_GRACE_SECONDS:
                raise ConnectionErrorReply(409, "seat_in_use", f"{seat['name']} is still connected. Waiting for that connection to close; Cancel stops rejoining.")
            recent = {key: expiry for key, expiry in seat["recent_requests"].items() if expiry > now}
            if nonce in recent:
                raise ConnectionErrorReply(409, "connection_replay", "This check-in was already used. Reconnecting will send a fresh request.")
            if len(recent) >= 64:
                raise ConnectionErrorReply(429, "connection_busy", "Too many connection changes. Wait a moment while reconnecting retries.", retry_after_s=5)
            recent[nonce] = now + REQUEST_WINDOW_SECONDS * 2
            changed = seat["route"] != route or seat["instance"] != instance
            path_changed = seat["instance"] != instance or any(seat["route"].get(key) != route.get(key) for key in ("ice_identity", "ice_virtual_port", "listen_addrs", "generation"))
            seat.update(instance=instance, last_check_in=now, recent_requests=recent, route=route)
            if now - seat["lease_issued"] >= SEAT_TOKEN_SECONDS // 2:
                seat["lease_issued"] = int(now)
            if changed:
                record["route_revision"] += 1
            record["retain_until"] = now + MATCH_RETENTION_SECONDS
            self._save(sid, record)
            needs_relay = path_changed or not seat["relay"] or seat["relay"].get("expires_at", 0) - now <= SEAT_TOKEN_SECONDS
            return self._reply(sid, record, seat, now), needs_relay, dict(host_claim, route=route) if host_claim else None

    def frame_tie(self, sid: str, envelope: dict[str, Any], now: float) -> dict[str, Any]:
        answer, _renew, _host_claim = self.check_in(sid, envelope, now)
        data = json.loads(base64.b64decode(envelope["signed_request"], validate=True))
        request = data.get("frame_tie")
        if not isinstance(request, dict):
            raise ConnectionErrorReply(400, "frame_tie_request", "The frame check-in was incomplete. Reconnecting will retry.")
        generation = bounded_int(request, "generation", 0, 2**32 - 2)
        round_id = bounded_int(request, "round_id", 1, 2**64 - 1)
        frame = bounded_int(request, "frame", 0, 2**64 - 1)
        config_hash = bounded_hex(request, "config_hash", 32).hex()
        host = bounded_int(request, "host_seat", 0, MAX_SEATS - 1)
        owners, members = request.get("owners"), request.get("members")
        query_only = request.get("query_only", False)
        if (not isinstance(owners, list) or not isinstance(members, list) or len(owners) not in (2, 4)
                or any(type(seat) is not int or seat < 0 or seat >= MAX_SEATS for seat in owners + members)
                or owners != sorted(set(owners)) or members != sorted(set(members))
                or len(members) * 2 != len(owners) or not set(members) <= set(owners)
                or type(query_only) is not bool or host not in owners or answer["seat"] not in owners
                or (not query_only and answer["seat"] not in members)):
            raise ConnectionErrorReply(400, "frame_tie_request", "The tied groups did not name retained seats. Reconnecting will retry.")
        key = f"{generation}:{round_id}:{config_hash}:{frame}"
        with self._lock:
            record = self._match(sid, now)
            ties = record.setdefault("frame_ties", {})
            prior = ties.get(key)
            if prior is not None and "winner" in prior:
                if prior["owners"] != owners or prior["host"] != host:
                    raise ConnectionErrorReply(409, "frame_tie_mismatch", "The groups disagree about the retained seats. Reconnecting will retry.")
                # A newer admin generation or roster cannot rewrite an already
                # displayed decision. The caller's current seat authentication
                # above still has to succeed before this history is disclosed.
                return dict(connection_protocol=CONNECTION_PROTOCOL, generation=generation, round_id=round_id,
                            frame=frame, config_hash=config_hash, status="decided", members=prior["winner"])
            session = record.get("session") or {}
            if (generation != session.get("migration_gen", 0) or host != record.get("host_seat")
                    or config_hash != session.get("fields", {}).get("match_config_hash")):
                raise ConnectionErrorReply(409, "frame_tie_round", "The check-in names another host generation or match. Reconnecting will retry.")
            if any(str(seat) not in record["seats"] or record["seats"][str(seat)]["participant"] in record["removed"] for seat in owners):
                raise ConnectionErrorReply(403, "frame_tie_seats", "A seat in this check-in was removed by the host.")
            retained = sorted(int(seat) for seat, lease in record["seats"].items() if int(seat) < 4 and lease["participant"] not in record["removed"])
            if owners != retained:
                raise ConnectionErrorReply(409, "frame_tie_owners", "The check-in did not name every retained seat. Reconnecting will retry.")
            if key not in ties and query_only:
                return dict(connection_protocol=CONNECTION_PROTOCOL, generation=generation, round_id=round_id,
                            frame=frame, config_hash=config_hash, status="waiting")
            if key not in ties:
                ties[key] = {"owners": owners, "host": host, "until": now + FRAME_TIE_COLLECT_SECONDS, "groups": []}
            tie = ties[key]
            if tie["owners"] != owners or tie["host"] != host:
                raise ConnectionErrorReply(409, "frame_tie_mismatch", "The groups disagree about the retained seats. Reconnecting will retry.")
            # Finalize an expired window before accepting another check-in: a
            # host arriving after the full second cannot replace its winner.
            if "winner" not in tie and now >= tie["until"] and tie["groups"]:
                tie["winner"] = tie["groups"][0]
            if "winner" not in tie and not query_only:
                groups = tie["groups"]
                if members not in groups and (not groups or set(members).isdisjoint(groups[0])):
                    groups.append(members)
                if members in groups and host in members:
                    tie["winner"] = members
            # Return waiting immediately; no HTTP worker sleeps on a tie.
            # Keep every decision for the match's lifetime, including after
            # later ties, so an old observer can never reopen a decided tie.
            result = {"connection_protocol": CONNECTION_PROTOCOL, "generation": generation, "round_id": round_id,
                      "frame": frame, "config_hash": config_hash, "status": "decided" if "winner" in tie else "waiting"}
            if "winner" in tie:
                result["members"] = tie["winner"]
            self._save(sid, record)
            return result

    def note_host(self, sid: str, seat_id: int, generation: int, now: float) -> None:
        """Only SessionDirectory calls this after verifying the current host token."""
        with self._lock:
            record = self._match(sid, now)
            if record.get("session", {}).get("migration_gen", 0) != generation:
                return
            record.update(host_seat=seat_id, host_last_heard=now)
            self._save(sid, record)

    def host_change(self, sid: str, envelope: dict[str, Any], now: float,
                    host_heard_at: float, issue_token) -> dict[str, Any]:
        # check_in verifies the participant signature, seat binding, instance,
        # request age and nonce before any authority report is consumed.
        answer, _renew, _host_claim = self.check_in(sid, envelope, now)
        request = json.loads(base64.b64decode(envelope["signed_request"], validate=True))
        change = request.get("host_change")
        if not isinstance(change, dict):
            raise ConnectionErrorReply(400, "host_change_request", "The host-change report was incomplete. Reconnecting will retry.")
        generation = bounded_int(change, "generation", 0, 2**32 - 2)
        round_id = bounded_int(change, "round_id", 1, 2**64 - 1)
        applied = bounded_int(change, "applied_frame", 0, 2**64 - 2)
        prepared = bounded_int(change, "prepared_frame", applied, 2**64 - 2)
        config_hash = bounded_hex(change, "config_hash", 32).hex()
        with self._lock:
            record = self._match(sid, now)
            snapshot = record.get("session") or {}
            decision = next((item for item in reversed(record.get("host_changes", [])) if item["previous_generation"] == generation), record.get("host_change"))
            if snapshot.get("fields", {}).get("persistent_world") is True:
                raise ConnectionErrorReply(409, "world_host_fixed", "This world keeps its configured host. Reconnecting will retry.")
            agreement = change.get("agreement")
            if agreement is not None:
                return self._host_agreement(sid, record, answer["seat"], agreement, generation, round_id, config_hash, now, issue_token)
            if decision and decision["previous_generation"] == generation:
                if decision["round_id"] != round_id or decision["config_hash"] != config_hash:
                    raise ConnectionErrorReply(409, "host_change_round", "The host changed in another round. Refreshing the match will retry.")
                return self._host_change_reply(record, answer["seat"], decision)
            current = snapshot.get("migration_gen", 0)
            if generation != current:
                raise ConnectionErrorReply(409, "host_generation", "This host generation has been superseded. Reconnecting will follow the current host.", host_generation=current)
            old_host = record.get("host_seat")
            if old_host is None:
                return dict(connection_protocol=CONNECTION_PROTOCOL, status="waiting", reason="host_not_bound")
            if config_hash != snapshot.get("fields", {}).get("match_config_hash"):
                raise ConnectionErrorReply(409, "host_change_config", "The host-change report names another match configuration. Reconnecting will retry.")
            heard = max(host_heard_at, record.get("host_last_heard", 0), record["seats"].get(str(old_host), {}).get("last_check_in", 0))
            if now < heard or now - heard < HOST_DISCONNECT_SECONDS:
                return dict(connection_protocol=CONNECTION_PROTOCOL, status="waiting", reason="host_alive")
            # The caller only reports its own retained prefix. It cannot name a
            # host or remove another participant from the directory's live set.
            seat_id = answer["seat"]
            if seat_id == old_host:
                return dict(connection_protocol=CONNECTION_PROTOCOL, status="waiting", reason="host_alive")
            reports = record.setdefault("host_change_reports", {})
            reports[str(seat_id)] = dict(generation=generation, round_id=round_id, config_hash=config_hash,
                                        applied_frame=applied, prepared_frame=prepared, received_at=now)
            live = sorted(int(key) for key, seat in record["seats"].items()
                          if int(key) != old_host and seat["participant"] not in record["removed"]
                          and seat.get("route", {}).get("state") != "left"
                          and 0 <= now - seat.get("last_check_in", 0) < HOST_DISCONNECT_SECONDS)
            complete = live and all(str(peer) in reports and all(reports[str(peer)][field] == value
                                    for field, value in (("generation", generation), ("round_id", round_id), ("config_hash", config_hash)))
                                    and now - reports[str(peer)]["received_at"] < HOST_DISCONNECT_SECONDS for peer in live)
            if not complete:
                self._save(sid, record)
                return dict(connection_protocol=CONNECTION_PROTOCOL, status="waiting", reason="collecting_prefixes")
            successor = live[0]
            donor = min(live, key=lambda peer: (-reports[str(peer)]["prepared_frame"], peer))
            boundary = reports[str(donor)]["prepared_frame"]
            decision = dict(session_id=sid, previous_generation=generation, generation=generation + 1,
                            old_host_seat=old_host, host_seat=successor, members=live, donor_seat=donor,
                            boundary=boundary, round_id=round_id, config_hash=config_hash)
            self._commit_host_change(sid, record, decision, now, issue_token)
            return self._host_change_reply(record, seat_id)

    def _host_agreement(self, sid, record, seat_id, agreement, generation, round_id, config_hash, now, issue_token):
        if not isinstance(agreement, dict):
            raise ConnectionErrorReply(400, "host_agreement", "The host agreement was incomplete. Reconnecting will retry.")
        host = bounded_int(agreement, "host_seat", 0, 3)
        boundary = bounded_int(agreement, "boundary", 0, 2**64 - 2)
        members = agreement.get("members")
        previous = record.get("host_change")
        current = record.get("session", {}).get("migration_gen", 0)
        old_host = previous["old_host_seat"] if previous and previous["previous_generation"] == generation else record.get("host_seat")
        expected = sorted(int(key) for key, seat in record["seats"].items()
                          if int(key) < 4 and int(key) != old_host and seat["participant"] not in record["removed"]
                          and seat.get("route", {}).get("state") != "left"
                          and (int(key) == seat_id or 0 <= now - seat.get("last_check_in", 0) < HOST_DISCONNECT_SECONDS))
        heard = max(record.get("host_last_heard", 0), record["seats"].get(str(old_host), {}).get("last_check_in", 0))
        if (not isinstance(members, list) or any(type(peer) is not int for peer in members)
                or members != expected or not members or host not in members or seat_id not in members
                or (current == generation and (now < heard or now - heard < HOST_DISCONNECT_SECONDS))
                or current not in (generation, generation + 1)
                or config_hash != record.get("session", {}).get("fields", {}).get("match_config_hash")):
            raise ConnectionErrorReply(409, "host_agreement", "The host agreement does not name every remaining owner. Reconnecting will retry.")
        value = dict(previous_generation=generation, generation=generation + 1, round_id=round_id,
                     config_hash=config_hash, host_seat=host, boundary=boundary, members=members)
        reports = record.setdefault("host_agreement_reports", {})
        prior = reports.get(str(seat_id))
        if prior and prior["previous_generation"] == generation and prior != value:
            raise ConnectionErrorReply(409, "host_agreement_conflict", "This player already agreed another host. Reconnecting will retry.")
        reports[str(seat_id)] = value
        if not all(reports.get(str(peer)) == value for peer in members):
            self._save(sid, record)
            return dict(connection_protocol=CONNECTION_PROTOCOL, status="waiting", reason="collecting_agreement")
        if previous and all(previous.get(key) == item for key, item in value.items()):
            return self._host_change_reply(record, seat_id)
        # No single peer can overwrite a reservation. Every remaining owner must
        # independently attest the exact same completed fallback, including its tick.
        decision = dict(value, session_id=sid, old_host_seat=old_host, donor_seat=host)
        self._commit_host_change(sid, record, decision, now, issue_token)
        return self._host_change_reply(record, seat_id)

    def _commit_host_change(self, sid, record, decision, now, issue_token):
        snapshot = record["session"]
        successor = decision["host_seat"]
        snapshot["migration_gen"] = decision["generation"]
        snapshot["token"] = issue_token(decision["generation"])
        snapshot["last_beat"] = now
        route = record["seats"][str(successor)].get("route", {})
        fields = snapshot["fields"]
        fields.update(listen_addrs=route.get("listen_addrs", []), listen_port=route.get("listen_port", 0))
        identity, virtual_port = route.get("ice_identity", ""), route.get("ice_virtual_port", 0)
        if identity and virtual_port:
            fields.update(ice_identity=identity, ice_virtual_port=virtual_port)
        else:
            fields.pop("ice_identity", None); fields.pop("ice_virtual_port", None)
        fields["join_mode"] = "either" if identity and fields["listen_addrs"] else "ice" if identity else "ip"
        history = record.setdefault("host_changes", [])
        history.append(dict(decision))
        del history[:-MAX_FRAME_TIES]
        record.update(session=snapshot, host_change=decision, host_seat=successor, host_last_heard=now,
                      host_change_reports={}, retain_until=now + MATCH_RETENTION_SECONDS)
        self._save(sid, record)

    @staticmethod
    def _host_change_reply(record: dict[str, Any], seat_id: int, decision=None) -> dict[str, Any]:
        decision = decision or record["host_change"]
        reply = dict(connection_protocol=CONNECTION_PROTOCOL, status="decided", decision=dict(decision))
        if seat_id == decision["host_seat"] and decision["generation"] == record["session"]["migration_gen"]:
            reply["host_token"] = record["session"]["token"]
        return reply

    def reserved_host(self, sid: str, generation: int, token: str) -> bool:
        with self._lock:
            record = self._records.get(sid, {})
            decision = record.get("host_change", {})
            return (decision.get("generation") == generation and isinstance(token, str)
                    and hmac.compare_digest(token, record.get("session", {}).get("token", "")))

    def set_relay(self, sid: str, seat_id: int, generation: int, offer: dict[str, Any], now: float) -> bool:
        with self._lock:
            record = self._match(sid, now)
            seat = record["seats"].get(str(seat_id))
            if not seat or seat["generation"] != generation:
                return False
            seat["relay"] = offer
            self._save(sid, record)
            return True

    def _reply(self, sid: str, record: dict[str, Any], seat: dict[str, Any], now: float) -> dict[str, Any]:
        peers = [dict(seat=item["seat"], name=item["name"], route=item["route"]) for item in record["seats"].values() if item["participant"] not in record["removed"]]
        result = dict(connection_protocol=CONNECTION_PROTOCOL, authority_key=self.public_key, seat_token=self._token(sid, seat, seat["lease_issued"]), seat=seat["seat"], generation=seat["generation"], expires_at=seat["lease_issued"] + SEAT_TOKEN_SECONDS, check_in_s=CHECK_IN_SECONDS, route_revision=record["route_revision"], peers=peers)
        if seat["relay"] and seat["relay"].get("expires_at", 0) > now:
            result["relay"] = seat["relay"]
        return result
