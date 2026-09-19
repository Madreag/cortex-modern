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
              own checkpoint and the client rejoins with its stored ticket. Both peers reach the same
              later tick with equal per-tick hashes, and the client's log says whether it loaded its own
              copy of the checkpoint or was sent the host's.
              RED before the change: no restart manifest is written, -net-resume-match does not exist,
              and a host that dies takes its match with it.

Ports: this driver owns 48720-48729; the first three arms take consecutive ports from its base and the
resume arm takes 48725-48728 (two rounds of two peers), so it never
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
import zipfile
from pathlib import Path

from compare_sim_traces import strict_compare
from run_sim_test import make_run

CAPTURE = re.compile(r"^\[autosave\] tick=(\d+) capture_ms=(\d+(?:\.\d+)?) bytes=(\d+)$", re.MULTILINE)
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
RETAINED_AUTOSAVES = 3  # AutosaveStore::c_RetainedAutosaves; the engine's own keep= value is held to it below.


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
                "-tick-hashes", "-max-ticks", str(ticks), "-out", str(root / f"{who}_trace.json"),
                "-net-match-report", str(root / f"{who}_report.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        args += extra.get(who, [])
        runs[who] = make_run(repo, args, root / who, 420, env={"CCCP_HEADLESS": "1"})

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
    passed, comparison = strict_compare(root / "host_trace.json", root / "client_trace.json", ticks)
    assert passed, comparison
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
                       {"host": ["-determinism-selftest-perturb", "-determinism-selftest-perturb-tick", str(perturb_at),
                                 "-net-match-e2e-resync"],
                        "client": ["-net-match-e2e-resync"]})
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
            "captures": captures, "anchor_lines": {who: [" ".join(row) for row in anchors[who]] for who in anchors}}


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
                "-net-autosave-seconds", "1", "-net-match-resync-on-desync",
                "-tick-hashes", "-max-ticks", "1200", "-out", str(first / f"{who}_trace.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        runs[who] = make_run(repo, args, first / who, 420, env={"CCCP_HEADLESS": "1"})

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
                "-net-autosave-seconds", "1", "-net-match-resync-on-desync",
                "-tick-hashes", "-max-ticks", str(resume_ticks), "-out", str(second / f"{who}_trace.json")]
        args += ["-net-host", "-net-resume-match", match_id, "-net-resume-tick", str(resume_tick)] if who == "host" else ["-net-join", "127.0.0.1"]
        resumed[who] = make_run(repo, args, second / who, 420, env={"CCCP_HEADLESS": "1"})
        # What a restarted process finds on its own disk: its checkpoints, its manifests, its admission
        # file and its ticket. Copied, never moved: the first round's evidence stays where it was.
        source = first / who / "runtime/Autosaves"
        target = second / who / "runtime/Autosaves"
        if source.exists():
            shutil.copytree(source, target, dirs_exist_ok=True)
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
    if held_locally:
        assert HELD_LAUNCH.search(client_log), "the client answered that it holds the checkpoint and then loaded something else"
    else:
        assert RECEIVED_LAUNCH.search(client_log), f"the client neither held nor received the checkpoint: {offer[3]}"
    for who in ("host", "client"):
        assert resumed_records[who].get("exit_code") == 0, (who, resumed_records[who].get("exit_code"), resumed_records[who].get("error"))
        assert not resumed_records[who].get("timed_out"), who
    # The resumed round is lockstep: the two peers' per-tick hashes must agree, tick for tick.
    traces = {who: second / f"{who}_trace.json" for who in ("host", "client")}
    for who, path in traces.items():
        assert path.exists(), f"{who} wrote no tick-hash trace"
    # The resumed round starts behind the checkpoint's tick, so the peers are compared over the ticks
    # they both ran, from the first one the resumed round applied.
    passed, compared = strict_compare(traces["host"], traces["client"], first_tick=resume_tick + 1)
    assert passed, f"the resumed round's peers diverged: {compared}"
    reached = max(int(row[0]) for row in CAPTURE.findall(host_log)) if CAPTURE.search(host_log) else 0
    return {"match_id": match_id, "kill_tick": kill_tick, "resume_tick": resume_tick,
            "client_held_the_archive": held_locally, "manifests": [path.name for path in manifests],
            "resumed_captures_to": reached, "peer_comparison": compared}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48720)
    parser.add_argument("--arm", choices=("all", "restore", "retention", "anchor", "resume"), default="all")
    args = parser.parse_args()
    if not 48720 <= args.port <= 48724:
        parser.error("this detector owns 48720-48729; its arms' ports must fit inside it")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    with (repo / "Cortex Command.exe").open("rb") as exe:
        exe_sha = hashlib.file_digest(exe, "sha256").hexdigest()
    result = {"exe_sha256": exe_sha, "arms": {}}
    arms = {"restore": arm_restore, "retention": arm_retention, "anchor": arm_anchor, "resume": arm_resume}
    if args.arm != "all":
        arms = {args.arm: arms[args.arm]}
    for index, (arm, run) in enumerate(arms.items()):
        details = {}
        result["arms"][arm] = details
        # The resume arm runs two rounds of two peers, on its own pair of ports inside the block.
        armPort = 48725 if arm == "resume" else args.port + index
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
