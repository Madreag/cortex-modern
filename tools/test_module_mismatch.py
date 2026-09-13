"""Show a mod-mismatch refusal on the joiner's landing status label, in both directions.

Two phases, each a real host plus one joiner on loopback:

    client-extra (port 47721)  Extra.rte staged in the joiner's runtime/Mods -> joiner must Remove it
    host-extra   (port 47722)  Extra.rte staged in the host's runtime/Mods    -> joiner must Install it

The module is a minimal DataModule written into the private runtime's Mods directory after
make_run() stages it and before run.start() - the repo's Data/ and the installed Mods/ are
never touched. The joiner's LabelMultiplayerLandingStatus text is read back through the
existing assert_label seam; the driver then checks the first-line break and the named group.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re

from run_sim_test import make_run
from test_lobby_lifecycle import wait_for_log


LABEL = "LabelMultiplayerLandingStatus"
PREFIX = "This host's mods do not match yours."
MODULE = "Extra.rte"


def game_version(repo):
    header = Path(repo) / "Source/System/GameVersion.h"
    match = re.search(r'c_VersionString\s*=\s*"([^"]+)"', header.read_text(encoding="utf-8"))
    if not match:
        raise RuntimeError("could not read c_VersionString from " + str(header))
    return match.group(1)


def stage_extra_module(run, version):
    """Write a minimal valid DataModule into this run's private runtime/Mods before start()."""
    module_dir = run.cwd / "Mods" / MODULE
    module_dir.mkdir()
    (module_dir / "Index.ini").write_text(
        "DataModule\n\tModuleName = Extra\n\tSupportedGameVersion = " + version + "\n",
        encoding="utf-8")
    return module_dir


def label_text(log):
    """The landing label's full text, echoed by assert_label; may span lines once formatted."""
    match = re.search(r'assert_label ' + LABEL + ' "yours" text="(.*?)" (?:PASS|FAIL)', log, re.S)
    return match.group(1) if match else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port-client-extra", type=int, default=47721)
    parser.add_argument("--port-host-extra", type=int, default=47722)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    version = game_version(options.repo)
    result = {"pass": False, "version": version}
    checks, details = {}, {}
    runs, records = {}, {}

    def start(phase, name, host, port, suffix, extra_module):
        script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
        if host:
            script += f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {port}\nsettext TextHostPlayers 2\nsettext TextHostInputDelay 3\nactivate ButtonMultiplayerCreate\n"
        else:
            script += f"activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\n"
        path = root / phase / f"{name}.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(script + suffix, encoding="utf-8")
        args = ["-menu-script", path, "-num-lua-states", 4]
        run = make_run(options.repo, args, root / phase / name, 150)
        if extra_module:
            details[phase]["staged_module"] = str(stage_extra_module(run, version))
        runs[(phase, name)] = run.start()
        return runs[(phase, name)]

    try:
        # phase name -> (port, side that carries Extra.rte, verb the joiner must be told)
        phases = [
            ("client-extra", options.port_client_extra, "Joiner", "Remove"),
            ("host-extra", options.port_host_extra, "Host", "Install"),
        ]
        for phase, port, extra_side, verb in phases:
            details[phase] = {"port": port, "extra_module_on": extra_side, "expected_verb": verb}
            host_script = ("wait_error could not join\nassert_substate Lobby\n"
                           "assert_enabled ButtonMultiplayerStart 0\nassert_error could not join\n"
                           "dump_lobby\nscreenshot host-mod-mismatch\nexit\n")
            host = start(phase, "Host", True, port, host_script, extra_side == "Host")
            wait_for_log(host, "activate ButtonMultiplayerCreate ok=1")
            joiner_script = (f"wait_state Failed\nwait 5\nassert_substate Landing\n"
                             f"assert_label {LABEL} yours\nassert_enabled ButtonMultiplayerJoinGame 1\n"
                             "dump_lobby\nscreenshot joiner-landing-status\nexit\n")
            start(phase, "Joiner", False, port, joiner_script, extra_side == "Joiner")
            records[(phase, "Joiner")] = runs[(phase, "Joiner")].finish()
            records[(phase, "Host")] = runs[(phase, "Host")].finish()

        for phase, port, extra_side, verb in phases:
            host_log = (root / phase / "Host/stdout.log").read_text(errors="replace")
            joiner_log = (root / phase / "Joiner/stdout.log").read_text(errors="replace")
            for name, log in (("Host", host_log), ("Joiner", joiner_log)):
                record = records[(phase, name)]
                console = root / phase / name / "runtime/LogConsole.txt"
                full_log = log + ("\n" + console.read_text(errors="replace") if console.exists() else "")
                errors = re.findall(r"^.*(?:FAILED:|FAIL:|ERROR:|EXCEPTION_|RTE Assert|RTE Abort|stack traceback).*$", full_log, re.M)
                checks[f"{phase}_{name}_process"] = record["exit_code"] == 0 and not record["timed_out"]
                checks[f"{phase}_{name}_no_errors"] = not errors
                checks[f"{phase}_{name}_desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
                checks[f"{phase}_{name}_binary"] = record["exe_sha256"] == records[(phase, "Host")]["exe_sha256"]
                details[phase][name] = {"errors": errors, "binary": record["exe_sha256"], "exit": record["exit_code"]}
            checks[f"{phase}_host_stayed_in_lobby"] = ("assert_substate expected=Lobby actual=Lobby PASS" in host_log
                                                       and "assert_enabled ButtonMultiplayerStart expected=0 actual=0 PASS" in host_log)
            checks[f"{phase}_host_reason_visible"] = ('assert_error "could not join"' in host_log
                                                      and "A player could not join:" in host_log)
            host_status = re.search(r'assert_error "could not join" status="(.*?)" (?:PASS|FAIL)', host_log, re.S)
            details[phase]["host_error_text"] = host_status.group(1) if host_status else None
            checks[f"{phase}_host_sentence_unchanged"] = bool(host_status) and "\n" not in host_status.group(1)

            checks[f"{phase}_joiner_landed"] = "assert_substate expected=Landing actual=Landing PASS" in joiner_log
            checks[f"{phase}_joiner_control_enabled"] = "assert_enabled ButtonMultiplayerJoinGame expected=1 actual=1 PASS" in joiner_log
            checks[f"{phase}_joiner_never_started"] = "dump_lobby state=Failed" in joiner_log and "[menu-mp]" not in joiner_log
            text = label_text(joiner_log)
            details[phase]["joiner_label_text"] = text
            checks[f"{phase}_recognized_summary"] = text is not None and text.startswith(PREFIX)
            checks[f"{phase}_first_line_break"] = text is not None and text.startswith(PREFIX + "\n")
            checks[f"{phase}_named_{verb.lower()}"] = text is not None and f"{verb}: {MODULE}" in text
            for name, image in (("Host", "host-mod-mismatch.png"), ("Joiner", "joiner-landing-status.png")):
                images = list((root / phase / name / "runtime/ScreenShots").glob(Path(image).stem + "_*.png"))
                checks[f"{phase}_{name}_screenshot"] = len(images) == 1 and images[0].stat().st_size > 0
                details[phase][name]["screenshot"] = str(images[0]) if len(images) == 1 else None
        result.update({"checks": checks, "details": details})
        result["pass"] = all(checks.values())
    except Exception as error:
        result["error"] = str(error)
        result["checks"] = checks
        result["details"] = details
    finally:
        for run in runs.values():
            run.close()
        (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [key for key, ok in result.get("checks", {}).items() if not ok],
                      "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
