"""A real abort must dump the rendered frame, including on a hidden/offscreen drawable.

The abort lever runs after the menu frame is rendered. Its dump must exactly match
one of the recorder's final raw RGB frames; no desktop/front-buffer substitution,
pixel masks, tolerances or missing-image pass is accepted.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image

from run_sim_test import make_run


def run_case(repo: Path, out: Path, timeout: float = 90) -> dict:
    out.mkdir(parents=True, exist_ok=False)
    video = out / "video"
    video.mkdir()
    script = out / "abort.menu.txt"
    script.write_text("wait_ms 2000\nassert_screen MainScreen\n"
                      "activate ButtonMainToOptions\nwait_ms 500\n"
                      "assert_screen SettingsScreen\nwait_ms 500\n"
                      "fire_abort deliberate frame readback probe\n", encoding="utf-8")
    run = make_run(repo, ["-menu-script", str(script), "-record-video", str(video),
                          "-record-video-fps", "60"], out / "run", timeout,
                   env={"CCCP_HEADLESS": "1", "CC_RUNNER_IGNORE_FULLSCREEN": "1",
                        "CCCP_TEST_RECORD_ENCODER": ""})
    try:
        launched = run.start().finish()
    finally:
        run.close()
    logs = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in
                     (out / "run/stdout.log", out / "run/stderr.log") if path.is_file())
    aborted = out / "run/runtime/AbortScreen.png"
    defects = []
    if launched.get("exit_code") != 3 or launched.get("timed_out"):
        defects.append(f"abort exit {launched.get('exit_code')}, timeout={launched.get('timed_out')}")
    if "deliberate frame readback probe" not in logs or "because:" not in logs:
        defects.append("original abort reason absent")
    if "0xC0000005" in logs or "GLAD: ERROR" in logs:
        defects.append("GL/secondary crash in frame readback")
    matches = []
    image_hash = None
    if not aborted.is_file():
        defects.append("AbortScreen.png missing: rendered frame readback failed")
    else:
        with Image.open(aborted) as image:
            picture = image.convert("RGB")
            size, pixels = picture.size, picture.tobytes()
        image_hash = hashlib.sha256(pixels).hexdigest()
        for frame in sorted((video / "frames").glob("*.png"))[-60:]:
            with Image.open(frame) as image:
                candidate = image.convert("RGB")
                if candidate.size == size and candidate.tobytes() == pixels:
                    matches.append(frame.name)
        if not matches:
            defects.append("abort dump differs from every final recorded RGB frame")
    scored = {"pass": not defects, "probe": "fail" if defects else "pass", "defects": defects,
              "exit_code": launched.get("exit_code"), "exe_sha256": launched.get("exe_sha256"),
              "raw_rgb_sha256": image_hash, "matching_frames": matches,
              "abort_screen": str(aborted), "launch": str(out / "run/launch.json")}
    (out / "result.json").write_text(json.dumps(scored, indent=2) + "\n", encoding="utf-8")
    return scored


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=90)
    options = parser.parse_args()
    result = run_case(options.repo.resolve(), options.out.resolve(), options.timeout)
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
