"""Row 513: load retained 7.0 and September 20 saves and play 600 ticks from each.

Inputs must be retained .ccsave files; this check never manufactures an old save. The
source path, bytes, timestamp and SHA-256 are recorded before staging the individual file.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run


def check(repo, root, label, source, declared=None):
    result = {"pass": False, "source": str(source) if source else None}
    if source is None or not source.is_file():
        result["error"] = f"retained {label} .ccsave is required; no replacement fixture was generated"
        return result
    source = source.resolve()
    payload = source.read_bytes()
    result.update(source=str(source), bytes=len(payload), mtime_ns=source.stat().st_mtime_ns,
                  sha256=hashlib.sha256(payload).hexdigest())
    if declared is not None:
        result['input_receipt'] = declared
        if declared.get('label') != label or declared.get('sha256') != result['sha256'] or declared.get('bytes') != len(payload):
            result['error'] = f'retained {label} bytes differ from the shipped input receipt'
            return result
    name = "row513_" + label
    saved_tick = 0
    try:
        with zipfile.ZipFile(source) as archive:
            if "Restore.ini" in archive.namelist():
                for line in archive.read("Restore.ini").decode("utf-8").splitlines():
                    key, separator, value = line.partition("=")
                    if separator and key.strip() == "SavedTick":
                        saved_tick = int(value.strip())
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        result['error'] = f'retained {label} archive: {type(error).__name__}: {error}'
        return result
    probe = root / "probe/script.json"
    probe.parent.mkdir(parents=True)
    probe.write_text(json.dumps({"schema": 1, "timeout_ms": 180000, "steps": [
        {"op": "wait", "screen": "Gameplay"}, {"op": "wait", "sim_at_least": saved_tick + 850},
        {"op": "finish"}]}))
    # Ordinary saved games expose the simulation clock without a multiplayer trace collector.
    run = make_run(repo, ["-load-game", name, "-max-ticks", saved_tick + 900], root / "engine", 300,
                   env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)})
    saved = Path(run.cwd) / "Userdata/UserSavedGames.rte" / f"{name}.ccsave"
    saved.parent.mkdir(parents=True, exist_ok=True)
    # Creating this module before boot also makes its usual bootstrap Index.ini our responsibility.
    (saved.parent / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n"
                                           "\tScanFolderContents = 0\n\tIgnoreMissingItems = 0\n", encoding="utf-8")
    saved.write_bytes(payload)
    try:
        result["record"] = run.start().finish()
        text = (root / "engine/stdout.log").read_text(errors="replace")
        observed = json.loads((probe.parent / "net-ui-result.json").read_text())
        readings = [step["observed"]["sim_frame"] for step in observed.get("steps", [])]
        first, last = readings[0], readings[-1]
        result.update(ticks=last - first, first_tick=first, last_tick=last, saved_tick=saved_tick, probe=observed)
        result["pass"] = (result["record"]["exit_code"] == 0 and not result["record"].get("timed_out")
                          and f'[load-game] loaded "{name}"' in text and observed.get("pass") and last - first >= 600
                          and all(not step["observed"]["paused"] and not step["observed"]["editing"]
                                  for step in observed.get("steps", [])))
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
    parser.add_argument('--inputs', type=Path, help='the shipped save input manifest with source paths and byte hashes')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    try:
        declared = {row['label']: row for row in json.loads(args.inputs.read_text())['inputs']} if args.inputs else None
    except (OSError, ValueError, KeyError, TypeError) as error:
        (args.out/'result.json').write_text(json.dumps(dict(passed=False, error=f'save inputs: {error}'))+'\n')
        return 1
    results = {label: check(args.repo.resolve(), args.out.resolve() / label, label, source,
                           declared.get(label, {}) if declared is not None else None)
               for label, source in (("original7", args.original_7), ("fork0920", args.fork_0920))}
    (args.out / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({label: {key: value for key, value in result.items() if key in ("pass", "error", "ticks", "sha256")}
                      for label, result in results.items()}))
    return 0 if all(result["pass"] for result in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
