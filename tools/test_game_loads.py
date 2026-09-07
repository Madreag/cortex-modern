"""Exercise valid, corrupt and incomplete saves with an already staged game."""

import argparse
import io
import json
from pathlib import Path
import struct
import zipfile

from PIL import Image

from run_sim_test import make_run


CASES = ("success", "legacy_rgb", "missing_file", "empty_archive", "missing_ini", "empty_ini",
         "missing_material", "missing_fg", "missing_bg", "invalid_png", "crc_error",
         "truncated_archive", "short_entry", "mismatched_dimensions", "wrong_activity_type", "bad_number")


def pack(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return output.getvalue()


def png(data, pixel=None, mode=None, size=None):
    with Image.open(io.BytesIO(data)) as image:
        image.load()
        if pixel is not None:
            image.putpixel((0, 0), pixel)
        if mode:
            image = image.convert(mode)
        if size:
            image = image.resize(size)
        output = io.BytesIO()
        image.save(output, format="PNG")
        return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--timeout", type=float, default=90)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(options.source) as archive:
        assert archive.testzip() is None
        seed = {name: archive.read(name) for name in archive.namelist()}
    seed["Save Mat.png"] = png(seed["Save Mat.png"], pixel=30)
    cases = options.case or CASES
    results = {}
    for case in cases:
        entries = dict(seed)
        entries["Save Mat.png"] = png(entries["Save Mat.png"], pixel=28)
        if case == "legacy_rgb":
            entries = {name: png(data, mode="RGBA") if name.endswith(".png") else data for name, data in entries.items()}
        elif case == "empty_archive":
            entries.clear()
        elif case in ("missing_ini", "missing_material", "missing_fg", "missing_bg"):
            entries.pop({"missing_ini": "Save.ini", "missing_material": "Save Mat.png",
                         "missing_fg": "Save FG.png", "missing_bg": "Save BG.png"}[case])
        elif case == "empty_ini":
            entries["Save.ini"] = b""
        elif case == "invalid_png":
            entries["Save FG.png"] = b"invalid image data"
        elif case == "mismatched_dimensions":
            entries["Save FG.png"] = png(entries["Save FG.png"], size=(13, 7))
        elif case == "wrong_activity_type":
            entries["Save.ini"] = entries["Save.ini"].replace(b"Activity = GAScripted", b"Activity = InvalidActivity", 1)
        elif case == "bad_number":
            entries["Save.ini"] += b"\nSimUpdateCount = invalid\n"
        candidate = pack(entries)
        if case in ("crc_error", "short_entry"):
            with zipfile.ZipFile(io.BytesIO(candidate)) as archive:
                offset = archive.getinfo("Save FG.png").header_offset
            central = candidate.index(b"PK\x01\x02")
            while True:
                length, extra, comment = struct.unpack_from("<HHH", candidate, central + 28)
                if candidate[central + 46:central + 46 + length] == b"Save FG.png":
                    break
                central += 46 + length + extra + comment
            candidate = bytearray(candidate)
            if case == "crc_error":
                value = struct.unpack_from("<I", candidate, central + 16)[0] ^ 1
                struct.pack_into("<I", candidate, central + 16, value)
                struct.pack_into("<I", candidate, offset + 14, value)
            else:
                value = struct.unpack_from("<I", candidate, central + 24)[0] + 100
                struct.pack_into("<I", candidate, central + 24, value)
                struct.pack_into("<I", candidate, offset + 22, value)
        elif case == "truncated_archive":
            candidate = candidate[:len(candidate) // 2]
        out = root / case
        success = case in ("success", "legacy_rgb")
        run = make_run(options.repo, ["-scenario", "SimBaseline", "-seed", 42, "-max-ticks", 3,
                                     "-tick-hashes", "-out", out / "trace.json", "-load-io-success-selftest" if success else "-load-io-selftest", "load_candidate"], out, options.timeout)
        directory = Path(run.cwd) / "Userdata/UserSavedGames.rte"
        directory.mkdir()
        (directory / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n")
        (directory / "load_seed.ccsave").write_bytes(pack(seed))
        if case != "missing_file":
            (directory / "load_candidate.ccsave").write_bytes(candidate)
        try:
            record = run.start().finish()
        finally:
            run.close()
        log = (out / "stdout.log").read_text(errors="replace")
        checks = {"exit": record["exit_code"] == 0 and not record["timed_out"],
                  "native": f"[load-selftest] PASS loaded={int(success)} preserved=1 restarted=1 terrain=1" in log,
                  "cleanup": (Path(run.cwd) / "LogConsole.txt").is_file(),
                  "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
        results[case] = {"pass": all(checks.values()), "checks": checks, "binary": record["exe_sha256"],
                         "exit_code": record["exit_code"], "timed_out": record["timed_out"]}
        (root / "result.json").write_text(json.dumps({"pass": len(results) == len(cases) and all(r["pass"] for r in results.values()), "results": results}, indent=2))
        print(json.dumps({"case": case, **results[case]}), flush=True)
    return 0 if all(r["pass"] for r in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
