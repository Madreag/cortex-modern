"""Row 516: the Network page retains a community directory URL exactly across save."""

import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run, seed_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    url = "https://community.example.invalid/custom-directory"
    script = root / "settings.menu.txt"
    script.write_text("wait 40\nactivate ButtonMainToOptions\nwait 10\n"
                      "select_settings_page Network:Internet\nwait 5\n"
                      f"set_text TextNetworkDirUrl {url}\nassert_label TextNetworkDirUrl {url}\n"
                      "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n")
    run = make_run(args.repo.resolve(), ["-menu-script", script], root / "engine", 120, env={"CCCP_HEADLESS": "1"})
    seed_settings(run, {"SessionDirectoryUrl": "seed.example.invalid/serve"})
    result = {"pass": False, "url": url}
    try:
        record = run.start().finish()
        text = (root / "engine/stdout.log").read_text(errors="replace")
        settings = (Path(run.cwd) / "Userdata/Settings.ini").read_text(errors="replace")
        saved = re.search(r"(?m)^\s*SessionDirectoryUrl\s*=([^\r\n]*)", settings)
        result.update(record=record, saved=saved[1].strip() if saved else None)
        result["pass"] = record["exit_code"] == 0 and f'text="{url}" PASS' in text and result["saved"] == url
    finally:
        run.close()
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "record"}))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
