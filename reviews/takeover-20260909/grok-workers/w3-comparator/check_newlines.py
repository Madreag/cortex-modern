from pathlib import Path

paths = {
    "main_compare": Path(r"D:\Projects\p4b-interp-validation\tools\compare_snapshots.py"),
    "control_compare": Path(r"D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910\control\compare_snapshots.py"),
    "repaired_compare": Path(r"D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910\repaired\compare_snapshots.py"),
    "main_test_compare": Path(r"D:\Projects\p4b-interp-validation\tools\test_compare_snapshots.py"),
    "frozen_test_compare": Path(r"D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910\repaired\test_compare_snapshots.py"),
    "roles_test": Path(r"D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910\repaired\test_snapshot_inventory_roles.py"),
}
out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w3-comparator\newline-audit.txt")
lines = []
for name, path in paths.items():
    data = path.read_bytes()
    crlf = data.count(b"\r\n")
    lf = data.count(b"\n") - crlf
    cr = data.count(b"\r") - crlf
    lines.append(f"{name} size={len(data)} crlf={crlf} lf={lf} cr={cr}")
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines))
