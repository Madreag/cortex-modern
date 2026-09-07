"""Compare exported PNG pixels with their source bitmaps in an isolated game."""

import argparse
import io
import json
from pathlib import Path
import zipfile

from PIL import Image, ImageChops

from run_sim_test import make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    root = options.out.resolve()
    run = make_run(options.repo, ["-scenario", "SimBaseline", "-seed", 42, "-max-ticks", 3,
                                 "-tick-hashes", "-out", root / "trace.json", "-bitmap-save-selftest"], root, 90)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (root / "stdout.log").read_text(errors="replace")
    checks = {"process": record["exit_code"] == 0 and not record["timed_out"],
              "native": "[bitmap-save-selftest] PASS" in log and ": FAIL" not in log,
              "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
    directory = root / "runtime/ScreenShots"
    details = {}
    for depth in (8, 24, 32):
        for height in (1, 7):
            name = f"bitmap_indexed_{depth}_{height}"
            try:
                with Image.open(directory / f"{name}.ppm") as source, Image.open(directory / f"{name}.png") as output:
                    output.load()
                    diff = ImageChops.difference(source.convert("RGB"), output.convert("RGB"))
                    checks[name] = output.mode == "P" and output.size == (13, height) and diff.getbbox() is None
                    details[name] = {"mode": output.mode, "size": output.size, "difference": diff.getbbox()}
            except Exception as error:
                checks[name] = False
                details[name] = {"error": str(error)}
    for name in ("bitmap_world", "bitmap_preview"):
        try:
            paths = list(directory.glob(name + "_*.png"))
            assert len(paths) == 1, f"expected one PNG, found {len(paths)}"
            with Image.open(paths[0]) as output:
                output.load()
                details[name] = {"path": str(paths[0]), "mode": output.mode, "size": output.size}
                if name == "bitmap_world":
                    with Image.open(directory / (name + ".ppm")) as source:
                        diff = ImageChops.difference(source.convert("RGB"), output.convert("RGB"))
                        checks[name] = source.size == output.size and diff.getbbox() is None
                        details[name]["difference"] = diff.getbbox()
                else:
                    checks[name] = output.mode == "P" and output.size == (170, 80) and any(a != b for a, b in output.convert("RGB").getextrema())
        except Exception as error:
            checks[name] = False
            details[name] = {"error": str(error)}
    try:
        paths = list((root / "runtime").rglob("bitmap_indices.ccsave"))
        assert len(paths) == 1, f"expected one save, found {len(paths)}"
        with zipfile.ZipFile(paths[0]) as archive:
            with Image.open(io.BytesIO(archive.read("Save Mat.png"))) as materials:
                materials.load()
                indices = list(materials.crop((0, 0, 256, 1)).getdata())
                checks["saved_material_indices"] = materials.mode == "P" and indices == list(range(256))
                details["saved_material_indices"] = {"mode": materials.mode, "indices": indices}
    except Exception as error:
        checks["saved_material_indices"] = False
        details["saved_material_indices"] = {"error": str(error)}
    result = {"pass": all(checks.values()), "checks": checks, "details": details, "binary": record["exe_sha256"]}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "checks": checks, "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
