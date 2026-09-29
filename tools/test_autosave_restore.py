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
import ast
import hashlib
import json
import math
import os
import re
import shutil
import tempfile
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
# Both peers' archive writers pause this long before each task (CC_TEST_SAVER_DELAY_MS); 0 is off. Set by --saver-delay-ms.
SAVER_DELAY_MS = 0
# The client's archive writer alone pauses this long before each task, so the host's newest checkpoint is one the client has
# not published yet; overrides SAVER_DELAY_MS on the client. Set by --client-saver-delay-ms.
CLIENT_SAVER_DELAY_MS = 0
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


def captures_text(log: str, limit: int = 6) -> str:
    """The first captures as 'tick N: M ms, B bytes', so a capture's cost is never read in the wrong unit."""
    return ", ".join(f"tick {tick}: {ms} ms, {size} bytes" for tick, ms, size in CAPTURE.findall(log)[:limit]) or "none"


TAIL_LINE = re.compile(r"^\[net-world\] tail peer=(\d+) .*?delivered=(\d+) acknowledged=(\d+) rtt_ms=(\S+)", re.MULTILINE)
GATE_LINE = re.compile(r"^\[net-world\] catch-up gate peer=(\d+) gate=(\S+) applied=(\d+) acknowledged=\d+ horizon=(\d+)", re.MULTILINE)
OWN_SEAT_HELD = re.compile(r"^\[net-match\] hold peer=\d+ frame=(\d+) AI in control \(the host's own seat", re.MULTILINE)


def return_progress(host_log: str) -> str:
    """What the host last saw of a returning seat's catch-up, and whether its own seat was held: why a return never came."""
    parts = []
    if tails := TAIL_LINE.findall(host_log):
        peer, delivered, acknowledged, rtt = tails[-1]
        parts.append(f"tail to peer {peer} delivered {delivered}, acknowledged {acknowledged}, rtt {rtt} ms")
    gates = GATE_LINE.findall(host_log)
    if gates:
        peer, gate, applied, horizon = gates[-1]
        parts.append(f"last catch-up report from peer {peer}: gate {gate}, applied {applied} at horizon {horizon}")
    elif tails:
        parts.append("no catch-up report reached the host")
    if own := OWN_SEAT_HELD.findall(host_log):
        parts.append(f"the host's own seat was held at {own[0]}, so it named no capture while held")
    return "; ".join(parts) or "no tail was sent"


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
    """Holds the anchor arm's full-state verdict to injected_divergence_reasons, run to each pair's own tick count."""
    for name, verdict in verdicts.items():
        pair = Path(root) / name
        excused = []
        last_tick = run_ticks(pair, ANCHOR_TICKS)
        reasons = injected_divergence_reasons(pair / "host" / "stdout.log", pair / "client" / "stdout.log",
                                              verdict.get("divergences") or [], FULLSTATE_EVERY, last_tick, excused)
        verdict["injected_divergence"] = {"requires": "globals.sim_rng", "every": FULLSTATE_EVERY, "last_tick": last_tick,
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
        text, count = re.subn(rf"(?m)^([ \t]*{name}[ \t]*=[ \t]*)[^\r\n]*", lambda match: match[1] + value, text)
        if count == 0:
            text += f"\n\t{name} = {value}\n"
    path.write_text(text, encoding="utf-8")


def saver_delay_ms(who: str) -> int:
    """The writer lever each peer gets: the client's own delay, when set, instead of both peers' one."""
    return CLIENT_SAVER_DELAY_MS if who == "client" and CLIENT_SAVER_DELAY_MS else SAVER_DELAY_MS


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
        env = {"CCCP_HEADLESS": "1"}
        if delay := saver_delay_ms(who):
            env["CC_TEST_SAVER_DELAY_MS"] = str(delay)
        runs[who] = make_run(repo, args, root / who, 420, env=env)
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


AUTOSAVE_COALESCED = re.compile(r"^\[autosave-coalesced\] tick=\d+ replaced=(\d+) ", re.MULTILINE)
# A capture the match named that this peer did not take (it was catching up on a held seat).
AUTOSAVE_NOT_TAKEN = re.compile(r"^\[autosave\] named tick=(\d+) not taken: ", re.MULTILINE)
# An arm that needs N checkpoints written waits for them: a run that writes fewer (the saver is slower than the 2 s cadence,
# and the host names no capture while the last one is still being written) runs again with its whole schedule doubled,
# up to this many ticks. N is never lowered.
CHECKPOINT_WAIT_CAP_TICKS = 5600


class CheckpointsShort(AssertionError):
    """A run wrote fewer checkpoints than its arm needs while every other statement of the arm held."""

    def __init__(self, message: str, record: dict):
        super().__init__(message)
        self.record = record


def landed_checkpoints(log: str) -> tuple[list, int]:
    """The ticks of the autosaves a peer's log shows written, and how many captures its saver coalesced: a capture the
    engine's own [autosave-coalesced] line names as replaced was asked for and never written."""
    replaced = Counter(int(tick) for tick in AUTOSAVE_COALESCED.findall(log))
    coalesced = sum(replaced.values())
    landed = []
    for tick, _, _ in CAPTURE.findall(log):
        if replaced[int(tick)]:
            replaced[int(tick)] -= 1
        else:
            landed.append(int(tick))
    return landed, coalesced


def checkpoint_record(root: Path, ticks: int) -> dict:
    """What one run wrote per peer and how many captures each peer's writers coalesced."""
    record = {"ticks": ticks, "run": str(root)}
    for who in ("host", "client"):
        log = peer_log(root, who)
        landed, coalesced = landed_checkpoints(log)
        record[who] = {"landed": landed, "autosave_coalesced": coalesced, "fullstate_coalesced": len(FULLSTATE_COALESCED.findall(log)),
                       "not_taken": [int(tick) for tick in AUTOSAVE_NOT_TAKEN.findall(log)]}
    return record


def describe_checkpoints(record: dict) -> str:
    return f"ticks={record['ticks']} " + ", ".join(
        f"{who} wrote {len(record[who]['landed'])} (autosave coalesced {record[who]['autosave_coalesced']}, "
        f"full-state coalesced {record[who]['fullstate_coalesced']}, not taken {len(record[who].get('not_taken', []))})"
        for who in ("host", "client"))


def unexplained_missing(root: Path, record: dict) -> list:
    """Each checkpoint one peer wrote and the other did not, unless the other's own line names that capture as not written."""
    missing = []
    for who, other in (("host", "client"), ("client", "host")):
        log = peer_log(root, who)
        named = {int(tick) for tick in AUTOSAVE_NOT_TAKEN.findall(log)} | {int(tick) for tick in AUTOSAVE_COALESCED.findall(log)}
        missing += [f"{who} lacks {tick}" for tick in sorted(set(record[other]["landed"]) - set(record[who]["landed"])) if tick not in named]
    return missing


def wait_for_checkpoints(attempt, base_ticks: int, cap_ticks: int = CHECKPOINT_WAIT_CAP_TICKS) -> dict:
    """Runs attempt(scale, ticks) with the arm's schedule scaled 1, 2, 4... until a run writes the checkpoints the arm needs.
    A run that would pass cap_ticks is never started; every statement but the count fails the arm at once."""
    short_runs, scale = [], 1
    while True:
        try:
            details = attempt(scale, base_ticks * scale)
        except CheckpointsShort as short:
            short_runs.append(dict(short.record, short_of=str(short)))
            if base_ticks * scale * 2 > cap_ticks:
                raise AssertionError(f"{short}; still short at the {cap_ticks}-tick cap: "
                                     + "; ".join(describe_checkpoints(run) for run in short_runs)) from None
            scale *= 2
            continue
        details["checkpoint_wait"] = {"cap_ticks": cap_ticks, "scale": scale, "short_runs": short_runs,
                                      "written": describe_checkpoints(details["checkpoints"])}
        return details


def wait_root(root: Path, scale: int, ticks: int) -> Path:
    return root if scale == 1 else root / f"wait-{ticks}"


def run_ticks(pair: Path, default: int) -> int:
    """The -net-match-ticks a pair's host was launched with, read from its runner record."""
    try:
        argv = json.loads((Path(pair) / "host" / "launch.json").read_text(encoding="utf-8"))["argv"]
        argv = ast.literal_eval(argv) if isinstance(argv, str) else argv
        return int(argv[argv.index("-net-match-ticks") + 1])
    except (OSError, ValueError, KeyError, IndexError, SyntaxError, TypeError):
        return default


def arm_restore(repo: Path, root: Path, port: int, client_stall: str = "") -> dict:
    """A restore of the checkpoint the caller names reproduces the world that checkpoint recorded, and
    both peers restore the same one. The named checkpoint is the OLDEST of the retained set, so a restore
    that ignored the name and took the store's own pick would be red here. The two peers' world digests
    are NOT compared against each other: a checkpoint carries per-peer locals, so each peer is only held
    to the world its own checkpoint recorded."""
    # With two checkpoints the policy self-test cannot tell its own sub-results apart, so the restore fires
    # well past the retention limit: at a 2 s cadence (120 ticks) at least four stand behind tick 700; a run
    # that writes fewer waits with its schedule doubled, and the row asserts the count it actually observed.
    def attempt(scale: int, ticks: int) -> dict:
        run_root, restore_at = wait_root(root, scale, ticks), 700 * scale
        extra = {who: ["-net-autosave-restore", "oldest", "-net-autosave-restore-at", str(restore_at)]
                 for who in ("host", "client")}
        extra['client'] += stall_args(client_stall, scale)
        records = run_pair(repo, run_root, port, ticks, 2, extra)
        evidence = forced_hold_evidence(run_root, client_stall, scale)
        return dict(judge_restore(run_root, ticks, records), forced_hold=evidence)
    return wait_for_checkpoints(attempt, 800)


def judge_restore(root: Path, ticks: int, records: dict) -> dict:
    details, ticks_held = {}, {}
    for who in ("host", "client"):
        assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
        log = peer_log(root, who)
        line = RESTORE.search(log)
        assert line, f"{who} never reported a restore check: {root / who / 'stdout.log'}"
        policy = POLICY.search(log)
        assert policy and policy[1] == "PASS", f"{who} failed the checkpoint policy self-test: {policy[2] if policy else 'missing'}"
        verdict, match_id, tick, restored_tick, world_hash, expected, policy_flag = line.groups()
        captures, _ = landed_checkpoints(log)
        before = [captured for captured in captures if captured <= int(tick)]
        assert len(before) >= 1, f"{who} restored a checkpoint it never captured: {tick} of {captures}"
        held = checkpoints(root, who)
        name = f"{match_id}-{tick}.ccsave"
        assert name in held, (name, sorted(held))
        ticks_held[who] = sorted(int(fields["SavedTick"]) for fields in held.values())
        assert held[name]["WorldStructureHash"] == expected, (held[name], expected)
        assert restored_tick == tick, f"{who} restored a world standing on tick {restored_tick}, not {tick}"
        assert world_hash == expected, f"{who} did not restore the checkpoint's world: {world_hash} vs {expected}"
        assert policy_flag == "1" and verdict == "PASS", line.group(0)
        details[who] = {"match_id": match_id, "tick": int(tick), "world_hash": world_hash, "captures": captures,
                        "line": line.group(0), "policy": policy.group(0), "held": sorted(held)}
    assert details["host"]["match_id"] == details["client"]["match_id"], (
        f"the peers name different matches: {details['host']['match_id']} vs {details['client']['match_id']}")
    details["checkpoints"] = record = checkpoint_record(root, ticks)
    for who in ("host", "client"):
        captures = details[who]["captures"]
        if len(captures) <= RETAINED_AUTOSAVES:
            # A short run's peers may differ only by the captures a peer's own lines name as not written.
            unexplained = unexplained_missing(root, record)
            assert not unexplained, f"the peers wrote different checkpoints with no line naming the missing ones: {unexplained}"
            raise CheckpointsShort(f"{who} restored before the retention limit was exceeded: {captures}", record)
    assert details["host"]["tick"] == details["client"]["tick"], (
        f"the peers restored different checkpoints: {details['host']['tick']} vs {details['client']['tick']} "
        f"(not taken: host {record['host']['not_taken']}, client {record['client']['not_taken']})")
    for who in ("host", "client"):
        tick, captures = details[who]["tick"], details[who]["captures"]
        assert len(ticks_held[who]) == RETAINED_AUTOSAVES, (ticks_held[who], captures)
        assert tick == ticks_held[who][0], f"{who} did not restore the checkpoint it was told to: {tick} of {ticks_held[who]}"
        assert tick != ticks_held[who][-1], f"{who} restored the newest checkpoint, so the name decided nothing: {ticks_held[who]}"
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


def arm_retention(repo: Path, root: Path, port: int, client_stall: str = "") -> dict:
    """More checkpoints than the policy keeps leaves exactly the newest restorable ones, per peer."""
    def attempt(scale: int, ticks: int) -> dict:
        run_root = wait_root(root, scale, ticks)
        records = run_pair(repo, run_root, port, ticks, 2, {'client': stall_args(client_stall, scale)})
        evidence = forced_hold_evidence(run_root, client_stall, scale)
        return dict(judge_retention(run_root, ticks, records), forced_hold=evidence)
    return wait_for_checkpoints(attempt, 700)


def judge_retention(root: Path, ticks: int, records: dict) -> dict:
    details = {}
    for who in ("host", "client"):
        assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
        log = peer_log(root, who)
        captures, _ = landed_checkpoints(log)
        retained = RETAINED.findall(log)
        assert retained, f"{who} never reported a retention pass"
        keep = {int(row[1]) for row in retained}
        assert keep == {RETAINED_AUTOSAVES}, f"{who} used a retention count of {keep}"
        held = checkpoints(root, who)
        ticks_held = sorted(int(fields["SavedTick"]) for fields in held.values())
        assert ticks_held == sorted(captures)[-RETAINED_AUTOSAVES:], (ticks_held, captures)
        assert "[autosave] failed" not in log, f"{who} refused a capture or a publish"
        details[who] = {"captures": captures, "held": sorted(held), "retained_lines": len(retained),
                        "descriptors": {name: fields["_descriptor_sha256"] for name, fields in held.items()}}
    comparison = compare_live_window(root, 1, ticks)
    details["peer_comparison"] = comparison
    shared = details["host"]["descriptors"].keys() & details["client"]["descriptors"].keys()
    differ = sorted(name for name in shared if details["host"]["descriptors"][name] != details["client"]["descriptors"][name])
    assert not differ, f"the peers' restore descriptors differ: {differ}"
    details["checkpoints"] = record = checkpoint_record(root, ticks)
    for who in ("host", "client"):
        if len(details[who]["captures"]) <= RETAINED_AUTOSAVES:
            # A short run's peers may differ only by the captures a peer's own lines name as not written.
            unexplained = unexplained_missing(root, record)
            assert not unexplained, f"the peers wrote different checkpoints with no line naming the missing ones: {unexplained}"
            raise CheckpointsShort(f"{who} never exceeded the retention limit: {details[who]['captures']}", record)
    assert details["host"]["held"] == details["client"]["held"], (
        "the peers do not hold the same checkpoint names: "
        f"{details['host']['held']} vs {details['client']['held']} (not taken: host {record['host']['not_taken']}, client {record['client']['not_taken']})")
    # Every field of the descriptor is agreed by the match, so the two peers' copies are the same bytes.
    assert details["host"]["descriptors"] == details["client"]["descriptors"], (
        "the peers' restore descriptors differ: "
        f"{details['host']['descriptors']} vs {details['client']['descriptors']}")
    return details


def arm_anchor(repo: Path, root: Path, port: int, pause_slow_peers: bool = False, client_stall: str = "") -> dict:
    """A heal names one rewind point for the whole match, and it survives later rotation.

    The perturbation is timed late on purpose: at the stock tick 50 the heal lands before the first
    checkpoint (one 2 s interval = 120 ticks after the match starts), so the host would have nothing to
    name and the row would be red for a reason that is not the anchor mechanism. Tick 720 puts at least
    four checkpoints before the heal at the 2 s cadence, and the 1400-tick cap leaves room for more than the
    retention limit afterwards, so the named one can only survive by being pinned; a run that writes fewer
    waits with its schedule doubled. `client_stall` (TICK:MS) holds the client once, so it catches up across a capture it
    never takes; the anchor must still be one the client holds."""
    def attempt(scale: int, ticks: int) -> dict:
        # The live perturbation fires on a multiple of 30 ticks; 720 keeps every scaled run's on a full-state sample tick.
        run_root, perturb_at = wait_root(root, scale, ticks), 720 * scale
        # The perturbation waits for both seats to be live; a peer the host holds (a sanitizer build's slow client) keeps it
        # from landing, so such a build asks the host to pause for a slow peer instead.
        run_pair(repo, run_root, port, ticks, 2,
                 {"host": ["-net-test-perturb-when-live", "-determinism-selftest-perturb", "-determinism-selftest-perturb-tick", str(perturb_at),
                           "-net-match-e2e-resync"],
                  "client": ["-net-match-e2e-resync", *stall_args(client_stall, scale)]},
                 {"host": {"NetworkSlowPlayerPolicy": "Pause"}} if pause_slow_peers else None)
        evidence = forced_hold_evidence(run_root, client_stall, scale)
        return dict(judge_anchor(run_root, ticks, perturb_at), forced_hold=evidence)
    return wait_for_checkpoints(attempt, ANCHOR_TICKS)


def stall_args(client_stall: str, scale: int = 1) -> list[str]:
    """The client's one live stall, TICK:MS, its tick scaled with the run's schedule."""
    if not client_stall:
        return []
    tick, milliseconds = (int(part) for part in client_stall.split(":"))
    return ["-net-test-live-stall", f"{tick * scale}:{milliseconds}"]


def forced_hold_evidence(root: Path, lever: str, scale: int = 1, offset: int = 0) -> dict:
    """A requested stall is evidence only after its native hold and completed reclaim."""
    if not lever:
        return {'requested': False}
    tick, milliseconds = map(int, lever.split(':'))
    tick = tick * scale + offset
    host, client = peer_log(root, 'host'), peer_log(root, 'client')
    assert re.search(rf'\[net-test\] live stall frame={tick}\b', client), f'client stall {tick}:{milliseconds} never fired'
    identity = re.search(r'\[net-lockstep\] start [^\n]*local_peer=(\d+)', client)
    assert identity, 'client native seat identity is absent'
    seat = int(identity[1])
    holds = sorted(set(map(int, re.findall(rf'\[net-match\] hold peer={seat} frame=(\d+) AI in control', host))))
    reclaims = sorted(set(map(int, re.findall(rf'\[net-match\] seat-reclaimed peer={seat} frame=(\d+)', host))))
    # A seat returns privately, or in a persistent world through the world-join tail, which names the seat it activated.
    completed = set(map(int, re.findall(r'\[net-match\] private catch-up complete frame=(\d+)', client)))
    completed |= set(map(int, re.findall(rf'\[net-world\] catch-up complete peer={seat} at=(\d+)', client)))
    pairs = [(hold, back) for hold in holds if hold >= tick for back in reclaims if back > hold and back in completed]
    assert pairs, f'client stall {tick} has no native hold/completed reclaim: holds={holds}, reclaims={reclaims}'
    return dict(requested=True, stall_tick=tick, seat=seat, hold=pairs[0][0], reclaim=pairs[0][1])


def judge_anchor(root: Path, ticks: int, perturb_at: int) -> dict:
    injection = re.search(r"\[net-test\] live perturb frame=(\d+)", peer_log(root, "host"))
    assert injection and int(injection[1]) >= perturb_at, "the live-peer perturbation was never injected"
    anchors, captures = {}, {}
    for who in ("host", "client"):
        log = peer_log(root, who)
        captures[who], _ = landed_checkpoints(log)
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
        held[who] = checkpoints(root, who)
        assert f"{match_id}-{tick}.ccsave" in held[who], (
            f"{who} rotated away the agreed rewind point {match_id}-{tick}: {sorted(held[who])}")
        # The pin is read by the retention pass of the next checkpoint written, so it shows only once one follows the anchor.
        if any(captured > tick for captured in captures[who]):
            pinned = {int(row[2]) for row in RETAINED.findall(peer_log(root, who))}
            assert tick in pinned, f"{who} never pinned the agreed rewind point: {sorted(pinned)}"
    record = checkpoint_record(root, ticks)
    for who in ("host", "client"):
        before = [captured for captured in captures[who] if captured <= tick]
        after = [captured for captured in captures[who] if captured > tick]
        if len(before) < 4:
            raise CheckpointsShort(f"{who} healed with only {len(before)} checkpoints behind it: {captures[who]}", record)
        if len(after) < RETAINED_AUTOSAVES:
            raise CheckpointsShort(
                f"{who} wrote only {len(after)} checkpoints after the anchor, so nothing would have rotated it away: {captures[who]}", record)
    return {"match_id": match_id, "tick": tick, "host": sorted(held["host"]), "client": sorted(held["client"]),
            "captures": captures, "perturbed_tick": int(injection[1]), "anchor_lines": {who: [" ".join(row) for row in anchors[who]] for who in anchors},
            "checkpoints": record}


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


def arm_resume(repo: Path, root: Path, port: int, client_stall: str = "") -> dict:
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
        if who == 'client':
            args += stall_args(client_stall)
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
    died_hold = forced_hold_evidence(first, client_stall)
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
        if who == 'client' and client_stall:
            stall_tick, stall_ms = map(int, client_stall.split(':'))
            args += stall_args(f'{resume_tick + stall_tick}:{stall_ms}')
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
    resumed_hold = forced_hold_evidence(second, client_stall, offset=resume_tick)
    compared = compare_live_window(second, resume_tick + 1, resume_tick + resume_ticks)
    reached = max(int(row[0]) for row in CAPTURE.findall(host_log)) if CAPTURE.search(host_log) else 0
    return {"match_id": match_id, "kill_tick": kill_tick, "resume_tick": resume_tick,
            "client_held_the_archive": held_locally, "manifests": [path.name for path in manifests],
            "resumed_captures_to": reached, "peer_comparison": compared,
            "forced_hold": {'died': died_hold, 'resumed': resumed_hold}}


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
                      client_lacks_checkpoint: bool = False, rejoin_from_first_capture: bool = False, host_stall: str = "") -> dict:
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
    if host_stall:
        # The host's own lever: its simulation stalls once in the first boot, so the round judges the host's own seat.
        first_extra = {**first_extra, "host": [*first_extra.get("host", []), "-net-test-live-stall", host_stall]}
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
    assert records["_killed"], (f"the world host was never killed: captures {captures_text(peer_log(first, 'host'))}; "
                                f"the host exited while the kill waited on {records.get('_kill_blocked_by')}; "
                                f"the returner: {return_progress(peer_log(first, 'host'))}")
    first_hold = forced_hold_evidence(first, client_stall)
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
    second_hold = forced_hold_evidence(second, client_stall, offset=resume_tick)
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
    fresh_hold = forced_hold_evidence(fresh, client_stall)
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
            "holds": {half.name: len(HOLD.findall(peer_log(half, "host"))) for half in (first, second, fresh)},
            "forced_hold": {'boot1': first_hold, 'boot2': second_hold, 'fresh': fresh_hold}}


class CheckpointWaitTests(unittest.TestCase):
    """Ladder 7 rung 2 on 260 (S1.restore-all-3): under load the host wrote [130, 751, 1186, 1346] in the anchor run and
    [133, 686] in the retention run; the arms read that as a red engine instead of waiting for the checkpoints."""
    MATCH = "5354414745325034-7bc3c901ab20d0ca"

    @staticmethod
    def capture_lines(ticks, anchor=None) -> str:
        return "".join(f"[autosave] tick={tick} capture_ms=12.5 bytes=100\n[autosave] retained tick={tick} keep=3 "
                       f"pinned={anchor if anchor is not None and tick > anchor else 0} removed=0\n" for tick in ticks)

    def judge(self, judge, texts: dict, held: dict, *args):
        import sys
        import tempfile
        from unittest import mock
        module = sys.modules[__name__]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for who, text in texts.items():
                (root / who).mkdir()
                (root / who / "stdout.log").write_text(text, encoding="utf-8")
            fields = lambda tick: {"SavedTick": str(tick), "_descriptor_sha256": f"d{tick}"}
            with mock.patch.object(module, "checkpoints", lambda _, who: {f"{self.MATCH}-{tick}.ccsave": fields(tick) for tick in held[who]}), \
                    mock.patch.object(module, "compare_live_window", lambda *_: {"passed": True}):
                return judge(root, *args)

    def anchor_texts(self, ticks: list, anchor: int, fullstate_coalesced: int = 0) -> dict:
        coalesced = "".join(f"[fullstate-coalesced] tick={60 * (n + 2)} replaced={60 * (n + 1)} writing=13 waiting_bound=1\n"
                            for n in range(fullstate_coalesced))
        return {"host": "[net-test] live perturb frame=720\n" + coalesced + self.capture_lines(ticks, anchor)
                        + f"[autosave] anchor named match={self.MATCH} tick={anchor} local=ok\n",
                "client": coalesced + self.capture_lines(ticks, anchor) + f"[autosave] anchor received match={self.MATCH} tick={anchor} local=ok\n"}

    def test_landed_checkpoints_drop_only_the_captures_the_engine_names_replaced(self):
        log = (self.capture_lines([130, 250, 370]) + "[autosave-coalesced] tick=370 replaced=250 writing=130 waiting_bound=1\n"
               "[fullstate-coalesced] tick=360 replaced=300 writing=240 waiting_bound=1\n")
        self.assertEqual(landed_checkpoints(log), ([130, 370], 1))
        self.assertEqual(landed_checkpoints(self.capture_lines([130, 751, 1186, 1346])), ([130, 751, 1186, 1346], 0))

    def test_a_short_run_waits_with_its_schedule_doubled(self):
        calls = []

        def attempt(scale, ticks):
            calls.append((scale, ticks))
            record = {"ticks": ticks, "run": "r", **{who: {"landed": [130], "autosave_coalesced": 0, "fullstate_coalesced": 17} for who in ("host", "client")}}
            if scale == 1:
                raise CheckpointsShort("host healed with only 1 checkpoints behind it: [130]", record)
            return {"checkpoints": record}
        details = wait_for_checkpoints(attempt, 1400)
        self.assertEqual(calls, [(1, 1400), (2, 2800)])
        self.assertEqual(details["checkpoint_wait"]["scale"], 2)
        self.assertEqual([run["ticks"] for run in details["checkpoint_wait"]["short_runs"]], [1400])
        self.assertIn("full-state coalesced 17", details["checkpoint_wait"]["written"])

    def test_still_short_at_the_cap_fails_naming_every_run(self):
        calls = []

        def attempt(scale, ticks):
            calls.append(ticks)
            raise CheckpointsShort("host never exceeded the retention limit: [133, 686]",
                                   {"ticks": ticks, "run": "r", **{who: {"landed": [133, 686], "autosave_coalesced": 0, "fullstate_coalesced": 10}
                                                                   for who in ("host", "client")}})
        with self.assertRaises(AssertionError) as raised:
            wait_for_checkpoints(attempt, 700)
        self.assertNotIsInstance(raised.exception, CheckpointsShort)
        self.assertEqual(calls, [700, 1400, 2800, 5600])
        self.assertIn(f"still short at the {CHECKPOINT_WAIT_CAP_TICKS}-tick cap", str(raised.exception))
        self.assertIn("ticks=5600 host wrote 2 (autosave coalesced 0, full-state coalesced 10, not taken 0)", str(raised.exception))

    def test_any_other_statement_fails_at_once(self):
        calls = []

        def attempt(scale, ticks):
            calls.append(ticks)
            raise AssertionError("the peers do not agree on the rewind point")
        with self.assertRaisesRegex(AssertionError, "do not agree"):
            wait_for_checkpoints(attempt, 1400)
        self.assertEqual(calls, [1400])

    def test_the_rungs_anchor_runs_wait_instead_of_failing(self):
        # restore-all-3: one checkpoint behind the heal; restore-all-2: [126, 246, 366, 756] with the anchor at 366.
        for ticks, anchor, behind in (([130, 751, 1186, 1346], 130, 1), ([126, 246, 366, 756], 366, 3)):
            with self.subTest(anchor=anchor), self.assertRaises(CheckpointsShort) as raised:
                self.judge(judge_anchor, self.anchor_texts(ticks, anchor, 17), {"host": [anchor] + ticks[-3:], "client": [anchor] + ticks[-3:]}, 1400, 700)
            self.assertIn(f"host healed with only {behind} checkpoints behind it", str(raised.exception))
            self.assertEqual(raised.exception.record["host"]["fullstate_coalesced"], 17)
        ticks = [126, 246, 366, 486, 900, 1020, 1140]
        details = self.judge(judge_anchor, self.anchor_texts(ticks, 486), {"host": [486, 900, 1020, 1140], "client": [486, 900, 1020, 1140]}, 1400, 700)
        self.assertEqual(details["tick"], 486)

    def test_the_anchor_levers_reach_the_client_alone(self):
        import sys
        from unittest import mock
        module = sys.modules[__name__]
        self.assertEqual(stall_args(""), [])
        self.assertEqual(stall_args("690:1500", 2), ["-net-test-live-stall", "1380:1500"])
        with mock.patch.object(module, "SAVER_DELAY_MS", 0), mock.patch.object(module, "CLIENT_SAVER_DELAY_MS", 2500):
            self.assertEqual((saver_delay_ms("host"), saver_delay_ms("client")), (0, 2500))
        with mock.patch.object(module, "SAVER_DELAY_MS", 400), mock.patch.object(module, "CLIENT_SAVER_DELAY_MS", 0):
            self.assertEqual((saver_delay_ms("host"), saver_delay_ms("client")), (400, 400))

    def test_a_client_that_lacks_the_named_checkpoint_fails_at_once(self):
        # Ladder 12 rung 2 on 266 (S1.restore-all-1): the host named 668, a capture the held client never took.
        texts = self.anchor_texts([131, 432, 668, 823, 987, 1091], 668)
        texts["client"] = texts["client"].replace("tick=668 local=ok", "tick=668 local=not a regular file")
        with self.assertRaises(AssertionError) as raised:
            self.judge(judge_anchor, texts, {"host": [668, 987, 1091], "client": [987, 1091]}, 1400, 720)
        self.assertNotIsInstance(raised.exception, CheckpointsShort)
        self.assertIn("the client does not hold the named checkpoint: not a regular file", str(raised.exception))

    def test_a_short_anchor_run_still_fails_a_lost_rewind_point(self):
        with self.assertRaises(AssertionError) as raised:
            self.judge(judge_anchor, self.anchor_texts([130, 751, 1186, 1346], 130), {"host": [751, 1186, 1346], "client": [130, 751, 1186, 1346]}, 1400, 700)
        self.assertNotIsInstance(raised.exception, CheckpointsShort)
        self.assertIn("rotated away the agreed rewind point", str(raised.exception))

    def test_the_rungs_retention_run_waits_and_a_refused_capture_still_fails(self):
        records = {"host": {"exit_code": 0}, "client": {"exit_code": 0}}
        texts = {who: self.capture_lines([133, 686]) for who in ("host", "client")}
        with self.assertRaises(CheckpointsShort) as raised:
            self.judge(judge_retention, texts, {"host": [133, 686], "client": [133, 686]}, 700, records)
        self.assertIn("host never exceeded the retention limit: [133, 686]", str(raised.exception))
        refused = {who: text + "[autosave] failed tick=253 reason=capture refused\n" for who, text in texts.items()}
        with self.assertRaises(AssertionError) as raised:
            self.judge(judge_retention, refused, {"host": [133, 686], "client": [133, 686]}, 700, records)
        self.assertNotIsInstance(raised.exception, CheckpointsShort)
        texts = {who: self.capture_lines([133, 253, 373, 493]) for who in ("host", "client")}
        details = self.judge(judge_retention, texts, {"host": [253, 373, 493], "client": [253, 373, 493]}, 700, records)
        self.assertEqual(details["host"]["captures"], [133, 253, 373, 493])

    def test_a_held_peers_missed_capture_waits_only_when_its_own_line_names_it(self):
        # live-retention-2 on this lane: the client, held and catching up, did not take the capture the host wrote at 480.
        records = {"host": {"exit_code": 0}, "client": {"exit_code": 0}}
        held = {"host": [131, 480], "client": [131]}
        named = {"host": self.capture_lines([131, 480]),
                 "client": self.capture_lines([131]) + "[autosave] named tick=480 not taken: catch_up=true running=true\n"}
        with self.assertRaises(CheckpointsShort) as raised:
            self.judge(judge_retention, named, held, 700, records)
        self.assertEqual(raised.exception.record["client"]["not_taken"], [480])
        silent = dict(named, client=self.capture_lines([131]))
        with self.assertRaises(AssertionError) as raised:
            self.judge(judge_retention, silent, held, 700, records)
        self.assertNotIsInstance(raised.exception, CheckpointsShort)
        self.assertIn("client lacks 480", str(raised.exception))
        # Enough written on both: the names are compared in full, whatever a line says.
        counted = {"host": self.capture_lines([131, 251, 371, 480]),
                   "client": self.capture_lines([131, 251, 371]) + "[autosave] named tick=480 not taken: catch_up=true running=true\n"}
        with self.assertRaises(AssertionError):
            self.judge(judge_retention, counted, {"host": [251, 371, 480], "client": [131, 251, 371]}, 700, records)


class WorldRestartOracleTests(unittest.TestCase):
    def test_a_never_killed_world_names_the_capture_unit_and_the_returners_progress(self):
        # Ladder 12 rung 2 on 266 (S1.restore-all-1): the captures were read as seconds, and the reports that never came were not named.
        log = ("[autosave] tick=49 capture_ms=10.827 bytes=201235134\n[autosave] tick=93 capture_ms=5.451 bytes=201235124\n"
               "[net-world] tail peer=2 new=127 repeat=125 resend=2 bytes=74864 refused=0 in_flight=127 delivered=296 acknowledged=93 rtt_ms=0 "
               "link_rtt_ms=0 resend_ms=1000\n")
        self.assertEqual(captures_text(log), "tick 49: 10.827 ms, 201235134 bytes, tick 93: 5.451 ms, 201235124 bytes")
        self.assertEqual(return_progress(log), "tail to peer 2 delivered 296, acknowledged 93, rtt 0 ms; no catch-up report reached the host")
        held = log + ("[net-world] catch-up gate peer=2 gate=outside-lead applied=58 acknowledged=58 horizon=277 work_ticks=1\n"
                      "[net-match] hold peer=1 frame=163 AI in control (the host's own seat, AI of peer 1)\n")
        self.assertEqual(return_progress(held), "tail to peer 2 delivered 296, acknowledged 93, rtt 0 ms; last catch-up report from peer 2: gate "
                                                "outside-lead, applied 58 at horizon 277; the host's own seat was held at 163, so it named no capture while held")
        self.assertEqual(return_progress(""), "no tail was sent")

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

    def test_a_world_return_completes_through_its_tail(self):
        # Acceptance run 1, restore-all-1 world-restart boot1: the world-join tail names the seat it activated.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for who in ("host", "client"):
                (root / who).mkdir()
            (root / "host/stdout.log").write_text("[net-match] hold peer=2 frame=44 AI in control\n[net-match] seat-reclaimed peer=2 frame=335 live_actors=1\n", encoding="utf-8")
            client = "[net-test] live stall frame=40 ms=1500\n[net-lockstep] start round=7 frame=1 local_peer=2 peers=2 input_delay=3\n"
            (root / "client/stdout.log").write_text(client + "[net-world] catch-up complete peer=2 at=335 input_horizon=339\n", encoding="utf-8")
            self.assertEqual(forced_hold_evidence(root, "40:1500")["reclaim"], 335)
            (root / "client/stdout.log").write_text(client + "[net-world] catch-up complete peer=3 at=335 input_horizon=339\n", encoding="utf-8")
            with self.assertRaises(AssertionError):
                forced_hold_evidence(root, "40:1500")

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
    parser.add_argument("--client-stall", default="", help="TICK:MS for restore, retention, anchor, resume and world-restart; the client stalls once past each "
                        "round's start so the host holds its seat and the seat has to rejoin")
    parser.add_argument("--world-host-stall", default="", help="world-restart only: TICK:MS, the host's simulation stalls once in the "
                        "first boot, so the round judges the host's own seat")
    parser.add_argument("--anchor-client-stall", default="", help="anchor only: TICK:MS, the client stalls once so the host holds "
                        "its seat and it catches up across a capture it never takes")
    parser.add_argument("--fullstate-every", type=int, default=0,
                        help="every N committed ticks both peers hash their whole capture (-net-fullstate-hash-every); 0 is off")
    parser.add_argument("--client-lacks-checkpoint", action="store_true",
                        help="world-restart only: the client loses its copy of the resume checkpoint and is streamed the host's")
    parser.add_argument("--rejoin-from-first-capture", action="store_true",
                        help="world-restart only: the first boot's world keeps serving its first capture and the client is held at "
                        "tick 124, so its rejoin takes the first capture and catches up across everything since it")
    parser.add_argument("--saver-delay-ms", type=int, default=0,
                        help="both peers' archive writers pause this long before each task (CC_TEST_SAVER_DELAY_MS), a slow disk")
    parser.add_argument("--client-saver-delay-ms", type=int, default=0,
                        help="the client's archive writer alone pauses this long before each task, so a heal can come while the host's "
                        "newest checkpoint is still unpublished on the client")
    parser.add_argument("--pause-slow-peers", action="store_true",
                        help="anchor only: the host pauses for a slow peer instead of holding it, so a sanitizer build's heal lands")
    args = parser.parse_args()
    for lever in (args.client_stall, args.anchor_client_stall):
        if lever and not re.fullmatch(r'[1-9]\d*:[1-9]\d*', lever):
            parser.error('client stall must be positive TICK:MS')
    if args.arm == 'park' and (args.client_stall or args.anchor_client_stall):
        parser.error('park is the no-hold control; use a forced-hold arm')
    if args.fullstate_every < 0:
        parser.error("--fullstate-every must be 0 or positive")
    if not 0 <= args.saver_delay_ms <= 60000 or not 0 <= args.client_saver_delay_ms <= 60000:
        parser.error("--saver-delay-ms and --client-saver-delay-ms in 0..60000")
    global FULLSTATE_EVERY, SAVER_DELAY_MS, CLIENT_SAVER_DELAY_MS
    FULLSTATE_EVERY = args.fullstate_every
    SAVER_DELAY_MS = args.saver_delay_ms
    CLIENT_SAVER_DELAY_MS = args.client_saver_delay_ms
    if not 1024 <= args.port <= 65516:
        parser.error("the base port must leave room for twenty unprivileged ports")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    with engine_executable(repo).open("rb") as exe:
        exe_sha = file_sha256(exe)
    result = {"exe_sha256": exe_sha, "arms": {}}
    arms = {"restore": lambda repo, root, port: arm_restore(repo, root, port, args.client_stall),
            "retention": lambda repo, root, port: arm_retention(repo, root, port, args.client_stall),
            "anchor": lambda repo, root, port: arm_anchor(repo, root, port, args.pause_slow_peers, args.anchor_client_stall or args.client_stall),
            "resume": lambda repo, root, port: arm_resume(repo, root, port, args.client_stall),
            "world-restart": lambda repo, root, port: arm_world_restart(repo, root, port, args.client_stall, args.round_ticks,
                                                                        args.client_lacks_checkpoint, args.rejoin_from_first_capture,
                                                                        args.world_host_stall),
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
