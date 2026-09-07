"""Check completed game saves and failed overwrites during immediate shutdown."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import io
import json
from pathlib import Path
import zipfile

from PIL import Image

from run_sim_test import make_run


def deny_overwrite(path):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    return lambda: kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ui", action="store_true")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    previous = None
    results = {}
    for case in ("success", "overwrite", "missing_directory", "locked_destination", "directory_destination"):
        out = root / case
        name = "missing/save_io" if case == "missing_directory" else "save_io"
        run = make_run(options.repo, ["-scenario", "SimBaseline", "-seed", 42, "-max-ticks", 3,
                                     "-tick-hashes", "-out", out / "trace.json", "-save-menu-selftest" if options.ui else "-save-io-selftest", name], out, 90)
        directory = Path(run.cwd) / "Userdata/UserSavedGames.rte"
        directory.mkdir()
        (directory / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n")
        target = directory / "save_io.ccsave"
        unlock = lambda: None
        if case == "overwrite":
            target.write_bytes(b"old file to replace")
        elif case == "locked_destination":
            assert previous is not None, "a valid previous save is required"
            target.write_bytes(previous)
            unlock = deny_overwrite(target)
        elif case == "directory_destination":
            target.mkdir()
            (target / "previous.txt").write_text("keep")
        try:
            record = run.start().finish()
        finally:
            unlock()
            run.close()
        success = case in ("success", "overwrite")
        log = (out / "stdout.log").read_text(errors="replace")
        console_path = Path(run.cwd) / "LogConsole.txt"
        console = console_path.read_text(errors="replace") if console_path.exists() else ""
        checks = {"exit": record["exit_code"] == (0 if success else 1) and not record["timed_out"],
                  "completed": f"[save-selftest] completed={int(success)}" in log,
                  "cleanup": console_path.is_file(),
                  "report": ('SYSTEM: Game saved to "' in console) == success,
                  "failure_report": success or 'ERROR: Could not save game "' in console,
                  "no_temporary_files": not list(directory.glob("*.tmp.*")),
                  "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
        details = {"binary": record["exe_sha256"]}
        if success:
            if not options.ui:
                checks["pending_at_quit"] = "[save-selftest] queued=1 pending=1" in log
            try:
                with zipfile.ZipFile(target) as archive:
                    checks["archive_integrity"] = archive.testzip() is None
                    checks["complete_archive"] = {"Index.ini", "Save.ini", "Save Mat.png", "Save FG.png", "Save BG.png"} <= set(archive.namelist())
                    for name in archive.namelist():
                        if name.endswith(".png"):
                            with Image.open(io.BytesIO(archive.read(name))) as pixels:
                                pixels.load()
                                checks[name] = pixels.mode == "P" and all(pixels.size)
                previous = target.read_bytes()
            except Exception as error:
                checks["archive_integrity"] = False
                details["error"] = str(error)
        elif case == "locked_destination":
            checks["previous_save_preserved"] = target.read_bytes() == previous
            details["previous_sha256"] = hashlib.sha256(previous).hexdigest()
        elif case == "directory_destination":
            checks["directory_preserved"] = (target / "previous.txt").read_text() == "keep"
        else:
            checks["no_failed_save"] = not (directory / "missing/save_io.ccsave").exists()
        if options.ui:
            checks["menu"] = "[save-menu-selftest] PASS controls=1 pending_text=1 result_text=1" in log
        results[case] = {"pass": all(checks.values()), "checks": checks, "details": details}
        (root / "result.json").write_text(json.dumps({"pass": len(results) == 5 and all(r["pass"] for r in results.values()), "results": results}, indent=2))
        print(json.dumps({"case": case, **results[case]}), flush=True)
    return 0 if all(r["pass"] for r in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
