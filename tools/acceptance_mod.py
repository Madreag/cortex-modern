"""Copy and hash an unchanged installed module; preserve every staging artifact."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile

REPARSE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024**2), b""):
            digest.update(block)
    return digest.hexdigest()


def regular(path):
    info = path.lstat()
    if path.is_symlink() or getattr(info, "st_file_attributes", 0) & REPARSE:
        raise ValueError("module contains a link or reparse point")
    return info


def manifest(root):
    root = Path(root).absolute()
    regular(root)
    files, pending = [], [root]
    while pending:
        for path in pending.pop().iterdir():
            info = regular(path)
            if path.is_dir():
                pending.append(path)
            elif stat.S_ISREG(info.st_mode):
                name = path.relative_to(root).as_posix()
                if "\n" in name or "\r" in name or "\\" in name:
                    raise ValueError("module filename cannot be represented in the hash list")
                if path.name == ".env" or path.name.startswith(".env.") or path.suffix.lower() in (".pem", ".key"):
                    raise ValueError("module includes a credential-like file; no package made")
                files.append(dict(path=name, bytes=info.st_size, sha256=sha256(path)))
            else:
                raise ValueError("module includes a non-regular file")
    files.sort(key=lambda row: row["path"].encode("utf-8"))
    if not files:
        raise ValueError("empty module tree")
    folded = [r["path"].casefold() for r in files]
    if len(folded) != len(set(folded)):
        raise ValueError("module filenames collide on Windows")
    listing = "".join(f"{r['sha256']}  {r['path']}\n" for r in files).encode("utf-8")
    return dict(schema=1, module=root.name, algorithm="sha256(sorted UTF-8 sha256 + two spaces + relative POSIX path + LF)",
                tree_sha256=hashlib.sha256(listing).hexdigest(), files=files,
                file_count=len(files), bytes=sum(r["bytes"] for r in files))


def write_json(path, data):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(data, stream, indent=2, allow_nan=False)
        stream.write("\n")


def equal_manifests(values):
    if set(values) != {"pc", "edith", "mac", "linux"}:
        raise ValueError("four named box manifests are required")
    reference = values["pc"]
    for box, value in values.items():
        if any(value.get(k) != reference.get(k) for k in ("algorithm", "module", "files", "tree_sha256", "file_count", "bytes")):
            raise ValueError(f"{box}: module tree differs before engine launch")
    return reference["tree_sha256"]


def pack(source, destination, receipt):
    source, destination = Path(source), Path(destination)
    before = manifest(source)
    with tarfile.open(destination, "x") as archive:
        for row in before["files"]:
            archive.add(source / row["path"], arcname=row["path"], recursive=False)
    if manifest(source) != before:
        raise ValueError("installed source changed while packaging")
    write_json(receipt, before)
    return before


def install(archive_path, receipt_path, destination):
    expected = json.loads(Path(receipt_path).read_text(encoding="utf-8-sig"))
    destination = Path(destination).absolute()
    if destination.name != expected["module"]:
        raise ValueError("destination does not name the packaged module")
    if destination.exists():
        observed = manifest(destination)
        if observed != expected:
            raise ValueError("existing module differs; no file overwritten")
        return observed
    members = {r["path"]: r for r in expected["files"]}
    if len(members) != len(expected["files"]):
        raise ValueError("duplicate paths in manifest")
    with tarfile.open(archive_path, "r:") as archive:
        items = archive.getmembers()
        names = [item.name for item in items]
        if len(names) != len(set(names)) or set(names) != set(members):
            raise ValueError("archive file set differs from manifest")
        for item in items:
            path = PurePosixPath(item.name)
            if not item.isfile() or path.is_absolute() or ".." in path.parts or "\\" in item.name or ":" in item.name:
                raise ValueError("archive contains an unsafe entry")
            if item.size != members[item.name]["bytes"]:
                raise ValueError("archive file size differs from manifest")
        ancestor = destination.parent
        while True:
            if ancestor.exists():
                regular(ancestor)
            if ancestor == ancestor.parent:
                break
            ancestor = ancestor.parent
        destination.mkdir(parents=True, exist_ok=False)
        for item in items:
            target = destination.joinpath(*PurePosixPath(item.name).parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            with archive.extractfile(item) as source, target.open("xb") as output:
                for block in iter(lambda: source.read(1024**2), b""):
                    digest.update(block)
                    output.write(block)
            if digest.hexdigest() != members[item.name]["sha256"]:
                raise ValueError("archive file digest differs; partial install retained")
    observed = manifest(destination)
    if observed != expected:
        raise ValueError("installed module hash differs; partial install retained")
    return observed


def alter_one_byte(root, relative, scratch, receipt):
    root, scratch = Path(root).resolve(), Path(scratch).resolve()
    if root == scratch or not root.is_relative_to(scratch) or root.name != "VoidWanderers.rte":
        raise ValueError("mutation requires a scratch-only VoidWanderers copy")
    before = manifest(root)
    target = (root / relative).resolve()
    if not target.is_relative_to(root) or target.stat().st_size == 0:
        raise ValueError("mutation target must be a nonempty file inside the scratch module")
    with target.open("rb") as stream:
        old = stream.read(1)
    change = dict(path=target.relative_to(root).as_posix(), offset=0, original=old[0], replacement=old[0] ^ 1,
                  before=before, scratch=str(scratch), module=str(root))
    write_json(receipt, change)
    with target.open("r+b") as stream:
        stream.write(bytes([change["replacement"]]))
    after = manifest(root)
    changed = [r for r, s in zip(before["files"], after["files"]) if r != s]
    if len(changed) != 1 or before["bytes"] != after["bytes"]:
        raise ValueError("mutation changed more than one file")
    write_json(str(receipt)+".altered.json", after)
    return dict(files_changed=1, bytes_changed=1, before=before["tree_sha256"], altered=after["tree_sha256"])


def restore_one_byte(receipt):
    change = json.loads(Path(receipt).read_text(encoding="utf-8-sig"))
    root, scratch = Path(change["module"]).resolve(), Path(change["scratch"]).resolve()
    if not root.is_relative_to(scratch) or root == scratch or root.name != "VoidWanderers.rte":
        raise ValueError("restore is outside the scratch copy")
    altered = json.loads(Path(str(receipt)+".altered.json").read_text(encoding="utf-8"))
    if manifest(root) != altered:
        raise ValueError("scratch copy changed after mutation; refusing restore")
    target = (root / change["path"]).resolve()
    if not target.is_relative_to(root):
        raise ValueError("restore path leaves the module")
    with target.open("r+b") as stream:
        stream.seek(change["offset"])
        if stream.read(1) != bytes([change["replacement"]]):
            raise ValueError("altered byte no longer matches the receipt")
        stream.seek(change["offset"])
        stream.write(bytes([change["original"]]))
    restored = manifest(root)
    if restored != change["before"]:
        raise ValueError("restored tree does not match the original hash")
    write_json(str(receipt)+".restored.json", restored)
    return restored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    hash_args = sub.add_parser("hash")
    hash_args.add_argument("root", type=Path)
    hash_args.add_argument("--out", type=Path, required=True)
    pack_args = sub.add_parser("pack")
    pack_args.add_argument("source", type=Path)
    pack_args.add_argument("archive", type=Path)
    pack_args.add_argument("receipt", type=Path)
    install_args = sub.add_parser("install")
    install_args.add_argument("archive", type=Path)
    install_args.add_argument("receipt", type=Path)
    install_args.add_argument("destination", type=Path)
    install_args.add_argument("--out", type=Path, required=True)
    mutate = sub.add_parser("alter")
    mutate.add_argument("root", type=Path)
    mutate.add_argument("relative")
    mutate.add_argument("--scratch", type=Path, required=True)
    mutate.add_argument("--receipt", type=Path, required=True)
    restore = sub.add_parser("restore")
    restore.add_argument("receipt", type=Path)
    args = parser.parse_args()
    if args.command == "hash":
        value = manifest(args.root)
        write_json(args.out, value)
    elif args.command == "pack":
        value = pack(args.source, args.archive, args.receipt)
    elif args.command == "install":
        value = install(args.archive, args.receipt, args.destination)
        write_json(args.out, value)
    elif args.command == "alter":
        value = alter_one_byte(args.root, args.relative, args.scratch, args.receipt)
    else:
        value = restore_one_byte(args.receipt)
    print(json.dumps({k: v for k, v in value.items() if k in ("module", "tree_sha256", "bytes", "file_count", "before", "altered", "files_changed", "bytes_changed")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
