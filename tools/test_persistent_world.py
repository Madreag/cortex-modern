"""Detecting driver for persistent-world slice 1. Written, not run (no-tests rule).

Each named RED has its own engine flag so a startup that dies first cannot stand
in for the others. The completion pass launches these cases and scores the exact
FAIL lines.

Control-tree reversal (disclosed): the pinned control at bb7704b411 has none of
these -net-world-*-selftest flags. A control-tree launch of them never prints a
new PASS token. What that tree still reaches on its existing flags:
- "the match is already in progress" from live NewJoin (ordinary)
- "lockstep peer identity is invalid" from NetLockstep::Start
- H4 HandleLeaveRequest always closes the seat (no persistent-world exemption)
"""

from __future__ import annotations

import sys
from pathlib import Path

# Exact messages the completion pass will print when the wave-tip admission
# still refuses a live world, or a clean leave still ends one.
RED_FRESH_JOIN_LIVE_MATCH = "the match is already in progress"
RED_LOCKSTEP_IDENTITY = "a two-peer empty remote was accepted"
RED_CLEAN_LEAVE_STOPPED_WORLD = "clean-leave-stopped-world"
RED_WORLD_ID_DID_NOT_SURVIVE = "world-id-did-not-survive-restart"
RED_TRANSFER_BYTES = "world-join-transfer-bytes-in-not-equal-bytes-out"
RED_TRANSFER_DIGEST = "world-join-transfer-digest-mismatch"
RED_APPLIED_THROUGH = "appliedThrough-did-not-reach-E-minus-1"
RED_BINDING_MISSING = "activate-binding-missing"
RED_REJOIN = "rejoin after a clean leave did not land in the running world"
RED_H4_LEAVE_CLOSED_WORLD = "H4 clean leave closed a persistent-world seat"
RED_DIRECTORY_BOOT = "world_boot 0 was accepted on the C++ register decoder"
RED_CODEC = "WorldTransition codec did not round-trip"
RED_ORDINARY_IDENTITY = "ordinary identity did not stamp lockstep 22 and match config 4"
RED_ADMIT = "a due activation cancelled instead of admitting"

CASES = (
    {
        "name": "fresh-join-running-world",
        "argv": ["-net-world-live-selftest"],
        "red": RED_FRESH_JOIN_LIVE_MATCH,
        "pass_token": "[net-world-live-selftest] PASS",
    },
    {
        "name": "clean-leave-world-keeps-ticking",
        "argv": ["-net-world-leave-selftest"],
        "red": RED_CLEAN_LEAVE_STOPPED_WORLD,
        "pass_token": "[net-world-leave-selftest] PASS",
    },
    {
        "name": "rejoin-after-clean-leave",
        "argv": ["-net-world-rejoin-selftest"],
        "red": RED_REJOIN,
        "pass_token": "[net-world-rejoin-selftest] PASS",
    },
    {
        "name": "world-identity-survives-restart",
        "argv": ["-net-world-identity-selftest"],
        "red": RED_WORLD_ID_DID_NOT_SURVIVE,
        "pass_token": "[net-world-identity-selftest] PASS",
    },
    {
        "name": "joiner-transfer-round-trip",
        "argv": ["-net-world-transfer-selftest"],
        "red": RED_TRANSFER_BYTES,
        "pass_token": "[net-world-transfer-selftest] PASS",
    },
    {
        "name": "joiner-transfer-digest",
        "argv": ["-net-world-digest-selftest"],
        "red": RED_TRANSFER_DIGEST,
        "pass_token": "[net-world-digest-selftest] PASS",
    },
    {
        "name": "catch-up-applied-through-e-minus-1",
        "argv": ["-net-world-catchup-selftest"],
        "red": RED_APPLIED_THROUGH,
        "pass_token": "[net-world-catchup-selftest] PASS",
    },
    {
        "name": "activate-binding-present",
        "argv": ["-net-world-binding-selftest"],
        "red": RED_BINDING_MISSING,
        "pass_token": "[net-world-binding-selftest] PASS",
    },
    {
        "name": "ordinary-empty-remote-refused",
        "argv": ["-net-world-ordinary-start-selftest"],
        "red": RED_LOCKSTEP_IDENTITY,
        "pass_token": "[net-world-ordinary-start-selftest] PASS",
    },
    {
        "name": "h4-clean-leave-world-seat-open",
        "argv": ["-net-world-h4-leave-selftest"],
        "red": RED_H4_LEAVE_CLOSED_WORLD,
        "pass_token": "[net-world-h4-leave-selftest] PASS",
    },
    {
        "name": "directory-row-resume-and-world-boot",
        "argv": ["-net-world-directory-selftest"],
        "red": RED_DIRECTORY_BOOT,
        "pass_token": "[net-world-directory-selftest] PASS",
    },
    {
        "name": "world-transition-codec",
        "argv": ["-net-world-codec-selftest"],
        "red": RED_CODEC,
        "pass_token": "[net-world-codec-selftest] PASS",
    },
    {
        "name": "ordinary-identity-stamps-22-and-4",
        "argv": ["-net-world-ordinary-identity-selftest"],
        "red": RED_ORDINARY_IDENTITY,
        "pass_token": "[net-world-ordinary-identity-selftest] PASS",
    },
    {
        "name": "due-activation-admits",
        "argv": ["-net-world-admit-selftest"],
        "red": RED_ADMIT,
        "pass_token": "[net-world-admit-selftest] PASS",
    },
)


def score_stdout(stdout: str, case: dict) -> dict:
    red = case["red"]
    if red in (stdout or ""):
        return {"pass": False, "reason": f"FAIL: {red}"}
    token = case["pass_token"]
    if token not in (stdout or ""):
        return {"pass": False, "reason": f"missing {token}"}
    return {"pass": True, "reason": ""}


def directory_resume_same_world_id() -> None:
    """Directory detecting helper: the same world id resumes the existing row."""
    tools = Path(__file__).resolve().parent
    sys.path.insert(0, str(tools / "session_directory"))
    import session_directory

    world_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"
    directory = session_directory.SessionDirectory(expiry_s=60, heartbeat_s=15)
    row = {
        "name": "World",
        "activity": "Persistent World",
        "scene": "Grasslands",
        "mode": "pvp-skirmish",
        "peer_count": 2,
        "seats_free": 1,
        "game_version": "7.0.0",
        "build_id": "stage2-world",
        "network_protocol_version": 1,
        "lockstep_codec_version": 22,
        "controller_frame_version": 7,
        "match_config_hash": "a" * 64,
        "session_identity_hash": "b" * 64,
        "module_manifest_hash": "c" * 64,
        "listen_port": 42124,
        "listen_addrs": ["127.0.0.1"],
        "join_mode": "ip",
        "persistent_world": True,
        "world_id": world_id,
        "world_boot": 1,
        "resume_session_id": world_id,
    }
    first = directory.register(row, "127.0.0.1", 0.0)
    row["world_boot"] = 2
    second = directory.register(row, "127.0.0.1", 1.0)
    if first["session_id"] != world_id or second["session_id"] != world_id:
        raise AssertionError("directory resume did not keep the world id")
    if first["token"] == second["token"]:
        raise AssertionError("directory resume did not issue a new token")
