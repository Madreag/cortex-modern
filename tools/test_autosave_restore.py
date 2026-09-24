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
import os
import re
import shutil
import threading
import unittest
import zipfile
from pathlib import Path

from feel.retained_resume import PER_PEER_SUBSYSTEMS, read_live_hashes, split_passes
from run_sim_test import make_run
from feel_measure import stage_baseline

CAPTURE = re.compile(r"^\[autosave\] tick=(\d+) capture_ms=(\d+(?:\.\d+)?) bytes=(\d+)$", re.MULTILINE)
HOLD = re.compile(r"^\[net-match\] hold peer=\d+ frame=\d+ AI in control$", re.MULTILINE)
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
RETAINED_AUTOSAVES = 3  # The default of the NetworkAutosavesKept option (AutosaveStore::c_RetainedAutosaves), which
                        # these runs never set; the engine's own keep= value is held to it below.


WORLD_IDENTITY = re.compile(r"^\[net-world\] identity (\S+) boot=(\d+) round=(\d+)$", re.MULTILINE)
WORLD_START = re.compile(r"^\[net-lockstep\] start round=(\d+) frame=(\d+) local_peer=(\d+) peers=(\d+) input_delay=(\d+)$", re.MULTILINE)
WORLD_LOBBY = re.compile(r"^\[net-match-service-e2e\] lobby_snapshot: [^\n]*\bactivity=Persistent World\b[^\n]*$", re.MULTILINE)
WORLD_AGREED = re.compile(r'^\[autosave\] agreed match=(\S+) tick=(\d+) state=(".*")$', re.MULTILINE)


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


def run_pair(repo: Path, root: Path, port: int, ticks: int, seconds: int, extra: dict) -> dict:
    """Two peers of one match, each with the arm's own extra flags."""
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
        args += extra.get(who, [])
        runs[who] = make_run(repo, args, root / who, 420, env={"CCCP_HEADLESS": "1"})
        stage_baseline(runs[who], ticks)

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


def compare_live_window(root: Path, first_tick: int, last_tick: int) -> dict:
    """Require the complete window and compare every replay of every shared tick."""
    peers = {who: read_live_hashes(root / f"{who}-live.jsonl") for who in ("host", "client")}
    required = set(range(first_tick, last_tick + 1))
    by_tick = {}
    for who, rows in peers.items():
        indexed = {}
        for row in rows:
            indexed.setdefault(row["tick"], []).append(row)
        missing = sorted(required - indexed.keys())
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


def arm_anchor(repo: Path, root: Path, port: int) -> dict:
    """A heal names one rewind point for the whole match, and it survives later rotation.

    The perturbation is timed late on purpose: at the stock tick 50 the heal lands before the first
    checkpoint (one 2 s interval = 120 ticks after the match starts), so the host would have nothing to
    name and the row would be red for a reason that is not the anchor mechanism. Tick 700 puts at least
    four checkpoints before the heal, and the 1400-tick cap leaves room for more than the retention limit
    afterwards, so the named one can only survive by being pinned."""
    ticks, perturb_at = 1400, 700
    records = run_pair(repo, root, port, ticks, 2,
                       {"host": ["-net-test-perturb-when-live", "-determinism-selftest-perturb", "-determinism-selftest-perturb-tick", str(perturb_at),
                                 "-net-match-e2e-resync"],
                        "client": ["-net-match-e2e-resync"]})
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


def _world_peer_args(root: Path, who: str, port: int, ticks: int, extra: list) -> list:
    """One peer of a persistent world: the host is the dedicated world daemon, the client an ordinary join."""
    args = ["-net-port", str(port), "-net-match-peers", "2", "-net-match-input-delay", "3",
            "-net-autosave-seconds", "1", "-net-match-ticks", str(ticks),
            "-net-live-tick-hashes", str(root / f"{who}-live.jsonl"),
                "-tick-hashes", "-max-ticks", str(ticks), "-out", str(root / f"{who}_trace.json"),
            "-net-match-report", str(root / f"{who}_report.json")]
    if who == "host":
        args = ["-net-dedicated", "-net-persistent-world", *args]
    else:
        args = ["-net-match-service-e2e", "-net-join", "127.0.0.1", *args]
    return args + extra


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
                     carry=None) -> dict:
    """One round of a persistent world. `carry` is the previous round's root: its world state is
    copied into each staged runtime after the runner prepares it and before the process starts."""
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
            runs[who] = make_run(repo, _world_peer_args(root, who, port, ticks, extra.get(who, [])),
                                 root / who, 420, env={"CCCP_HEADLESS": "1"})
            if carry is not None:
                _carry_world_state(carry, who, Path(runs[who].cwd))
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        if kill_past:
            waiter = threading.Event()
            for _ in range(4200):
                waiter.wait(0.1)
                host_log, client_log = peer_log(root, "host"), peer_log(root, "client")
                captures = [int(row[0]) for row in CAPTURE.findall(host_log)]
                identity = WORLD_IDENTITY.search(host_log)
                ticket = root / "client/runtime/Userdata/reconnect.ticket"
                published = []
                if identity:
                    store = root / "host/runtime/Autosaves"
                    published = [int(path.stem.rsplit("-", 1)[1]) for path in store.glob(f"{identity[1]}-*.ccmanifest")
                                 if path.with_suffix(".ccsave").exists() and (store / f"{identity[1]}.admission").exists()]
                if _world_kill_ready(host_log, client_log, kill_past, published, ticket.exists()):
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
    compared = compare_live_window(root, first_tick, last_tick)
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


def arm_world_restart(repo: Path, root: Path, port: int, client_stall: str = "") -> dict:
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
    kill_tick, round_ticks = 400, 600

    def stall(start: int, extra: dict) -> dict:
        """The stress lever: the client stalls once past `start`, so its seat is held and has to rejoin."""
        if not client_stall:
            return extra
        tick, milliseconds = (int(part) for part in client_stall.split(":"))
        return {**extra, "client": [*extra.get("client", []), "-net-test-live-stall", f"{start + tick}:{milliseconds}"]}

    records = _run_world_round(repo, first, port, 1200, stall(0, {}), kill_past=kill_tick)
    assert records["_killed"], f"the world host was never killed: captures {CAPTURE.findall(peer_log(first, 'host'))[:6]}"
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
    resumed = _run_world_round(repo, second, port + 2, resume_end, stall(resume_tick, {}), carry=first)
    host_log = peer_log(second, "host")
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48720)
    parser.add_argument("--arm", choices=("all", "restore", "retention", "anchor", "resume", "world-restart"), default="all")
    parser.add_argument("--client-stall", default="", help="world-restart only: TICK:MS, the client stalls once past each "
                        "round's start so the host holds its seat and the seat has to rejoin")
    args = parser.parse_args()
    if not 1024 <= args.port <= 65516:
        parser.error("the base port must leave room for twenty unprivileged ports")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    with (repo / "Cortex Command.exe").open("rb") as exe:
        exe_sha = hashlib.file_digest(exe, "sha256").hexdigest()
    result = {"exe_sha256": exe_sha, "arms": {}}
    arms = {"restore": arm_restore, "retention": arm_retention, "anchor": arm_anchor, "resume": arm_resume,
            "world-restart": lambda repo, root, port: arm_world_restart(repo, root, port, args.client_stall)}
    if args.arm != "all":
        arms = {args.arm: arms[args.arm]}
    for index, (arm, run) in enumerate(arms.items()):
        details = {}
        result["arms"][arm] = details
        # The resume and world-restart arms run several rounds of two peers, each on its own ports.
        armPort = {"resume": args.port + 5, "world-restart": args.port + 10}.get(arm, args.port + index)
        try:
            details.update(run(repo, root / arm, armPort), passed=True)
            print(f"PASS {arm}", flush=True)
        except Exception as error:
            details.update(passed=False, error=str(error))
            print(f"FAIL {arm}: {error}", flush=True)
    (root / "result.json").write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
    return 0 if all(arm["passed"] for arm in result["arms"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
