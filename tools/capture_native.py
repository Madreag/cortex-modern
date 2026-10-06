"""Reuse a verified native build in its caller-owned repository.

The installed dispatcher still owns placement, reservations and launch. An
optional CORTEX_CAPTURE_NATIVE_BUILDS JSON file maps its box names to repo and
receipt paths. With no mapping the dispatcher's normal build path is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import subprocess


def verify(repo: Path, receipt: Path, head: str, exe: str) -> dict:
    repo, receipt = repo.resolve(), receipt.resolve()
    if not receipt.is_relative_to(repo.parent):
        raise RuntimeError("native build receipt is outside the caller's lane")
    relative = PurePosixPath(exe)
    if relative.is_absolute() or ".." in relative.parts:
        raise RuntimeError("native executable path escapes its repository")
    binary = (repo / relative).resolve()
    if not binary.is_relative_to(repo):
        raise RuntimeError("native executable resolves outside its repository")
    actual_head = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    if actual_head != head:
        raise RuntimeError("native repository differs from the frozen case commit")
    if subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no"]).strip():
        raise RuntimeError("native repository has uncommitted inputs")
    record = json.loads(receipt.read_text())
    if record.get("commit") != head or record.get("build_exit_code") != 0:
        raise RuntimeError("native build receipt differs from the frozen case commit")
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if digest != record.get("executable_sha256"):
        raise RuntimeError("native executable differs from its build receipt")
    log = Path(record["build_log"]).resolve()
    if not log.is_relative_to(repo.parent) or hashlib.sha256(log.read_bytes()).hexdigest() != record.get("build_log_sha256"):
        raise RuntimeError("native build log differs from its receipt")
    return {"repo": str(repo), "exe": str(binary), "commit": head, "executable_sha256": digest,
            "build_receipt": str(receipt), "verified": True}


def configure_backend(backend) -> None:
    mapping = os.environ.get("CORTEX_CAPTURE_NATIVE_BUILDS")
    if not mapping:
        return
    builds = json.loads(Path(mapping).read_text(encoding="utf-8-sig"))
    original = backend.native_build
    helper = Path(__file__).read_text(encoding="utf-8")
    backend.sources["capture_native.py"] = helper
    backend.control_id = hashlib.sha256(json.dumps(backend.sources, sort_keys=True).encode()).hexdigest()[:20]

    def native_build(box, claim, request):
        spec = next((value for key, value in builds.items() if key.casefold() == box["name"].casefold()), None)
        if spec is None:
            return original(box, claim, request)
        for key in ("repo", "receipt"):
            if not PurePosixPath(spec[key]).is_absolute():
                raise RuntimeError("native build reuse requires absolute caller-owned paths")
        command = [box["python"], claim["control"] + "/capture_native.py", "--repo", spec["repo"],
                   "--receipt", spec["receipt"], "--head", claim["head"], "--exe", box["exe"]]
        backend.rpc(box, "renew", {"claim": claim}, timeout=15)
        output = backend.guarded_run(["ssh", "-T", "-o", "BatchMode=yes", box["ssh"], shlex.join(command)], timeout=60)
        record = json.loads(output)
        if not record.get("verified") or record.get("commit") != claim["head"]:
            raise RuntimeError("native build reuse verification failed")
        claim.update(repo=record["repo"], exe=record["exe"], native_build=record)
        backend.work.mkdir(parents=True, exist_ok=True)
        (backend.work / (request["run_id"] + "-native-build.json")).write_text(json.dumps(record, indent=2) + "\n")
    backend.native_build = native_build


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--exe", required=True)
    args = parser.parse_args()
    print(json.dumps(verify(args.repo, args.receipt, args.head, args.exe)))


if __name__ == "__main__":
    main()
