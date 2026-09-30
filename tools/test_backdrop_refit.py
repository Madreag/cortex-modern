"""After a repair between peers at different resolutions, the client draws the backdrops at its own screen's scale.

Reads a finished mp-repair-cross-resolution run: each peer logs '[scene] backdrop NAME fit scale=X,Y' when it fits a backdrop to its
screen and '[scene] backdrop NAME restored scale=X,Y' when a restore hands it another machine's image. The client's last scale per
backdrop after the repair must equal the scale it fit at its own scene load, and the host's must differ (else the run proves nothing).

    python tools/test_backdrop_refit.py <run root holding host/ and client/>
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"^\[scene\] backdrop (.+?) (fit|restored) scale=([-0-9.]+),([-0-9.]+)$", re.MULTILINE)


def scales(log: str) -> list[tuple[str, str, tuple[float, float]]]:
    return [(match[1], match[2], (float(match[3]), float(match[4]))) for match in LINE.finditer(log)]


def verdict(host_log: str, client_log: str) -> dict:
    host, client = scales(host_log), scales(client_log)
    own = {}
    for name, kind, value in client:
        if kind == "fit":
            own.setdefault(name, value)
    host_fit = {}
    for name, kind, value in host:
        if kind == "fit":
            host_fit.setdefault(name, value)
    restored = [index for index, (_, kind, _) in enumerate(client) if kind == "restored"]
    after = {}
    if restored:
        for name, kind, value in client[restored[0]:]:
            after[name] = value
    differing = sorted(name for name in own if name in host_fit and host_fit[name] != own[name])
    wrong = {name: {"after_repair": after.get(name), "own": own[name], "host": host_fit.get(name)} for name in differing if after.get(name) != own[name]}
    return {"pass": bool(restored) and bool(differing) and not wrong, "restores": len(restored), "backdrops_scaled_differently": differing,
            "wrong_after_repair": wrong}


def main(argv: list[str]) -> int:
    root = Path(argv[0])
    result = verdict((root / "host/stdout.log").read_text(encoding="utf-8", errors="replace"),
                     (root / "client/stdout.log").read_text(encoding="utf-8", errors="replace"))
    print(f"[backdrop-refit] {'PASS' if result['pass'] else 'FAIL'} {json.dumps(result)}")
    return 0 if result["pass"] else 1


def self_test() -> int:
    host = "[scene] backdrop Sky fit scale=1.588235,1.588235\n"
    fitted = "[scene] backdrop Sky fit scale=1.000000,1.000000\n[scene] backdrop Sky restored scale=1.588235,1.588235\n"
    good = verdict(host, fitted + "[scene] backdrop Sky fit scale=1.000000,1.000000\n")
    bad = verdict(host, fitted)
    ok = good["pass"] and not bad["pass"] and bad["wrong_after_repair"]["Sky"]["after_repair"] == (1.588235, 1.588235) and not verdict(host, host)["pass"]
    print(f"[backdrop-refit self-test] {'PASS' if ok else 'FAIL'} refit {good['pass']} kept-the-host's {bad['pass']}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(self_test() if sys.argv[1:] == ["--self-test"] else main(sys.argv[1:]))
