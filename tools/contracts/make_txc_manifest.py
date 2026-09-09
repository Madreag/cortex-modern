"""Pin a freshly built tree as a run_audit.py build manifest.

run_audit.py wants {head, exe_sha256, source_patch, artifacts}: it rehashes every artifact before and after each case,
so unrelated development can continue in another tree while the run holds. Static libraries the solution rebuilds are
artifacts, not source, so they are pinned rather than carried in the patch.
"""

from pathlib import Path
import argparse
import hashlib
import json
import subprocess

LIBS = [
    "external/sources/allegro 4.4.3.1-custom/_Bin/allegro-release.lib",
    "external/sources/luabind-0.7.1/_Bin/luabind-release.lib",
]


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True, help="the built worktree")
    parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="directory to write build.json and source.patch into",
    )
    parser.add_argument("--build-log", type=Path)
    parser.add_argument("--note", default="")
    options = parser.parse_args()
    repo, out = options.repo.resolve(), options.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    patch = subprocess.check_output(
        [
            "git",
            "diff",
            "--binary",
            "HEAD",
            "--",
            ".",
            *[":(exclude)" + name for name in LIBS],
        ],
        cwd=repo,
    )
    patch_path = out / "source.patch"
    patch_path.write_bytes(patch)

    artifacts = {
        str((repo / name).resolve()): digest(repo / name)
        for name in ["Cortex Command.exe", "Cortex Command.pdb", *LIBS]
    }
    if options.build_log:
        artifacts[str(options.build_log.resolve())] = digest(options.build_log)
    artifacts[str(patch_path)] = digest(patch_path)

    manifest = {
        "head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "exe_sha256": digest(repo / "Cortex Command.exe"),
        "source_patch": str(patch_path),
        "artifacts": artifacts,
        "source_note": options.note,
    }
    (out / "build.json").write_text(json.dumps(manifest, indent=2))
    print(
        json.dumps(
            {
                "manifest": str(out / "build.json"),
                "head": manifest["head"],
                "exe_sha256": manifest["exe_sha256"],
                "source_patch_bytes": len(patch),
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
