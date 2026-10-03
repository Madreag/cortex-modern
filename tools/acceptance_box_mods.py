"""Install the same packaged module on three remote lane trees, then compare all four hashes."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tarfile

from acceptance_mod import equal_manifests, write_json

ALIASES = {"edith": "edith", "mac": "Erol-Mac", "linux": "3090"}
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def shell_command(box, arguments):
    if box["name"] == "edith":
        return "& " + " ".join("'"+str(arg).replace("'", "''")+"'" for arg in arguments)
    return shlex.join(map(str, arguments))


def checked(argv, **kwargs):
    result = subprocess.run(argv, stdin=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            timeout=300, creationflags=NO_WINDOW, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{Path(argv[0]).name} failed with exit {result.returncode}; no engine launched")
    return result


def validate_target(box, lane):
    if box.get("name") not in ALIASES or box.get("ssh") != ALIASES[box["name"]]:
        raise ValueError("target must use its saved box alias")
    root = box.get("scratch", "")
    prefix = {"edith": "D:/mx/", "mac": "/Users/erol/cortex-workers/", "linux": "/home/erol/cortex-workers/"}[box["name"]]
    if root != prefix+lane or any(c in lane for c in "/\\\r\n'\""):
        raise ValueError("remote scratch must be this lane's own root")
    destination = box.get("destination", "")
    if not destination.startswith(root+"/") or not destination.endswith("/Data/VoidWanderers.rte") or "/../" in destination:
        raise ValueError("mod destination must be inside the lane's engine tree")
    if not box.get("python"):
        raise ValueError("remote Python is not named")


def fetch_receipt(box, remote, local):
    scratch = box["scratch"]
    name = remote.rsplit("/", 1)[-1]
    archive_path = local.with_suffix(".tar")
    tar = ["env", "COPYFILE_DISABLE=1", "tar"] if box["name"] == "mac" else ["tar.exe" if box["name"] == "edith" else "tar"]
    command = shell_command(box, [*tar, "-cf", "-", "-C", scratch, name])
    with archive_path.open("xb") as stream:
        checked(["ssh", "-o", "BatchMode=yes", box["ssh"], command], stdout=stream)
    with tarfile.open(archive_path) as archive:
        members = archive.getmembers()
        if len(members) != 1 or members[0].name != name or not members[0].isfile() or members[0].size > 4*1024**2:
            raise ValueError("remote receipt archive differs from requested file")
        value = archive.extractfile(members[0]).read()
    with local.open("xb") as stream:
        stream.write(value)
    counter = shell_command(box, [box["python"], "-c", "from pathlib import Path; import sys; print(int(Path(sys.argv[1]).is_file()))", remote])
    counted = checked(["ssh", "-o", "BatchMode=yes", box["ssh"], counter], stdout=subprocess.PIPE)
    if counted.stdout.strip() != b"1":
        raise ValueError("remote/local evidence count differs")
    write_json(local.with_suffix(".fetch.json"), dict(remote_files=1, local_files=1, tar_retained=True, remote_retained=True))
    return json.loads(value)


def install_boxes(targets, lane, archive, receipt, out, dry_run=False):
    if {b.get("name") for b in targets} != set(ALIASES) or len(targets) != 3:
        raise ValueError("exactly EDITH, Mac and Linux are required")
    for box in targets:
        validate_target(box, lane)
    source = Path(__file__).with_name("acceptance_mod.py")
    reference = json.loads(Path(receipt).read_text(encoding="utf-8-sig"))
    values, plans = {"pc": reference}, []
    for box in targets:
        scratch = box["scratch"]
        remote_script, remote_archive, remote_receipt = (scratch+"/"+n for n in ("acceptance_mod.py", "void-wanderers.tar", "mod-pc.json"))
        remote_result = scratch+"/mod-installed-"+out.name+".json"
        args = [box["python"], remote_script, "install", remote_archive, remote_receipt, box["destination"], "--out", remote_result]
        plans.append(dict(box=box["name"], destination=box["destination"], command=args))
        if dry_run:
            continue
        mkdir = ("New-Item -ItemType Directory -Force -Path '"+scratch+"' | Out-Null" if box["name"] == "edith"
                 else "mkdir -p "+shlex.quote(scratch))
        checked(["ssh", "-o", "BatchMode=yes", box["ssh"], mkdir], stdout=subprocess.PIPE)
        for local, remote in ((source, remote_script), (archive, remote_archive), (receipt, remote_receipt)):
            checked(["scp", "-q", str(local), box["ssh"]+":"+remote], stdout=subprocess.PIPE)
        checked(["ssh", "-o", "BatchMode=yes", box["ssh"], shell_command(box, args)], stdout=subprocess.PIPE)
        values[box["name"]] = fetch_receipt(box, remote_result, out / (box["name"]+".json"))
    if dry_run:
        return dict(dry_run=True, installations=plans)
    digest = equal_manifests(values)
    result = dict(passed=True, tree_sha256=digest, files=reference["file_count"], bytes=reference["bytes"],
                  tree_hashes={name: doc["tree_sha256"] for name, doc in values.items()},
                  evidence_fetch="tar over ssh; remote/local counts checked in a separate command; no deletion")
    write_json(out / "mod-equality.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", type=Path, required=True)
    parser.add_argument("--lane", required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    result = install_boxes(json.loads(args.targets.read_text(encoding="utf-8-sig")), args.lane,
                           args.archive, args.receipt, args.out, args.dry_run)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
