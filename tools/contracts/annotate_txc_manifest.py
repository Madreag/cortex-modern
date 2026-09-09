"""Record why the control archive was regenerated, leaving the case names and hashes exactly as the generator wrote them."""

from pathlib import Path
import hashlib
import json

controls = Path(
    "D:/Projects/reviews/recovery-2026-09-07/contract-audit/full-runtime-transaction-controls-20"
)
path = controls / "manifest.json"
manifest = json.loads(path.read_text())
before = [dict(case) for case in manifest["cases"]]

manifest["regenerated"] = {
    "utc": "2026-09-09",
    "why": "The Source20 control archive full-runtime-transaction-controls-20 and its seed run "
    "full-runtime-staged-seed-1 were removed by the cleanup of 2026-09-09, so the transaction step of "
    "run_remaining_gates.py died in run_audit.py while copying the snapshots into fixture_sources, before any "
    "engine ran. These controls replace them.",
    "basis": "Source40 main: worktree stage2/transaction-controls at 269d2ee7184b58313c1ddfd84bd7c3fbd84553eb with no "
    "engine source modification, MSBuild Final x64 with GNS enabled.",
    "exe_sha256": "e2b77e3ed493aa51ab4f3e46c3cc9dc646f6bc8fdc5482675585376aadb37d63",
    "build_manifest": "D:\\mx\\txc\\build-txc-1\\build.json",
    "seed_run": "D:\\mx\\txc\\seed\\full-runtime-staged-seed-1 (run_audit.py --operations save --ticks 100 "
    "--seed-marker 731 --late, over D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay with "
    "tools/fixtures/mod_checkpoint.lua and mod_global_callbacks.lua)",
    "generator": "graph_controls.py, unmodified; it parsed and re-encoded all five seed Lua graphs with exact byte "
    "identity before writing any control.",
    "directory_name": "The -20 directory and file names are kept because transaction-controls-source20.json names them "
    "literally in every job argv; the contents are Source40, not Source20.",
    "superseded": {
        "source20_seed_sha256": "47fc61c384b6d979c23e0fdb35de441df0113188e507a7d4151170a922d2d24d",
        "source20_valid_full_sha256": "1fa06120f6e2ea2e361b5200cecd61f275de6f02d7f3297f5b77cff019c9805d",
        "recovered_from": "Erol-Mac:/Users/erol/projects/cccp/audit-20260907-evidence/paused-20260908-0223.tar.gz "
        "(89d5f573e7cdad414dfecfcb62f68d8780d669ba4707d2e778797d7ad9ed78d1), which still holds the "
        "deleted Source20 archive; a copy is at D:\\mx\\txc\\source20-recovered. Those controls are "
        "kept as reference only: they were cut from a Source20 save and are not a valid "
        "identical-graph control for this executable.",
    },
}

assert [dict(case) for case in manifest["cases"]] == before, "cases must be untouched"
path.write_text(json.dumps(manifest, indent=2))
print(
    json.dumps(
        {
            "manifest": str(path),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "cases": len(manifest["cases"]),
            "keys": sorted(manifest),
        },
        indent=2,
    )
)
