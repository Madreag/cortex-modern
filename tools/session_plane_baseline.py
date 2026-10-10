"""Add only the current behavioral probes to an otherwise unchanged baseline checkout."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tip", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    options = parser.parse_args()
    tip, baseline = options.tip.resolve(), options.baseline.resolve()
    sha = subprocess.check_output(["git", "-C", str(baseline), "rev-parse", "HEAD"], text=True).strip()
    if not sha.startswith("107dbdab7c"):
        raise SystemExit("The probes require the recorded integration baseline")
    metadata = subprocess.check_output(["git", "-C", str(baseline), "rev-parse", "--git-path", "session-plane-baseline-probes.json"], text=True).strip()
    proof_path = Path(metadata)
    if not proof_path.is_absolute():
        proof_path = baseline / proof_path
    previous_path = proof_path if proof_path.exists() else baseline.parent / "baseline-probes.json"
    if subprocess.check_output(["git", "-C", str(baseline), "status", "--porcelain"], text=True).strip():
        if not previous_path.exists():
            raise SystemExit("The baseline must be clean before adding probes")
        previous = json.loads(previous_path.read_text(encoding="utf-8"))
        for relative, digest in previous["files"].items():
            target = (baseline / relative).resolve()
            if not target.is_relative_to(baseline) or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
                raise SystemExit("A baseline probe differs from its staged evidence")
        for relative in previous["files"]:
            tracked = subprocess.run(["git", "-C", str(baseline), "ls-files", "--error-unmatch", "--", relative], capture_output=True).returncode == 0
            if tracked:
                subprocess.run(["git", "-C", str(baseline), "restore", "--", relative], check=True)
            else:
                (baseline / relative).unlink()
        if subprocess.check_output(["git", "-C", str(baseline), "status", "--porcelain"], text=True).strip():
            raise SystemExit("The baseline has edits outside the staged probes")

    changed = []

    def write(relative, text):
        (baseline / relative).write_text(text, encoding="utf-8", newline="\n")
        changed.append(relative)

    def insert(relative, needle, addition):
        text = (baseline / relative).read_text(encoding="utf-8")
        if text.count(needle) != 1:
            raise SystemExit(f"The baseline anchor differs: {relative}")
        write(relative, text.replace(needle, needle + addition, 1))

    for name in ("NetSessionPlaneSelfTest.cpp", "NetSessionPlaneSelfTest.h", "NetLockstepSelfTest.h",
                 "NetMatchSelfTest.cpp", "NetReconnectSessionSelfTest.cpp", "NetRejoinMatrixSelfTest.cpp"):
        write("Source/Network/" + name, (tip / "Source/Network" / name).read_text(encoding="utf-8"))
    native = (tip / "Source/Network/NetLockstepSelfTest.cpp").read_text(encoding="utf-8")
    # Run the rewritten assertions themselves, including their unchanged claim,
    # controller, ticket and replay comparisons. No coordinator implementation
    # is backported. These two unchanged, tip-only readout checks need APIs that
    # do not exist on the baseline; retain their rows as explicit unavailability,
    # and never credit those messages as behavioral red evidence.
    unavailable = ("AppliedInputPrefix", "LinkQuality")
    for method in unavailable:
        signature = "\t\tstatic bool " + method + "(std::string* error) {"
        begin = native.index(signature) + len(signature)
        end = native.index("\n\t\t}", begin)
        native = (native[:begin] + '\n#if __has_include("NetPeerSessionWire.h")' + native[begin:end]
                  + '\n#else\n\t\t\t*error = "BASELINE-UNAVAILABLE: ' + method
                  + ' requires the new peer protocol API; not behavioral evidence";\n\t\t\treturn false;\n#endif' + native[end:])
    visible = "host.IsSeatHoldVisible(late, 1083)"
    if native.count(visible) != 1:
        raise SystemExit("The baseline visibility probe anchor differs")
    native = native.replace(visible, "([]<class Peer>(const Peer& p, uint8_t seat) { "
                            "if constexpr (requires { p.IsSeatHoldVisible(seat, 1083); }) return p.IsSeatHoldVisible(seat, 1083); "
                            "else return p.HasHeldAISeat(seat); })(host, late)")
    write("Source/Network/NetLockstepSelfTest.cpp", native)
    # Access declarations let current probes inspect the old state without
    # changing a single baseline decision or transport path.
    for name, anchor in (("NetLockstep.h", "class NetLockstepCoordinator {"), ("NetMatchService.h", "class NetMatchService")):
        relative = "Source/Network/" + name
        current = (tip / relative).read_text(encoding="utf-8")
        original = (baseline / relative).read_text(encoding="utf-8")
        friends = [line for line in current.splitlines() if line.strip().startswith("friend ") and line not in original.splitlines()]
        if name == "NetMatchService.h":
            anchor = original[original.index(anchor):original.index("{", original.index(anchor)) + 1]
        insert(relative, anchor, "\n" + "\n".join(friends))
    insert("Source/Main.cpp", '#include "NetLockstepSelfTest.h"', '\n#include "NetSessionPlaneSelfTest.h"')
    needle = '\t\tif (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-selftest") {'
    text = (baseline / "Source/Main.cpp").read_text(encoding="utf-8")
    write("Source/Main.cpp", text.replace(needle, '\t\tif (argv[i] != nullptr && std::string(argv[i]) == "-net-session-plane-selftest") { return NetSessionPlaneSelfTest::Run(); }\n' + needle, 1))
    insert("Source/Network/meson.build", "'NetLockstepSelfTest.cpp',", "\n'NetSessionPlaneSelfTest.cpp',")
    insert("RTEA.vcxproj", '<ClCompile Include="Source\\Network\\NetLockstepSelfTest.cpp" />', '\n    <ClCompile Include="Source\\Network\\NetSessionPlaneSelfTest.cpp" />')
    # Launch routing changes only; no baseline controller, session, or decision code is replaced.
    write("tools/posix_test_runner.py", (tip / "tools/posix_test_runner.py").read_text(encoding="utf-8"))
    write("tools/session_directory/test_session_directory.py", (tip / "tools/session_directory/test_session_directory.py").read_text(encoding="utf-8"))
    proof = {"baseline": sha, "probe_source": subprocess.check_output(["git", "-C", str(tip), "rev-parse", "HEAD"], text=True).strip(),
             "files": {name: hashlib.sha256((baseline / name).read_bytes()).hexdigest() for name in changed},
             "behavior_changes": False, "launch_change": "visible POSIX routing",
             "unavailable_not_credited": list(unavailable),
             "probe_suites": ["net-session-plane", "net-lockstep", "net-input-acceptance", "net-lockstep-released-claims",
                              "net-lockstep-release-paths", "net-lockstep-seat-succession", "net-lockstep-seat-admission",
                              "net-match", "net-reconnect-session", "net-rejoin-matrix", "net-directory"]}
    proof_path.write_text(json.dumps(proof, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
