"""Check save-list metadata and recovery from damaged index files."""

import argparse
import io
import json
from pathlib import Path
import re
import shlex
import struct
import zipfile

from run_sim_test import make_run


CASES = ("valid", "invalid_zip", "missing_index", "empty_index", "malformed_index", "crc_error", "short_entry", "directory")
INDEX = b"ActivityName = Known Activity\nOriginalScenePresetName = Known Scene\n"


def archive(data, include=True):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as zipped:
        if include:
            zipped.writestr("Index.ini", data)
        zipped.writestr("other.txt", "not save metadata")
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, action="append")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    results = {}
    cases = options.case or CASES
    for case in cases:
        data = archive(INDEX)
        if case == "invalid_zip":
            data = b"not an archive"
        elif case == "missing_index":
            data = archive(b"", include=False)
        elif case == "empty_index":
            data = archive(b"")
        elif case == "malformed_index":
            data = archive(b"ActivityName = Known Activity\nThis is not a property\n")
        elif case in ("crc_error", "short_entry"):
            data = bytearray(data)
            central = data.index(b"PK\x01\x02")
            if case == "crc_error":
                value = struct.unpack_from("<I", data, central + 16)[0] ^ 1
                struct.pack_into("<I", data, central + 16, value)
                struct.pack_into("<I", data, 14, value)
            else:
                value = struct.unpack_from("<I", data, central + 24)[0] + 100
                struct.pack_into("<I", data, central + 24, value)
                struct.pack_into("<I", data, 22, value)
        out = root / case
        run = make_run(options.repo, ["-scenario", "SimBaseline", "-seed", 42, "-max-ticks", 3,
                                     "-tick-hashes", "-out", out / "trace.json", "-save-catalog-selftest"], out, 45)
        directory = Path(run.cwd) / "Userdata/UserSavedGames.rte"
        directory.mkdir()
        (directory / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n")
        (directory / "valid.ccsave").write_bytes(archive(INDEX))
        if case == "directory":
            (directory / "candidate.ccsave").mkdir()
        else:
            (directory / "candidate.ccsave").write_bytes(data)
        try:
            record = run.start().finish()
        finally:
            run.close()
        log = (out / "stdout.log").read_text(errors="replace")
        rows = {}
        for line in log.splitlines():
            if line.startswith('[save-catalog] "'):
                name, scene, activity = shlex.split(line.removeprefix("[save-catalog] "))
                rows[name] = (scene, activity)
        expected = {"valid": ("Known Scene", "Known Activity")}
        if case != "directory":
            expected["candidate"] = ("Known Scene", "Known Activity") if case == "valid" else ("", "Save details unavailable")
        checks = {"exit": record["exit_code"] == 0 and not record["timed_out"],
                  "rows": rows == expected,
                  "complete": f"[save-catalog] complete=1 count={len(expected)}" in log,
                  "cleanup": (Path(run.cwd) / "LogConsole.txt").is_file(),
                  "no_abort": not re.search(r"RTE (?:Assert|Abort)|Runtime Error", log),
                  "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
        results[case] = {"pass": all(checks.values()), "checks": checks, "rows": rows,
                         "binary": record["exe_sha256"], "exit_code": record["exit_code"]}
        (root / "result.json").write_text(json.dumps({"pass": len(results) == len(cases) and all(r["pass"] for r in results.values()), "results": results}, indent=2))
        print(json.dumps({"case": case, **results[case]}), flush=True)
    return 0 if all(r["pass"] for r in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
