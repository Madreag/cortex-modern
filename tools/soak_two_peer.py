"""The two-peer soak: one long service match with autosave on, forced holds that each end in a rejoin, and memory
sampled on both peers every minute.

    python tools/soak_two_peer.py --out <dir> [--minutes 45] [--autosave-seconds 60] [--holds 3] [--stall-ms 1500]
                                  [--port 49880] [--fullstate-every N] [--sample-seconds 60] [--saver-delay-ms N]
                                  [--client-free-run] [--terrain-events FROM:TO] [--fullstate-dump]

Both engines run through run_sim_test.make_run and the private-desktop runner with CCCP_HEADLESS=1. The client carries
the forced-hold lever (-net-test-live-stall TICK:MS) once per hold, spread evenly through the match, so its seat is held
and it has to rejoin while the host plays on. Exit 0 only when both peers exit 0, every shared tick's live hash agrees,
each forced hold was taken and ended in a completed private catch-up, no client named a new host while the host
lived, the host published the autosaves its cadence owes, and a memory sample exists for every minute. result.json carries the counts; memory.jsonl the samples.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import math
import re
import subprocess
import sys
from feel.report import peer_id_of, return_hold_violations
from cross_report import pace_verdict
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run  # noqa: E402
from feel.launch_budget import install_memory_guard  # noqa: E402
from compare_sim_traces import compare_fullstate  # noqa: E402
from feel.retained_resume import compare_live_hashes, read_live_hashes, PER_PEER_SUBSYSTEMS  # noqa: E402
from feel.report import own_hold_windows  # noqa: E402
from compare_sim_traces import CORE  # noqa: E402
from feel_measure import input_pattern, private_settings, stage_baseline  # noqa: E402

PORT_LO, PORT_HI = 49880, 49889
TICKS_PER_SECOND = 60
install_memory_guard()


def census_pace(log: Path) -> dict[int, dict]:
    """Each [mem-census] line's own pace (wall_tps, sim_ms_per_tick, ...) by the tick that closes its window."""
    rows = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines() if log.is_file() else []:
        if line.startswith("[mem-census] ") and " pace: " in line:
            fields = dict(part.split("=", 1) for part in line.split(" pace: ", 1)[1].split() if "=" in part)
            rows[int(line.split("tick=")[1].split()[0])] = {key: float(value) for key, value in fields.items()
                                                          if value.replace(".", "", 1).replace("-", "", 1).isdigit()}
    return rows


def window_sim_ms(census: dict[int, dict], start: int, end: int) -> float | None:
    """The engine's sim cost over the census window that holds most of [start, end]."""
    middle = (start + end) // 2
    closing = min((tick for tick in census if tick >= middle), default=None)
    return census[closing].get("sim_ms_per_tick") if closing is not None else None


def excused_return_holds(root: Path, rows: list[dict]) -> tuple[list[dict], list[dict]]:
    """Splits holds after a return into the excused (the held engine said it is a slow machine and its own sim does not
    fit the tick there) and the rest."""
    engines = {peer_id_of(root / f"{peer}_report.json"): peer for peer in ("host", "client")}
    logs = {peer: (root / peer / "stdout.log").read_text(encoding="utf-8", errors="replace") if (root / peer / "stdout.log").is_file() else ""
            for peer in ("host", "client")}
    excused, kept = [], []
    for row in rows:
        engine = engines.get(row["held_peer"])
        slow = any(f"[net-lockstep] slow machine peer {row['held_peer']} at frame {frame} " in text
                   for text in logs.values() for frame in range(row["return_tick"], row["hold_tick"] + 1))
        sim = window_sim_ms(census_pace(root / engine / "stdout.log"), row["return_tick"], row["hold_tick"]) if engine else None
        fits = pace_verdict({"pace": {"sim_ms_per_tick": sim, "wall_tps": None}})["gated"]
        (excused if slow and sim is not None and not fits else kept).append(dict(row, held_engine=engine, sim_ms_per_tick=sim,
                                                                                   slow_machine_line=slow))
    return excused, kept


def acceptance_history(root: Path, ticks: int, expected_rounds: int = 1) -> dict:
    records = {peer: read_live_hashes(root / f"{peer}-live.jsonl") for peer in ("host", "client")}
    errors, indexed, windows, paces = [], {}, {}, {}
    rounds = {row.get("round") for rows in records.values() for row in rows}
    if None in rounds or len(rounds) != expected_rounds:
        errors.append("round coverage differs from the declared workload")
    for peer, rows in records.items():
        log = root / peer / "stdout.log"
        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        windows[peer] = own_hold_windows(text.splitlines())
        # The soak's own injected stall belongs to the seat it stalls, like the hold it causes: that seat's away time starts there.
        stalls = [int(frame) for frame in re.findall(r"^\[net-test\] live stall frame=(\d+)", text, re.M)]
        windows[peer] = [(rid, next((stall for stall in stalls if stall <= start <= stall + 30), start), end) for rid, start, end in windows[peer]]
        census = census_pace(log)
        indexed[peer] = {}
        for row in rows:
            indexed[peer].setdefault((row.get("round"), row["tick"]), []).append(row)
        paces[peer] = []
        for round_id in rounds:
            away = {tick for rid, start, end in windows[peer] if rid == round_id for tick in range(start, end)}
            # The engine commits its terminal tick one past the match's tick count, on both peers alike.
            terminal = ticks + 1
            required = set(range(1, terminal + 1)) - away
            present = {tick for rid, tick in indexed[peer] if rid == round_id}
            if not required <= present or terminal not in present or present - set(range(1, terminal + 1)):
                errors.append(f"{peer} round {round_id}: missing/extra ticks or terminal tick")
            for start in range(300, ticks, 3600):
                end = min(start + 3600, ticks)
                needed = set(range(start, end + 1)) - away
                if not needed <= present:
                    continue
                # Charge every measured adjacent interval outside this peer's proved hold. A short hold
                # must not exempt the rest of its entire minute from the pace verdict.
                intervals = [(tick, tick + 1) for tick in range(start, end) if tick in needed and tick + 1 in needed]
                durations = [max(row.get('wall_ms', float('nan')) for row in indexed[peer][round_id, high]) -
                             min(row.get('wall_ms', float('nan')) for row in indexed[peer][round_id, low]) for low, high in intervals]
                elapsed = sum(durations)
                rate = len(intervals) * 1000 / elapsed if elapsed > 0 and all(d >= 0 and math.isfinite(d) for d in durations) else None
                # A window is gated when this engine's own sim fits the tick there; a slower one is reported, not gated.
                verdict = pace_verdict({"pace": {"sim_ms_per_tick": window_sim_ms(census, start, end),
                                                 "wall_tps": rate if rate is not None and math.isfinite(rate) else None}})
                ok = verdict["passed"] if verdict["sim_ms_per_tick"] is not None else rate is not None and math.isfinite(rate) and rate >= 59.5
                paces[peer].append(dict(round=round_id, first=start, last=end, wall_tps=rate, passed=ok,
                                        sim_ms_per_tick=verdict["sim_ms_per_tick"], gated=verdict["gated"] or verdict["sim_ms_per_tick"] is None))
                if not ok:
                    errors.append(f"{peer} round {round_id}: pace window {start}-{end} below 59.5 TPS")
        if not paces[peer]:
            errors.append(f"{peer}: no complete pace window")
    shared = set(indexed['host']) & set(indexed['client'])
    for key in sorted(shared):
        values = indexed['host'][key] + indexed['client'][key]
        canonical = [{k: v for k, v in row.get('subsystems', {}).items() if k not in PER_PEER_SUBSYSTEMS} for row in values]
        if any(not (CORE | {'controller'}) <= value.keys() or value != canonical[0] for value in canonical) or \
                any(row.get('sim_gated') != values[0].get('sim_gated') or row.get('paused', False) != values[0].get('paused', False) for row in values):
            errors.append(f"hash mismatch or incomplete schema at {key}")
            break
    if not shared:
        errors.append("no shared hashes")
    return dict(**{'pass': not errors}, errors=errors, pace_windows=paces,
                hold_windows=windows, compared_keys=len(shared), expected_ticks=ticks)


class MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


def process_memory(run) -> dict | None:
    """The engine's resident and private bytes, read from its own process; None once it has exited."""
    if sys.platform == "win32":
        handle = getattr(run, "process", None)
        if not handle:
            return None
        counters = MemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(handle), ctypes.byref(counters), counters.cb):
            return None
        return {"working_set": counters.WorkingSetSize, "private": counters.PagefileUsage,
                "peak_working_set": counters.PeakWorkingSetSize}
    pid = (getattr(run, "record", {}) or {}).get("pid")
    if not pid:
        return None
    result = subprocess.run(["ps", "-o", "rss=,vsz=", "-p", str(pid)], capture_output=True, text=True)
    fields = result.stdout.split()
    return {"working_set": int(fields[0]) * 1024, "virtual": int(fields[1]) * 1024} if len(fields) == 2 else None


def count(path: Path, needle: str, also: str = "") -> int:
    """Lines that start with `needle` (and carry `also`): one per event, never the event's detail lines."""
    text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    return sum(line.startswith(needle) and also in line for line in text.splitlines())


def census_growth(log: Path, settled_tick: int) -> dict | None:
    """The engine's [mem-census] lines: the first at or past the settled tick, the last, and every record that grew between them."""
    rows = []
    if log.is_file():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("[mem-census] "):
                rows.append({key: int(value) for key, _, value in (token.partition("=") for token in line.split()[1:]) if value.lstrip("-").isdigit()})
    first = next((row for row in rows if row.get("tick", 0) >= settled_tick), None)
    if not rows or not first:
        return None
    last = rows[-1]
    return {"lines": len(rows), "first_tick": first.get("tick"), "last_tick": last.get("tick"),
            "grew": {key: [first[key], last[key]] for key in last if key != "tick" and key in first and last[key] > first[key]}}


def terrain_event_ticks(root: Path, peer: str, tag: str) -> list[int]:
    """The ticks of one peer's traced terrain events with this tag, from the window-end or the desync flush."""
    ticks: set[int] = set()
    for suffix in (".wend.terrainevents.txt", ".desync.terrainevents.txt"):
        path = root / f"{peer}_trace.json{suffix}"
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[1] == tag and fields[0].isdigit():
                ticks.add(int(fields[0]))
    return sorted(ticks)


def census_private(log: Path) -> dict:
    """Each [mem-census] line's private_mb by its tick."""
    rows = {}
    if log.is_file():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("[mem-census] "):
                fields = dict(token.partition("=")[::2] for token in line.split()[1:])
                if fields.get("tick", "").isdigit() and fields.get("private_mb", "").isdigit():
                    rows[int(fields["tick"])] = int(fields["private_mb"])
    return rows


def pace_across_own_seat_hold(root: Path) -> dict | None:
    """Each peer's census pace in the windows wholly before the host's first own-seat hold and wholly after its reclaim."""
    text = (root / "host" / "stdout.log").read_text(encoding="utf-8", errors="replace") if (root / "host" / "stdout.log").is_file() else ""
    hold = next((int(line.split("frame=")[1].split()[0]) for line in text.splitlines()
                 if line.startswith("[net-match] hold peer=") and "the host's own seat" in line), None)
    back = next((int(line.split("frame=")[1].split()[0]) for line in text.splitlines()
                 if line.startswith("[net-match] seat-reclaimed peer=") and hold is not None and int(line.split("frame=")[1].split()[0]) >= hold), None)
    if hold is None or back is None:
        return None
    summary: dict = {"hold_frame": hold, "reclaim_frame": back}
    for peer in ("host", "client"):
        rows = []
        for line in (root / peer / "stdout.log").read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("[mem-census] ") and " pace: " in line:
                fields = dict(part.split("=", 1) for part in line.split(" pace: ", 1)[1].split() if "=" in part)
                numbers = {key: float(value) for key, value in fields.items() if value.replace(".", "", 1).replace("-", "", 1).isdigit()}
                rows.append({"tick": int(line.split("tick=")[1].split()[0]), **numbers})
        spans, previous = {"before": [], "after": []}, 0
        for row in rows:
            if row["tick"] <= hold:
                spans["before"].append(row)
            elif previous >= back:
                spans["after"].append(row)
            previous = row["tick"]
        keys = ("wall_tps", "sim_ms_per_tick", "update_ms_per_tick", "draw_ms_per_tick", "preview_ms_per_tick", "interface_ms_per_tick",
                "ms_per_frame_drawn", "max_iteration_draw_ms")
        summary[peer] = {name: {"windows": len(span), **{key: round(sum(r.get(key, 0.0) for r in span) / len(span), 3) for key in keys}} if span else None
                         for name, span in spans.items()}
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--minutes", type=float, default=45)
    parser.add_argument("--autosave-seconds", type=int, default=60)
    parser.add_argument("--holds", type=int, default=3)
    parser.add_argument("--stall-ms", type=int, default=1500)
    parser.add_argument("--host-stall", action="append", default=[], metavar="TICK:MS",
                        help="the host stalls MS at TICK (-net-test-live-stall), so its session plane holds the host's own seat; repeatable")
    parser.add_argument("--lag", type=int, default=0, help="the client's added one-way delay in ms (its -net-fake-lag is the round trip, twice this)")
    parser.add_argument("--loss", type=int, default=0, help="the client's GNS packet loss in percent (CC_TEST_GNS_LOSS_PERCENT)")
    parser.add_argument("--port", type=int, default=PORT_LO)
    parser.add_argument("--port-block", default=f"{PORT_LO}-{PORT_HI}", help="the calling lane's own port block, LO-HI; --port stays inside it")
    parser.add_argument("--fullstate-every", type=int, default=0)
    parser.add_argument("--client-sim-cost-us", type=int, default=0,
                        help="the client spends this much more per sim tick (a slower machine's sim cost on this box)")
    parser.add_argument("--client-sim-cost-from-tick", type=int, default=0,
                        help="the client's added sim cost starts at this tick: a machine that turns slow mid-match")
    parser.add_argument("--client-sim-cost-until-tick", type=int, default=0, help="the client's added sim cost ends at this tick (0: never)")
    parser.add_argument("--host-sim-cost-us", type=int, default=0, help="the host spends this much more per sim tick")
    parser.add_argument("--host-sim-cost-from-tick", type=int, default=0, help="the host's added sim cost starts at this tick")
    parser.add_argument("--host-sim-cost-until-tick", type=int, default=0, help="the host's added sim cost ends at this tick (0: never)")
    parser.add_argument("--client-draw-cost-us", type=int, default=0,
                        help="the client spends this much more per drawn frame (a slower machine's draw cost on this box)")
    parser.add_argument("--sample-seconds", type=float, default=60)
    parser.add_argument("--rematch", action="store_true", help="both peers ride the e2e rematch: when match 1 ends they return to the lobby and play match 2")
    parser.add_argument("--end-round-tick", type=int, default=0, help="both peers end the round at this sim tick (team 0 wins), so a rematch starts while a seat may still be held")
    parser.add_argument("--rematches", type=int, default=0, help="with --rematch: ride that many rematches in a row")
    parser.add_argument("--mute-input", default="", help="FRAME:COUNT - the client sends none of its own input for those frames while it keeps simulating them")
    parser.add_argument("--stall-each-round", type=int, default=0,
                        help="TICK - the client also stalls --stall-ms at this tick of every round, so each round of a rematch chain holds its seat and catches up")
    parser.add_argument("--census-histogram", action="store_true", help="the census also sums the heaps and walks the heap by block size (seconds per line, the simulation waiting on the heap lock)")
    parser.add_argument("--no-tick-trace", action="store_true",
                        help="keep no per-tick trace in memory: the checks read the live stream, and over an endurance soak the trace is its own grower")
    parser.add_argument("--census-probe-size", type=int, default=0, help="print the first bytes of heap blocks of this size")
    parser.add_argument("--census-atom-stacks", action="store_true", help="the census samples where Atoms are constructed")
    parser.add_argument("--census-ticks", type=int, default=3600, help="ticks between the engine's memory census lines; 0 = none")
    parser.add_argument("--saver-delay-ms", type=int, default=0, help="both peers' archive writers pause this long before each task (CC_TEST_SAVER_DELAY_MS), a slow disk on a fast one")
    parser.add_argument("--client-free-run", action="store_true", help="the client runs -free-run-sim (one tick per loop iteration, no frame drawn), "
                        "so a draw that writes the simulation shows as a difference between the peers")
    parser.add_argument("--terrain-events", default="", help="FROM:TO - both peers trace their terrain events in that tick window (CC_TERRAIN_EVENTS); "
                        "result.json names each peer's clean ticks")
    parser.add_argument("--fullstate-dump", action="store_true", help="with --fullstate-every: each peer writes every captured text section "
                        "under <out>/<peer>-fullstate/<tick>/ (-net-fullstate-dump), so a divergent section can be diffed")
    parser.add_argument("--fullstate-dump-sections", default="", help="with --fullstate-dump: the comma-separated sections each peer keeps "
                        "(CC_TEST_FULLSTATE_DUMP_SECTIONS), so a long soak keeps only the section it diffs")
    parser.add_argument("--cross-records", action="store_true", help="both peers write the cross harness's event records (CC_TEST_CROSS_RECORDS) beside their runs")
    parser.add_argument("--cross-event-limit", type=int, default=0, help="with --cross-records: the records' byte budget (CC_TEST_CROSS_EVENT_RAW_LIMIT); 0 = the engine's")
    options = parser.parse_args(argv)
    low, _, high = options.port_block.partition("-")
    if not (low.isdigit() and high.isdigit() and int(low) <= options.port <= int(high) - 4):
        parser.error(f"--port leaves room for the peers' ports inside the block {options.port_block}")
    if options.minutes <= 0 or options.holds < 0 or not 0 < options.stall_ms <= 20000 or options.fullstate_every < 0 or options.stall_each_round < 0:
        parser.error("--minutes > 0, --holds >= 0, --stall-ms in 1..20000, --fullstate-every >= 0, --stall-each-round >= 0")
    if not 0 <= options.saver_delay_ms <= 60000:
        parser.error("--saver-delay-ms in 0..60000")
    for spec in options.host_stall:
        tick, _, milliseconds = spec.partition(":")
        if not (tick.isdigit() and milliseconds.isdigit() and int(tick) > 0 and 0 < int(milliseconds) <= 20000):
            parser.error("--host-stall TICK:MS with TICK > 0 and MS in 1..20000")
    window = options.terrain_events.split(":")
    if options.terrain_events and not (len(window) == 2 and all(part.isdigit() for part in window) and int(window[0]) <= int(window[1])):
        parser.error("--terrain-events FROM:TO with FROM <= TO")
    if not 60 <= options.autosave_seconds <= 3600:
        parser.error("--autosave-seconds stays in the product's 60..3600 range")
    if os.environ.get("CCCP_HEADLESS", "1") != "1":
        parser.error("CCCP_HEADLESS must stay 1")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = options.repo.resolve(), options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    ticks = int(options.minutes * 60 * TICKS_PER_SECOND)
    stalls = [ticks * (index + 1) // (options.holds + 1) for index in range(options.holds)]
    script = root / "input.txt"
    input_pattern(script)
    plan = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "repo": str(repo), "ticks": ticks, "minutes": options.minutes,
            "autosave_seconds": options.autosave_seconds, "stalls": [f"{tick}:{options.stall_ms}" for tick in stalls],
            "host_stalls": options.host_stall,
            "port": options.port, "fullstate_every": options.fullstate_every, "sample_seconds": options.sample_seconds,
            "saver_delay_ms": options.saver_delay_ms, "stall_each_round": options.stall_each_round,
            "client_free_run": options.client_free_run, "terrain_events": options.terrain_events}
    (root / "plan.json").write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    runs, records = {}, {}
    samples: list[dict] = []
    stop = threading.Event()
    started = time.monotonic()

    def sample() -> None:
        with open(root / "memory.jsonl", "a", encoding="utf-8") as handle:
            while True:
                row = {"elapsed_s": round(time.monotonic() - started, 1),
                       **{peer: process_memory(run) for peer, run in runs.items()}}
                samples.append(row)
                handle.write(json.dumps(row) + "\n")
                handle.flush()
                if stop.wait(options.sample_seconds):
                    return

    try:
        for peer in ("host", "client"):
            flags = ["-seed", "42", "-max-ticks", str(ticks), *([] if options.no_tick_trace else ["-tick-hashes"]), "-out", str(root / f"{peer}_trace.json"),
                     "-net-live-tick-hashes", str(root / f"{peer}-live.jsonl"), "-input-script", str(script),
                     "-net-match-service-e2e", "-net-port", str(options.port), "-net-match-ticks", str(ticks),
                     "-net-match-humans", "2", "-net-match-peers", "2", "-net-match-cpu-slots", "0",
                     "-net-match-service-preset", "Determinism FeelBaseline", "-net-match-service-module", "UserScenes.rte",
                     "-net-match-auto-delay", "-net-reconnect-ticket", str(root / f"{peer}.ticket"),
                     "-net-match-report", str(root / f"{peer}_report.json")]
            if options.fullstate_every:
                flags += ["-net-fullstate-hash-every", str(options.fullstate_every)]
                if options.fullstate_dump:
                    flags += ["-net-fullstate-dump", str(root / f"{peer}-fullstate")]
            if options.census_ticks:
                flags += ["-memory-census-ticks", str(options.census_ticks)]
            if options.census_histogram:
                flags += ["-memory-census-histogram"]
            if options.census_atom_stacks:
                flags += ["-memory-census-atom-stacks"]
            if options.census_probe_size:
                flags += ["-memory-census-probe-size", str(options.census_probe_size)]
            if peer == "host":
                flags += ["-net-host", "-net-autosave-seconds", str(options.autosave_seconds)]
                for spec in options.host_stall:
                    flags += ["-net-test-live-stall", spec]
            else:
                flags += ["-net-join", "127.0.0.1"]
                if options.lag:
                    flags += ["-net-fake-lag", str(2 * options.lag)]
                if options.client_free_run:
                    flags += ["-free-run-sim"]
                for tick in stalls:
                    flags += ["-net-test-live-stall", f"{tick}:{options.stall_ms}"]
                if options.stall_each_round:
                    flags += ["-net-test-live-stall-each-round", f"{options.stall_each_round}:{options.stall_ms}"]
            if options.rematch:
                flags += ["-net-match-e2e-rematch"]
                if options.rematches > 1:
                    flags += ["-net-match-e2e-rematches", str(options.rematches)]
            if options.end_round_tick:
                flags += ["-net-match-e2e-end-round-tick", str(options.end_round_tick)]
            env = {"CCCP_HEADLESS": "1"}
            if options.fullstate_dump_sections:
                env["CC_TEST_FULLSTATE_DUMP_SECTIONS"] = options.fullstate_dump_sections
            if options.saver_delay_ms:
                env["CC_TEST_SAVER_DELAY_MS"] = str(options.saver_delay_ms)
            if options.terrain_events:
                env["CC_TERRAIN_EVENTS"] = options.terrain_events
            if options.cross_records:
                env["CC_TEST_CROSS_RECORDS"] = str(root / f"{peer}-records" / "events.jsonl")
                (root / f"{peer}-records").mkdir()
                if options.cross_event_limit:
                    env["CC_TEST_CROSS_EVENT_RAW_LIMIT"] = str(options.cross_event_limit)
            if peer == "client" and options.loss:
                env["CC_TEST_GNS_LOSS_PERCENT"] = str(options.loss)
            if peer == "client" and options.mute_input:
                env["CC_TEST_LOCKSTEP_MUTE_INPUT"] = options.mute_input
            cost, from_tick, until_tick = ((options.client_sim_cost_us, options.client_sim_cost_from_tick, options.client_sim_cost_until_tick) if peer == "client" else
                                           (options.host_sim_cost_us, options.host_sim_cost_from_tick, options.host_sim_cost_until_tick))
            if cost:
                env["CCCP_TEST_SIM_COST_US"] = str(cost)
                if from_tick:
                    env["CCCP_TEST_SIM_COST_FROM_TICK"] = str(from_tick)
                if until_tick:
                    env["CCCP_TEST_SIM_COST_UNTIL_TICK"] = str(until_tick)
            if peer == "client" and options.client_draw_cost_us:
                env["CCCP_TEST_DRAW_COST_US"] = str(options.client_draw_cost_us)
            run = make_run(repo, flags, root / peer, timeout=options.minutes * 60 + 600, env=env)
            runs[peer] = run
            private_settings(run, 60)
            stage_baseline(run, ticks, 2)
            run.start()
            if peer == "host":
                time.sleep(.75)
        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        finishers = {peer: threading.Thread(target=lambda p=peer: records.__setitem__(p, runs[p].finish())) for peer in runs}
        for thread in finishers.values():
            thread.start()
        for thread in finishers.values():
            thread.join()
    finally:
        stop.set()
        for run in runs.values():
            run.close()
        for peer, run in runs.items():
            records.setdefault(peer, run.record)
    elapsed = time.monotonic() - started
    live = compare_live_hashes(root / "host-live.jsonl", root / "client-live.jsonl", 1)
    hashes_equal = bool(live) and all(row["compared_ticks"] > 0 and row["mismatched_ticks"] == 0
                                      and row["mismatched_applied_input_ticks"] == 0 for row in live)
    fullstate = compare_fullstate(root / "host" / "stdout.log", root / "client" / "stdout.log") if options.fullstate_every else None
    holds = count(root / "host" / "stdout.log", "[net-match] hold peer=")
    rejoins = count(root / "client" / "stdout.log", "[net-match] private catch-up complete")
    # Every round's own stall ends in an in-place catch-up, the rematches' rounds included.
    rounds = 1 + (max(1, options.rematches) if options.rematch else 0)
    acceptance = acceptance_history(root, options.end_round_tick or ticks, rounds)
    each_round = count(root / "client" / "stdout.log", "[net-test] live stall frame=", " round_index=")
    in_place = count(root / "client" / "stdout.log", "[net-match] private catch-up complete", " in_place=1")
    # The host is never killed here, so a client that names a new host has split the match in two.
    split = count(root / "client" / "stdout.log", "[net-match] Host left - ")
    autosaves = count(root / "host" / "stdout.log", "[autosave] tick=", "capture_ms=")
    # The cadence is in simulation seconds, so what is owed follows the ticks the host reached, not the wall clock.
    reached = max((row["last_tick"] for row in live), default=0)
    owed = int(reached // (TICKS_PER_SECOND * options.autosave_seconds)) - 1
    minutes_sampled = sum(1 for row in samples if row.get("host") and row.get("client"))
    exits = {peer: {"exit_code": record.get("exit_code"), "timed_out": record.get("timed_out")} for peer, record in records.items()}
    checks = {"exits": all(row["exit_code"] == 0 and not row["timed_out"] for row in exits.values()),
              "hashes_equal": hashes_equal, "fullstate": fullstate is None or bool(fullstate.get("passed")),
              "holds": holds >= options.holds, "rejoins": rejoins >= options.holds, "no_split_brain": split == 0,
              "autosaves": autosaves >= max(0, owed), "memory_sampled": minutes_sampled >= int(options.minutes),
              "complete_history_and_pace": acceptance['pass']}
    return_holds = {peer: return_hold_violations((root / peer / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace'))
                    for peer in ('host', 'client')}
    excused_holds = {peer: excused_return_holds(root, rows) for peer, rows in return_holds.items()}
    checks['no_hold_after_return'] = not any(kept for _, kept in excused_holds.values())
    if options.host_stall:
        checks['host_stalls_fired'] = count(root / 'host/stdout.log', '[net-test] live stall frame=') == len(options.host_stall)
        checks['host_returned'] = count(root / 'host/stdout.log', '[net-match] seat-reclaimed peer=1') >= len(options.host_stall)
    if options.stall_each_round:
        checks["each_round_caught_up"] = each_round >= rounds and in_place >= rounds
    first = next((row for row in samples if row.get("host") and row.get("client")), None)
    last = next((row for row in reversed(samples) if row.get("host") and row.get("client")), None)
    growth = {peer: {"first": first[peer]["working_set"], "last": last[peer]["working_set"],
                     "peak": max(row[peer]["working_set"] for row in samples if row.get(peer))}
              for peer in ("host", "client")} if first and last else None
    # Growth is read from minute 10, past the match's warm-up, to the last sample; the census names the records behind it.
    settled = next((row for row in samples if row.get("host") and row.get("client") and row["elapsed_s"] >= 600), None)
    from_minute_10 = {peer: {"minute_10": settled[peer]["working_set"], "last": last[peer]["working_set"],
                             "percent": round(100.0 * (last[peer]["working_set"] - settled[peer]["working_set"]) / settled[peer]["working_set"], 1)}
                      for peer in ("host", "client")} if settled and last and settled is not last else None
    census = {peer: census_growth(root / peer / "stdout.log", TICKS_PER_SECOND * 600) for peer in ("host", "client")}
    # A capture the writer could not take yet replaces the one still waiting; each replacement is one line.
    coalesced = {peer: count(root / peer / "stdout.log", "[fullstate-coalesced] ") + count(root / peer / "stdout.log", "[autosave-coalesced] ")
                 for peer in ("host", "client")}
    private_mb = {peer: census_private(root / peer / "stdout.log") for peer in ("host", "client")}
    clean_ticks = {peer: terrain_event_ticks(root, peer, "clean") for peer in ("host", "client")} if options.terrain_events else None
    result = {"pass": all(checks.values()), "checks": checks, "exits": exits, "elapsed_s": round(elapsed, 1),
              "holds_after_returns": {peer: kept for peer, (_, kept) in excused_holds.items()},
              "holds_after_returns_excused": {peer: excused for peer, (excused, _) in excused_holds.items()},
              "ticks_reached": reached, "holds_taken": holds, "rejoins_completed": rejoins, "client_named_a_new_host": split,
              "autosaves_published": autosaves, "autosaves_owed": owed,
              "each_round_stalls": each_round, "in_place_catch_ups": in_place, "rounds": rounds,
              "live_hashes": live, "fullstate": fullstate, "memory_samples": len(samples), "memory_working_set": growth,
              "memory_from_minute_10": from_minute_10, "census": census, "census_private_mb": private_mb, "coalesced_captures": coalesced,
              "clean_ticks": clean_ticks, "pace_across_own_seat_hold": pace_across_own_seat_hold(root),
              "plan": plan, "acceptance_history": acceptance}
    (root / "result.json").write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
    print(f"[soak] {'PASS' if result['pass'] else 'FAIL'} {json.dumps(checks)} holds={holds} rejoins={rejoins} "
          f"autosaves={autosaves}/{owed} samples={len(samples)} holds_after_returns={sum(len(kept) for _, kept in excused_holds.values())} -> {root / 'result.json'}", flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
