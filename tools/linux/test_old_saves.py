"""Row 513: load retained 7.0 and September 20 saves and play 600 ticks from each.

Inputs must be retained .ccsave files; this check never manufactures an old save. The
source path, bytes, timestamp and SHA-256 are recorded before staging the individual file.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_sim_traces import load_trace
from run_sim_test import make_run


def check(repo, root, label, source):
    result = {"pass": False, "source": str(source) if source else None}
    if source is None or not source.is_file():
        result["error"] = f"retained {label} .ccsave is required; no replacement fixture was generated"
        return result
    source = source.resolve()
    payload = source.read_bytes()
    result.update(source=str(source), bytes=len(payload), mtime_ns=source.stat().st_mtime_ns,
                  sha256=hashlib.sha256(payload).hexdigest())
    name = "row513_" + label
    run = make_run(repo, ["-load-game", name, "-max-ticks", 600, "-tick-hashes", "-out", root / "trace.json"],
                   root / "engine", 300, env={"CCCP_HEADLESS": "1"})
    saved = Path(run.cwd) / "Userdata/UserSavedGames.rte" / f"{name}.ccsave"
    saved.parent.mkdir(parents=True, exist_ok=True)
    # Creating this module before boot also makes its usual bootstrap Index.ini our responsibility.
    (saved.parent / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n"
                                           "\tScanFolderContents = 0\n\tIgnoreMissingItems = 0\n", encoding="utf-8")
    saved.write_bytes(payload)
    try:
        result["record"] = run.start().finish()
        text = (root / "engine/stdout.log").read_text(errors="replace")
        ticks, info = load_trace(root / "trace.json")
        result.update(ticks=len(ticks), first_tick=min(ticks), last_tick=max(ticks), trace=info)
        result["pass"] = (result["record"]["exit_code"] == 0 and not result["record"].get("timed_out")
                          and f'[load-game] loaded "{name}"' in text and len(ticks) == 600
                          and max(ticks) - min(ticks) == 599)
    except Exception as error:
        result["error"] = repr(error)
    finally:
        run.close()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--original-7", type=Path)
    parser.add_argument("--fork-0920", type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    results = {label: check(args.repo.resolve(), args.out.resolve() / label, label, source)
               for label, source in (("original7", args.original_7), ("fork0920", args.fork_0920))}
    (args.out / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({label: {key: value for key, value in result.items() if key in ("pass", "error", "ticks", "sha256")}
                      for label, result in results.items()}))
    return 0 if all(result["pass"] for result in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
