"""Small helpers for retained acceptance evidence and local runner reservations."""
from contextlib import contextmanager
import datetime as dt
import json
import os
from pathlib import Path
import secrets
import stat
import subprocess
import sys

MST = dt.timezone(dt.timedelta(hours=-7))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False)+"\n", encoding="utf-8")


def retained_bytes(root):
    pending, total = [Path(root)], 0
    while pending:
        for entry in pending.pop().iterdir():
            info = entry.lstat()
            if entry.is_symlink() or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400):
                continue
            if entry.is_dir(): pending.append(entry)
            elif entry.is_file(): total += info.st_size
    return total


def check_storage(root):
    total = retained_bytes(root)
    if total >= 5_000_000_000:
        raise RuntimeError("scratch reached 5 GB; stopped without deleting evidence")
    return total


def local_load():
    command = "Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(Cortex Command.*|cl|link)\\.exe$' } | Select-Object ProcessId,Name | ConvertTo-Json -Compress"
    result = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True,
                            timeout=30, check=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    value = json.loads(result.stdout) if result.stdout.strip() else []
    return value if isinstance(value, list) else [value]


@contextmanager
def local_reservation(root):
    if sys.platform != "win32":
        raise RuntimeError("local acceptance capture runs on Windows only")
    marker = Path("D:/mx/FEEL-MATRIX-RUNNING")
    load = local_load()
    if load:
        write_json(Path(root)/"reservation-blocked.json", dict(load=load, reason="engineer/build priority"))
        raise RuntimeError("a build or engine is active; no acceptance engine launched")
    token = secrets.token_hex(24)
    record = dict(stream_root=str(root), pid=os.getpid(), token=token,
                  stamp=dt.datetime.now(MST).strftime("%Y-%m-%d %H:%M:%S MST"))
    previous = os.environ.get("CCCP_FEEL_MATRIX_RUN")
    with marker.open("x", encoding="utf-8") as stream:
        json.dump(record, stream)
    os.environ["CCCP_FEEL_MATRIX_RUN"] = token
    try:
        if local_load():
            raise RuntimeError("a build or engine appeared during reservation; no engine launched")
        yield
    finally:
        current = json.loads(marker.read_text(encoding="utf-8")) if marker.exists() else {}
        if current.get("token") == token:
            marker.unlink()
        if previous is None: os.environ.pop("CCCP_FEEL_MATRIX_RUN", None)
        else: os.environ["CCCP_FEEL_MATRIX_RUN"] = previous


def private_run(repo, flags, out, timeout, env=None):
    from run_sim_test import make_run, seed_settings
    variables = {**(env or {}), "CCCP_HEADLESS": "1", "CC_RUNNER_IGNORE_FULLSCREEN": "1", "PYTHONDONTWRITEBYTECODE": "1"}
    run = make_run(repo, flags, out, timeout, env=variables)
    seed_settings(run, {"NetworkPortMapEnable": "0", "Fullscreen": "0"})
    return run
