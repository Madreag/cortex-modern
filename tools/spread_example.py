"""Worked use of spread_peers.run_case with the unchanged mp-host-join case.

python tools/spread_example.py --out <lane>/example --seat-box Z13
    --pool-dispatcher <installed>/run_on_pool.py --port-block LO-HI --port LO

The callback uses the existing video driver for its staging, start gates,
recording, rendering and every review assertion. No scenario is rewritten.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import e2e_video as video
from spread_peers import Match, Peer, run_case, configure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seat-box", required=True)
    parser.add_argument("--pool-dispatcher", type=Path, required=True)
    parser.add_argument("--pool-registry", type=Path)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--port-block", required=True)
    parser.add_argument("--peer-port", action="append", default=[])
    args = parser.parse_args()
    configure(args)
    original = video.run_one
    def spread_run(options, scenario, definition, index, out):
        root = Path(out)/definition.get("name", f"run{index}")
        size = tuple(map(int, (options.size or scenario["size"]).split("x")))
        peers = [Peer("host", os="windows", size=size, reviewed=True, timeout=scenario["timeout_s"]),
                 Peer("client", os="windows", size=size, timeout=scenario["timeout_s"])]
        def drive(case):
            options.remote_capture = case
            result = original(options, scenario, definition, index, out)
            result.update(topology="spread", peer_boxes=case.result()["peer_boxes"])
            for peer in result["peers"]:
                peer.update(topology="spread", box=case.result()["peer_boxes"][peer["peer"]])
            return result
        result = run_case(options.repo, root, peers, Match(options.port, options.port + 9), drive=drive,
                          peer_boxes=f"host=EROL-PC,seat2={args.seat_box}", dispatcher=args.pool_dispatcher, registry=args.pool_registry)
        return result["driver_result"]
    video.run_one = spread_run
    sys.argv = [str(Path(video.__file__)), "--repo", str(args.repo), "--out", str(args.out), "--scenario", "mp-host-join",
                "--port", str(args.port), "--port-block", args.port_block, "--scratch-limit-bytes", str(3 << 30)]
    code = video.main()
    # Record proof topology on the ordinary aggregate artifacts too.
    for name in ("capture.json", "manifest.json", "review.json"):
        path = args.out/name
        if path.is_file():
            value = json.loads(path.read_text(encoding="utf-8"))
            value["topology"] = "spread"
            value["peer_boxes"] = {"host": "EROL-PC", "client": args.seat_box}
            video.write_json(path, value)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
