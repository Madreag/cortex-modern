"""Transport refusal cases and real mod-mismatch join/host UI checks.

The version arms deliberately remain incomplete until supplied UI runs show both the
joiner and host reason. A transport selftest alone cannot prove a visible menu message.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import hashlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run


def version_ui_checks(manifest, source_sha):
    """Read the generated capture's own Linux menu assertions, bound to this source and file hashes."""
    checks = dict(version_ui_identity=manifest.get('platform') == 'linux' and manifest.get('source_sha') == source_sha,
                  version_ui_generation=manifest.get('generator_exit_code') == 0)
    capture = manifest.get('capture') or {}
    capture_path = Path(capture.get('path') or '')
    checks['version_ui_capture'] = capture_path.is_file() and hashlib.sha256(capture_path.read_bytes()).hexdigest() == capture.get('sha256')
    for kind in ('build', 'protocol'):
        for who in ('host', 'joiner'):
            path = Path((manifest.get(kind) or {}).get(who) or '')
            valid = path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == manifest.get('input_sha256', {}).get(str(path))
            text = path.read_text(errors='replace') if valid else ''
            checks[f'{kind}_{who}_ui'] = bool(re.search(r'(?m)^\[menu-script\] assert_(?:net_label|error)\b[^\n]*\b' + kind + r'\b[^\n]*\bPASS\s*$', text, re.I))
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=50305)
    parser.add_argument("--version-ui", type=Path, help="JSON mapping build/protocol to host/joiner stdout paths")
    args = parser.parse_args()
    root, repo = args.out.resolve(), args.repo.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"pass": False, "checks": {}}
    run = make_run(repo, ["-net-session-selftest"], root / "session", 180, env={"CCCP_HEADLESS": "1"})
    try:
        result["session_record"] = run.start().finish()
        text = (root / "session/stdout.log").read_text(errors="replace")
        for name, reason, key in (("build", "BuildMismatch", "build_id"), ("protocol", "ProtocolMismatch", "network_protocol_version")):
            result["checks"][name + "_refused"] = bool(re.search(r"admission refused reason=" + reason + r" .*key=" + key, text))
        result["checks"]["session_exit"] = result["session_record"]["exit_code"] == 0
    finally:
        run.close()
    command = [sys.executable, str(repo / "tools/test_module_mismatch.py"), "--repo", str(repo), "--out", str(root / "mods")]
    for index, name in enumerate(("client-extra", "host-extra", "mixed", "overflow")):
        command += ["--port-" + name, str(args.port + index)]
    with (root / "mods-driver.log").open("w") as log:
        result["mods_exit"] = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
    mod_result = json.loads((root / "mods/result.json").read_text())
    # Require exact actionable module names and retain every process, host and UI check.
    result["mod_ui"] = {}
    for phase, detail in mod_result.get("details", {}).items():
        result["mod_ui"][phase] = detail.get("joiner_label_lines") == detail.get("expected_lines")
    other_checks = {name: ok for name, ok in mod_result.get("checks", {}).items() if not name.endswith("_line_structure")}
    result["checks"]["mod_ui"] = (len(result["mod_ui"]) == 4 and all(result["mod_ui"].values())
                                        and bool(other_checks) and all(other_checks.values()))
    try:
        version_ui = json.loads(args.version_ui.read_text()) if args.version_ui else {}
    except (OSError, ValueError) as error:
        version_ui = {}
        result['input_error'] = f'version UI input: {type(error).__name__}: {error}'
    source = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    result['checks'].update(version_ui_checks(version_ui, source))
    if not args.version_ui:
        result["handoff"] = {"row": 512, "owner": "Thread A", "source": "Source/Network/NetSession.cpp:1661",
                             "reason": "Build/protocol rejection UI still needs real mismatched-peer menu captures; source-only checks do not count.",
                             "request": "Provide build/protocol fault peers through the admission test seam, preserving the refusal and naming the difference on both menus."}
    result["pass"] = all(result["checks"].values())
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"pass": result["pass"], "checks": result["checks"], "handoff": result.get("handoff")}))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
