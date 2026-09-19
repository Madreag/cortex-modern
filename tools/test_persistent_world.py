"""Detecting cases for the persistent world: one engine flag or helper per RED.

Each case names the exact FAIL line that scores it, so a startup that dies first
cannot stand in for another case.
"""

from __future__ import annotations

import os
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
RED_DIRECTORY_RESUME = "directory resume did not keep the world id"
RED_DIRECTORY_TOKEN = "directory resume did not issue a new token"
RED_DIRECTORY_SEIZED = "directory resume took a row without its token"
RED_CODEC = "WorldTransition codec did not round-trip"
RED_ORDINARY_IDENTITY = "ordinary identity did not stamp lockstep 22 and match config 4"
RED_ADMIT = "a due activation cancelled instead of admitting"
RED_ORDINARY_JOIN_ACCEPTED = "an ordinary NewJoin was accepted"
RED_CATCHUP_PAST_TAIL = "catch-up-ran-past-the-tail"
RED_CATCHUP_CEILING = "catch-up-exceeded-the-ceiling"
RED_TAKE_PUMP = "take-pumped-the-session"
RED_WATERMARK = "image-watermark-wiped"
RED_TICKET_WORLD = "ticket-rejoin-did-not-target-the-world"
RED_SPECTATOR_IMAGE = "spectator-activation-before-its-image"
RED_SPECTATOR_SEAT = "spectator-activated-a-seat"
RED_SPECTATOR_IDS = "spectator-lobby-id-leaked"
RED_TYPED_ADDRESS = "typed-address-targeted-the-wrong-world"
RED_SECOND_JOIN = "second-join-had-no-image"
RED_IMAGE_QUEUE = "joiner-image-replaced"
RED_LOCKSTEP_START_HELD = "world-join-lockstep-held-the-sim-update"
RED_LOCKSTEP_START_MISSED = "world-join-lockstep-did-not-start"
RED_IDENTITY_RECORD = "world-identity-record-did-not-round-trip"
RED_STALE_TRANSITION = "a stale holder generation was applied"
RED_ORDINARY_TRANSITION = "ordinary-round-applied-a-world-transition"
RED_READY_FRAME = "committed ready-frame pack dropped a remote Controller or command"
RED_CATCHUP_RELEASED_EARLY = "catch-up-released-the-sim-before-its-round"
RED_STOP_GATE_SWALLOWED = "lockstep-stop-gate-swallowed-the-catch-up"
RED_CATCHUP_OUTLIVED = "world-catch-up-outlived-its-round"
RED_ACTIVATE_TOOK_A_BRAIN = "activate-took-another-members-brain"
RED_ACTIVATE_BOUND_NO_SEAT = "activate-bound-a-seat-the-roster-does-not-name"
RED_BOOTSTRAP_READ_THE_ARCHIVE = "bootstrap-read-the-archive-it-cannot-send"
RED_REFUSED_TRANSFER_NOT_RETRIED = "refused-transfer-was-marked-started"
RED_HOST_KEPT_TAIL_BYTES = "host-kept-a-joiner-tail-chunk"
RED_CONFIG_RESENT = "bootstrap-resent-the-match-config"
RED_SPECTATOR_IMAGE_GATE = "spectator-image-never-left-the-host"
RED_MEMBER_IMAGE_GATE = "member-image-never-left-the-host"
RED_ADMIT_BEHIND_SENT_INPUT = "world-member-admitted-behind-the-sent-input"
RED_ADMIT_REPLAY = "world-member-missed-the-frames-sent-before-its-admission"
RED_ADMIT_DICTIONARY = "world-member-cannot-decode-the-observation-dictionary"
RED_WINDOW_COPY = "window-copy-before-the-members-start-failed-the-round"
RED_EXISTING_MEMBER_DICTIONARY = "existing-member-lost-the-dictionary-across-an-admission"
RED_REPLAY_TABLE_DIVERGED = "replayed-member-table-diverged-from-the-live-table"
RED_IMAGE_BEFORE_WRITE = "world-image-published-before-the-archive-was-written"
RED_IMAGE_SIM_READ = "world-image-publish-read-the-sim-thread"

CASES = (
    {
        "name": "fresh-join-running-world",
        "argv": ["-net-world-live-selftest"],
        "red": RED_FRESH_JOIN_LIVE_MATCH,
        "pass_token": "[net-world-live-selftest] PASS",
    },
    {
        "name": "ordinary-live-join-names-failure",
        "argv": ["-net-world-ordinary-live-selftest"],
        "red": RED_ORDINARY_JOIN_ACCEPTED,
        "pass_token": "[net-world-ordinary-live-selftest] PASS",
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
        "name": "world-identity-survives-host-restart",
        "kind": "host_restart",
        "fn": "host_restart_same_world_id",
        "argv": ["-net-world-identity-print-selftest"],
        "red": RED_WORLD_ID_DID_NOT_SURVIVE,
        "pass_token": "[net-world-identity-print-selftest] PASS",
        "restarts": 2,
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
        "name": "directory-resume-same-world-id",
        "kind": "python",
        "fn": "directory_resume_same_world_id",
        "red": RED_DIRECTORY_RESUME,
        "also_red": (RED_DIRECTORY_TOKEN, RED_DIRECTORY_SEIZED),
        "pass_token": "[directory-resume] PASS",
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
        "also_red": (
            RED_ADMIT_BEHIND_SENT_INPUT,
            RED_ADMIT_REPLAY,
            RED_ADMIT_DICTIONARY,
            RED_WINDOW_COPY,
            RED_EXISTING_MEMBER_DICTIONARY,
            RED_REPLAY_TABLE_DIVERGED,
        ),
        "pass_token": "[net-world-admit-selftest] PASS",
    },
    {
        "name": "catch-up-stops-at-a-tail-gap",
        "argv": ["-net-world-catchup-gap-selftest"],
        "red": RED_CATCHUP_PAST_TAIL,
        "pass_token": "[net-world-catchup-gap-selftest] PASS",
    },
    {
        "name": "catch-up-keeps-its-ceiling",
        "argv": ["-net-world-catchup-ceiling-selftest"],
        "red": RED_CATCHUP_CEILING,
        "pass_token": "[net-world-catchup-ceiling-selftest] PASS",
    },
    {
        "name": "take-does-not-pump-the-session",
        "argv": ["-net-world-take-pump-selftest"],
        "red": RED_TAKE_PUMP,
        "pass_token": "[net-world-take-pump-selftest] PASS",
    },
    {
        "name": "image-watermark-survives-an-empty-copy",
        "argv": ["-net-world-watermark-selftest"],
        "red": RED_WATERMARK,
        "pass_token": "[net-world-watermark-selftest] PASS",
    },
    {
        "name": "ticket-rejoin-targets-the-world",
        "argv": ["-net-world-ticket-selftest"],
        "red": RED_TICKET_WORLD,
        "pass_token": "[net-world-ticket-selftest] PASS",
    },
    {
        "name": "spectator-activation-waits-for-its-image",
        "argv": ["-net-world-spectator-image-selftest"],
        "red": RED_SPECTATOR_IMAGE,
        "pass_token": "[net-world-spectator-image-selftest] PASS",
    },
    {
        "name": "spectator-never-takes-a-seat",
        "argv": ["-net-world-spectator-seat-selftest"],
        "red": RED_SPECTATOR_SEAT,
        "pass_token": "[net-world-spectator-seat-selftest] PASS",
    },
    {
        "name": "spectator-lobby-ids-recycle",
        "argv": ["-net-world-spectator-ids-selftest"],
        "red": RED_SPECTATOR_IDS,
        "pass_token": "[net-world-spectator-ids-selftest] PASS",
    },
    {
        "name": "typed-address-targets-only-its-host",
        "argv": ["-net-world-typed-address-selftest"],
        "red": RED_TYPED_ADDRESS,
        "pass_token": "[net-world-typed-address-selftest] PASS",
    },
    {
        "name": "second-join-at-the-same-tick",
        "argv": ["-net-world-second-join-selftest"],
        "red": RED_SECOND_JOIN,
        "pass_token": "[net-world-second-join-selftest] PASS",
    },
    {
        "name": "queued-image-keeps-the-first-joiner",
        "argv": ["-net-world-image-queue-selftest"],
        "red": RED_IMAGE_QUEUE,
        "pass_token": "[net-world-image-queue-selftest] PASS",
    },
    {
        # Two arms in one process: the silent remote must cost updates, not one held update, and the
        # released remote must bring the joiner's lockstep up at E.
        "name": "world-join-lockstep-start-frees-the-sim-update",
        "argv": ["-net-world-lockstep-start-selftest"],
        "red": RED_LOCKSTEP_START_HELD,
        "also_red": RED_LOCKSTEP_START_MISSED,
        "pass_token": "[net-world-lockstep-start-selftest] PASS",
    },
    {
        "name": "world-identity-record-round-trip",
        "argv": ["-net-world-identity-selftest"],
        "red": RED_IDENTITY_RECORD,
        "pass_token": "[net-world-identity-selftest] PASS",
    },
    {
        "name": "stale-or-off-plane-world-transition-refused",
        "argv": ["-net-world-stale-selftest"],
        "red": RED_STALE_TRANSITION,
        "also_red": RED_ORDINARY_TRANSITION,
        "pass_token": "[net-world-stale-selftest] PASS",
    },
    {
        "name": "ready-frame-pack-keeps-remotes",
        "argv": ["-net-world-ready-frame-selftest"],
        "red": RED_READY_FRAME,
        "pass_token": "[net-world-ready-frame-selftest] PASS",
    },
    {
        "name": "catch-up-holds-the-sim-until-its-round-runs",
        "argv": ["-net-world-catchup-hold-selftest"],
        "red": RED_CATCHUP_RELEASED_EARLY,
        "also_red": (RED_STOP_GATE_SWALLOWED, RED_CATCHUP_OUTLIVED),
        "pass_token": "[net-world-catchup-hold-selftest] PASS",
    },
    {
        "name": "activate-never-takes-another-members-brain",
        "argv": ["-net-world-activate-brain-selftest"],
        "red": RED_ACTIVATE_TOOK_A_BRAIN,
        "also_red": RED_ACTIVATE_BOUND_NO_SEAT,
        "pass_token": "[net-world-activate-brain-selftest] PASS",
    },
    {
        "name": "world-image-published-from-the-writer",
        "argv": ["-net-world-image-publish-selftest"],
        "red": RED_IMAGE_BEFORE_WRITE,
        "also_red": RED_IMAGE_SIM_READ,
        "pass_token": "[net-world-image-publish-selftest] PASS",
    },
    {
        "name": "host-bootstrap-refusals-and-retries",
        "argv": ["-net-world-bootstrap-selftest"],
        "red": RED_BOOTSTRAP_READ_THE_ARCHIVE,
        "also_red": (
            RED_REFUSED_TRANSFER_NOT_RETRIED,
            RED_HOST_KEPT_TAIL_BYTES,
            RED_SPECTATOR_IMAGE_GATE,
            RED_MEMBER_IMAGE_GATE,
            RED_CONFIG_RESENT,
        ),
        "pass_token": "[net-world-bootstrap-selftest] PASS",
    },
)


def score_stdout(stdout: str, case: dict) -> dict:
    also = case.get("also_red")
    also = list(also) if isinstance(also, (list, tuple)) else [also]
    for red in [case["red"], *also]:
        if red and red in (stdout or ""):
            return {"pass": False, "reason": f"FAIL: {red}"}
    token = case["pass_token"]
    if token not in (stdout or ""):
        return {"pass": False, "reason": f"missing {token}"}
    return {"pass": True, "reason": ""}


def _world_row(world_id: str, boot: int, resume_token: str = "") -> dict:
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
        "lockstep_codec_version": 23,
        "controller_frame_version": 7,
        "match_config_hash": "a" * 64,
        "session_identity_hash": "b" * 64,
        "module_manifest_hash": "c" * 64,
        "listen_port": 42124,
        "listen_addrs": ["127.0.0.1"],
        "join_mode": "ip",
        "persistent_world": True,
        "world_id": world_id,
        "world_boot": boot,
        "resume_session_id": world_id,
    }
    if resume_token:
        row["resume_token"] = resume_token
    return row


def directory_resume_same_world_id() -> None:
    """Directory detecting helper: the world's own token resumes its row, nothing else does."""
    tools = Path(__file__).resolve().parent
    sys.path.insert(0, str(tools / "session_directory"))
    import session_directory

    world_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"
    directory = session_directory.SessionDirectory(expiry_s=60, heartbeat_s=15)
    first = directory.register(_world_row(world_id, 1), "127.0.0.1", 0.0)
    second = directory.register(_world_row(world_id, 2, first["token"]), "127.0.0.1", 1.0)
    if first["session_id"] != world_id or second["session_id"] != world_id:
        raise AssertionError("directory resume did not keep the world id, got %r and %r" % (first["session_id"], second["session_id"]))
    if first["token"] == second["token"]:
        raise AssertionError("directory resume did not issue a new token, both are %r" % (first["token"],))
    try:
        directory.register(_world_row(world_id, 3, "not-the-row-token"), "203.0.113.9", 2.0)
    except PermissionError:
        pass
    else:
        raise AssertionError("directory resume took a row without its token, world %r" % (world_id,))
    listed = directory.list_sessions(3.0, None, None, None)["sessions"]
    if not listed or listed[0].get("world_boot") != 2:
        raise AssertionError("directory resume took a row without its token, the row now reads %r" % (listed,))


def _printed_world_id(text: str) -> str:
    for line in (text or "").splitlines():
        if "world-id=" in line:
            return line.split("world-id=", 1)[1].strip()
    return ""


def host_restart_same_world_id(repo: Path, out: Path) -> None:
    """Two runner launches sharing one identity directory; the world id must match after the restart."""
    tools = Path(__file__).resolve().parent
    sys.path.insert(0, str(tools))
    import run_sim_test

    out = Path(out)
    # Each launch gets its own private runtime, so the identity both of them boot from is named here.
    identity = out / "world-identity"
    identity.mkdir(parents=True, exist_ok=True)
    env = {"CCCP_HEADLESS": "1", "CCCP_WORLD_IDENTITY_DIR": str(identity)}
    printed = []
    for index in (1, 2):
        run = run_sim_test.make_run(repo, ["-net-world-identity-print-selftest"], out / f"launch{index}", timeout=180, env=env)
        run.start().finish()
        log = out / f"launch{index}" / "stdout.log"
        printed.append(_printed_world_id(log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""))
    if not printed[0] or printed[0] != printed[1]:
        raise AssertionError("world-id-did-not-survive-restart: " + repr(printed))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "directory-resume":
        directory_resume_same_world_id()
        print("[directory-resume] PASS")
    elif len(sys.argv) > 1 and sys.argv[1] == "host-restart":
        host_restart_same_world_id(Path(sys.argv[2]), Path(sys.argv[3] if len(sys.argv) > 3 else os.getcwd()))
        print("[net-world-identity-print-selftest] PASS")
        # Both launches went through the runner: nothing here starts the executable itself.
    else:
        directory_resume_same_world_id()
        print("[directory-resume] PASS")
