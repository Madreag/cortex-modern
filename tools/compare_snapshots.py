#!/usr/bin/env python
"""Compare two lockstep save-game snapshots taken at the same synced tick.

The SIM payload (the Save.ini `Scene` block + every PNG layer) must be byte-identical across
peers; the `Activity` block carries legitimate per-peer view state (player bindings, screens),
so its diff is reported but tolerated. Exit 0 = sim payload identical.
"""

import argparse
import hashlib
import re
import sys
import zipfile


def split_top_level(text: str) -> dict:
    """Split an ini stream into top-level properties (lines with no leading tab start a block)."""
    blocks = {}
    key = None
    lines = []
    for line in text.splitlines(keepends=True):
        if line.strip() and not line.startswith(("\t", " ")):
            if key is not None:
                blocks.setdefault(key, []).append("".join(lines))
            key = line.split("=", 1)[0].strip()
            lines = [line]
        else:
            lines.append(line)
    if key is not None:
        blocks.setdefault(key, []).append("".join(lines))
    return blocks


def normalize_snapshot_name(text: str, name: str) -> str:
    pattern = r"(?m)^(\t{1,2}PresetName[ \t]*=[ \t]*)" + re.escape(name) + r"([ \t]*\r?)$"
    return re.sub(pattern, lambda match: match[1] + "SNAPSHOT" + match[2], text)


# Each peer runs as PlayerOne on its own team, so the local player's slot bindings legitimately
# differ per peer. Everything else in the Activity block (ActivityState, team funds, generic saved
# values, CPU team, delivery delay, techs, ...) is sim state and MUST match across peers.
_ACTIVITY_LOCAL_VIEW = {
    "TeamOfPlayer1",
    "FundsContributionOfPlayer1",
    "TeamFundsShareOfPlayer1",
    "Player1IsHuman",
}


def split_activity_props(block: str) -> dict:
    """Split an Activity block into its depth-1 properties (one leading tab). Values are lists to
    tolerate a repeated key."""
    props: dict = {}
    key = None
    lines: list = []
    for line in block.splitlines(keepends=True):
        if line.startswith("\t") and not line.startswith("\t\t"):
            if key is not None:
                props.setdefault(key, []).append("".join(lines))
            key = line[1:].split("=", 1)[0].strip()
            lines = [line]
        elif key is not None:
            lines.append(line)
    if key is not None:
        props.setdefault(key, []).append("".join(lines))
    return props


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("snapshot_a")
    parser.add_argument("snapshot_b")
    args = parser.parse_args()

    # The save bakes its OWN file name into the scene/terrain PresetNames — metadata, not sim
    # state. Normalize both to a fixed token before comparing.
    import os

    base_a = os.path.splitext(os.path.basename(args.snapshot_a))[0]
    base_b = os.path.splitext(os.path.basename(args.snapshot_b))[0]

    with zipfile.ZipFile(args.snapshot_a) as za, zipfile.ZipFile(args.snapshot_b) as zb:
        names_a = sorted(za.namelist())
        names_b = sorted(zb.namelist())
        if len(names_a) != len(set(names_a)) or len(names_b) != len(set(names_b)):
            print("FAIL: duplicate archive entries")
            return 1
        if "Save.ini" not in names_a or "Save.ini" not in names_b:
            print("FAIL: Save.ini is missing")
            return 1
        if names_a != names_b:
            print(f"FAIL: entry lists differ: {names_a} vs {names_b}")
            return 1

        failures = []
        activity_diff_lines = 0
        for name in names_a:
            data_a = za.read(name)
            data_b = zb.read(name)
            if name == "Save.ini":
                text_a = normalize_snapshot_name(data_a.decode("utf-8"), base_a)
                text_b = normalize_snapshot_name(data_b.decode("utf-8"), base_b)
                blocks_a = split_top_level(text_a)
                blocks_b = split_top_level(text_b)
                for side, blocks in (("a", blocks_a), ("b", blocks_b)):
                    for required in ("Scene", "Activity"):
                        if len(blocks.get(required, [])) != 1:
                            failures.append(f"Save.ini {side} needs exactly one '{required}' block")
                if set(blocks_a) != set(blocks_b):
                    failures.append(
                        f"Save.ini top-level properties differ: {sorted(blocks_a)} vs {sorted(blocks_b)}"
                    )
                    continue
                for key in blocks_a:
                    if blocks_a[key] == blocks_b[key]:
                        continue
                    if key == "Activity":
                        if len(blocks_a[key]) != 1 or len(blocks_b[key]) != 1:
                            continue
                        # Compare every Activity property; only the local-player view fields may differ.
                        props_a = split_activity_props(blocks_a[key][0])
                        props_b = split_activity_props(blocks_b[key][0])
                        for prop in sorted(set(props_a) | set(props_b)):
                            if props_a.get(prop, []) == props_b.get(prop, []):
                                continue
                            if prop in _ACTIVITY_LOCAL_VIEW:
                                activity_diff_lines += 1
                            else:
                                failures.append(
                                    f"Save.ini Activity property '{prop}' differs across peers (sim state)"
                                )
                    else:
                        failures.append(f"Save.ini '{key}' block differs (sim payload)")
            elif data_a != data_b:
                ha = hashlib.sha256(data_a).hexdigest()[:12]
                hb = hashlib.sha256(data_b).hexdigest()[:12]
                failures.append(
                    f"{name} differs ({len(data_a)}B {ha} vs {len(data_b)}B {hb})"
                )

        if failures:
            for failure in failures:
                print(f"FAIL: {failure}")
            return 1
        print(
            f"PASS: sim payload byte-identical across peers "
            f"({len(names_a)} entries; activity view-block diff lines: {activity_diff_lines})"
        )
        return 0


if __name__ == "__main__":
    sys.exit(main())
