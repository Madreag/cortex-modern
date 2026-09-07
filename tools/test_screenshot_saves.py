"""Check rendered PNGs, save reporting, and pending saves at process shutdown."""

import argparse
import json
from pathlib import Path

from PIL import Image

from run_sim_test import make_run


CASES = {
    "screen": (["menu_first", "menu_second"], 50, True),
    "immediate_exit": (["menu_last"], 0, True),
    "missing_directory": (["missing/fail"], 50, False),
    "failed_immediate_exit": (["missing/last"], 0, False),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    results = {}
    for name, (names, wait, success) in CASES.items():
        script = root / f"{name}.txt"
        commands = "".join(f"screenshot {n}\n" for n in names)
        script.write_text(f"wait 40\nassert_screen MainScreen\n{commands}wait {wait}\nexit\n", encoding="utf-8")
        out = root / name
        run = make_run(options.repo, ["-menu-script", script], out, 90)
        try:
            record = run.start().finish()
        finally:
            run.close()
        images = []
        for path in (out / "runtime/ScreenShots").glob("*.png"):
            try:
                with Image.open(path) as image:
                    image.load()
                    extrema = image.convert("RGB").getextrema()
                    images.append({"path": str(path), "size": image.size, "extrema": extrema,
                                   "valid": image.size == (960, 540) and any(a != b for a, b in extrema)})
            except Exception as error:
                images.append({"path": str(path), "error": str(error), "valid": False})
        log = (out / "runtime/LogConsole.txt").read_text(errors="replace")
        steps = (out / "stdout.log").read_text(errors="replace")
        expected = len(names) if success else 0
        checks = {
            "process": record["exit_code"] == 0 and not record["timed_out"],
            "steps": "actual=MainScreen PASS" in steps and "[menu-script] FAIL" not in steps,
            "png_count": len(images) == expected,
            "pixels": all(image["valid"] for image in images),
            "success_messages": log.count("SYSTEM: Screen was dumped to:") == expected,
            "error_messages": log.count("ERROR: Unable to save bitmap to:") == (0 if success else len(names)),
            "desktop": record["input_desktop_before"] == record["input_desktop_after"],
        }
        result = {"pass": all(checks.values()), "checks": checks, "images": images, "binary": record["exe_sha256"]}
        results[name] = result
        (root / "result.json").write_text(json.dumps({"pass": len(results) == len(CASES) and all(r["pass"] for r in results.values()),
                                                     "results": results}, indent=2), encoding="utf-8")
        print(json.dumps({"case": name, "pass": result["pass"], "checks": checks}), flush=True)
    return 0 if all(r["pass"] for r in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
