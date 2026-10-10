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
    proof_path = baseline.parent / "baseline-probes.json"
    if subprocess.check_output(["git", "-C", str(baseline), "status", "--porcelain"], text=True).strip():
        if not proof_path.exists():
            raise SystemExit("The baseline must be clean before adding probes")
        previous = json.loads(proof_path.read_text(encoding="utf-8"))
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

    for name in ("NetSessionPlaneSelfTest.cpp", "NetSessionPlaneSelfTest.h"):
        write("Source/Network/" + name, (tip / "Source/Network" / name).read_text(encoding="utf-8"))
    native = (tip / "Source/Network/NetLockstepSelfTest.cpp").read_text(encoding="utf-8")
    guards = native[native.index("\tstruct SessionPlaneRecoveryTest {"):native.index("\t\tstatic bool AppliedInputPrefix(")]
    guards += "\t};\n\n"
    begin = native.index("\tbool NetLockstepSelfTest::CheckSessionRecoveryGuard(")
    guards += native[begin:native.index("\n\t// Return/recording fixtures", begin)]
    insert("Source/Network/NetLockstepSelfTest.cpp", "namespace RTE {", "\n" + guards)
    insert("Source/Network/NetLockstep.h", "class NetLockstepCoordinator {", "\n\t\tfriend struct SessionPlaneRecoveryTest;")
    insert("Source/Network/NetLockstepSelfTest.h", "static int Run();", "\n\t\tstatic bool CheckSessionRecoveryGuard(unsigned arm, std::string* error);")
    insert("Source/Main.cpp", '#include "NetLockstepSelfTest.h"', '\n#include "NetSessionPlaneSelfTest.h"')
    needle = '\t\tif (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-selftest") {'
    text = (baseline / "Source/Main.cpp").read_text(encoding="utf-8")
    write("Source/Main.cpp", text.replace(needle, '\t\tif (argv[i] != nullptr && std::string(argv[i]) == "-net-session-plane-selftest") { return NetSessionPlaneSelfTest::Run(); }\n' + needle, 1))
    insert("Source/Network/meson.build", "'NetLockstepSelfTest.cpp',", "\n'NetSessionPlaneSelfTest.cpp',")
    insert("RTEA.vcxproj", '<ClCompile Include="Source\\Network\\NetLockstepSelfTest.cpp" />', '\n    <ClCompile Include="Source\\Network\\NetSessionPlaneSelfTest.cpp" />')
    # Launch routing changes only; no baseline controller, session, or decision code is replaced.
    write("tools/posix_test_runner.py", (tip / "tools/posix_test_runner.py").read_text(encoding="utf-8"))
    proof = {"baseline": sha, "probe_source": subprocess.check_output(["git", "-C", str(tip), "rev-parse", "HEAD"], text=True).strip(),
             "files": {name: hashlib.sha256((baseline / name).read_bytes()).hexdigest() for name in changed},
             "behavior_changes": False, "launch_change": "visible POSIX routing"}
    proof_path.write_text(json.dumps(proof, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
