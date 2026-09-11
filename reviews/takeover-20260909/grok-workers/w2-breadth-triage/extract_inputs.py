"""Read-only extraction of breadth v3 inputs. Writes only under this folder."""
import json
import os
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")
ROOT = Path(r"D:\mx\s41b3")


def dump_json(name):
    src = ROOT / name
    data = json.loads(src.read_text(encoding="utf-8", errors="replace"))
    (OUT / f"raw_{name}").write_text(json.dumps(data, indent=2), encoding="utf-8")
    return data


def main():
    listing = []
    for p in sorted(ROOT.iterdir()):
        listing.append(f"{p.name}\t{'DIR' if p.is_dir() else 'FILE'}\t{p.stat().st_size if p.exists() else '?'}")
    (OUT / "s41b3_listing.txt").write_text("\n".join(listing), encoding="utf-8")

    breadth = dump_json("breadth.json")
    plan = dump_json("plan.json")
    drivers = dump_json("drivers.json")
    try:
        dump_json("pin.json")
    except FileNotFoundError:
        (OUT / "raw_pin.json").write_text("MISSING\n", encoding="utf-8")

    # summarize breadth structure
    keys = list(breadth.keys()) if isinstance(breadth, dict) else ["LIST"]
    (OUT / "breadth_keys.txt").write_text("\n".join(map(str, keys)), encoding="utf-8")

    # write compact summary of non-pass
    def walk(obj, path="$"):
        if isinstance(obj, dict):
            for k, v in obj.items():
                yield from walk(v, f"{path}.{k}")
        elif isinstance(obj, list):
            for i, v in enumerate(obj):
                yield from walk(v, f"{path}[{i}]")
        else:
            s = str(obj)
            if any(x in s.lower() for x in ("fail", "non-pass", "nonpass", "error", "red", "reject")):
                yield f"{path}={s[:400]}"

    hits = list(walk(breadth))
    (OUT / "breadth_fail_walk.txt").write_text("\n".join(hits[:2000]), encoding="utf-8")

    # plan/driver keys
    (OUT / "plan_keys.txt").write_text(
        json.dumps(
            {
                "type": type(plan).__name__,
                "keys": list(plan.keys()) if isinstance(plan, dict) else f"list[{len(plan)}]",
                "sample": plan if not isinstance(plan, (dict, list)) else None,
                "driver_type": type(drivers).__name__,
                "driver_keys": list(drivers.keys())[:80] if isinstance(drivers, dict) else f"list[{len(drivers)}]",
            },
            indent=2,
            default=str,
        ),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
