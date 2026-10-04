"""A game loaded from a save saves again: the Save Game menu after Load Game, generation after generation.

Inputs are the retained .ccsave files test_old_saves reads (a September 20 fork save and an original 7.0 save); they are
staged byte for byte and never edited. Rows:
  fork0920_resave             Load Game of the fork save, then the Save Game menu saves it, and the save it wrote loads and saves again.
  original7_second_generation Load Game of the 7.0 save, Save Game, Load Game of that save, Save Game again.
  original7_unique_ids        After Load Game of the 7.0 save the unique ID counter stands at or above every live object's ID.
--chain LABEL=PATH:TICKS repeats load -> play TICKS -> save --cycles times from PATH, then loads the last save and plays TICKS.
"""

import argparse
import json
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run

REFUSED = re.compile(r"^\[scriptgraph\] save refused: (.*)$", re.M)


def stage(run, name, payload):
    folder = Path(run.cwd) / "Userdata/UserSavedGames.rte"
    folder.mkdir(parents=True, exist_ok=True)
    # Creating this module before boot also makes its usual bootstrap Index.ini our responsibility.
    (folder / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n"
                                      "\tScanFolderContents = 0\n\tIgnoreMissingItems = 0\n", encoding="utf-8")
    (folder / f"{name}.ccsave").write_bytes(payload)
    return folder


def load_and_save(repo, root, source, written, menu=True, after=0, env=None):
    """Load source, play `after` ticks, save as `written`; the save's outcome, its refusals and the written bytes' path."""
    args = ["-load-game", "loaded", "-save-menu-selftest" if menu else "-save-io-selftest", written]
    if after:
        args += ["-save-io-selftest-after", str(after)]
    run = make_run(repo, args, root / "engine", 400, env={"CCCP_HEADLESS": "1", **(env or {})})
    folder = stage(run, "loaded", Path(source).read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()
    text = (root / "engine/stdout.log").read_text(errors="replace")
    produced = folder / f"{written}.ccsave"
    kept = root / f"{written}.ccsave"
    if produced.is_file():
        shutil.copyfile(produced, kept)
    played = re.findall(r"^\[save-selftest\] played (\d+) ticks from (\d+)", text, re.M)
    census = re.search(r"^\[load-game\] uid_counter=(-?\d+) highest_known=(-?\d+)", text, re.M)
    result = {"exit": record.get("exit_code"), "timed_out": record.get("timed_out"), "loaded": '[load-game] loaded "loaded"' in text,
              "saved": "[save-selftest] completed=1" in text, "refusals": len(REFUSED.findall(text)), "first_refusals": REFUSED.findall(text)[:3],
              "game_over": "Cannot save when there's no game running, or the game is finished!" in
                           (Path(run.cwd) / "LogConsole.txt").read_text(errors="replace") if (Path(run.cwd) / "LogConsole.txt").is_file() else False,
              "played": [int(ticks) for ticks, _ in played], "save": str(kept) if kept.is_file() else None,
              "census": [int(census.group(1)), int(census.group(2))] if census else None}
    if menu:
        result["menu_pass"] = "[save-menu-selftest] PASS" in text
    return result


def saved_cleanly(result):
    return result["loaded"] and result["saved"] and result["refusals"] == 0 and result["save"] is not None and result.get("menu_pass", True)


def fork0920_resave(repo, root, fork0920):
    first = load_and_save(repo, root / "first", fork0920, "resaved")
    second = load_and_save(repo, root / "second", first["save"], "resaved_again") if first["save"] else None
    return {"pass": saved_cleanly(first) and second is not None and saved_cleanly(second), "first": first, "second": second}


def original7_second_generation(repo, root, original7):
    first = load_and_save(repo, root / "first", original7, "generation1")
    second = load_and_save(repo, root / "second", first["save"], "generation2") if first["save"] else None
    return {"pass": saved_cleanly(first) and second is not None and saved_cleanly(second), "first": first, "second": second}


def original7_unique_ids(repo, root, original7):
    loaded = load_and_save(repo, root, original7, "census", env={"CC_TEST_UID_CENSUS": "1"})
    census = loaded["census"]
    # Every object made after the load draws above the counter, so no live object may hold an ID above it.
    return {"pass": loaded["loaded"] and census is not None and census[0] >= census[1], "census": census, "load": loaded}


def chain(repo, root, source, ticks, cycles):
    rows, current = [], Path(source)
    for cycle in range(1, cycles + 1):
        row = load_and_save(repo, root / f"cycle{cycle}", current, f"cycle{cycle}", menu=False, after=ticks)
        rows.append(row)
        if not saved_cleanly(row) or row["played"] != [ticks]:
            break
        current = Path(row["save"])
    reload = None
    if len(rows) == cycles and saved_cleanly(rows[-1]) and rows[-1]["played"] == [ticks]:
        reload = load_and_save(repo, root / "reload", current, "reload", menu=False, after=ticks)
    accepted = sum(1 for row in rows if saved_cleanly(row) and row["played"] == [ticks])
    return {"pass": accepted == cycles and reload is not None and saved_cleanly(reload) and reload["played"] == [ticks],
            "ticks": ticks, "saves_attempted": len(rows), "saves_accepted": accepted, "rows": rows, "reload": reload}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--original-7", type=Path)
    parser.add_argument("--fork-0920", type=Path)
    parser.add_argument("--rows", default="fork0920_resave,original7_second_generation,original7_unique_ids")
    parser.add_argument("--chain", action="append", default=[], metavar="LABEL=PATH:TICKS")
    parser.add_argument("--cycles", type=int, default=3)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    repo, out = args.repo.resolve(), args.out.resolve()
    inputs = {"fork0920_resave": args.fork_0920, "original7_second_generation": args.original_7, "original7_unique_ids": args.original_7}
    rows = {"fork0920_resave": fork0920_resave, "original7_second_generation": original7_second_generation, "original7_unique_ids": original7_unique_ids}
    results = {}
    for name in [row for row in args.rows.split(",") if row]:
        source = inputs[name]
        if source is None or not source.is_file():
            results[name] = {"pass": False, "error": f"retained save for {name} is required; no replacement fixture was generated"}
            continue
        results[name] = rows[name](repo, out / name, source.resolve())
    for spec in args.chain:
        label, _, rest = spec.partition("=")
        path, _, ticks = rest.rpartition(":")
        results[f"chain_{label}"] = chain(repo, out / f"chain_{label}", Path(path).resolve(), int(ticks), args.cycles)
    (out / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({name: {key: value for key, value in result.items() if key in ("pass", "error", "census", "saves_attempted", "saves_accepted")}
                      for name, result in results.items()}))
    return 0 if results and all(result["pass"] for result in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
