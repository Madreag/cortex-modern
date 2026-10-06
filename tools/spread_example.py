"""Worked use of spread_peers.run_case with the unchanged mp-host-join case.

python tools/spread_example.py --out <lane>/example
    --peer-boxes host=<BOX>,seat2=<BOX> --pool-registry <installed>/boxes.json
    --runner-label <lane> --port-block LO-HI --port LO

The callback uses the existing video driver for its staging, start gates,
recording, rendering and every review assertion. No scenario is rewritten.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import e2e_video as video
from spread_peers import Match, Peer, run_case, configure, add_arguments, named_peer_boxes, pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    add_arguments(parser)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--port-block", required=True)
    args = parser.parse_args()
    configure(args)
    requested = named_peer_boxes([Peer('host'), Peer('client')], pairs(args.peer_boxes))
    original = video._run_one
    def spread_run(options, scenario, definition, index, out):
        root = Path(out)/definition.get("name", f"run{index}")
        size = tuple(map(int, (options.size or scenario["size"]).split("x")))
        peers = [Peer("host", os="windows", size=size, reviewed=True, recorder=True, timeout=scenario["timeout_s"]),
                 Peer("client", os="any", size=size, timeout=scenario["timeout_s"])]
        def drive(case):
            options.remote_capture = case
            result = original(options, scenario, definition, index, out)
            result.update(topology="spread", peer_boxes=case.result()["peer_boxes"])
            for peer in result["peers"]:
                peer.update(topology="spread", box=case.result()["peer_boxes"][peer["peer"]])
                peer["record"].update(topology="spread", box=peer["box"])
            return result
        result = run_case(options.repo, root, peers, Match(options.port, options.port + 9,
                          parameters={'label': args.runner_label, 'runner_wait': args.runner_wait,
                                      'wait_for_holder': args.wait_for_holder}), drive=drive,
                          peer_boxes=args.peer_boxes, dispatcher=args.pool_dispatcher, registry=args.pool_registry)
        return result["driver_result"]
    video.run_one = spread_run
    sys.argv = [str(Path(video.__file__)), "--repo", str(args.repo), "--out", str(args.out), "--scenario", "mp-host-join",
                "--port", str(args.port), "--port-block", args.port_block, "--scratch-limit-bytes", str(3 << 30)]
    sys.argv += ['--peer-boxes', args.peer_boxes]
    for option in ('pool_dispatcher', 'pool_registry', 'runner_label', 'runner_wait', 'wait_for_holder'):
        if getattr(args, option):
            sys.argv += ['--' + option.replace('_', '-'), str(getattr(args, option))]
    for peer_port in args.peer_port:
        sys.argv += ["--peer-port", peer_port]
    code = video.main()
    # Record proof topology on the ordinary aggregate artifacts too.
    for name in ("capture.json", "manifest.json", "review.json"):
        path = args.out/name
        if path.is_file():
            value = json.loads(path.read_text(encoding="utf-8"))
            value["topology"] = "spread"
            receipt = args.out/"run0/spread-result.json"
            value["peer_boxes"] = json.loads(receipt.read_text(encoding="utf-8")).get("peer_boxes", {}) if receipt.is_file() else {}
            value["requested_peer_boxes"] = requested
            video.write_json(path, value)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
