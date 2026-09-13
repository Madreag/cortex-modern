"""Show a mod-mismatch refusal on the joiner's landing status label, four cases on loopback.

Each phase is a real host plus one joiner:

    client-extra (port 47725)  Extra.rte staged in the joiner's runtime/Mods -> joiner must Remove it
    host-extra   (port 47726)  Extra.rte staged in the host's runtime/Mods    -> joiner must Install it
    mixed        (port 47727)  host-only AInstall.rte, joiner-only BRemove.rte, and CUpdate.rte on
                               both sides with different DataModule Version -> all three groups at once
    overflow     (port 47728)  eight host-only More*.rte -> Install names six, then the source's
                               'and N more differences' elision suffix

Modules are minimal DataModules written into the private runtime's Mods directory after
make_run() stages it and before run.start() - the repo's Data/ and the installed Mods/ are
never touched. The joiner's LabelMultiplayerLandingStatus text is read back through the
existing assert_label seam; the driver then checks the first-line break, the Install line,
Remove/Update sharing the next line on the ASCII "; " display delimiter, every named group
and any truncation suffix the source actually emits.
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
# maxNamedPerGroup in DescribeModuleDiff is 6; eight names force the elision suffix.
OVERFLOW_MODULES = [f"More{i}.rte" for i in range(1, 9)]
WIRE_DOT = " · "
DISPLAY_SEP = "; "
HASH_SUFFIX = re.compile(r" \([a-z_0-9]+: \S+ vs \S+\)$")


def game_version(repo):
    header = Path(repo) / "Source/System/GameVersion.h"
    match = re.search(r'c_VersionString\s*=\s*"([^"]+)"', header.read_text(encoding="utf-8"))
    if not match:
        raise RuntimeError("could not read c_VersionString from " + str(header))
    return match.group(1)


def stage_module(run, dir_name, friendly, game_ver, version=None):
    """Write a minimal valid DataModule into this run's private runtime/Mods before start()."""
    module_dir = run.cwd / "Mods" / dir_name
    module_dir.mkdir()
    ini = f"DataModule\n\tModuleName = {friendly}\n"
    if version is not None:
        ini += f"\tVersion = {version}\n"
    ini += f"\tSupportedGameVersion = {game_ver}\n"
    (module_dir / "Index.ini").write_text(ini, encoding="utf-8")
    return module_dir


def expected_label(install=None, others=()):
    """The exact landing text FormatModuleMismatchStatus produces, minus the hash suffix values."""
    lines = [PREFIX]
    if install:
        lines.append("Install: " + ", ".join(install))
    if others:
        lines.append(DISPLAY_SEP.join(others))
    return lines


def label_text(log):
    """The landing label's full text, echoed by assert_label; may span lines once formatted."""
    match = re.search(r'assert_label ' + LABEL + ' "yours" text="(.*?)" (?:PASS|FAIL)', log, re.S)
    return match.group(1) if match else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port-client-extra", type=int, default=47725)
    parser.add_argument("--port-host-extra", type=int, default=47726)
    parser.add_argument("--port-mixed", type=int, default=47727)
    parser.add_argument("--port-overflow", type=int, default=47728)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    version = game_version(options.repo)
    result = {"pass": False, "version": version}
    checks, details = {}, {}
    runs, records = {}, {}

    def start(phase, name, host, port, suffix, modules):
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
        staged = [str(stage_module(run, dir_name, friendly, version, mod_version))
                  for dir_name, friendly, mod_version in modules]
        details[phase][f"staged_{name.lower()}"] = staged
        runs[(phase, name)] = run.start()
        return runs[(phase, name)]

    try:
        # install names land on line 2; the remaining pieces share one line on "; ",
        # and BuildRejectText's ' (<key>: <a>.. vs <b>..)' rides on the last piece.
        phases = [
            {"name": "client-extra", "port": options.port_client_extra,
             "host_mods": [], "joiner_mods": [(MODULE, "Extra", None)],
             "install": None, "others": [f"Remove: {MODULE}"]},
            {"name": "host-extra", "port": options.port_host_extra,
             "host_mods": [(MODULE, "Extra", None)], "joiner_mods": [],
             "install": [MODULE], "others": []},
            {"name": "mixed", "port": options.port_mixed,
             "host_mods": [("AInstall.rte", "AInstall", None), ("CUpdate.rte", "CUpdate", 2)],
             "joiner_mods": [("BRemove.rte", "BRemove", None), ("CUpdate.rte", "CUpdate", 1)],
             "install": ["AInstall.rte"],
             "others": ["Remove: BRemove.rte", "Update: CUpdate.rte (you 1, host 2)"],
             "separators": ["; Remove: BRemove.rte", "; Update: CUpdate.rte (you 1, host 2)"]},
            {"name": "overflow", "port": options.port_overflow,
             "host_mods": [(m, m[:-4], None) for m in OVERFLOW_MODULES], "joiner_mods": [],
             "install": OVERFLOW_MODULES[:6], "others": ["and 2 more differences"],
             "separators": ["; and 2 more differences"],
             "absent": OVERFLOW_MODULES[6:]},
        ]
        for spec in phases:
            phase = spec["name"]
            details[phase] = {"port": spec["port"], "expect": {"install": spec["install"], "others": spec["others"]}}
            host_script = ("wait_error could not join\nassert_substate Lobby\n"
                           "assert_enabled ButtonMultiplayerStart 0\nassert_error could not join\n"
                           "dump_lobby\nscreenshot host-mod-mismatch\ngoto_main\n"
                           "assert_screen MainScreen\nexit\n")
            host = start(phase, "Host", True, spec["port"], host_script, spec["host_mods"])
            wait_for_log(host, "activate ButtonMultiplayerCreate ok=1")
            joiner_script = (f"wait_state Failed\nwait 5\nassert_substate Landing\n"
                             f"assert_label {LABEL} yours\nassert_enabled ButtonMultiplayerJoinGame 1\n"
                             "dump_lobby\nscreenshot joiner-landing-status\ngoto_main\n"
                             "assert_screen MainScreen\nexit\n")
            start(phase, "Joiner", False, spec["port"], joiner_script, spec["joiner_mods"])
            records[(phase, "Joiner")] = runs[(phase, "Joiner")].finish()
            records[(phase, "Host")] = runs[(phase, "Host")].finish()

        for spec in phases:
            phase = spec["name"]
            host_log = (root / phase / "Host/stdout.log").read_text(encoding="utf-8", errors="replace")
            joiner_log = (root / phase / "Joiner/stdout.log").read_text(encoding="utf-8", errors="replace")
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
            checks[f"{phase}_host_no_wire_delimiter"] = bool(host_status) and WIRE_DOT not in host_status.group(1)
            if spec.get("separators"):
                checks[f"{phase}_host_display_separators"] = bool(host_status) and all(
                    sep in host_status.group(1) for sep in spec["separators"])
            checks[f"{phase}_host_returned_to_main"] = "assert_screen expected=MainScreen actual=MainScreen PASS" in host_log

            checks[f"{phase}_joiner_landed"] = "assert_substate expected=Landing actual=Landing PASS" in joiner_log
            checks[f"{phase}_joiner_control_enabled"] = "assert_enabled ButtonMultiplayerJoinGame expected=1 actual=1 PASS" in joiner_log
            checks[f"{phase}_joiner_never_started"] = "dump_lobby state=Failed" in joiner_log and "[menu-mp]" not in joiner_log
            text = label_text(joiner_log)
            details[phase]["joiner_label_text"] = text
            checks[f"{phase}_recognized_summary"] = text is not None and text.startswith(PREFIX)
            checks[f"{phase}_first_line_break"] = text is not None and text.startswith(PREFIX + "\n")
            checks[f"{phase}_no_wire_delimiter"] = text is not None and WIRE_DOT not in text
            checks[f"{phase}_joiner_returned_to_main"] = "assert_screen expected=MainScreen actual=MainScreen PASS" in joiner_log
            for group in (["Install"] if spec["install"] else []) + [o.split(":")[0] for o in spec["others"] if ":" in o]:
                checks[f"{phase}_named_{group.lower()}"] = text is not None and f"{group}: " in text

            lines = text.split("\n") if text is not None else []
            want = expected_label(spec["install"], spec["others"])
            tail = lines[-1][len(want[-1]):] if lines and lines[-1].startswith(want[-1]) else None
            details[phase]["joiner_label_lines"] = lines
            details[phase]["expected_lines"] = want
            checks[f"{phase}_line_structure"] = (len(lines) == len(want)
                                                 and lines[0] == PREFIX
                                                 and all(lines[i] == want[i] for i in range(1, len(want) - 1))
                                                 and tail is not None
                                                 and bool(HASH_SUFFIX.fullmatch(tail)))
            if spec["install"] and spec["others"]:
                checks[f"{phase}_install_line"] = len(lines) > 1 and lines[1] == want[1]
            if len(spec["others"]) > 1:
                checks[f"{phase}_remove_update_shared_line"] = (lines[-1].startswith(spec["others"][0] + DISPLAY_SEP)
                                                                and DISPLAY_SEP.join(spec["others"]) in lines[-1])
            for elided in spec.get("absent", []):
                checks[f"{phase}_elided_{elided.split('.')[0].lower()}"] = text is not None and elided not in text
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
