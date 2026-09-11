"""Extract peer_input_delays and lockstep fields from fl200 reports."""
import json
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")
folder = Path(r"D:\mx\s41b3\j28\fl\fl200")

def lockstep(report):
    return report.get("service", {}).get("runner", {}).get("lockstep", {})

summary = {}
for peer in ("host", "client"):
    p = folder / f"{peer}_report.json"
    data = json.loads(p.read_text(encoding="utf-8-sig"))
    ls = lockstep(data)
    summary[peer] = {
        "resyncs": data.get("resyncs"),
        "running_ticks": data.get("running_ticks"),
        "pace": data.get("pace"),
        "peer_input_delays": ls.get("peer_input_delays"),
        "input_delay": ls.get("input_delay"),
        "start_packets_sent": ls.get("start_packets_sent"),
        "start_retransmits": ls.get("start_retransmits"),
        "lockstep_keys": sorted(ls.keys()),
        "lockstep": ls,
    }

(OUT / "fl200_delays.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps({k: {kk: vv for kk, vv in v.items() if kk != "lockstep"} for k, v in summary.items()}, indent=2))
