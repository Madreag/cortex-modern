"""Hour-plus persistent-world soak configuration and retained census/journal receipts."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import stat

from acceptance_runtime import write_json
from feel.report import reduce_memory_census

MINUTES = (10, 30, 50, 60)


def configuration(seconds=3660):
    if type(seconds) is not int or seconds < 3660:
        raise ValueError("world soak must run for at least 61 minutes")
    return dict(elapsed_s=seconds, late_join_elapsed_s=3000, autosave_seconds=60,
                fullstate_every=60, census_every_s=60, census_ticks=3600,
                ticks=seconds*60+1, journal_minutes=list(MINUTES),
                memory=dict(warmup_s=120, slope_bytes_per_minute=8*1024**2,
                            retained_bytes=128*1024**2),
                journal_pruning="not-landed")


def journal_receipt(runtime, minute, elapsed_s):
    if minute not in MINUTES or not minute*60 <= elapsed_s < minute*60+1:
        raise ValueError("journal observation missed its declared minute")
    store = Path(runtime)/"Userdata/UserSavedGames.rte"
    files = []
    if store.is_symlink() or getattr(store, "is_junction", lambda: False)():
        raise ValueError("journal store is a link")
    if store.exists():
        for path in sorted(store.glob("*.ccsave.inputs")):
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("journal is not a regular file")
            files.append(dict(path=path.name, bytes=info.st_size))
    return dict(minute=minute, elapsed_s=elapsed_s, bytes=sum(r["bytes"] for r in files) if files else None,
                files=files, present=bool(files))


def census_receipts(text):
    current = reduce_memory_census(text)
    series = []
    for line in text.splitlines():
        if not line.startswith("[mem-census] "):
            continue
        uptime = re.search(r"\buptime_ms=(\d+)", line)
        process = re.search(r"\b(?:private|resident)_mb=(\d+)", line)
        if uptime and process:
            series.append(dict(uptime_ms=int(uptime[1]), process_bytes=int(process[1])*1024**2))
    return dict(current_reduce_memory_census=current, raw_series=series,
                declared_slope_bytes_per_minute=8*1024**2, declared_retained_bytes=128*1024**2)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=3660)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--census-log", type=Path)
    parser.add_argument("--plan-only", action="store_true")
    args, driver_args = parser.parse_known_args(argv)
    plan = configuration(args.seconds)
    args.out.mkdir(parents=True, exist_ok=False)
    write_json(args.out/"soak-plan.json", plan)
    if args.census_log:
        result = census_receipts(args.census_log.read_text(encoding="utf-8", errors="replace"))
        write_json(args.out/"census.json", result)
        return 0 if result["raw_series"] else 1
    if args.plan_only:
        print(json.dumps(plan, indent=2))
        return 0
    import world_mod_cross
    return world_mod_cross.main(["--acceptance-row", "world-soak", "--out", str(args.out/"run"),
                                 "--ticks", str(plan["ticks"]), *driver_args])


if __name__ == "__main__":
    raise SystemExit(main())
