"""Run the unchanged scene checks through the published native peer interface."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def stage_package(case, repo):
    """Keep the installed, untracked package in each native private runtime."""
    import vw_battery as battery

    package = repo / "Data/VoidWanderers.rte"
    expected = battery.digest(battery.manifest_lines(package))
    if expected != battery.RECORDED_DIGEST:
        raise RuntimeError("installed Void Wanderers package differs from its recorded digest")
    factory = case.make_run

    def make_run(*args, **kwargs):
        run = factory(*args, **kwargs)
        destination = Path(run.cwd) / "Mods/VoidWanderers.rte"
        shutil.copytree(package, destination)
        actual = battery.digest(battery.manifest_lines(destination))
        if actual != expected:
            raise RuntimeError("native staging changes the Void Wanderers package")
        (Path(run.out) / "mod-manifest.json").write_text(json.dumps({
            "files": len(battery.manifest_lines(destination)), "content_digest": actual,
            "recorded_digest": battery.RECORDED_DIGEST}, indent=2) + "\n", encoding="utf-8")
        return run

    case.make_run = make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", type=Path, required=True)
    parser.add_argument("--shared-tools", type=Path, required=True)
    parser.add_argument("--driver", type=Path)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--case", choices=["purge-view", "team-change", "held-switch", "camera"])
    parser.add_argument("--boundary", choices=["single", "pair", "local", "nested", "revived"])
    parser.add_argument("--peer-boxes")
    parser.add_argument("--network", choices=["ice", "direct"], default="ice")
    parser.add_argument("--port", type=int, default=47650)
    parser.add_argument("--directory-port", type=int, default=47659)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    repo = options.repo.resolve()
    sys.path.insert(0, str(repo / "tools"))
    sys.path.insert(1, str(options.shared_tools.resolve()))
    spread = load("spread_peers", options.interface.resolve())
    parameters = {"network": options.network, "lane": "sol-void-wanderers-scenes-20261005"}

    if options.case or options.boundary:
        if not options.out or options.arguments or (options.case and options.boundary):
            parser.error("a detector needs --out and exactly one case, without scene arguments")
        single = options.boundary in ["single", "nested"]
        names = ["single"] if single else ["host", "client"]
        peers = [spread.Peer(name, os="windows", size=(960, 540), timeout=120) for name in names]
        filename = "mod-void-wanderers-resume.py" if options.case else "mod-void-wanderers-engine.py"
        detector = load("vw_spread_detector", Path(__file__).with_name(filename))

        def drive(case):
            import run_sim_test
            stage_package(case, repo)
            original = run_sim_test.make_run
            run_sim_test.make_run = case.make_run
            arguments = [str(Path(__file__)), "--repo", str(repo), "--out", str(options.out), "--port", str(options.port)]
            if options.case:
                arguments += ["--case", options.case]
                if options.case == "purge-view":
                    arguments += ["--paired"]
            elif options.boundary in ["single", "nested"]:
                arguments += ["--single"] + (["--nested-brain"] if options.boundary == "nested" else [])
            elif options.boundary == "local":
                arguments += ["--local-hook"]
            elif options.boundary == "revived":
                arguments += ["--revived-banner"]
            try:
                sys.argv = arguments
                return detector.main()
            finally:
                run_sim_test.make_run = original

        result = spread.run_case(repo, options.out, peers, spread.Match(options.port, options.directory_port, parameters),
                                 drive=drive, peer_boxes=options.peer_boxes)
        print(json.dumps({key: result[key] for key in ["topology", "peer_boxes", "identities", "driver_result"] if key in result}, indent=2))
        return result["driver_result"]

    if not options.driver or not options.arguments:
        parser.error("a scene needs --driver and its existing driver arguments after --")
    driver = load("vw_spread_driver", options.driver.resolve())
    driver.SCENARIO_DIR = repo / "tools/e2e"

    def run_scene(arguments, scenario, run, index, out):
        definitions = run.get("peers") or scenario.get("peers") or []
        if arguments.dry_run:
            return driver._run_one(arguments, scenario, run, index, out)
        root = Path(out) / run.get("name", f"run{index}")
        size = tuple(map(int, (arguments.size or run.get("size") or scenario.get("size") or "960x540").split("x")))
        peers = [spread.Peer(peer["name"], os="windows", size=size,
                             timeout=run.get("timeout_s") or scenario.get("timeout_s") or 300) for peer in definitions]

        def drive(case):
            stage_package(case, repo)
            arguments.remote_capture = case
            try:
                result = driver._run_one(arguments, scenario, run, index, out)
                result.update(topology="spread" if len(peers) > 1 else "single-peer", peer_boxes=case.result()["peer_boxes"], repo=str(repo))
                for peer in result["peers"]:
                    peer.update(topology=result["topology"], box=result["peer_boxes"][peer["peer"]])
                    peer["record"]["topology"] = result["topology"]
                return result
            finally:
                arguments.remote_capture = None

        result = spread.run_case(repo, root, peers, spread.Match(driver.port_for(index, arguments.port), options.directory_port, parameters),
                                 drive=drive, peer_boxes=options.peer_boxes)
        return result["driver_result"]

    driver.run_one = run_scene
    remaining = options.arguments[1:] if options.arguments[0] == "--" else options.arguments
    sys.argv = [str(options.driver), "--repo", str(repo), "--spread", *remaining]
    return driver.main()


if __name__ == "__main__":
    raise SystemExit(main())
