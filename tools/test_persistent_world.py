"""Detecting driver for persistent-world slice 1. Written, not run (no-tests rule).

The future completion pass launches these cases and scores the exact FAIL lines.
A suite that prints one of the RED messages below, or exits 0 without the PASS
token, is the defect the engine still has at the wave tip.
"""

from __future__ import annotations

# Exact messages the completion pass will print when the wave-tip admission
# still refuses a live world, or a clean leave still ends one.
RED_FRESH_JOIN_LIVE_MATCH = "the match is already in progress"
RED_LOCKSTEP_IDENTITY = "lockstep peer identity is invalid"
RED_LOCKSTEP_NO_REMOTES = "lockstep has no remote transport targets"
RED_CLEAN_LEAVE_STOPPED_WORLD = "clean-leave-stopped-world"
RED_WORLD_ID_DID_NOT_SURVIVE = "world-id-did-not-survive-restart"
RED_TRANSFER_BYTES = "world-join-transfer-bytes-in-not-equal-bytes-out"
RED_TRANSFER_DIGEST = "world-join-transfer-digest-mismatch"
RED_APPLIED_THROUGH = "appliedThrough-did-not-reach-E-minus-1"
RED_BINDING_MISSING = "activate-binding-missing"

CASES = (
    {
        "name": "fresh-join-running-world",
        "argv": ["-net-world-join-selftest"],
        "red": RED_FRESH_JOIN_LIVE_MATCH,
        "pass_token": "[net-world-join-selftest] PASS",
    },
    {
        "name": "clean-leave-world-keeps-ticking",
        "argv": ["-net-world-join-selftest"],
        "red": RED_CLEAN_LEAVE_STOPPED_WORLD,
        "pass_token": "[net-world-join-selftest] PASS",
    },
    {
        "name": "rejoin-after-clean-leave",
        "argv": ["-net-world-join-selftest"],
        "red": RED_FRESH_JOIN_LIVE_MATCH,
        "pass_token": "[net-world-join-selftest] PASS",
    },
    {
        "name": "world-identity-survives-restart",
        "argv": ["-net-world-join-selftest"],
        "red": RED_WORLD_ID_DID_NOT_SURVIVE,
        "pass_token": "[net-world-join-selftest] PASS",
    },
    {
        "name": "joiner-transfer-round-trip",
        "argv": ["-net-world-join-selftest"],
        "red": RED_TRANSFER_BYTES,
        "pass_token": "[net-world-join-selftest] PASS",
    },
    {
        "name": "joiner-transfer-digest",
        "argv": ["-net-world-join-selftest"],
        "red": RED_TRANSFER_DIGEST,
        "pass_token": "[net-world-join-selftest] PASS",
    },
    {
        "name": "catch-up-applied-through-e-minus-1",
        "argv": ["-net-world-join-selftest"],
        "red": RED_APPLIED_THROUGH,
        "pass_token": "[net-world-join-selftest] PASS",
    },
    {
        "name": "activate-binding-present",
        "argv": ["-net-world-join-selftest"],
        "red": RED_BINDING_MISSING,
        "pass_token": "[net-world-join-selftest] PASS",
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
