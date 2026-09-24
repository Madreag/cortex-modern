"""Detecting cases for the persistent world: one engine flag or helper per RED.

Each case names the exact FAIL line that scores it, so a startup that dies first
cannot stand in for another case.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
from pathlib import Path

# Ports: this driver owns 48860-48879; only the world-segment arm binds one (a two-peer world round).
SEGMENT_PORT = 48860


def _wire_version(header: str, owner: str, name: str) -> int:
    """One class-scoped constexpr from the sources under test, so an expectation cannot pin a stale literal."""
    path = Path(__file__).resolve().parent.parent / header
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
    found = list(re.finditer(rf"(?m)^[ \t]*static\s+constexpr\s+\w+\s+{name}\s*=\s*(\d+)\s*;", body))
    if len(found) != 1:
        raise RuntimeError(f"{path}: expected exactly one definition of {owner}::{name}, found {len(found)}")
    return int(found[0].group(1))

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
RED_REJOIN_FIRST_OFFER = "the world offered the first join no ticket"
RED_H4_LEAVE_CLOSED_WORLD = "H4 clean leave closed a persistent-world seat"
RED_DIRECTORY_BOOT = "world_boot 0 was accepted on the C++ register decoder"
RED_DIRECTORY_RESUME = "directory resume did not keep the world id"
RED_DIRECTORY_TOKEN = "directory resume did not issue a new token"
RED_DIRECTORY_SEIZED = "directory resume took a row without its token"
RED_CODEC = "WorldTransition codec did not round-trip"
RED_ORDINARY_IDENTITY = ("ordinary identity did not stamp lockstep " +
                         str(_wire_version("Source/Network/NetLockstep.h", "NetLockstepCodec", "c_Version")) +
                         " and match config " +
                         str(_wire_version("Source/Network/NetMatchConfig.h", "NetMatchConfigUtil", "c_Version")))
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
RED_CAPACITY_WIRE = "world-capacity-did-not-ride-the-v5-config"
RED_OVERFLOW_REFUSAL = "world-full-refusal-missing"
RED_CONCURRENT_ACTIVATION = "concurrent-joins-shared-an-activation"
RED_PROMOTION_MISSING = "promotion-never-happened"
RED_RESPAWN_MISSING = "seat-respawn-never-scheduled"
RED_RESPAWN_EARLY = "seat-respawn-came-early"
RED_RESPAWN_TWICE = "seat-respawn-fired-twice"
RED_RESPAWN_FREED_SEAT = "seat-respawn-freed-the-seat"
RED_RESPAWN_TRANSITION = "seat-respawn-transition-is-wrong"
RED_RESPAWN_GENERATION = "seat-respawn-moved-the-generation"
RED_RESPAWN_NO_BRAIN = "seat-respawn-binds-no-brain"
RED_RESPAWN_WIRE = "seat-respawn-left-the-wire"
RED_RESPAWN_UNKNOWN_KIND = "seat-respawn-accepted-an-unknown-kind"
RED_RESPAWN_DELAY = "seat-respawn-delay-is-wrong"
RED_RESPAWN_LIVING_BRAIN = "seat-respawn-took-a-living-brain"
RED_PROMOTION_WRONG_WATCHER = "promotion-took-the-wrong-watcher"
RED_PROMOTION_HELD_SLOT = "promotion-took-a-held-slot"
RED_PROMOTION_GENERATION = "promotion-kept-the-old-generation"
RED_PROMOTION_BRAIN = "promotion-bound-the-wrong-brain"
RED_PROMOTION_SECOND_WATCHER = "promotion-moved-the-second-watcher"
RED_PROMOTION_DECLINE = "promotion-ignored-a-decline"
RED_PROMOTION_LOBBY_ID = "promotion-kept-the-watcher-lobby-id"
RED_PROMOTION_BEHIND_INPUT = "promotion-announced-behind-the-sent-input"
RED_CLEAN_LEAVE_WRONG_SEAT = "clean-leave-released-the-wrong-seat"
RED_CLEAN_LEAVE_LIVE_MEMBER = "clean-leave-released-a-live-member"
RED_CLEAN_LEAVE_MISSED = "clean-leave-was-not-detected"
RED_DUE_WALK_SKIPPED = "due-walk-skipped-the-member"
RED_RESPAWN_DELAY_SOURCES = "world-respawn-delay-has-two-sources"
RED_RESPAWN_DELAY_NOT_CONFIG = "world-respawn-delay-is-not-the-config"
RED_RELEASE_KEPT_CONTROL = "release-kept-the-departed-control"
RED_RELEASE_TOOK_LIVE_CONTROL = "release-took-a-live-members-control"
RED_RELEASE_CLONED_BRAIN = "release-left-the-brain-to-a-clone"
RED_RECLAIM_HOLD_SLOT = "reclaim-hold-missed-the-slot"
RED_RECLAIM_HOLD_WRONG_SLOT = "reclaim-hold-fenced-the-wrong-slot"
RED_RECLAIM_HOLD_STRANGER = "reclaim-hold-let-a-stranger-in"
RED_RECLAIM_HOLD_OWN_HOLDER = "reclaim-hold-refused-its-own-holder"
RED_PROMOTED_DROP_SEAT_ID = "promoted-drop-named-the-seats-id"
RED_PROMOTED_DROP_ACTORS = "promoted-drop-took-the-wrong-actors"
RED_PROMOTED_DROP_PLAIN_SEAT = "promoted-drop-moved-a-plain-seat"
RED_PROMOTED_FENCE_MISSED = "promoted-fence-missed-the-slot"
RED_PROMOTED_FENCE_STRANGER = "promoted-fence-held-a-stranger"
RED_PROMOTED_RESEAT_SEAT_ID = "promoted-reseat-named-the-seats-id"
RED_PROMOTED_RESEAT_ACTORS = "promoted-reseat-took-the-wrong-actors"
RED_PROMOTED_RESEAT_HOLD = "promoted-reseat-resolved-the-wrong-hold"
RED_WORLD_FIRST_SEAT = "world-first-seat-unnamable"
RED_WORLD_FIRST_SEAT_MATCH = "world-first-seat-moved-a-match"
RED_WATCHER_NO_SEAT = "watcher-has-no-seat"
RED_WATCHER_TOOK_MEMBER_SEAT = "watcher-took-a-members-seat"
RED_WATCHER_PAST_BOUND = "watcher-past-the-bound"
RED_WATCHER_REFUSED = "watcher-refused-by-a-full-world"
RED_FRESH_STOLE_SEAT = "fresh-join-stole-a-held-seat"
RED_FRESH_OPENED_HOLD = "fresh-join-opened-a-hold"
RED_SEAT_SUBSTITUTED = "held-seat-was-substituted"
RED_RECLAIM_REFUSED = "reclaim-was-refused"
RED_EXPIRED_HOLD = "expired-hold-never-promoted"
RED_RECLAIM_DOUBLED = "reclaim-doubled-the-bootstrap"
RED_CONCURRENT_CAPTURE = "concurrent-joins-recaptured-the-world"
RED_CONCURRENT_RESTART = "concurrent-join-dropped-the-first-restart"
RED_OVERFLOW_BOUND = "world-spectator-bound-ignored"
RED_OVERFLOW_ADMITTED = "world-admitted-past-its-bound"
RED_OVERFLOW_SEATS = "world-seats-refused-below-capacity"
RED_OVERFLOW_ROW = "world-row-lost-its-spectator-count"
RED_REFUSAL_LIVE_BINDING = "world-refusal-took-a-live-binding"
RED_REFUSAL_UNDELIVERED = "world-refusal-never-reached-the-joiner"
RED_CAPACITY_MOVED_ORDINARY = "world-capacity-moved-an-ordinary-config"
RED_CAPACITY_HASH = "world-capacity-left-the-v5-hash"
RED_CAPACITY_UNBOUNDED = "world-capacity-decoded-past-its-bound"
RED_CAPACITY_VALIDATED = "world-capacity-passed-validation"
RED_CAPACITY_SLOTS = "world-capacity-left-the-slot-table"
RED_CAPACITY_RECORD = "world-capacity-left-the-identity-record"

RED_WORLD_RESTART_CHECKPOINT = "world-restart-did-not-open-on-the-checkpoint"
RED_WORLD_RESTART_BOOT = "world-restart-did-not-advance-the-boot"
RED_WORLD_RESTART_ROUND = "world-restart-lost-the-round"
RED_WORLD_RESTART_SIDE_STATE = "world-restart-lost-the-side-state"
RED_WORLD_RESTART_IMAGE = "world-restart-image-named-the-old-boot"
RED_WORLD_RESTART_TICKETS = "world-restart-lost-its-tickets"
RED_WORLD_RESTART_HELD_SEAT = "world-restart-row-offered-a-held-seat"
RED_WORLD_RESTART_STALE_GENERATION = "world-restart-admitted-a-stale-generation"
RED_WORLD_RESTART_ROW_WORLD = "world-restart-row-lost-the-world"
RED_WORLD_RESTART_ROW_WATCHERS = "world-restart-row-lost-its-watchers"
RED_WORLD_RESTART_ROW_TOKEN = "world-restart-row-lost-its-token"
RED_WORLD_FRESH_RESUMED = "world-fresh-resumed-anyway"
RED_WORLD_FRESH_DROPPED = "world-fresh-dropped-the-old-checkpoints"
RED_WORLD_LEFT_ROUND = "world-resumed-a-round-it-had-left"
RED_WORLD_ANOTHER_MATCH = "world-resumed-another-match"
RED_WORLD_STOP_NO_CHECKPOINT = "world-clean-stop-wrote-no-checkpoint"
RED_WORLD_STOP_TWICE = "world-clean-stop-wrote-twice"
RED_WORLD_STOP_UNOWED = "world-clean-stop-wrote-a-checkpoint-it-should-not"
RED_WORLD_WATCH_WRONG_SESSION = "world-watch-browsed-the-wrong-session"
RED_WORLD_WATCH_MISSED = "world-watch-missed-the-returned-world"
RED_WORLD_TICKET_LOBBY_SESSION = "world-ticket-kept-the-lobby-session"

RED_SEGMENT_HEADER_LOST = "world-segment-header-lost"
RED_SEGMENT_ROLL_MISSED = "world-segment-roll-missed"
RED_SEGMENT_OUTLIVED = "world-segment-outlived-its-checkpoint"
RED_SEGMENT_DROPPED_EARLY = "world-segment-dropped-with-a-kept-checkpoint"
RED_SEGMENT_PLAYBACK_MISSING = "world-segment-playback-admitted-a-missing-checkpoint"
RED_SEGMENT_PLAYBACK_DIGEST = "world-segment-playback-admitted-another-world"
RED_SEGMENT_PLAYBACK_START = "world-segment-playback-started-off-the-checkpoint"
RED_SEGMENT_PLAYBACK_REFUSED = "world-segment-playback-refused-its-own-checkpoint"
RED_SEGMENT_PLAYBACK_TICK = "world-segment-playback-stood-on-the-wrong-tick"
RED_SEGMENT_PLAYBACK_SIDE_STATE = "world-segment-playback-lost-the-side-state"
RED_SEGMENT_PLAYBACK_MANIFEST = "world-segment-playback-admitted-a-manifestless-checkpoint"
RED_HEAL_WINDOW_CHANGED = "heal-window-changed"
RED_HEAL_WINDOW_FIRST_THREE = "heal-window-refused-the-first-three"
RED_HEAL_WINDOW_FOURTH = "heal-window-admitted-a-fourth"
RED_HEAL_WINDOW_OUTSIDE = "heal-window-refused-a-heal-outside-it"
RED_HEAL_WINDOW_RESUMED = "heal-window-survived-a-resumed-round"
RED_BROWSER_WORLD_ROW = "browser-world-row-text-changed"
RED_BROWSER_ORDINARY_ROW = "browser-ordinary-row-text-changed"
RED_BROWSER_ROW_MISSING = "browser-world-row-missing"

# The world-segment arm's own REDs: what the driver scores when a real world's segments are read back.
RED_SEGMENT_NOT_WRITTEN = "world-wrote-no-segment"
RED_SEGMENT_LOST_ITS_CHECKPOINT = "world-segment-lost-its-checkpoint"
RED_SEGMENT_HEADER_MISSING = "world-segment-verify-reported-no-header"
RED_SEGMENT_DIGEST = "world-segment-digest-differs-from-its-archive"
RED_SEGMENT_BOOTED_THE_PRESET = "world-segment-playback-booted-the-preset"
RED_SEGMENT_CHAIN_BROKE = "world-segment-chain-broke"
RED_SEGMENT_HASHES_DIVERGED = "world-segment-playback-hashes-diverged"
RED_SEGMENT_TAIL_UNRECORDED = "world-segment-chain-left-the-host-tail-unrecorded"
RED_RESUMED_NO_SEGMENT = "resumed-world-wrote-no-segment"
RED_RESUMED_ORDINARY_FILE = "resumed-world-recorded-an-ordinary-file"
RED_RESUMED_HEADER_WRONG = "resumed-world-segment-header-wrong"
RED_RESUMED_HELD_FRAMES = "resumed-world-held-its-first-frames"
RED_RESUMED_NO_DIGEST = "resumed-world-recorded-without-a-digest"
RED_RESUMED_UNARMED = "resumed-world-recorded-unarmed"

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
        "also_red": (RED_REJOIN_FIRST_OFFER,),
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
        "also_red": (
            RED_WORLD_RESTART_ROW_WORLD,
            RED_WORLD_RESTART_ROW_WATCHERS,
            RED_WORLD_RESTART_ROW_TOKEN,
        ),
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
        "name": "ordinary-identity-stamps-26-and-6",
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
        "name": "seat-respawn-keeps-the-world-running",
        "argv": ["-net-world-respawn-selftest"],
        "red": RED_RESPAWN_MISSING,
        "also_red": (
            RED_RESPAWN_EARLY,
            RED_RESPAWN_TWICE,
            RED_RESPAWN_FREED_SEAT,
            RED_RESPAWN_TRANSITION,
            RED_RESPAWN_GENERATION,
            RED_RESPAWN_NO_BRAIN,
            RED_RESPAWN_WIRE,
            RED_RESPAWN_UNKNOWN_KIND,
            RED_RESPAWN_DELAY,
            RED_RESPAWN_LIVING_BRAIN,
        ),
        "pass_token": "[net-world-respawn-selftest] PASS",
    },
    {
        "name": "freed-slot-promotes-the-oldest-spectator",
        "argv": ["-net-world-promotion-selftest"],
        "red": RED_PROMOTION_MISSING,
        "also_red": (
            RED_PROMOTION_WRONG_WATCHER,
            RED_PROMOTION_HELD_SLOT,
            RED_PROMOTION_GENERATION,
            RED_PROMOTION_BRAIN,
            RED_PROMOTION_SECOND_WATCHER,
            RED_PROMOTION_DECLINE,
            RED_PROMOTION_LOBBY_ID,
            RED_PROMOTION_BEHIND_INPUT,
        ),
        "pass_token": "[net-world-promotion-selftest] PASS",
    },
    {
        "name": "clean-leave-releases-only-the-seat-that-left",
        "argv": ["-net-world-clean-leave-selftest"],
        "red": RED_CLEAN_LEAVE_WRONG_SEAT,
        "also_red": (RED_CLEAN_LEAVE_LIVE_MEMBER, RED_CLEAN_LEAVE_MISSED),
        "pass_token": "[net-world-clean-leave-selftest] PASS",
    },
    {
        "name": "due-spectator-leaves-the-member-due",
        "argv": ["-net-world-due-walk-selftest"],
        "red": RED_DUE_WALK_SKIPPED,
        "pass_token": "[net-world-due-walk-selftest] PASS",
    },
    {
        "name": "world-respawn-delay-has-one-source",
        "argv": ["-net-world-respawn-delay-selftest"],
        "red": RED_RESPAWN_DELAY_SOURCES,
        "also_red": (RED_RESPAWN_DELAY_NOT_CONFIG,),
        "pass_token": "[net-world-respawn-delay-selftest] PASS",
    },
    {
        "name": "release-frees-the-departed-brain",
        "argv": ["-net-world-release-control-selftest"],
        "red": RED_RELEASE_KEPT_CONTROL,
        "also_red": (RED_RELEASE_TOOK_LIVE_CONTROL, RED_RELEASE_CLONED_BRAIN),
        "pass_token": "[net-world-release-control-selftest] PASS",
    },
    {
        "name": "reclaim-hold-follows-the-seats-slot",
        "argv": ["-net-world-reclaim-hold-selftest"],
        "red": RED_RECLAIM_HOLD_SLOT,
        "also_red": (
            RED_RECLAIM_HOLD_WRONG_SLOT,
            RED_RECLAIM_HOLD_STRANGER,
            RED_RECLAIM_HOLD_OWN_HOLDER,
        ),
        "pass_token": "[net-world-reclaim-hold-selftest] PASS",
    },
    {
        "name": "promoted-seat-drops-and-reseats-its-slot",
        "argv": ["-net-world-promoted-seat-id-selftest"],
        "red": RED_PROMOTED_DROP_SEAT_ID,
        "also_red": (
            RED_PROMOTED_DROP_ACTORS,
            RED_PROMOTED_DROP_PLAIN_SEAT,
            RED_PROMOTED_FENCE_MISSED,
            RED_PROMOTED_FENCE_STRANGER,
            RED_PROMOTED_RESEAT_SEAT_ID,
            RED_PROMOTED_RESEAT_ACTORS,
            RED_PROMOTED_RESEAT_HOLD,
        ),
        "pass_token": "[net-world-promoted-seat-id-selftest] PASS",
    },
    {
        "name": "world-admits-its-configured-watcher",
        "argv": ["-net-world-watcher-seat-selftest"],
        "red": RED_WATCHER_NO_SEAT,
        "also_red": (
            RED_WATCHER_TOOK_MEMBER_SEAT,
            RED_WATCHER_PAST_BOUND,
            RED_WATCHER_REFUSED,
        ),
        "pass_token": "[net-world-watcher-seat-selftest] PASS",
    },
    {
        "name": "dedicated-world-first-seat-can-be-named",
        "argv": ["-net-world-first-seat-selftest"],
        "red": RED_WORLD_FIRST_SEAT,
        "also_red": (RED_WORLD_FIRST_SEAT_MATCH,),
        "pass_token": "[net-world-first-seat-selftest] PASS",
    },
    {
        "name": "reclaim-outranks-a-fresh-join",
        "argv": ["-net-world-reclaim-selftest"],
        "red": RED_FRESH_STOLE_SEAT,
        "also_red": (
            RED_FRESH_OPENED_HOLD,
            RED_SEAT_SUBSTITUTED,
            RED_RECLAIM_REFUSED,
            RED_EXPIRED_HOLD,
            RED_RECLAIM_DOUBLED,
        ),
        "pass_token": "[net-world-reclaim-selftest] PASS",
    },
    {
        "name": "concurrent-joins-keep-their-own-activation",
        "argv": ["-net-world-concurrent-selftest"],
        "red": RED_CONCURRENT_ACTIVATION,
        "also_red": (RED_CONCURRENT_CAPTURE, RED_CONCURRENT_RESTART),
        "pass_token": "[net-world-concurrent-selftest] PASS",
    },
    {
        "name": "spectator-overflow-is-bounded",
        "argv": ["-net-world-overflow-selftest"],
        "red": RED_OVERFLOW_REFUSAL,
        "also_red": (
            RED_OVERFLOW_BOUND,
            RED_OVERFLOW_ADMITTED,
            RED_OVERFLOW_SEATS,
            RED_OVERFLOW_ROW,
            RED_REFUSAL_LIVE_BINDING,
            RED_REFUSAL_UNDELIVERED,
        ),
        "pass_token": "[net-world-overflow-selftest] PASS",
    },
    {
        "name": "world-capacity-rides-the-v5-config",
        "argv": ["-net-world-capacity-selftest"],
        "red": RED_CAPACITY_WIRE,
        "also_red": (
            RED_CAPACITY_MOVED_ORDINARY,
            RED_CAPACITY_HASH,
            RED_CAPACITY_UNBOUNDED,
            RED_CAPACITY_VALIDATED,
            RED_CAPACITY_SLOTS,
            RED_CAPACITY_RECORD,
        ),
        "pass_token": "[net-world-capacity-selftest] PASS",
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
    {
        "name": "world-restart-opens-on-its-checkpoint",
        "argv": ["-net-world-restart-selftest"],
        "red": RED_WORLD_RESTART_CHECKPOINT,
        "also_red": (
            RED_WORLD_RESTART_BOOT,
            RED_WORLD_RESTART_ROUND,
            RED_WORLD_RESTART_SIDE_STATE,
            RED_WORLD_RESTART_IMAGE,
        ),
        "pass_token": "[net-world-restart-selftest] PASS",
    },
    {
        "name": "world-restart-keeps-its-tickets",
        "argv": ["-net-world-restart-tickets-selftest"],
        "red": RED_WORLD_RESTART_TICKETS,
        "also_red": (
            RED_WORLD_RESTART_HELD_SEAT,
            RED_WORLD_RESTART_STALE_GENERATION,
        ),
        "pass_token": "[net-world-restart-tickets-selftest] PASS",
    },
    {
        "name": "world-fresh-flag-opens-a-new-round",
        "argv": ["-net-world-fresh-selftest"],
        "red": RED_WORLD_FRESH_RESUMED,
        "also_red": (
            RED_WORLD_FRESH_DROPPED,
            RED_WORLD_LEFT_ROUND,
            RED_WORLD_ANOTHER_MATCH,
        ),
        "pass_token": "[net-world-fresh-selftest] PASS",
    },
    {
        "name": "world-clean-stop-writes-one-final-checkpoint",
        "argv": ["-net-world-final-checkpoint-selftest"],
        "red": RED_WORLD_STOP_NO_CHECKPOINT,
        "also_red": (
            RED_WORLD_STOP_TWICE,
            RED_WORLD_STOP_UNOWED,
        ),
        "pass_token": "[net-world-final-checkpoint-selftest] PASS",
    },
    {
        "name": "world-return-watch-keys-on-the-world-id",
        "argv": ["-net-world-return-watch-selftest"],
        "red": RED_WORLD_WATCH_WRONG_SESSION,
        "also_red": (
            RED_WORLD_WATCH_MISSED,
            RED_WORLD_TICKET_LOBBY_SESSION,
        ),
        "pass_token": "[net-world-return-watch-selftest] PASS",
    },
    {
        "name": "world-segment-header-round-trips",
        "argv": ["-net-world-segment-selftest"],
        "red": RED_SEGMENT_HEADER_LOST,
        "pass_token": "[net-world-segment-selftest] PASS",
    },
    {
        "name": "world-recorder-rolls-at-every-checkpoint",
        "argv": ["-net-world-segment-roll-selftest"],
        "red": RED_SEGMENT_ROLL_MISSED,
        "also_red": (
            RED_SEGMENT_OUTLIVED,
            RED_SEGMENT_DROPPED_EARLY,
        ),
        "pass_token": "[net-world-segment-roll-selftest] PASS",
    },
    {
        "name": "resumed-world-records-a-segment",
        "argv": ["-net-world-segment-resume-selftest"],
        "red": RED_RESUMED_NO_SEGMENT,
        "also_red": (
            RED_RESUMED_ORDINARY_FILE,
            RED_RESUMED_HEADER_WRONG,
            RED_RESUMED_HELD_FRAMES,
            RED_RESUMED_NO_DIGEST,
            RED_RESUMED_UNARMED,
        ),
        "pass_token": "[net-world-segment-resume-selftest] PASS",
    },
    {
        "name": "world-segment-playback-stands-on-the-checkpoint",
        "argv": ["-net-world-segment-playback-selftest"],
        "red": RED_SEGMENT_PLAYBACK_MISSING,
        "also_red": (
            RED_SEGMENT_PLAYBACK_DIGEST,
            RED_SEGMENT_PLAYBACK_START,
            RED_SEGMENT_PLAYBACK_REFUSED,
            RED_SEGMENT_PLAYBACK_TICK,
            RED_SEGMENT_PLAYBACK_SIDE_STATE,
            RED_SEGMENT_PLAYBACK_MANIFEST,
        ),
        "pass_token": "[net-world-segment-playback-selftest] PASS",
    },
    {
        "name": "heal-cap-is-a-window",
        "argv": ["-net-world-heal-window-selftest"],
        "red": RED_HEAL_WINDOW_FOURTH,
        "also_red": (
            RED_HEAL_WINDOW_CHANGED,
            RED_HEAL_WINDOW_FIRST_THREE,
            RED_HEAL_WINDOW_OUTSIDE,
            RED_HEAL_WINDOW_RESUMED,
        ),
        "pass_token": "[net-world-heal-window-selftest] PASS",
    },
    {
        "name": "browser-world-row-text",
        "argv": ["-net-world-browser-row-selftest"],
        "red": RED_BROWSER_WORLD_ROW,
        "also_red": (
            RED_BROWSER_ORDINARY_ROW,
            RED_BROWSER_ROW_MISSING,
        ),
        "pass_token": "[net-world-browser-row-selftest] PASS",
    },
    {
        "name": "world-segment-replay",
        "kind": "world_segment",
        "fn": "world_segment_replay",
        "argv": [],
        "red": RED_SEGMENT_NOT_WRITTEN,
        "also_red": (
            RED_RESUMED_NO_SEGMENT,
            RED_RESUMED_HEADER_WRONG,
            RED_SEGMENT_LOST_ITS_CHECKPOINT,
            RED_SEGMENT_HEADER_MISSING,
            RED_SEGMENT_DIGEST,
            RED_SEGMENT_BOOTED_THE_PRESET,
            RED_SEGMENT_CHAIN_BROKE,
            RED_SEGMENT_HASHES_DIVERGED,
        ),
        "pass_token": "[world-segment-replay] PASS",
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
        "network_protocol_version": _wire_version("Source/Network/NetProtocol.h", "NetProtocol", "c_Version"),
        "lockstep_codec_version": _wire_version("Source/Network/NetLockstep.h", "NetLockstepCodec", "c_Version"),
        "controller_frame_version": _wire_version("Source/Network/ControllerFrame.h", "ControllerFrame", "c_Version"),
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


def world_segment_replay(repo: Path, out: Path, port: int = SEGMENT_PORT, fullstate_every: int = 0) -> None:
    """A recording world cuts a segment at every checkpoint; each segment replays from that checkpoint.

    Written, NOT run (the 2026-09-16 order). The arm runs one short world round with the recorder
    armed, then verifies and plays the FIRST segment the world wrote: the verify line must report the
    segment header, the playback must stand on that checkpoint instead of booting the preset, every
    canonical tick hash must equal the recording host's from checkpoint+1, and the playback must chain
    into the next segment at the tick the next checkpoint was captured on.

    RED before the change: no `<worldId>-<tick>.ccreplay` is ever written (the recorder keeps one
    file), `-net-replay-verify` prints no `"segment"` field, and `-net-replay` of a world recording
    boots the preset at the recording's first tick instead of the checkpoint's world.
    """
    tools = Path(__file__).resolve().parent
    sys.path.insert(0, str(tools))
    import run_sim_test
    import test_autosave_restore as restore

    # Every N committed ticks both peers of each round hash their whole capture; each round's pair must match.
    restore.FULLSTATE_EVERY = fullstate_every
    out = Path(out)
    world = out / "world"
    world.mkdir(parents=True, exist_ok=True)
    # The runner creates each peer's run directory itself; the recording lands in the host's once it exists.
    recording = world / "host" / "match.ccreplay"
    records = restore._run_world_round(repo, world, port, 1200, {"host": ["-net-replay-out", str(recording)]})
    for who in ("host", "client"):
        assert records[who].get("exit_code") == 0, (who, records[who].get("exit_code"), records[who].get("error"))
    host_log = restore.peer_log(world, "host")
    identity = restore.WORLD_IDENTITY.findall(host_log)
    assert identity, RED_SEGMENT_NOT_WRITTEN + ": the world host never printed its identity"
    world_id = identity[0][0]

    autosaves = world / "host" / "runtime/Autosaves"
    segments = sorted(autosaves.glob(f"{world_id}-*.ccreplay"), key=lambda path: int(path.stem.split("-")[-1]))
    assert len(segments) >= 2, f"{RED_SEGMENT_NOT_WRITTEN}: the world wrote {[p.name for p in segments]}"
    first_tick = int(segments[0].stem.split("-")[-1])
    second_tick = int(segments[1].stem.split("-")[-1])
    held = restore.checkpoints(world, "host")
    archive = held.get(f"{world_id}-{first_tick}.ccsave")
    assert archive, f"{RED_SEGMENT_LOST_ITS_CHECKPOINT}: no archive stands under {segments[0].name}"

    # The verify line reports the header the segment stands on, digest included.
    verify_out = out / "verify.json"
    verify = run_sim_test.make_run(repo, ["-net-replay-verify", str(segments[0]), "-out", str(verify_out)],
                                   out / "verify", timeout=180, env={"CCCP_HEADLESS": "1"})
    verify.start().finish()
    verify_log = (out / "verify" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    line = next((text for text in verify_log.splitlines() if "[net-replay-verify]" in text), "")
    assert line, f"{RED_SEGMENT_HEADER_MISSING}: -net-replay-verify printed nothing for {segments[0].name}"
    report = json.loads(line.split("[net-replay-verify]", 1)[1].strip())
    assert report.get("segment") is True, f"{RED_SEGMENT_HEADER_MISSING}: {report}"
    assert report.get("world_id") == world_id, (report.get("world_id"), world_id)
    assert report.get("segment_tick") == first_tick, (report.get("segment_tick"), first_tick)
    assert report.get("world_digest") == archive["WorldStructureHash"], \
        f"{RED_SEGMENT_DIGEST}: the header says {report.get('world_digest')}, the archive {archive['WorldStructureHash']}"
    assert report.get("first_frame") == first_tick + 1, (report.get("first_frame"), first_tick)

    # Playback stands on the checkpoint: the world's own Autosaves are what the replay run reads.
    # The runner stages the replay's runtime; the world's Autosaves are copied into it before the process starts.
    replay = out / "replay"
    trace = replay / "replay_trace.json"
    play = run_sim_test.make_run(repo, ["-net-replay", str(replay / "runtime/Autosaves" / segments[0].name),
                                        "-tick-hashes", "-out", str(trace)],
                                 replay, timeout=600, env={"CCCP_HEADLESS": "1"})
    assert Path(play.cwd).resolve() == (replay / "runtime").resolve(), (play.cwd, replay)
    shutil.copytree(autosaves, Path(play.cwd) / "Autosaves", dirs_exist_ok=True)
    play.start().finish()
    play_log = (replay / "stdout.log").read_text(encoding="utf-8", errors="replace")
    assert f"[net-replay] segment stands on checkpoint tick={first_tick}" in play_log, \
        f"{RED_SEGMENT_BOOTED_THE_PRESET}: {play_log[-2000:]}"
    assert f"segment_tick={first_tick}" in play_log and "outcome=completed" in play_log, \
        f"{RED_SEGMENT_BOOTED_THE_PRESET}: the playback did not finish on the checkpoint"
    # The chain: the next segment is taken at the tick its checkpoint was captured on, no reload.
    assert f"[net-replay] segment chained into" in play_log and f"at tick {second_tick}" in play_log, \
        f"{RED_SEGMENT_CHAIN_BROKE}: {play_log[-2000:]}"
    # Every canonical tick hash of the replayed world equals the recording host's, from checkpoint+1 to the host's last tick.
    from compare_sim_traces import strict_compare
    windowed = {}
    for name, source in (("host", world / "host_trace.json"), ("replay", trace)):
        data = json.loads(source.read_text(encoding="utf-8-sig"))
        windowed[name] = data
    host_last = max(entry["tick"] for entry in windowed["host"]["runs"][0]["tick_hashes"])
    replay_last = max(entry["tick"] for entry in windowed["replay"]["runs"][0]["tick_hashes"])
    last = min(host_last, replay_last)
    for name, data in windowed.items():
        data["runs"][0]["tick_hashes"] = [entry for entry in data["runs"][0]["tick_hashes"] if first_tick < entry["tick"] <= last]
        (out / f"{name}_from_checkpoint.json").write_text(json.dumps(data), encoding="utf-8")
    passed, compared = strict_compare(str(out / "host_from_checkpoint.json"), str(out / "replay_from_checkpoint.json"),
                                      expected_ticks=last - first_tick, first_tick=first_tick + 1)
    assert passed, f"{RED_SEGMENT_HASHES_DIVERGED}: {compared}"
    # The chain plays every tick the host ran: a tail the segments never recorded is its own failure.
    assert replay_last >= host_last, f"{RED_SEGMENT_TAIL_UNRECORDED}: the host ran to {host_last}, the segments end at {replay_last}"

    # The restarted world: its round opens ON a checkpoint, so its FIRST recording is a segment named
    # for the tick it resumed from - not an ordinary file that names no world.
    restarted = out / "restarted"
    restarted.mkdir(parents=True, exist_ok=True)
    again = restore._run_world_round(repo, restarted, port + 2, 600,
                                     {"host": ["-net-replay-out", str(restarted / "host" / "match.ccreplay")]}, carry=world)
    for who in ("host", "client"):
        assert again[who].get("exit_code") == 0, (who, again[who].get("exit_code"), again[who].get("error"))
    restarted_log = restore.peer_log(restarted, "host")
    resuming = restore.RESUMING.search(restarted_log)
    assert resuming, f"{RED_RESUMED_NO_SEGMENT}: the restarted world never reported the checkpoint it opened on"
    resume_tick = int(resuming[2])
    resumed_segment = restarted / "host" / "runtime/Autosaves" / f"{world_id}-{resume_tick}.ccreplay"
    assert resumed_segment.is_file(),         f"{RED_RESUMED_NO_SEGMENT}: no segment stands on the resumed tick {resume_tick}"
    assert not (restarted / "host" / "match.ccreplay").exists(),         f"{RED_RESUMED_ORDINARY_FILE}: the resumed round wrote an ordinary recording too"
    verify_two = run_sim_test.make_run(repo, ["-net-replay-verify", str(resumed_segment)],
                                       out / "verify_resumed", timeout=180, env={"CCCP_HEADLESS": "1"})
    verify_two.start().finish()
    resumed_log = (out / "verify_resumed" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    resumed_line = next((text for text in resumed_log.splitlines() if "[net-replay-verify]" in text), "")
    assert resumed_line, f"{RED_RESUMED_HEADER_WRONG}: -net-replay-verify printed nothing for {resumed_segment.name}"
    resumed_report = json.loads(resumed_line.split("[net-replay-verify]", 1)[1].strip())
    assert resumed_report.get("segment") is True and resumed_report.get("segment_tick") == resume_tick, resumed_report
    assert resumed_report.get("world_id") == world_id, (resumed_report.get("world_id"), world_id)
    assert resumed_report.get("first_frame") == resume_tick + 1, (resumed_report.get("first_frame"), resume_tick)
    if fullstate_every:
        verdicts = restore.fullstate_pairs(out)
        (out / "fullstate.json").write_text(json.dumps(verdicts, indent=2) + "\n", encoding="utf-8")
        tripped = {name: verdict["reasons"] for name, verdict in verdicts.items() if not verdict["passed"]}
        assert verdicts and not tripped, f"full-state oracle: {tripped or 'no two-peer round was sampled'}"

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


USAGE = """usage: test_persistent_world.py [directory-resume | world-segment REPO [OUT] | host-restart REPO [OUT]] [--fullstate-every N]

  --fullstate-every N  world-segment only: every N committed ticks both peers of each world round hash their whole
                       capture (-net-fullstate-hash-every) and every round's pair must match; 0 is off"""


if __name__ == "__main__":
    if "-h" in sys.argv or "--help" in sys.argv:
        print(USAGE)
        sys.exit(0)
    FULLSTATE_EVERY = 0
    if "--fullstate-every" in sys.argv:
        at = sys.argv.index("--fullstate-every")
        FULLSTATE_EVERY = int(sys.argv[at + 1])
        del sys.argv[at:at + 2]
    if len(sys.argv) > 1 and sys.argv[1] == "directory-resume":
        directory_resume_same_world_id()
        print("[directory-resume] PASS")
    elif len(sys.argv) > 1 and sys.argv[1] == "world-segment":
        world_segment_replay(Path(sys.argv[2]), Path(sys.argv[3] if len(sys.argv) > 3 else os.getcwd()), fullstate_every=FULLSTATE_EVERY)
        print("[world-segment-replay] PASS")
        # Every launch went through run_sim_test.make_run: nothing here starts the executable itself.
    elif len(sys.argv) > 1 and sys.argv[1] == "host-restart":
        host_restart_same_world_id(Path(sys.argv[2]), Path(sys.argv[3] if len(sys.argv) > 3 else os.getcwd()))
        print("[net-world-identity-print-selftest] PASS")
        # Both launches went through the runner: nothing here starts the executable itself.
    else:
        directory_resume_same_world_id()
        print("[directory-resume] PASS")
