"""Packages the Windows build: CortexCommand-mp-<version>-win64.zip with the executable, the DLLs it ships with, Data and the
VERSION, LICENSE and README files, a MANIFEST.json naming every file with its bytes and sha256 inside the zip, and the zip's own
sha256 beside it. The version is the VERSION file at the tree's root.

  python tools/package_windows.py [--repo <tree>] [--out <dir>]     (the default out is <tree>/dist)
  python tools/package_windows.py --verify <unpacked dir>           (every manifest file present with its sha256)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXECUTABLE = "Cortex Command.exe"
# The sanitizer runtime belongs to the ASan configuration's builds, never to a release.
EXCLUDED_DLLS = re.compile(r"clang_rt\.asan.*\.dll", re.I)
VERSION_FORM = re.compile(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def package_files(repo: Path) -> list[Path]:
    """The executable, the DLLs beside it, every file under Data (no link followed) and the tree's VERSION, LICENSE, README."""
    files = [repo / EXECUTABLE]
    files += sorted(path for path in repo.glob("*.dll") if not EXCLUDED_DLLS.fullmatch(path.name))
    for folder, directories, names in os.walk(repo / "Data", followlinks=False):
        directories[:] = sorted(name for name in directories if not (Path(folder) / name).is_symlink())
        files += [Path(folder) / name for name in sorted(names)]
    files += [repo / name for name in ("VERSION", "LICENSE", "README.md") if (repo / name).is_file()]
    return files


def package(repo: Path, out: Path) -> Path:
    version = (repo / "VERSION").read_text(encoding="utf-8").strip()
    if not VERSION_FORM.fullmatch(version):
        raise SystemExit(f"VERSION reads {version!r}, not a version")
    if not (repo / EXECUTABLE).is_file():
        raise SystemExit(f"no {EXECUTABLE} in {repo}: build first")
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f"CortexCommand-mp-{version}-win64.zip"
    entries = []
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in package_files(repo):
            name = path.relative_to(repo).as_posix()
            bundle.write(path, name)
            entries.append(dict(path=name, bytes=path.stat().st_size, sha256=sha256(path)))
        manifest = dict(version=version, executable=EXECUTABLE, files=entries)
        bundle.writestr("MANIFEST.json", json.dumps(manifest, indent=1))
    (out / f"{archive.name}.sha256").write_text(f"{sha256(archive)}  {archive.name}\n", encoding="utf-8")
    print(f"[package] {archive} files={len(entries)} bytes={archive.stat().st_size} version={version}", flush=True)
    return archive


def verify(unpacked: Path) -> int:
    manifest = json.loads((unpacked / "MANIFEST.json").read_text(encoding="utf-8"))
    bad = [entry["path"] for entry in manifest["files"] if not (unpacked / entry["path"]).is_file() or sha256(unpacked / entry["path"]) != entry["sha256"]]
    print(f"[package] verify {unpacked} files={len(manifest['files'])} bad={len(bad)} {bad[:5]}", flush=True)
    return 0 if not bad else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--verify", type=Path, help="check an unpacked package against its manifest")
    options = parser.parse_args(argv)
    if options.verify:
        return verify(options.verify.resolve())
    repo = options.repo.resolve()
    package(repo, (options.out or repo / "dist").resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
