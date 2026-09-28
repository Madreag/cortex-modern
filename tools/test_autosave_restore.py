"""Check that every autosave a match writes can be restored, that retention keeps exactly the set a
rejoin can use, and that both peers name the same rewind point.

Three rows, each with the statement that is red without the engine change:

  restore   - a checkpoint the caller names - the OLDEST the match still retains, not the one the store
              would pick by itself - is restored through the Autosaves load path, and the restored world
              is the one that checkpoint recorded.
              RED before the change: no load path reaches Autosaves/ and the archive carries no tick
              identity, so `[autosave] restore_check` cannot be produced at all.
  retention - a match that writes more checkpoints than the policy keeps ends up holding exactly the
              newest restorable ones, under the same file names on both peers.
              RED before the change: the peers name their checkpoints with their own process id and
              wall clock, so the two retained sets share no file name.
  anchor    - a heal that lands after several checkpoints makes the host name one of them and the client
              record that same one; the named checkpoint survives the rotation of the ones that follow.
              RED before the change: no rewind point is named, carried or pinned anywhere.
  resume    - the host process is KILLED mid-match; a new host process restarts the same match from its
              own checkpoint and the client rejoins with its stored ticket. The client's copy of that
              checkpoint is removed first, so the round is played with ONE held peer and ONE streamed
              peer: both reach the same later tick with equal per-tick hashes, which they cannot do if
              the two paths resume onto different seats.
              RED before the change: no restart manifest is written, -net-resume-match does not exist,
              and a host that dies takes its match with it.
  world-restart - a PERSISTENT WORLD host is killed and started again on the same install with no
              -net-resume-match at all: it comes back as the same world (same UUID, boot and round
              advanced by one) standing on its own newest checkpoint, and the client's ticket still
              admits it to its own seat. A second half runs the same restart with -net-world-fresh and
              requires a new round from the scene with the old checkpoints untouched.
              RED before the change: a world boot resolves no resume, so the restarted host opens the
              scene and prints no `[autosave] resuming` line; -net-world-fresh does not parse.

Ports: the default range is 48720-48739; the first three arms take consecutive ports from its base, the
resume arm starts at base + 5 and world-restart at base + 10 (three rounds of two peers), so the default never
overlaps tools/test_autosave.py (48211-48219, 48500-48519), tools/test_post_match_report.py or
tools/test_post_match_combined.py (48215) or tools/test_match_chat.py (48700-48705) and can run beside them.

Nothing here widens a comparison: the restored-world statement is the engine's own world-structure
digest against the digest the checkpoint recorded, and the retained set is compared exactly.
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import threading
import time
import unittest
import zipfile
from collections import Counter
from pathlib import Path

from compare_sim_traces import FULLSTATE, compare_fullstate, load_fullstate
from feel.report import own_hold_windows
from feel.retained_resume import PER_PEER_SUBSYSTEMS, read_live_hashes, split_passes
from run_sim_test import make_run, engine_executable, file_sha256
from feel_measure import stage_baseline

CAPTURE = re.compile(r"^\[autosave\] tick=(\d+) capture_ms=(\d+(?:\.\d+)?) bytes=(\d+)$", re.MULTILINE)
HOLD = re.compile(r"^\[net-match\] hold peer=\d+ frame=\d+ AI in control$", re.MULTILINE)
PARK_RELEASED = re.compile(r"^\[net-lockstep\] capture park released frame=(\d+) end=(\d+) capture_ms=(\d+)$", re.MULTILINE)
PARK_COMMITTED = re.compile(r"^\[net-lockstep\] capture park committed frame=(\d+) end=(\d+) frames=(\d+) with_input=(\d+)$", re.MULTILINE)
RETAINED = re.compile(r"^\[autosave\] retained tick=(\d+) keep=(\d+) pinned=(\d+) removed=(\d+)$", re.MULTILINE)
RESTORE = re.compile(r"^\[autosave\] restore_check (PASS|FAIL) match=(\S+) tick=(\d+) sim_update_count=(\d+) "
                     r"world_hash=(\S+) expected=(\S+) policy=(\d)$", re.MULTILINE)
POLICY = re.compile(r"^\[autosave-store-selftest\] (PASS|FAIL) (.*)$", re.MULTILINE)
ANCHOR = re.compile(r"^\[autosave\] anchor (named|received) match=(\S+) tick=(\d+) local=(.*)$", re.MULTILINE)
RESUMING = re.compile(r"^\[autosave\] resuming match=(\S+) tick=(\d+) activity=(.*) peers=(\d+) directory=(\S+)$", re.MULTILINE)
OFFER = re.compile(r"^\[autosave\] resume offer match=(\S+) tick=(\d+) (held locally|not held: .*)$", re.MULTILINE)
HELD_LAUNCH = re.compile(r"^\[net-match\] launching from the held checkpoint: (\S+)$", re.MULTILINE)
RECEIVED_LAUNCH = re.compile(r"^\[net-match\] launching from the received snapshot: (\S+)$", re.MULTILINE)
FAMILY_LOCK = Path("D:/mx/LEAD_FAMILY.lock")
# Every N committed ticks each peer hashes its whole capture (-net-fullstate-hash-every); 0 is off. Set by --fullstate-every.
FULLSTATE_EVERY = 0
RETAINED_AUTOSAVES = 3  # The default of the NetworkAutosavesKept option (AutosaveStore::c_RetainedAutosaves), which
                        # these runs never set; the engine's own keep= value is held to it below.


WORLD_IDENTITY = re.compile(r"^\[net-world\] identity (\S+) boot=(\d+) round=(\d+)$", re.MULTILINE)
WORLD_START = re.compile(r"^\[net-lockstep\] start round=(\d+) frame=(\d+) local_peer=(\d+) peers=(\d+) input_delay=(\d+)$", re.MULTILINE)
WORLD_LOBBY = re.compile(r"^\[net-match-service-e2e\] lobby_snapshot: [^\n]*\bactivity=Persistent World\b[^\n]*$", re.MULTILINE)
WORLD_AGREED = re.compile(r'^\[autosave\] agreed match=(\S+) tick=(\d+) state=(".*")$', re.MULTILINE)


def fullstate_args() -> list:
    return ["-net-fullstate-hash-every", str(FULLSTATE_EVERY)] if FULLSTATE_EVERY else []


SEAT_HELD = re.compile(r"^\[net-match\] hold peer=\d+ frame=(\d+) AI in control", re.MULTILINE)
SEAT_ADMITTED = re.compile(r"^\[net-match\] private catch-up complete frame=(\d+)", re.MULTILINE)
SEAT_BACK = re.compile(r"^\[net-match\] seat-reclaimed peer=\d+ frame=(\d+)", re.MULTILINE)
# How long a kill waits on a held seat's return before it ends the phase anyway, so a seat that never returns still fails the oracle.
WORLD_KILL_RETURN_WAIT_POLLS = 300


# The kill's wait for that sample: each peer's writer coalesces periodic samples on its own timing, so the samples both
# peers write come unevenly; the wait is twice the longest gap seen between them, never under the floor or over the cap.
WORLD_KILL_SAMPLE_WAIT_MIN_S = 30
WORLD_KILL_SAMPLE_WAIT_CAP_S = 240


def _shared_samples(host_log: str, client_log: str) -> list:
    """The (round, tick) full-state samples both peers have written."""
    host = {(int(round_id), int(tick)) for tick, _, _, round_id in FULLSTATE.findall(host_log)}
    return sorted(host & {(int(round_id), int(tick)) for tick, _, _, round_id in FULLSTATE.findall(client_log)})


def sample_wait_bound_s(shared_seen_at: list) -> float:
    """shared_seen_at: the wall seconds at which each new shared sample was seen."""
    gaps = [later - earlier for earlier, later in zip(shared_seen_at, shared_seen_at[1:])]
    return min(WORLD_KILL_SAMPLE_WAIT_CAP_S, max(WORLD_KILL_SAMPLE_WAIT_MIN_S, 2 * max(gaps, default=0)))


def _arrival_unsampled(host_log: str, client_log: str) -> bool:
    """A seat held or admitted after the peers' last shared full-state sample: ending the phase now fails the round's oracle
    (unsampled_admissions), so the kill waits for the next sample both peers take."""
    arrivals = [int(frame) for frame in SEAT_HELD.findall(host_log)] + [int(frame) for frame in SEAT_ADMITTED.findall(client_log)]
    if not arrivals:
        return False
    shared = [tick for _, tick in _shared_samples(host_log, client_log)]
    return not shared or max(shared) < max(arrivals)


def _seat_mid_return(host_log: str) -> bool:
    """The host has held a seat it has not reclaimed yet: ending the phase now leaves that seat unsampled after its hold."""
    return len(SEAT_HELD.findall(host_log)) > len(SEAT_BACK.findall(host_log))


def unsampled_admissions(verdict: dict, host_log: Path, client_log: Path) -> str:
    """A round never ends with a seat mid-admission and unsampled: every hold and every return must be followed by a shared sample."""
    sampled = verdict.get("sampled_ticks") or []
    if not sampled:
        return ""
    last = max(sampled)
    # A hold agreed for a frame past the round's last tick (the capped stop's drain) never touched the round.
    ticks = [row["tick"] for row in read_live_hashes(host_log.parent.parent / "host-live.jsonl")]
    end = max(ticks) if ticks else float("inf")
    held = [int(frame) for frame in SEAT_HELD.findall(host_log.read_text(encoding="utf-8", errors="replace")) if last < int(frame) <= end]
    admitted = [int(frame) for frame in SEAT_ADMITTED.findall(client_log.read_text(encoding="utf-8", errors="replace")) if last < int(frame) <= end]
    return f"a seat came in after the last shared sample {last}: held at {held}, admitted at {admitted}" if held or admitted else ""


def fullstate_pairs(root: Path, admissions: bool = False) -> dict:
    """The full-state oracle's verdict for every two-peer round under an arm's directory, keyed by the round's directory.
    admissions: a seat that comes in after the round's last shared sample fails the round too."""
    results = {}
    for host_log in sorted(Path(root).rglob("host/stdout.log")):
        client_log = host_log.parent.parent / "client" / "stdout.log"
        if client_log.is_file():
            verdict = compare_fullstate(host_log, client_log)
            if admissions and (reason := unsampled_admissions(verdict, host_log, client_log)):
                verdict["reasons"].append(reason)
                verdict["passed"] = False
            results[host_log.parent.parent.relative_to(root).as_posix()] = verdict
    return results


PERTURB = re.compile(r"^\[net-test\] live perturb frame=(\d+)$", re.MULTILINE)
FULLSTATE_COALESCED = re.compile(r"^\[fullstate-coalesced\] tick=\d+ replaced=(\d+) ", re.MULTILINE)


FULLSTATE_LABELLED = re.compile(r"^\[fullstate-(canonical|restored)\] tick=(\d+) hash=[0-9a-f]{16} sections=(\S+) round=\d+\s*$", re.MULTILINE)
FULLSTATE_SCOPE = re.compile(r"^\[fullstate-scope\] tick=(\d+) round=(\d+) label=sample per_peer=(\S*)\s*$", re.MULTILINE)
FULLSTATE_LABELLED_SCOPE = re.compile(r"^\[fullstate-scope\] tick=(\d+) round=\d+ label=(canonical|restored) per_peer=(\S*)\s*$", re.MULTILINE)
ANCHOR_TICKS = 1400


def injected_divergence_reasons(host_log: Path, client_log: Path, divergences: list, every: int, last_tick: int, excused: list | None = None) -> list:
    """What keeps the anchor arm's heal from being proven, from the two peers' logs; empty when it is.

    The arm advances the host's sim RNG at the start of one tick (Main.cpp's live perturbation) and the oracle samples that
    tick's end, after the tick drew from the advanced stream: that one sample differs, the sim RNG among its sections. The
    heal is proven only by (1) every scheduled sample after it, to the run's end, taken by both peers in the same round,
    (2) no shared section leaving either peer's samples unless the capture named it this machine's own, and (3) each
    peer's first restored world equal to the canonical capture of the snapshot the heal sent: two peers restoring the same
    wrong state agree with each other, not with it. A sample a peer's own lines name as not taken (its saver coalesced it,
    or its seat was held) is expected-absent for that peer and listed in excused; at least one sample after the heal must
    be taken by both."""
    excused = [] if excused is None else excused
    texts = {who: path.read_text(encoding="utf-8", errors="replace") if path.is_file() else "" for who, path in (("host", host_log), ("client", client_log))}
    injected = PERTURB.search(texts["host"])
    if not injected:
        return ["the live perturbation was never injected"]
    tick, reasons = int(injected[1]), []
    seen = [(entry["tick"], entry["sections"]) for entry in divergences]
    if len(seen) != 1 or seen[0][0] != tick or "globals.sim_rng" not in seen[0][1]:
        reasons.append(f"the injected divergence at tick {tick} was expected as the one divergent sample, naming globals.sim_rng; the oracle saw {seen}")
    samples = {"host": load_fullstate(host_log), "client": load_fullstate(client_log)}
    # A peer's absent sample is expected only where its own lines name it: its saver coalesced it, or its seat was held.
    replaced = {who: Counter(int(sampled) for sampled in FULLSTATE_COALESCED.findall(text)) for who, text in texts.items()}
    holds = {who: own_hold_windows(text.splitlines()) for who, text in texts.items()}
    shared = 0
    for sampled in range(tick - tick % every + every, last_tick + 1, every):
        rounds = {who: {key[0] for key in samples[who] if key[1] == sampled} for who in samples}
        missing = [who for who in samples if not rounds[who]]
        unnamed = []
        for who in missing:
            others = {round_id for other in samples if other != who for round_id in rounds[other]}
            if replaced[who][sampled] > 0:
                replaced[who][sampled] -= 1
                excused.append({"peer": who, "tick": sampled, "reason": "coalesced"})
            elif any(first <= sampled < end and (not others or round_id in others) for round_id, first, end in holds[who]):
                excused.append({"peer": who, "tick": sampled, "reason": "held"})
            else:
                unnamed.append(who)
        if unnamed:
            reasons.append(f"no {' or '.join(unnamed)} sample at tick {sampled} after the heal")
        elif missing:
            continue
        elif rounds["host"] != rounds["client"]:
            reasons.append(f"the peers sampled tick {sampled} in different rounds: {sorted(rounds['host'])} vs {sorted(rounds['client'])}")
        else:
            shared += 1
    if not shared:
        reasons.append(f"no sample both peers took after the heal at tick {tick}")
    for who, text in texts.items():
        named = {(int(round_id), int(sampled)): set(filter(None, sections.split(","))) for sampled, round_id, sections in FULLSTATE_SCOPE.findall(text)}
        previous = None
        for key in sorted(samples[who], key=lambda key: (key[1], key[0])):
            names = {name for _, sections in samples[who][key] for name, _ in sections}
            unexplained = sorted((previous or set()) - names - named.get(key, set()))
            if unexplained:
                reasons.append(f"{who}: {unexplained} left the compared sections at tick {key[1]} with no reason named")
            previous = names
    labelled = {who: [(label, int(sampled), dict(item.rsplit(":", 1) for item in sections.split(",")))
                      for label, sampled, sections in FULLSTATE_LABELLED.findall(text)] for who, text in texts.items()}
    # A section one capture keeps for itself (a Lua state that holds actors, per machine) is named in that capture's scope line.
    own = {who: {(label, int(sampled)): set(filter(None, sections.split(","))) for sampled, label, sections in FULLSTATE_LABELLED_SCOPE.findall(text)}
           for who, text in texts.items()}
    canonical = [(sampled, sections) for label, sampled, sections in labelled["host"] if label == "canonical" and sampled >= tick]
    if not canonical:
        reasons.append("the host logged no canonical capture of the snapshot it healed from")
        return reasons
    canonical_tick, canonical_sections = canonical[0]
    canonical_own = own["host"].get(("canonical", canonical_tick), set())
    for who in texts:
        restored = [(sampled, sections) for label, sampled, sections in labelled[who] if label == "restored" and sampled >= tick]
        if not restored:
            reasons.append(f"{who} logged no restored world after the heal")
            continue
        restored_tick, restored_sections = restored[0]
        restored_own = own[who].get(("restored", restored_tick), set())
        differing = sorted(name for name in canonical_sections.keys() | restored_sections.keys()
                           if canonical_sections.get(name) != restored_sections.get(name)
                           and not (name not in canonical_sections and name in canonical_own) and not (name not in restored_sections and name in restored_own))
        if restored_tick != canonical_tick or differing:
            reasons.append(f"{who}'s first restored world (tick {restored_tick}) differs from the canonical snapshot (tick {canonical_tick}) in {differing}")
    return reasons


def expect_injected_divergence(root: Path, verdicts: dict) -> None:
    """Holds the anchor arm's full-state verdict to injected_divergence_reasons, run to ANCHOR_TICKS."""
    for name, verdict in verdicts.items():
        pair = Path(root) / name
        excused = []
        reasons = injected_divergence_reasons(pair / "host" / "stdout.log", pair / "client" / "stdout.log",
                                              verdict.get("divergences") or [], FULLSTATE_EVERY, ANCHOR_TICKS, excused)
        verdict["injected_divergence"] = {"requires": "globals.sim_rng", "every": FULLSTATE_EVERY, "last_tick": ANCHOR_TICKS,
                                          "excused_after_heal": excused}
        verdict["passed"] = not reasons and verdict.get("compared_samples", 0) > 0
        verdict["reasons"] = reasons


def _seat_rows(root: Path, who: str) -> dict:
    """The seats the peer's own match report names, keyed by stable seat."""
    path = root / f"{who}_report.json"
    if not path.exists():
        return {}
    report = json.loads(path.read_text(encoding="utf-8"))
    summary = report.get("last_match") or report.get("service", {}).get("last_match")
    if not summary:
        return {}
    return {peer["seat"]: peer for peer in summary.get("peers", [])}


def pin_settings(run, values: dict) -> None:
    """Writes the named Settings.ini values into a staged peer's own runtime."""
    path = Path(run.cwd) / "Userdata/Settings.ini"
    text = path.read_text(encoding="utf-8-sig")
    for name, value in values.items():
        text, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*", lambda match: match[1] + value, text)
        if count == 0:
            text += f"\n\t{name} = {value}\n"
    path.write_text(text, encoding="utf-8")


def run_pair(repo: Path, root: Path, port: int, ticks: int, seconds: int, extra: dict, settings: dict | None = None, load_objects: int = 0) -> dict:
    """Two peers of one match, each with the arm's own extra flags and Settings.ini values."""
    if FAMILY_LOCK.exists():
        raise RuntimeError(f"engine launch prohibited while {FAMILY_LOCK} exists")
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    for who in ("host", "client"):
        args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
                "-net-match-ticks", str(ticks), "-net-match-input-delay", "3",
                "-net-autosave-seconds", str(seconds),
                "-net-match-service-preset", "Determinism FeelBaseline", "-net-match-service-module", "UserScenes.rte",
                "-net-live-tick-hashes", str(root / f"{who}-live.jsonl"),
                "-tick-hashes", "-max-ticks", str(ticks), "-out", str(root / f"{who}_trace.json"),
                "-net-match-report", str(root / f"{who}_report.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        args += extra.get(who, []) + fullstate_args()
        runs[who] = make_run(repo, args, root / who, 420, env={"CCCP_HEADLESS": "1"})
        stage_baseline(runs[who], ticks, load_objects=load_objects)
        if settings and settings.get(who):
            pin_settings(runs[who], settings[who])

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
    try:
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    return records


def peer_log(root: Path, who: str) -> str:
    return "\n".join((root / who / name).read_text(encoding="utf-8", errors="replace")
                     for name in ("stdout.log", "stderr.log") if (root / who / name).exists())


def carry_store_files(source: Path, target: Path) -> None:
    """Carry regular store files without traversing directories or reparse points."""
    if source.is_symlink() or getattr(source, "is_junction", lambda: False)():
        raise RuntimeError(f"store is a reparse point: {source}")
    target.mkdir(parents=True, exist_ok=True)
    for path in source.iterdir():
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
            raise RuntimeError(f"store entry is a reparse point: {path}")
        if path.is_file():
            shutil.copy2(path, target / path.name)
        elif path.is_dir():
            raise RuntimeError(f"unexpected directory in store: {path}")


def descriptor(path: Path) -> dict:
    """The checkpoint's own restore identity, read the way a restore reads it."""
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None, f"corrupt checkpoint: {path}"
        names = set(archive.namelist())
        assert {"Restore.ini", "Index.ini", "Save.ini", "Save Mat.png", "Save FG.png", "Save BG.png"} <= names, (path, sorted(names))
        fields = {}
        for line in archive.read("Restore.ini").decode("utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                fields[key.strip()] = value.strip()
        raw = archive.read("Restore.ini")
        state = archive.read("Save.ini").decode("utf-8")
    world = re.search(r"(?m)^\s*SimUpdateCount = (\d+)\s*$", state)
    assert world, f"checkpoint carries no SimUpdateCount: {path}"
    fields["_world_tick"] = world[1]
    fields["_descriptor_sha256"] = hashlib.sha256(raw).hexdigest()
    return fields


def checkpoints(root: Path, who: str) -> dict:
    directory = root / who / "runtime/Autosaves"
    files = sorted(directory.glob("*.ccsave"))
    held = {}
    for path in files:
        fields = descriptor(path)
        tick = int(fields["SavedTick"])
        assert path.name == f"{fields['MatchId']}-{tick}.ccsave", f"name does not carry the match and tick: {path}"
        assert fields["_world_tick"] == fields["SavedTick"], f"descriptor tick differs from the world: {path}"
        assert fields["RestoreSchema"] == "1", fields
        assert int(fields["SessionId"]) > 0 and int(fields["RoundId"]) > 0, fields
        assert fields["GameVersion"] and fields["ModuleManifestHash"] and fields["DeterministicConfigHash"], fields
        assert len(fields["WorldStructureHash"]) == 64, fields
        held[path.name] = fields
    return held


def arm_restore(repo: Path, root: Path, port: int) -> dict:
    """A restore of the checkpoint the caller names reproduces the world that checkpoint recorded, and
    both peers restore the same one. The named checkpoint is the OLDEST of the retained set, so a restore
    that ignored the name and took the store's own pick would be red here. The two peers' world digests
    are NOT compared against each other: a checkpoint carries per-peer locals, so each peer is only held
    to the world its own checkpoint recorded."""
    # With two checkpoints the policy self-test cannot tell its own sub-results apart, so the restore fires
    # well past the retention limit: at a 2 s cadence (120 ticks) at least four stand behind tick 700, and
    # the row asserts the count it actually observed.
    ticks, restore_at = 800, 700
    extra = {who: ["-net-autosave-restore", "oldest", "-net-autosave-restore-at", str(restore_at)]
             for who in ("host", "client")}
    records = run_pair(repo, root, port, ticks, 2, extra)
    details = {}
    for who in ("host", "client"):
        assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
        log = peer_log(root, who)
        line = RESTORE.search(log)
        assert line, f"{who} never reported a restore check: {root / who / 'stdout.log'}"
        policy = POLICY.search(log)
        assert policy and policy[1] == "PASS", f"{who} failed the checkpoint policy self-test: {policy[2] if policy else 'missing'}"
        verdict, match_id, tick, restored_tick, world_hash, expected, policy_flag = line.groups()
        captures = [int(captured) for captured, _, _ in CAPTURE.findall(log)]
        before = [captured for captured in captures if captured <= int(tick)]
        assert len(captures) > RETAINED_AUTOSAVES, f"{who} restored before the retention limit was exceeded: {captures}"
        assert len(before) >= 1, f"{who} restored a checkpoint it never captured: {tick} of {captures}"
        held = checkpoints(root, who)
        name = f"{match_id}-{tick}.ccsave"
        assert name in held, (name, sorted(held))
        ticks_held = sorted(int(fields["SavedTick"]) for fields in held.values())
        assert len(ticks_held) == RETAINED_AUTOSAVES, (ticks_held, captures)
        assert int(tick) == ticks_held[0], f"{who} did not restore the checkpoint it was told to: {tick} of {ticks_held}"
        assert int(tick) != ticks_held[-1], f"{who} restored the newest checkpoint, so the name decided nothing: {ticks_held}"
        assert held[name]["WorldStructureHash"] == expected, (held[name], expected)
        assert restored_tick == tick, f"{who} restored a world standing on tick {restored_tick}, not {tick}"
        assert world_hash == expected, f"{who} did not restore the checkpoint's world: {world_hash} vs {expected}"
        assert policy_flag == "1" and verdict == "PASS", line.group(0)
        details[who] = {"match_id": match_id, "tick": int(tick), "world_hash": world_hash, "captures": captures,
                        "line": line.group(0), "policy": policy.group(0), "held": sorted(held)}
    assert details["host"]["match_id"] == details["client"]["match_id"], (
        f"the peers name different matches: {details['host']['match_id']} vs {details['client']['match_id']}")
    assert details["host"]["tick"] == details["client"]["tick"], (
        f"the peers restored different checkpoints: {details['host']['tick']} vs {details['client']['tick']}")
    return details


def compare_live_window(root: Path, first_tick: int, last_tick: int, away: dict | None = None) -> dict:
    """Require the complete window and compare every replay of every shared tick. `away` names, per peer, the
    inclusive tick ranges it was held and rejoined past on a newer image: the AI played them, the peer never did."""
    peers = {who: read_live_hashes(root / f"{who}-live.jsonl") for who in ("host", "client")}
    required = set(range(first_tick, last_tick + 1))
    by_tick = {}
    for who, rows in peers.items():
        indexed = {}
        for row in rows:
            indexed.setdefault(row["tick"], []).append(row)
        skipped = {tick for low, high in (away or {}).get(who, []) for tick in range(low, high + 1)}
        missing = sorted(required - skipped - indexed.keys())
        assert not missing, f"{who} missing {len(missing)} required ticks: {missing[:8]}"
        by_tick[who] = indexed
    shared = sorted((by_tick["host"].keys() & by_tick["client"].keys()) & set(range(first_tick, max(by_tick["host"]) + 1)))
    mismatches, compared = [], 0
    shared_part = lambda row: {name: value for name, value in row["subsystems"].items() if name not in PER_PEER_SUBSYSTEMS}
    for tick in shared:
        # Every replay of one peer agrees in full; the two peers agree on all but the routing each does for itself.
        for who in ("host", "client"):
            readings = by_tick[who][tick]
            for row in readings[1:]:
                compared += 1
                if row["sim_gated"] != readings[0]["sim_gated"] or row["subsystems"] != readings[0]["subsystems"]:
                    mismatches.append(tick)
        host, client = by_tick["host"][tick][0], by_tick["client"][tick][0]
        compared += 1
        if host["sim_gated"] != client["sim_gated"] or shared_part(host) != shared_part(client):
            mismatches.append(tick)
    assert not mismatches, f"live passes disagree at {len(mismatches)} ticks: {mismatches[:8]}"
    assert max(by_tick["host"]) == max(by_tick["client"]), f"peer tails differ: {max(by_tick['host'])} vs {max(by_tick['client'])}"
    return {"passed": True, "required_ticks": len(required), "compared_readings": compared,
            "shared_ticks": len(shared), "first_tick": first_tick, "last_tick": shared[-1],
            "passes": {who: len(split_passes(rows)) for who, rows in peers.items()}}


def arm_retention(repo: Path, root: Path, port: int) -> dict:
    """More checkpoints than the policy keeps leaves exactly the newest restorable ones, per peer."""
    ticks = 700
    records = run_pair(repo, root, port, ticks, 2, {})
    details = {}
    for who in ("host", "client"):
        assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
        log = peer_log(root, who)
        captures = [int(tick) for tick, _, _ in CAPTURE.findall(log)]
        retained = RETAINED.findall(log)
        assert len(captures) > RETAINED_AUTOSAVES, f"{who} never exceeded the retention limit: {captures}"
        assert retained, f"{who} never reported a retention pass"
        keep = {int(row[1]) for row in retained}
        assert keep == {RETAINED_AUTOSAVES}, f"{who} used a retention count of {keep}"
        held = checkpoints(root, who)
        ticks_held = sorted(int(fields["SavedTick"]) for fields in held.values())
        assert ticks_held == sorted(captures)[-RETAINED_AUTOSAVES:], (ticks_held, captures)
        assert "[autosave] failed" not in log, f"{who} refused a capture or a publish"
        details[who] = {"captures": captures, "held": sorted(held), "retained_lines": len(retained),
                        "descriptors": {name: fields["_descriptor_sha256"] for name, fields in held.items()}}
    assert details["host"]["held"] == details["client"]["held"], (
        "the peers do not hold the same checkpoint names: "
        f"{details['host']['held']} vs {details['client']['held']}")
    # Every field of the descriptor is agreed by the match, so the two peers' copies are the same bytes.
    assert details["host"]["descriptors"] == details["client"]["descriptors"], (
        "the peers' restore descriptors differ: "
        f"{details['host']['descriptors']} vs {details['client']['descriptors']}")
    comparison = compare_live_window(root, 1, ticks)
    details["peer_comparison"] = comparison
    return details


def arm_anchor(repo: Path, root: Path, port: int, pause_slow_peers: bool = False) -> dict:
    """A heal names one rewind point for the whole match, and it survives later rotation.

    The perturbation is timed late on purpose: at the stock tick 50 the heal lands before the first
    checkpoint (one 2 s interval = 120 ticks after the match starts), so the host would have nothing to
    name and the row would be red for a reason that is not the anchor mechanism. Tick 700 puts at least
    four checkpoints before the heal, and the 1400-tick cap leaves room for more than the retention limit
    afterwards, so the named one can only survive by being pinned."""
    ticks, perturb_at = ANCHOR_TICKS, 700
    # The perturbation waits for both seats to be live; a peer the host holds (a sanitizer build's slow client) keeps it
    # from landing, so such a build asks the host to pause for a slow peer instead.
    records = run_pair(repo, root, port, ticks, 2,
                       {"host": ["-net-test-perturb-when-live", "-determinism-selftest-perturb", "-determinism-selftest-perturb-tick", str(perturb_at),
                                 "-net-match-e2e-resync"],
                        "client": ["-net-match-e2e-resync"]},
                       {"host": {"NetworkSlowPlayerPolicy": "Pause"}} if pause_slow_peers else None)
    injection = re.search(r"\[net-test\] live perturb frame=(\d+)", peer_log(root, "host"))
    assert injection and int(injection[1]) >= perturb_at, "the live-peer perturbation was never injected"
    anchors, captures = {}, {}
    for who in ("host", "client"):
        log = peer_log(root, who)
        captures[who] = [int(captured) for captured, _, _ in CAPTURE.findall(log)]
        found = ANCHOR.findall(log)
        assert found, f"{who} recorded no rewind anchor: {root / who / 'stdout.log'}"
        anchors[who] = found
    named = [row for row in anchors["host"] if row[0] == "named"]
    received = [row for row in anchors["client"] if row[0] == "received"]
    assert named, "the host named no rewind point for the heal"
    assert received, "the client recorded no rewind point from the host"
    assert named[-1][1] == received[-1][1] and named[-1][2] == received[-1][2], (
        f"the peers do not agree on the rewind point: host {named[-1][1:3]} vs client {received[-1][1:3]}")
    match_id, tick = named[-1][1], int(named[-1][2])
    assert named[-1][3] == "ok", f"the host cannot restore the checkpoint it named: {named[-1][3]}"
    assert received[-1][3] == "ok", f"the client does not hold the named checkpoint: {received[-1][3]}"
    held = {}
    for who in ("host", "client"):
        before = [captured for captured in captures[who] if captured <= tick]
        after = [captured for captured in captures[who] if captured > tick]
        assert len(before) >= 4, f"{who} healed with only {len(before)} checkpoints behind it: {captures[who]}"
        assert len(after) >= RETAINED_AUTOSAVES, (
            f"{who} wrote only {len(after)} checkpoints after the anchor, so nothing would have rotated it away: {captures[who]}")
        held[who] = checkpoints(root, who)
        assert f"{match_id}-{tick}.ccsave" in held[who], (
            f"{who} rotated away the agreed rewind point {match_id}-{tick}: {sorted(held[who])}")
        log = peer_log(root, who)
        pinned = {int(row[2]) for row in RETAINED.findall(log)}
        assert tick in pinned, f"{who} never pinned the agreed rewind point: {sorted(pinned)}"
    return {"match_id": match_id, "tick": tick, "host": sorted(held["host"]), "client": sorted(held["client"]),
            "captures": captures, "perturbed_tick": int(injection[1]), "anchor_lines": {who: [" ".join(row) for row in anchors[who]] for who in anchors}}


def arm_park(repo: Path, root: Path, port: int) -> dict:
    """At the product's shortest autosave interval (60 s) every capture park commits each player's input: no frame of the
    window is committed empty for the players, and the window is the capture's own cost, not a round trip on a 250 ms seed.
    RED before the change: every park frame's input was erased on every peer, so no peer reports a park frame committed with
    input, and a loopback window ran 16 frames or more against a capture of a few frames."""
    ticks = 4200
    records = run_pair(repo, root, port, ticks, 60, {})
    details = {}
    tick_ms = 1000.0 / 60.0
    for who in ("host", "client"):
        record = records.get(who, {})
        assert "error" not in record and not record.get("timed_out"), (who, record)
        log = peer_log(root, who)
        captures = [float(ms) for _, ms, _ in CAPTURE.findall(log)]
        windows = sorted({(int(start), int(end)) for start, end, _ in PARK_RELEASED.findall(log)})
        assert captures and windows, f"{who} took no capture park in a 70 s match at 60 s autosave: captures={captures} windows={windows}"
        committed = PARK_COMMITTED.findall(log)
        assert committed, f"{who}: no park frame was committed with the players' input; windows={windows}"
        frames, with_input = int(committed[-1][2]), int(committed[-1][3])
        assert frames > 0 and frames == with_input, f"{who}: {frames - with_input} of {frames} park frames were committed empty"
        # Before any park was measured the window is the slow-player bound (3 ticks); after, the slowest capture's frames.
        allowed = max(3, math.ceil(max(captures) / tick_ms))
        wide = [(start, end) for start, end in windows if end - start + 1 > allowed]
        assert not wide, f"{who}: park windows wider than the capture costs: {wide} allowed={allowed} frames (slowest capture {max(captures)} ms)"
        holds = HOLD.findall(log)
        assert not holds, (who, holds)
        details[who] = dict(windows=windows, captures_ms=captures, park_frames=frames, park_frames_with_input=with_input, allowed_frames=allowed)
    return details


def arm_resume(repo: Path, root: Path, port: int) -> dict:
    """A match whose host process is killed is restarted from its own checkpoint, and the client rejoins.

    The kill is the runner's own termination, never a leave: the client keeps its ticket and the host
    keeps every checkpoint and manifest it published. The second round is started with
    -net-resume-match, which reopens the lobby on the checkpoint's restart manifest.

    RED before the change (written, not run): the first round writes no manifest, so nothing lists as
    resumable and the restarted host refuses with "checkpoint refused"; -net-resume-match itself does
    not parse, so the run ends before a lobby exists.
    """
    if FAMILY_LOCK.exists():
        raise RuntimeError(f"engine launch prohibited while {FAMILY_LOCK} exists")
    root.mkdir(parents=True, exist_ok=False)
    first, second = root / "died", root / "resumed"
    first.mkdir(parents=True, exist_ok=False)
    kill_tick, resume_ticks = 400, 600

    # Round one: a two-peer match that keeps checkpoints every simulated second. The host is killed
    # once it has published a checkpoint past the kill tick; the client is left to lose it.
    runs, records = {}, {}
    for who in ("host", "client"):
        args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
                "-net-match-ticks", "1200", "-net-match-input-delay", "3",
                "-net-autosave-seconds", "1", "-net-match-e2e-resync",
                "-net-match-service-preset", "Determinism FeelBaseline", "-net-match-service-module", "UserScenes.rte",
                "-net-live-tick-hashes", str(first / f"{who}-live.jsonl"),
                "-tick-hashes", "-max-ticks", "1200", "-out", str(first / f"{who}_trace.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        args += fullstate_args()
        runs[who] = make_run(repo, args, first / who, 420, env={"CCCP_HEADLESS": "1"})
        stage_baseline(runs[who], 1200)

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive, args=(who,)) for who in ("host", "client")]
    killed = False
    try:
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        deadline = threading.Event()
        for _ in range(4200):  # 420 s at the poll below, the runner's own budget.
            deadline.wait(0.1)
            captures = [int(row[0]) for row in CAPTURE.findall(peer_log(first, "host"))]
            if any(tick >= kill_tick for tick in captures):
                runs["host"].terminate(code=137, reason="host process killed mid-match")
                killed = True
                break
            if not threads[0].is_alive():
                break
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    assert killed, f"the host was never killed: captures {CAPTURE.findall(peer_log(first, 'host'))[:6]}"
    held = {who: checkpoints(first, who) for who in ("host", "client")}
    assert held["host"], "the killed host left no checkpoint to resume from"
    match_id = next(iter(held["host"].values()))["MatchId"]
    manifests = sorted((first / "host" / "runtime/Autosaves").glob(f"{match_id}-*.ccmanifest"))
    assert manifests, f"the host wrote no restart manifest for {match_id}"
    admission = first / "host" / "runtime/Autosaves" / f"{match_id}.admission"
    assert admission.exists(), f"the host wrote no admission file for {match_id}"
    resume_tick = max(int(fields["SavedTick"]) for fields in held["host"].values()
                      if (first / "host" / "runtime/Autosaves" / f"{match_id}-{fields['SavedTick']}.ccmanifest").exists())

    # Round two: the same two machines, each with what it kept - the host's checkpoints and manifest,
    # the client's checkpoints and its ticket. The host restarts the match; the client rejoins.
    second.mkdir(parents=True, exist_ok=False)
    resumed, resumed_records = {}, {}
    for who in ("host", "client"):
        args = ["-net-match-service-e2e", "-net-port", str(port + 2), "-net-match-peers", "2",
                "-net-match-ticks", str(resume_ticks), "-net-match-input-delay", "3",
                "-net-autosave-seconds", "1", "-net-match-e2e-resync",
                "-net-match-service-preset", "Determinism FeelBaseline", "-net-match-service-module", "UserScenes.rte",
                "-net-live-tick-hashes", str(second / f"{who}-live.jsonl"),
                "-tick-hashes", "-max-ticks", str(resume_ticks), "-out", str(second / f"{who}_trace.json")]
        args += ["-net-host", "-net-resume-match", match_id, "-net-resume-tick", str(resume_tick)] if who == "host" else ["-net-join", "127.0.0.1"]
        args += fullstate_args()
        resumed[who] = make_run(repo, args, second / who, 420, env={"CCCP_HEADLESS": "1"})
        stage_baseline(resumed[who], 1200)
        # What a restarted process finds on its own disk: its checkpoints, its manifests, its admission
        # file and its ticket. Copied, never moved: the first round's evidence stays where it was.
        source = first / who / "runtime/Autosaves"
        target = second / who / "runtime/Autosaves"
        if source.exists():
            carry_store_files(source, target)
        if who == "client":
            # ONE peer holds the checkpoint and ONE is streamed it: the host keeps its copy, the client
            # loses the archive the host will resume on, so the round is played across both paths. Two
            # peers that restore different seats or different worlds diverge on the first compared tick.
            for leftover in target.glob(f"{match_id}-{resume_tick}.*"):
                leftover.unlink()
        for name in ("reconnect.ticket", "NetworkIdentity.key"):
            carried = first / who / "runtime/Userdata" / name
            if carried.exists():
                (second / who / "runtime/Userdata").mkdir(parents=True, exist_ok=True)
                shutil.copy2(carried, second / who / "runtime/Userdata" / name)

    def drive_resumed(who: str) -> None:
        try:
            resumed_records[who] = resumed[who].start().finish()
        except Exception as error:
            resumed_records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive_resumed, args=(who,)) for who in ("host", "client")]
    try:
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in resumed.values():
            run.close()

    host_log, client_log = peer_log(second, "host"), peer_log(second, "client")
    resuming = RESUMING.search(host_log)
    assert resuming, "the restarted host never reported which match it was resuming"
    assert resuming[1] == match_id and int(resuming[2]) == resume_tick, (resuming[1], resuming[2], match_id, resume_tick)
    offer = OFFER.search(client_log)
    assert offer, "the client never answered the host's resume offer"
    assert offer[1] == match_id and int(offer[2]) == resume_tick, (offer[1], offer[2])
    held_locally = offer[3] == "held locally"
    # The client's own copy of that checkpoint was removed above, so it must be STREAMED the host's.
    assert not held_locally, "the client answered held for a checkpoint this arm removed from its store"
    assert RECEIVED_LAUNCH.search(client_log), f"the client neither held nor received the checkpoint: {offer[3]}"
    assert HELD_LAUNCH.search(host_log), "the host did not load its own copy of the checkpoint it resumed"
    for who in ("host", "client"):
        assert resumed_records[who].get("exit_code") == 0, (who, resumed_records[who].get("exit_code"), resumed_records[who].get("error"))
        assert not resumed_records[who].get("timed_out"), who
    # The resumed round is lockstep: the two peers' per-tick hashes must agree, tick for tick.
    compared = compare_live_window(second, resume_tick + 1, resume_tick + resume_ticks)
    reached = max(int(row[0]) for row in CAPTURE.findall(host_log)) if CAPTURE.search(host_log) else 0
    return {"match_id": match_id, "kill_tick": kill_tick, "resume_tick": resume_tick,
            "client_held_the_archive": held_locally, "manifests": [path.name for path in manifests],
            "resumed_captures_to": reached, "peer_comparison": compared}


def _world_peer_args(root: Path, who: str, port: int, ticks: int, extra: list, own_ticks: int = 0) -> list:
    """One peer of a persistent world: the host is the dedicated world daemon, the client an ordinary join.
    `own_ticks` is the cap a peer counts from its own first tick; it defaults to `ticks` for a round that starts at 0."""
    args = ["-net-port", str(port), "-net-match-peers", "2", "-net-match-input-delay", "3",
            "-net-autosave-seconds", "1", "-net-match-ticks", str(own_ticks or ticks),
            "-net-live-tick-hashes", str(root / f"{who}-live.jsonl"),
                "-tick-hashes", "-max-ticks", str(ticks), "-out", str(root / f"{who}_trace.json"),
            "-net-match-report", str(root / f"{who}_report.json")]
    if who == "host":
        args = ["-net-dedicated", "-net-persistent-world", *args]
    else:
        args = ["-net-match-service-e2e", "-net-join", "127.0.0.1", *args]
    return args + extra + fullstate_args()


def _carry_world_state(source: Path, who: str, runtime: Path) -> None:
    """What a restarted process finds on its own disk: the world's identity record, its checkpoints,
    its manifests and admission file, and - for the client - the ticket it still holds.

    The copy lands in the runtime the runner has just staged, not in the run directory: `make_run`
    refuses an output directory that already exists, so nothing may be written there beforehand."""
    for relative in ("Autosaves", "Worlds"):
        held = source / who / "runtime" / relative
        if held.exists():
            carry_store_files(held, runtime / relative)
    for name in ("reconnect.ticket", "NetworkIdentity.key", "ParticipantIdentity.key"):
        carried = source / who / "runtime/Userdata" / name
        if carried.exists():
            (runtime / "Userdata").mkdir(parents=True, exist_ok=True)
            shutil.copy2(carried, runtime / "Userdata" / name)


def _world_seated_tick(host_log: str, client_log: str):
    """The initial host roster proves lobby seating without ordering separate process logs."""
    entered = WORLD_LOBBY.search(client_log)
    if entered is None:
        return None
    client_start = WORLD_START.search(client_log)
    local = re.search(r"\| peer(\d+)=[^|\n]*\(team-?\d+,local,ready,ping\d+ms\)", entered[0])
    peer = int(client_start[3]) if client_start else int(local[1]) if local else None
    if peer is None:
        return None
    first_capture = CAPTURE.search(host_log)
    initial_log = host_log[:first_capture.start()] if first_capture else ""
    host_start = WORLD_START.search(initial_log)
    if (host_start and client_start and host_start[1] == client_start[1]
            and host_start[2] == client_start[2] == "1"):
        for snapshot in WORLD_LOBBY.finditer(initial_log):
            if (" is_host=1 " in snapshot[0] and " remote_ready=1 " in snapshot[0]
                    and re.search(rf"\| peer{peer}=[^|\n]*\(team-?\d+,remote,ready,ping\d+ms\)", snapshot[0])):
                return 1
    activations = re.findall(rf"(?m)^\[net-world\] activate peer={peer} at=(\d+)$", host_log)
    return int(activations[-1]) if activations else None


def _world_kill_ready(host_log: str, client_log: str, kill_past: int, published: list[int], ticket_exists: bool) -> bool:
    seated_tick = _world_seated_tick(host_log, client_log)
    return (seated_tick is not None and ticket_exists
            and any(int(row[0]) >= kill_past for row in CAPTURE.findall(host_log))
            and any(tick >= seated_tick for tick in published))


def _run_world_round(repo: Path, root: Path, port: int, ticks: int, extra: dict, kill_past: int = 0,
                     carry=None, own_ticks: int = 0, after_carry=None) -> dict:
    """One round of a persistent world. `carry` is the previous round's root: its world state is
    copied into each staged runtime after the runner prepares it and before the process starts;
    `after_carry(who, runtime)` then edits what that peer finds on its disk."""
    if FAMILY_LOCK.exists():
        raise RuntimeError(f"engine launch prohibited while {FAMILY_LOCK} exists")
    runs, records = {}, {}
    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive, args=(who,)) for who in ("host", "client")]
    killed = False
    try:
        for who in ("host", "client"):
            runs[who] = make_run(repo, _world_peer_args(root, who, port, ticks, extra.get(who, []), own_ticks),
                                 root / who, 420, env={"CCCP_HEADLESS": "1"})
            if carry is not None:
                _carry_world_state(carry, who, Path(runs[who].cwd))
            if after_carry is not None:
                after_carry(who, Path(runs[who].cwd))
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        if kill_past:
            waiter = threading.Event()
            waited_on_return = 0
            sample_wait_began, shared_seen_at, shared_count = None, [], 0
            for _ in range(4200):
                waiter.wait(0.1)
                host_log, client_log = peer_log(root, "host"), peer_log(root, "client")
                if FULLSTATE_EVERY and len(shared := _shared_samples(host_log, client_log)) > shared_count:
                    shared_count = len(shared)
                    shared_seen_at.append(time.monotonic())
                captures = [int(row[0]) for row in CAPTURE.findall(host_log)]
                identity = WORLD_IDENTITY.search(host_log)
                ticket = root / "client/runtime/Userdata/reconnect.ticket"
                published = []
                if identity:
                    store = root / "host/runtime/Autosaves"
                    published = [int(path.stem.rsplit("-", 1)[1]) for path in store.glob(f"{identity[1]}-*.ccmanifest")
                                 if path.with_suffix(".ccsave").exists() and (store / f"{identity[1]}.admission").exists()]
                ready = _world_kill_ready(host_log, client_log, kill_past, published, ticket.exists())
                blocked = None if ready else "the kill conditions (seated client, ticket, a capture past the kill tick, a published autosave)"
                # The kill waits for a held seat to come back, as a player's host would not die mid-rejoin on cue.
                if ready and _seat_mid_return(host_log) and waited_on_return < WORLD_KILL_RETURN_WAIT_POLLS:
                    waited_on_return += 1
                    ready, blocked = False, f"a held seat's return (held at {SEAT_HELD.findall(host_log)[-1]}, waited {waited_on_return / 10} s)"
                # And for the first sample both peers share after it, bounded by the cadence the shared samples keep.
                if ready and FULLSTATE_EVERY and _arrival_unsampled(host_log, client_log):
                    sample_wait_began = time.monotonic() if sample_wait_began is None else sample_wait_began
                    if time.monotonic() - sample_wait_began < sample_wait_bound_s(shared_seen_at):
                        ready = False
                        blocked = f"a shared sample after a seat came in (waited {time.monotonic() - sample_wait_began:.1f} s of {sample_wait_bound_s(shared_seen_at)} s)"
                records["_kill_blocked_by"] = blocked
                if ready:
                    records["_kill_waited_on_return_s"] = waited_on_return / 10
                    records["_kill_waited_on_sample_s"] = 0.0 if sample_wait_began is None else round(time.monotonic() - sample_wait_began, 1)
                    records["_kill_sample_wait_bound_s"] = sample_wait_bound_s(shared_seen_at) if FULLSTATE_EVERY else None
                    runs["host"].terminate(code=137, reason="world host process killed")
                    killed = True
                    records["_kill_capture_tick"] = max(captures)
                    break
                if not threads[0].is_alive():
                    break
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    records["_killed"] = killed
    return records


def world_offers(log: str) -> list[dict]:
    decoder = json.JSONDecoder()
    return [decoder.raw_decode(log[mark.end():])[0] for mark in re.finditer(r"\[net-world\] offer ", log)]


def held_away(log: str) -> list:
    """The ticks a held world client never simulated: from the tick its seat was held at through the image it rejoined on,
    when that image is newer. Its coverage starts again on the image; every tick from there is compared."""
    ranges = []
    for stop, image in re.findall(r"\[net-match\] recovery requested tick=(\d+) [^\n]*PeerHeld:[^\n]*\n(?:[^\n]*\n)*?\[net-match\] bootstrap checkpoint=(\d+) ", log):
        if int(image) >= int(stop):
            ranges.append((int(stop), int(image)))
    return ranges


def _compare_world_round(root: Path, world_id: str, resumed_tick: int, last_tick: int) -> dict:
    """Compare every tick the joiner can simulate, from its agreed checkpoint to the planned end."""
    host_rows = read_live_hashes(root / "host-live.jsonl")
    client_rows = read_live_hashes(root / "client-live.jsonl")
    assert host_rows and client_rows, "the world or joiner recorded no live hashes"
    first_tick = client_rows[0]["tick"]
    assert host_rows[0]["tick"] == resumed_tick + 1, (host_rows[0]["tick"], resumed_tick)
    assert first_tick >= resumed_tick + 1, (first_tick, resumed_tick)
    if first_tick != resumed_tick + 1:
        offers = world_offers(peer_log(root, "host"))
        assert any(offer["world_id"] == world_id and offer["tick"] + 1 == first_tick for offer in offers), \
            f"the joiner's first tick {first_tick} follows no world checkpoint offer"
    assert last_tick - first_tick + 1 >= 100, "the world shared fewer than 100 planned ticks"
    compared = compare_live_window(root, first_tick, last_tick, {"client": held_away(peer_log(root, "client"))})
    compared["joined_first_tick"] = first_tick
    return compared


def _world_checkpoint_state(manifest: str, host_log: str, world_id: str, tick: int, peers: set[int]) -> dict:
    """The manifest must preserve the exact side state handed to this checkpoint's capture."""
    captured = [json.loads(state) for match, saved, state in WORLD_AGREED.findall(host_log)
                if match == world_id and int(saved) == tick]
    assert captured, f"the host never recorded the agreed state of checkpoint {world_id}-{tick}"
    keys = ("ControlOwner = ", "DroppedControlOwner = ", "Applied = ", "TransferUid = ", "Binding = ")
    stored = "".join(line + "\n" for line in manifest.splitlines() if line.startswith(keys))
    assert stored == captured[-1], f"checkpoint {tick} changed its captured agreed state: {stored!r} != {captured[-1]!r}"
    bindings = re.findall(r"(?m)^Binding = (\d+),([0-9a-f]+)$", stored)
    assert len(bindings) == len(peers) and {int(peer) for peer, _ in bindings} == peers, \
        f"checkpoint {tick} lacks its bound peers' agreed bindings: {bindings} expected {sorted(peers)}"
    return {"control_owners": len(re.findall(r"(?m)^ControlOwner = ", stored)),
            "applied_sequences": len(re.findall(r"(?m)^Applied = ", stored)),
            "binding_peers": sorted(peers)}


def arm_world_restart(repo: Path, root: Path, port: int, client_stall: str = "", round_ticks: int = 600,
                      client_lacks_checkpoint: bool = False, rejoin_from_first_capture: bool = False) -> dict:
    """A persistent world host is KILLED and restarted on the same install with the same UUID,
    the seats the checkpoint held, and the client's stored ticket.

    The restart names no match: a world boot resumes its own newest checkpoint by default. The second
    half runs the same restart with -net-world-fresh and requires a NEW round from the scene instead.

    RED before the change (written, not run): a world boot never resolves a resume, so the restarted
    host opens the scene at tick 0 and prints no `[autosave] resuming` line at all; -net-world-fresh
    does not parse, so the fresh half ends before a lobby exists.
    """
    root.mkdir(parents=True, exist_ok=False)
    first, second, fresh = root / "boot1", root / "boot2", root / "fresh"
    first.mkdir(parents=True, exist_ok=False)
    kill_tick = 400

    def stall(start: int, extra: dict) -> dict:
        """The stress lever: the client stalls once past `start`, so its seat is held and has to rejoin."""
        if not client_stall:
            return extra
        tick, milliseconds = (int(part) for part in client_stall.split(":"))
        return {**extra, "client": [*extra.get("client", []), "-net-test-live-stall", f"{start + tick}:{milliseconds}"]}

    first_extra = stall(0, {})
    if rejoin_from_first_capture:
        # The rejoin lever: the world keeps serving its first capture (CC_TEST_WORLD_JOIN_FIRST_IMAGE) and the client is held
        # at tick 124, so it rejoins from the first capture and catches up across everything since it, the old-image rejoin
        # a loaded box produced by chance.
        first_extra = {**first_extra, "client": [*first_extra.get("client", []), *([] if client_stall else ["-net-test-live-stall", "124:300"])]}
        os.environ["CC_TEST_WORLD_JOIN_FIRST_IMAGE"] = "1"
    try:
        records = _run_world_round(repo, first, port, 1200, first_extra, kill_past=kill_tick)
    finally:
        os.environ.pop("CC_TEST_WORLD_JOIN_FIRST_IMAGE", None)
    if rejoin_from_first_capture:
        first_capture = min((int(row[0]) for row in CAPTURE.findall(peer_log(first, "host"))), default=None)
        images = [int(tick) for tick in re.findall(r"^\[net-match\] bootstrap checkpoint=(\d+) ", peer_log(first, "client"), re.MULTILINE)]
        assert images and first_capture is not None and images[0] == first_capture, \
            f"the lever did not rejoin the client from the first capture: capture {first_capture}, images {images}"
        desync = re.findall(r"^\[lockstep\] desync at frame \d+[^\n]*$", peer_log(first, "host"), re.MULTILINE)
        assert not desync, f"the rejoin from capture {first_capture} diverged: {desync[:2]}"
        # Every tick from the image to the last one both peers simulated, the replayed catch-up included, agrees.
        window = first / "lever-window"
        window.mkdir()
        rows = {who: read_live_hashes(first / f"{who}-live.jsonl") for who in ("host", "client")}
        last_shared = min(max(row["tick"] for row in rows["host"]), max(row["tick"] for row in rows["client"]))
        for who, peer_rows in rows.items():
            (window / f"{who}-live.jsonl").write_text("".join(json.dumps(row) + "\n" for row in peer_rows if row["tick"] <= last_shared),
                                                      encoding="utf-8")
        compare_live_window(window, first_capture + 1, last_shared, {"client": held_away(peer_log(first, "client"))})
    assert records["_killed"], (f"the world host was never killed: captures {CAPTURE.findall(peer_log(first, 'host'))[:6]}; "
                                f"the host exited while the kill waited on {records.get('_kill_blocked_by')}")
    identity = WORLD_IDENTITY.findall(peer_log(first, "host"))
    assert identity, "the world host never printed its identity"
    world_id, boot_one, round_one = identity[0][0], int(identity[0][1]), int(identity[0][2])
    held = checkpoints(first, "host")
    assert held, "the killed world left no checkpoint to resume from"
    assert all(fields["MatchId"] == world_id for fields in held.values()), (world_id, sorted(held))
    autosaves = first / "host" / "runtime/Autosaves"
    assert (autosaves / f"{world_id}.admission").exists(), "the killed world left no restart admission file"
    resume_tick = max(int(fields["SavedTick"]) for fields in held.values()
                      if (autosaves / f"{world_id}-{fields['SavedTick']}.ccmanifest").exists())
    manifest = (autosaves / f"{world_id}-{resume_tick}.ccmanifest").read_text(encoding="utf-8")
    assert re.search(r"(?m)^ManifestSchema = 3$", manifest), "the world checkpoint has no ordered manifest schema"
    assert re.search(rf"(?m)^WorldBoot = {boot_one}$", manifest), "the checkpoint manifest names a different boot"
    host_start, client_start = (WORLD_START.search(peer_log(first, who)) for who in ("host", "client"))
    assert host_start and client_start, "the seated peers never reported their lockstep start"
    side_state = _world_checkpoint_state(manifest, peer_log(first, "host"), world_id, resume_tick,
                                         {int(host_start[3]), int(client_start[3])})
    seats_one = _seat_rows(first, "client")
    assert seats_one, "the client never occupied a seat before the host was killed"
    assert (first / "client/runtime/Userdata/reconnect.ticket").is_file(), "the killed world's client kept no reconnect ticket"

    # Boot two: the same install, no -net-resume-match. The world reopens on its own newest checkpoint.
    second.mkdir(parents=True, exist_ok=False)
    resume_end = resume_tick + round_ticks
    # Each peer counts its cap from its own first tick, so the resumed round is given the ticks it runs past the checkpoint.
    def drop_client_copy(who: str, runtime: Path) -> None:
        # The client lost its copy of the checkpoint the world resumes on, so it is streamed the host's archive.
        if who == "client":
            for leftover in (runtime / "Autosaves").glob(f"{world_id}-{resume_tick}.*"):
                leftover.unlink()

    resumed = _run_world_round(repo, second, port + 2, resume_end, stall(resume_tick, {}), carry=first, own_ticks=round_ticks,
                               after_carry=drop_client_copy if client_lacks_checkpoint else None)
    host_log = peer_log(second, "host")
    if client_lacks_checkpoint:
        client_log = peer_log(second, "client")
        offer = OFFER.search(client_log)
        assert offer and offer[3] != "held locally", f"the client held the checkpoint this arm removed: {offer and offer[0]}"
        assert RECEIVED_LAUNCH.search(client_log), "the client was not streamed the host's checkpoint"
    restarted = WORLD_IDENTITY.findall(host_log)
    assert restarted, "the restarted world printed no identity"
    assert restarted[0][0] == world_id, (restarted[0][0], world_id)
    assert int(restarted[0][1]) == boot_one + 1, (restarted[0][1], boot_one)
    assert int(restarted[0][2]) == round_one + 1, (restarted[0][2], round_one)
    resuming = RESUMING.search(host_log)
    assert resuming, "the restarted world never reported which checkpoint it opened on"
    assert resuming[1] == world_id and int(resuming[2]) == resume_tick, (resuming[1], resuming[2], world_id, resume_tick)
    for who in ("host", "client"):
        assert resumed[who].get("exit_code") == 0, (who, resumed[who].get("exit_code"), resumed[who].get("error"))
        assert not resumed[who].get("timed_out"), who
    # The returning player lands on its OWN seat with the units the checkpoint held.
    seats_two = _seat_rows(second, "client")
    assert seats_two, "the rejoining client reported no seat of its own"
    assert seats_one.keys() == seats_two.keys(), (sorted(seats_one), sorted(seats_two))
    for seat, before in seats_one.items():
        assert seats_two[seat]["team"] == before["team"], (seat, seats_two[seat], before)
    resumed_report = json.loads((second / "client_report.json").read_text(encoding="utf-8"))
    reconnect = resumed_report["service"]["reconnect"]
    assert reconnect["client_used_stored_ticket"] and reconnect["client_reclaim_outcome"] == "reclaim_accepted", reconnect
    compared = _compare_world_round(second, world_id, resume_tick, resume_end)
    completed = RETAINED.findall(host_log)
    after_anchor = {int(row[0]) for row in completed if int(row[0]) > resume_tick}
    assert len(after_anchor) > RETAINED_AUTOSAVES, f"the resumed round never rotated past its anchor: {completed}"
    assert completed and all(int(row[1]) == RETAINED_AUTOSAVES and int(row[2]) == resume_tick for row in completed), completed
    held_two = checkpoints(second, "host")
    expected = set(sorted(after_anchor, reverse=True)[:RETAINED_AUTOSAVES]) | {resume_tick}
    assert {int(fields["SavedTick"]) for fields in held_two.values()} == expected, (sorted(held_two), sorted(expected))

    # The fresh flag: the same install and the same checkpoints, a NEW round from the scene.
    fresh.mkdir(parents=True, exist_ok=False)
    fresh_records = _run_world_round(repo, fresh, port + 4, round_ticks, stall(0, {"host": ["-net-world-fresh"]}), carry=second)
    fresh_log = peer_log(fresh, "host")
    assert not RESUMING.search(fresh_log), "a fresh world boot resumed a checkpoint anyway"
    fresh_identity = WORLD_IDENTITY.findall(fresh_log)
    assert fresh_identity and fresh_identity[0][0] == world_id, (fresh_identity, world_id)
    assert int(fresh_identity[0][1]) == boot_one + 2, (fresh_identity[0][1], boot_one)
    assert int(fresh_identity[0][2]) == round_one + 2, (fresh_identity[0][2], round_one)
    fresh_captures = [int(row[0]) for row in CAPTURE.findall(fresh_log)]
    assert fresh_captures, "the fresh world wrote no checkpoint of its own"
    # A round that resumed would never capture below the checkpoint it stood on; a fresh one starts at 0.
    assert min(fresh_captures) < resume_tick, (min(fresh_captures), resume_tick)
    for who in ("host", "client"):
        assert fresh_records[who].get("exit_code") == 0, (who, fresh_records[who].get("exit_code"))
        assert not fresh_records[who].get("timed_out"), who
    fresh_compared = _compare_world_round(fresh, world_id, 0, round_ticks)
    fresh_held = checkpoints(fresh, "host")
    old_rounds = {fields["RoundId"] for fields in held_two.values()}
    assert any(fields["RoundId"] not in old_rounds for fields in fresh_held.values()), \
        "retention discarded every checkpoint of the fresh world in favor of the previous round's higher ticks"
    for fields in fresh_held.values():
        fresh_manifest = (fresh / "host/runtime/Autosaves" / f"{world_id}-{fields['SavedTick']}.ccmanifest").read_text(encoding="utf-8")
        assert re.search(rf"(?m)^WorldBoot = {boot_one + 2}$", fresh_manifest), \
            "retention kept the previous boot ahead of the fresh world's completed checkpoints"
    return {"world_id": world_id, "boot": boot_one, "resume_tick": resume_tick,
            "kill_capture_tick": records["_kill_capture_tick"], "resume_end": resume_end,
            "kill_waited_on_return_s": records["_kill_waited_on_return_s"], "kill_waited_on_sample_s": records["_kill_waited_on_sample_s"],
            "kill_sample_wait_bound_s": records["_kill_sample_wait_bound_s"],
            "manifest_control_owners": side_state["control_owners"], "manifest_applied_sequences": side_state["applied_sequences"],
            "manifest_binding_peers": side_state["binding_peers"],
            "seats": sorted(seats_two), "fresh_first_capture": min(fresh_captures),
            "peer_comparison": compared, "fresh_peer_comparison": fresh_compared,
            "holds": {half.name: len(HOLD.findall(peer_log(half, "host"))) for half in (first, second, fresh)}}


class WorldRestartOracleTests(unittest.TestCase):
    def test_world_offer_has_its_own_record_boundary(self):
        expected = {"world_id": "retained", "tick": 367}
        text = '[autosave] tick=367 graph_[net-world] offer ' + json.dumps(expected) + '\npart=callbacks\n'
        self.assertEqual(world_offers(text), [expected])
        with self.assertRaises(json.JSONDecodeError):
            world_offers('[net-world] offer {"tick":')

    def test_live_window_skips_only_a_held_clients_away_ticks(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = [{"tick": tick, "sim_gated": str(tick), "subsystems": {"actors": str(tick)}} for tick in range(1, 7)]
            client = [row for row in host if row["tick"] not in (3, 4)]
            (root / "host-live.jsonl").write_text("".join(json.dumps(row) + "\n" for row in host))
            (root / "client-live.jsonl").write_text("".join(json.dumps(row) + "\n" for row in client))
            with self.assertRaisesRegex(AssertionError, "client missing 2 required ticks"):
                compare_live_window(root, 1, 6)
            self.assertTrue(compare_live_window(root, 1, 6, {"client": [(3, 4)]})["passed"])
            with self.assertRaisesRegex(AssertionError, "client missing 1 required ticks"):
                compare_live_window(root, 1, 6, {"client": [(3, 3)]})
        log = ("[net-match] recovery requested tick=462 catch_up=0 reason=tick 462 lockstep stopped: PeerHeld:held\n"
               "[net-match] rejoin phase Active -> Connecting\n[net-match] bootstrap checkpoint=522 local_peer=2\n"
               "[net-match] recovery requested tick=73 catch_up=0 reason=tick 73 lockstep stopped: PeerHeld:held\n"
               "[net-match] bootstrap checkpoint=61 local_peer=2\n")
        self.assertEqual(held_away(log), [(462, 522)])

    def test_live_window_checks_earlier_replays_and_missing_ticks(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = [{"tick": tick, "sim_gated": str(tick), "subsystems": {"actors": str(tick)}} for tick in range(1, 4)]
            def write(who, values):
                (root / f"{who}-live.jsonl").write_text("".join(json.dumps(row) + "\n" for row in values))
            write("host", rows); write("client", rows + rows)
            self.assertEqual(compare_live_window(root, 1, 3)["passes"]["client"], 2)
            corrupt = [dict(row) for row in rows]; corrupt[1]["sim_gated"] = "bad"
            write("client", corrupt + rows)
            with self.assertRaisesRegex(AssertionError, "live passes disagree"):
                compare_live_window(root, 1, 3)
            write("client", [rows[0], rows[2]])
            with self.assertRaisesRegex(AssertionError, "missing 1 required ticks"):
                compare_live_window(root, 1, 3)

    def test_live_window_leaves_each_peer_only_its_own_routing(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = [{"tick": tick, "sim_gated": str(tick), "subsystems": {"actors": str(tick), "controller_route": "host"}} for tick in range(1, 4)]
            routed = [dict(row, subsystems=dict(row["subsystems"], controller_route="client")) for row in rows]
            def write(who, values):
                (root / f"{who}-live.jsonl").write_text("".join(json.dumps(row) + "\n" for row in values))
            write("host", rows); write("client", routed + routed)
            self.assertTrue(compare_live_window(root, 1, 3)["passed"])
            # One peer's own replays keep the routing in the compare.
            strayed = [dict(row) for row in routed]
            strayed[1] = dict(strayed[1], subsystems=dict(strayed[1]["subsystems"], controller_route="other"))
            write("client", routed + strayed)
            with self.assertRaisesRegex(AssertionError, "live passes disagree"):
                compare_live_window(root, 1, 3)
            # Across the peers nothing wider than the routing is left out.
            wider = [dict(row) for row in routed]
            wider[1] = dict(wider[1], subsystems=dict(wider[1]["subsystems"], actors="bad"))
            write("client", wider)
            with self.assertRaisesRegex(AssertionError, "live passes disagree"):
                compare_live_window(root, 1, 3)

    HOST_START = "[net-lockstep] start round=2610485712550324653 frame=1 local_peer=1 peers=2 input_delay=3\n"
    CLIENT_START = "[net-lockstep] start round=2610485712550324653 frame=1 local_peer=2 peers=2 input_delay=3\n"
    HOST_LOBBY = ("[net-match-service-e2e] lobby_snapshot: state=Running is_host=1 members=1 local_ready=1 "
                  "remote_ready=1 activity=Persistent World scene=Grasslands mode=pvp-skirmish | peer2=Client(team0,remote,ready,ping0ms)\n")
    CLIENT_LOBBY = ("[net-match-service-e2e] lobby_snapshot: state=Running is_host=0 members=1 local_ready=1 "
                    "remote_ready=0 activity=Persistent World scene=Grasslands mode=pvp-skirmish | peer2=Client(team0,local,ready,ping0ms)\n")
    CAPTURES = '[autosave] tick=61 capture_ms=149.037 bytes=29499529\n[net-world] offer {"tick":61}\n[autosave] tick=421 capture_ms=190.0 bytes=29499529\n'

    def test_a_held_seat_is_mid_return_until_the_host_reclaims_it(self):
        held = "[net-match] hold peer=2 frame=44 AI in control\n"
        self.assertFalse(_seat_mid_return(self.HOST_START))
        self.assertTrue(_seat_mid_return(self.HOST_START + held))
        self.assertFalse(_seat_mid_return(self.HOST_START + held + "[net-match] seat-reclaimed peer=2 frame=300 live_actors=2\n"))

    def test_a_returned_seat_waits_for_the_next_shared_sample(self):
        def sample(tick: int) -> str:
            return f"[fullstate] tick={tick} hash=ef9b7943247e96ec sections=header:258ea10e07185ecb round=17914344590415242276\n"
        held = "[net-match] hold peer=2 frame=44 AI in control\n"
        back = "[net-match] seat-reclaimed peer=2 frame=385 live_actors=1\n"
        host = self.HOST_START + sample(9) + held + "".join(sample(t) for t in range(60, 361, 60)) + back
        client = self.CLIENT_START + sample(9) + back
        self.assertFalse(_arrival_unsampled(self.HOST_START + sample(9), self.CLIENT_START + sample(9)))
        # Mac stream 7, restore-all-3 boot1: back at 385, killed before the next shared sample.
        self.assertFalse(_seat_mid_return(host))
        self.assertTrue(_arrival_unsampled(host, client))
        self.assertTrue(_arrival_unsampled(host + sample(420), client))
        self.assertFalse(_arrival_unsampled(host + sample(420), client + sample(420)))
        admitted = "[net-match] private catch-up complete frame=430\n"
        self.assertTrue(_arrival_unsampled(host + sample(420), client + sample(420) + admitted))
        self.assertFalse(_arrival_unsampled(host + sample(480), client + sample(420) + admitted + sample(480)))

    def test_the_sample_wait_follows_the_shared_cadence(self):
        # green-soak-2 at --saver-delay-ms 2000: each peer coalesces on its own writer, so shared samples came 45 s apart.
        self.assertGreaterEqual(sample_wait_bound_s([0.0, 1.0, 46.0]), 90.0)
        self.assertEqual(sample_wait_bound_s([0.0, 1.0, 2.0]), WORLD_KILL_SAMPLE_WAIT_MIN_S)
        self.assertEqual(sample_wait_bound_s([]), WORLD_KILL_SAMPLE_WAIT_MIN_S)
        self.assertEqual(sample_wait_bound_s([0.0, 400.0]), WORLD_KILL_SAMPLE_WAIT_CAP_S)
        def sample(tick: int) -> str:
            return f"[fullstate] tick={tick} hash=ef9b7943247e96ec sections=header:258ea10e07185ecb round=7\n"
        self.assertEqual(_shared_samples(sample(60) + sample(120), sample(120) + sample(180)), [(7, 120)])

    def test_lobby_join_with_published_offers_can_be_killed(self):
        host = self.HOST_START + self.HOST_LOBBY + self.CAPTURES
        client = self.CLIENT_START + self.CLIENT_LOBBY
        self.assertEqual(_world_seated_tick(host, client), 1)
        self.assertTrue(_world_kill_ready(host, client, 400, [61, 361], True))
        self.assertTrue(_world_kill_ready(host, "client-only output\n" * 100 + client, 400, [61, 361], True))

    def test_late_join_waits_for_its_activation_and_checkpoint(self):
        host = self.HOST_START + self.CAPTURES
        client = self.CLIENT_START.replace("frame=1 ", "frame=360 ") + self.CLIENT_LOBBY
        self.assertFalse(_world_kill_ready(host, client, 400, [361], True))
        self.assertFalse(_world_kill_ready(host + "[net-world] activate peer=3 at=360\n", client, 400, [361], True))
        host += "[net-world] activate peer=2 at=360\n"
        self.assertEqual(_world_seated_tick(host, client), 360)
        self.assertFalse(_world_kill_ready(host, client, 400, [359], True))
        self.assertTrue(_world_kill_ready(host, client, 400, [360], True))

    def test_empty_lobby_and_incomplete_kill_evidence_wait(self):
        host = self.HOST_START + self.HOST_LOBBY + self.CAPTURES
        client = self.CLIENT_START + self.CLIENT_LOBBY
        self.assertFalse(_world_kill_ready(host, self.CLIENT_START, 400, [361], True))
        self.assertFalse(_world_kill_ready(host, client, 400, [361], False))
        self.assertFalse(_world_kill_ready(host, client, 400, [], True))
        self.assertFalse(_world_kill_ready(host, client, 422, [361], True))
        self.assertFalse(_world_kill_ready(self.HOST_START + self.CAPTURES + self.HOST_LOBBY, client, 400, [361], True))
        self.assertFalse(_world_kill_ready(host.replace("peer2=Client", "peer3=Client"), client, 400, [361], True))

    def test_checkpoint_preserves_bindings_without_sequenced_commands(self):
        state = ("TransferUid = 0\n"
                 "Binding = 1,43434c3316001000020000004d00000001000000b10400000000000001000d000001070100008180400000000000000000000000000000000000000000000000000000000000000000000000000000000000ad69cbb3ec4e3a24000000\n"
                 "Binding = 2,43434c3316001000020000004f00000002000000b10400000000000001000d0000010701008180408180400080634400403b4400005c443f773b4400005c443f773b44000000000000000000005c449648344400ad69cbb3ec4e3a24000000\n")
        world_id = "eee0a897-5451-4f26-89b8-dd86f21b9760"
        host = f"[autosave] agreed match={world_id} tick=1201 state={json.dumps(state)}\n"
        manifest = "ManifestSchema = 3\nWorldBoot = 1\nSavedTick = 1201\n" + state
        checked = _world_checkpoint_state(manifest, host, world_id, 1201, {1, 2})
        self.assertEqual(checked, {"control_owners": 0, "applied_sequences": 0, "binding_peers": [1, 2]})
        with self.assertRaisesRegex(AssertionError, "changed its captured agreed state"):
            _world_checkpoint_state(manifest.replace("Binding = 2,", "Binding = 3,"), host, world_id, 1201, {1, 2})
        with self.assertRaisesRegex(AssertionError, "never recorded"):
            _world_checkpoint_state(manifest, host, world_id, 1200, {1, 2})
        with self.assertRaisesRegex(AssertionError, "never recorded"):
            _world_checkpoint_state(manifest, host, "another-world", 1201, {1, 2})

    def test_checkpoint_refuses_lost_or_changed_side_state(self):
        state = "ControlOwner = 7,2\nDroppedControlOwner = 8,2\nApplied = 2,10\nTransferUid = 7\nBinding = 1,aa\nBinding = 2,bb\n"
        host = f"[autosave] agreed match=world tick=421 state={json.dumps(state)}\n"
        self.assertEqual(_world_checkpoint_state(state, host, "world", 421, {1, 2})["applied_sequences"], 1)
        for line in state.splitlines(keepends=True):
            with self.subTest(line=line), self.assertRaisesRegex(AssertionError, "changed its captured agreed state"):
                _world_checkpoint_state(state.replace(line, ""), host, "world", 421, {1, 2})
        with self.assertRaisesRegex(AssertionError, "changed its captured agreed state"):
            _world_checkpoint_state(state.replace("Applied = 2,10", "Applied = 2,9"), host, "world", 421, {1, 2})
        empty = "TransferUid = 0\n"
        with self.assertRaisesRegex(AssertionError, "lacks its bound peers"):
            _world_checkpoint_state(empty, f"[autosave] agreed match=world tick=421 state={json.dumps(empty)}\n", "world", 421, {1, 2})

    def test_injected_divergence_is_one_sample_then_a_proven_heal(self):
        """The anchor arm's full-state verdict from the peers' own logs: one divergent sample at the injected tick, every
        later scheduled sample on both peers, no shared section dropped without a reason, and each peer's first restored
        world equal to the canonical snapshot. RED before the change: every failing case below passed."""
        import tempfile
        base = {"header": "1" * 16, "globals.sim_rng": "2" * 16, "scene": "3" * 16, "graph.3": "4" * 16}

        def line(tag, tick, sections, round_id):
            return f"[{tag}] tick={tick} hash={'e' * 16} sections={','.join(f'{name}:{value}' for name, value in sections.items())} round={round_id}\n"

        def logs(client_last=1380, dropped_from=0, named=False, restored=None, canonical=True, canonical_own=None, absent=None, coalesced=None,
                 client_hold=None):
            texts = {"host": "[net-test] live perturb frame=720\n", "client": ""}
            for who, replaced in (coalesced or {}).items():
                texts[who] += "".join(f"[fullstate-coalesced] tick={tick + 60} replaced={tick} writing={tick - 60} waiting_bound=1\n" for tick in replaced)
            if client_hold:
                texts["client"] += (f"[net-lockstep] start round=8 frame=721 local_peer=2 peers=2 input_delay=4\n"
                                    f"[net-lockstep] hold of this seat at {client_hold[0]} revision=9 incarnation=1 state=Running\n"
                                    f"[net-match] seat-reclaimed peer=2 frame={client_hold[1]} live_actors=2\n")
            for who in texts:
                for tick in range(60, 1381, 60):
                    if who == "client" and tick > client_last:
                        break
                    if tick in (absent or {}).get(who, ()):
                        continue
                    round_id = 7 if tick <= 720 else 8
                    sections = dict(base)
                    if who == "host" and tick == 720:
                        sections.update({"globals.sim_rng": "5" * 16, "scene": "6" * 16})
                    if dropped_from and tick >= dropped_from:
                        del sections["graph.3"]
                    texts[who] += line("fullstate", tick, sections, round_id)
                    if named and dropped_from and tick >= dropped_from:
                        texts[who] += f"[fullstate-scope] tick={tick} round={round_id} label=sample per_peer=graph.3\n"
                if who == "host" and canonical:
                    kept = {name: value for name, value in base.items() if canonical_own is None or name != "graph.3"}
                    texts[who] += line("fullstate-canonical", 725, kept, 7)
                    if canonical_own:
                        texts[who] += f"[fullstate-scope] tick=725 round=7 label=canonical per_peer={canonical_own}" + chr(10)
                texts[who] += line("fullstate-restored", 725, restored or base, 8)
            return texts

        cases = {
            "a proven heal": (logs(), True),
            "a section named this machine's own": (logs(dropped_from=900, named=True), True),
            "no later client samples": (logs(client_last=780), False),
            "a section dropped with no reason": (logs(dropped_from=900), False),
            "both peers restore the same wrong state": (logs(restored={**base, "scene": "9" * 16}), False),
            "no canonical snapshot": (logs(canonical=False), False),
            "a section the canonical capture keeps for itself": (logs(canonical_own="graph.3"), True),
            "a section the canonical capture drops unnamed": (logs(canonical_own=""), False),
            # EDITH S1 restore-all on merge 254: the slow saver coalesced samples after the heal on each peer.
            "samples each peer's own saver coalesced after the heal": (
                logs(absent={"host": {840, 1020}, "client": {840, 960}}, coalesced={"host": [840, 1020], "client": [840, 960]}), True),
            "an absence no line of the peer names": (logs(absent={"client": {1200}}), False),
            "a coalesced line of the other peer excuses nothing": (logs(absent={"client": {1200}}, coalesced={"host": [1200]}), False),
            "a held peer's samples inside its own hold": (logs(absent={"client": {1200}}, client_hold=(1147, 1213)), True),
            "a sample after the hold's return": (logs(absent={"client": {1260}}, client_hold=(1147, 1213)), False),
            "no sample both peers took after the heal": (
                logs(absent={"host": set(range(780, 1381, 120)), "client": set(range(840, 1381, 120))},
                     coalesced={"host": list(range(780, 1381, 120)), "client": list(range(840, 1381, 120))}), False),
        }
        global FULLSTATE_EVERY
        every = FULLSTATE_EVERY
        FULLSTATE_EVERY = 60
        try:
            for name, (texts, passes) in cases.items():
                with self.subTest(name), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    for who, text in texts.items():
                        (root / who).mkdir()
                        (root / who / "stdout.log").write_text(text, encoding="utf-8")
                    verdicts = fullstate_pairs(root)
                    expect_injected_divergence(root, verdicts)
                    self.assertEqual(verdicts["."]["passed"], passes, verdicts["."]["reasons"])
        finally:
            FULLSTATE_EVERY = every

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48720)
    parser.add_argument("--arm", choices=("all", "restore", "retention", "anchor", "resume", "world-restart", "park"), default="all",
                        help="all runs every arm but park; park is the 60 s autosave input-keeping run")
    parser.add_argument("--round-ticks", type=int, default=600, help="world-restart only: the resumed and fresh rounds' length")
    parser.add_argument("--client-stall", default="", help="world-restart only: TICK:MS, the client stalls once past each "
                        "round's start so the host holds its seat and the seat has to rejoin")
    parser.add_argument("--fullstate-every", type=int, default=0,
                        help="every N committed ticks both peers hash their whole capture (-net-fullstate-hash-every); 0 is off")
    parser.add_argument("--client-lacks-checkpoint", action="store_true",
                        help="world-restart only: the client loses its copy of the resume checkpoint and is streamed the host's")
    parser.add_argument("--rejoin-from-first-capture", action="store_true",
                        help="world-restart only: the first boot's world keeps serving its first capture and the client is held at "
                        "tick 124, so its rejoin takes the first capture and catches up across everything since it")
    parser.add_argument("--pause-slow-peers", action="store_true",
                        help="anchor only: the host pauses for a slow peer instead of holding it, so a sanitizer build's heal lands")
    args = parser.parse_args()
    if args.fullstate_every < 0:
        parser.error("--fullstate-every must be 0 or positive")
    global FULLSTATE_EVERY
    FULLSTATE_EVERY = args.fullstate_every
    if not 1024 <= args.port <= 65516:
        parser.error("the base port must leave room for twenty unprivileged ports")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    with engine_executable(repo).open("rb") as exe:
        exe_sha = file_sha256(exe)
    result = {"exe_sha256": exe_sha, "arms": {}}
    arms = {"restore": arm_restore, "retention": arm_retention, "anchor": lambda repo, root, port: arm_anchor(repo, root, port, args.pause_slow_peers), "resume": arm_resume,
            "world-restart": lambda repo, root, port: arm_world_restart(repo, root, port, args.client_stall, args.round_ticks,
                                                                        args.client_lacks_checkpoint, args.rejoin_from_first_capture),
            "park": arm_park}
    if args.arm != "all":
        arms = {args.arm: arms[args.arm]}
    else:
        arms.pop("park")
    for index, (arm, run) in enumerate(arms.items()):
        details = {}
        result["arms"][arm] = details
        # The resume and world-restart arms run several rounds of two peers, each on its own ports.
        armPort = {"resume": args.port + 5, "world-restart": args.port + 10, "park": args.port + 16}.get(arm, args.port + index)
        try:
            details.update(run(repo, root / arm, armPort), passed=True)
            print(f"PASS {arm}", flush=True)
        except Exception as error:
            details.update(passed=False, error=str(error))
            print(f"FAIL {arm}: {error}", flush=True)
        if FULLSTATE_EVERY:
            details["fullstate"] = fullstate_pairs(root / arm, admissions=arm == "world-restart")
            if arm == "anchor":
                expect_injected_divergence(root / arm, details["fullstate"])
            tripped = [name for name, verdict in details["fullstate"].items() if not verdict["passed"]]
            if tripped or not details["fullstate"]:
                details["passed"] = False
                print(f"FAIL {arm} full state: " + "; ".join(f"{name}: {'; '.join(details['fullstate'][name]['reasons'])}" for name in tripped)
                      if tripped else f"FAIL {arm} full state: no two-peer round was sampled", flush=True)
    (root / "result.json").write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
    return 0 if all(arm["passed"] for arm in result["arms"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
