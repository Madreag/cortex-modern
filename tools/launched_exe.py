"""Read the exe a run actually launched from launch.json into a result header."""

from __future__ import annotations

import json
from pathlib import Path


def launched_from_json(path: Path) -> tuple[str | None, str | None]:
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not isinstance(data, dict):
        return None, None
    return data.get("exe_path"), data.get("exe_sha256")


def apply_launched_exe(result: dict, launch_paths) -> dict:
    """Set result exe/exe_sha256 from launch.json. Refuse PASS on a header/launch mismatch."""
    headers = []
    for path in launch_paths:
        path = Path(path)
        if not path.exists():
            result["pass"] = False
            result["exe_header_error"] = f"missing launch.json: {path}"
            return result
        headers.append(launched_from_json(path))
    if not headers or any(exe is None or sha is None for exe, sha in headers):
        result["pass"] = False
        result["exe_header_error"] = "launch.json missing exe_path or exe_sha256"
        return result
    exe, sha = headers[0]
    if any(pair != (exe, sha) for pair in headers):
        result["pass"] = False
        result["exe_header_error"] = "launch.json exe mismatch across peers"
        return result
    named_exe, named_sha = result.get("exe"), result.get("exe_sha256")
    if named_exe not in (None, exe) or named_sha not in (None, sha):
        result["pass"] = False
        result["exe_header_error"] = (
            f"header/launch mismatch header=({named_exe}, {named_sha}) launch=({exe}, {sha})"
        )
        result["exe"] = exe
        result["exe_sha256"] = sha
        return result
    result["exe"] = exe
    result["exe_sha256"] = sha
    return result
