"""Two lockstep peers whose windows differ in size must still play the same match.

Runs the net-match service e2e with a per-peer window size, compares the on-wire tick hashes
strictly and, when they part, names the first per-object simdump row that differs.
"""

import argparse
import hashlib
import json
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from compare_sim_traces import strict_compare
from run_sim_test import make_run
from test_telemetry_bundle import set_visual_resolution

PEERS = ("host", "client")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_size(text: str):
    width, _, height = text.partition("x")
    return int(width), int(height)


def peer_args(who: str, port: int, root: Path, ticks: int, seed: int):
    args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
            "-net-match-ticks", str(ticks), "-max-ticks", str(ticks), "-net-match-input-delay", "3",
            "-net-autosave-seconds", "0", "-seed", str(seed), "-num-lua-states", "4", "-tick-hashes",
            "-out", str(root / who / "trace.json"), "-net-match-report", str(root / who / "report.json")]
    return args + (["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"])


def load_dump(path: Path):
    """Group a CC_SIM_DUMP file into per-tick line lists."""
    ticks = {}
    with Path(path).open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            line = line.rstrip("\n")
            head = line.split(" ", 1)[0]
            if not head.isdigit():
                continue
            ticks.setdefault(int(head), []).append(line)
    return ticks


def first_object_divergence(host_dump: Path, client_dump: Path):
    """The first per-object row the two dumps disagree on, with the fields that differ."""
    if not Path(host_dump).exists() or not Path(client_dump).exists():
        return {"available": False, "reason": "a simdump is missing"}
    left, right = load_dump(host_dump), load_dump(client_dump)
    for tick in sorted(set(left) & set(right)):
        if left[tick] == right[tick]:
            continue
        if len(left[tick]) != len(right[tick]):
            return {"available": True, "tick": tick, "census_differs": True,
                    "host_rows": len(left[tick]), "client_rows": len(right[tick])}
        for host_line, client_line in zip(left[tick], right[tick]):
            if host_line == client_line:
                continue
            fields = []
            for host_token, client_token in zip(host_line.split(" "), client_line.split(" ")):
                if host_token != client_token:
                    fields.append({"host": host_token, "client": client_token})
            return {"available": True, "tick": tick, "census_differs": False,
                    "object": " ".join(host_line.split(" ")[1:4]), "fields": fields,
                    "host_line": host_line, "client_line": client_line}
    return {"available": True, "tick": None, "identical": True}


def run_pair(repo: Path, root: Path, port: int, ticks: int, seed: int, sizes, timeout: float):
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    try:
        for who in PEERS:
            env = {"CCCP_HEADLESS": "1", "CC_SIM_DUMP": f"1:{ticks}"}
            runs[who] = make_run(repo, peer_args(who, port, root, ticks, seed), root / who, timeout, env=env)
            set_visual_resolution(runs[who], *sizes[who])

        def drive(who):
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        threads = [threading.Thread(target=drive, args=(who,), daemon=True) for who in PEERS]
        threads[0].start()
        threading.Event().wait(2.0)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48570)
    parser.add_argument("--ticks", type=int, default=900)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--host-size", default="960x540")
    parser.add_argument("--client-size", default="640x360")
    parser.add_argument("--timeout", type=float, default=900)
    options = parser.parse_args()

    sizes = {"host": parse_size(options.host_size), "client": parse_size(options.client_size)}
    records = run_pair(options.repo, options.out, options.port, options.ticks, options.seed, sizes, options.timeout)
    result = {"sizes": {who: list(size) for who, size in sizes.items()}, "ticks": options.ticks,
              "seed": options.seed, "exe_sha256": sha256_file(options.repo / "Cortex Command.exe"),
              "records": records}
    result["processes"] = {who: bool(records.get(who, {}).get("exit_code") == 0 and not records.get(who, {}).get("timed_out"))
                           for who in PEERS}
    traces = {who: options.out / who / "trace.json" for who in PEERS}
    if all(path.exists() for path in traces.values()):
        passed, comparison = strict_compare(traces["host"], traces["client"], options.ticks)
    else:
        passed, comparison = False, {"reasons": ["a peer wrote no trace"]}
    result["simulation"] = comparison
    result["objects"] = first_object_divergence(options.out / "host" / "trace.json.simdump.txt",
                                                options.out / "client" / "trace.json.simdump.txt")
    result["passed"] = bool(passed and all(result["processes"].values()))
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if result["passed"]:
        print(f"PASS window-size sim: {comparison['compared_ticks']} ticks identical at "
              f"{options.host_size} vs {options.client_size}")
    else:
        print("FAIL window-size sim: " + "; ".join(comparison.get("reasons") or ["a peer did not finish"]), file=sys.stderr)
        objects = result["objects"]
        if objects.get("available") and objects.get("tick"):
            print(f"first differing object: tick {objects['tick']} {objects.get('object', 'census')} "
                  f"{json.dumps(objects.get('fields', []))}", file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
