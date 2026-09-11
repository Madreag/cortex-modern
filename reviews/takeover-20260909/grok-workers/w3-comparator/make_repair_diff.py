from pathlib import Path
import difflib
import hashlib
import re

root = Path(r"D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910")
out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w3-comparator")
out.mkdir(parents=True, exist_ok=True)

files = {
    "control": root / "control" / "compare_snapshots.py",
    "repaired": root / "repaired" / "compare_snapshots.py",
    "main": Path(r"D:\Projects\p4b-interp-validation\tools\compare_snapshots.py"),
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


hashes = {name: sha256(path) for name, path in files.items()}
extra = {
    "control_test_roles": sha256(root / "control" / "test_snapshot_inventory_roles.py"),
    "repaired_test_roles": sha256(root / "repaired" / "test_snapshot_inventory_roles.py"),
    "control_test_compare": sha256(root / "control" / "test_compare_snapshots.py"),
    "repaired_test_compare": sha256(root / "repaired" / "test_compare_snapshots.py"),
    "main_test_compare": sha256(Path(r"D:\Projects\p4b-interp-validation\tools\test_compare_snapshots.py")),
    "control_runtime": sha256(root / "control" / "snapshot_runtime.py"),
    "repaired_runtime": sha256(root / "repaired" / "snapshot_runtime.py"),
}
(out / "provenance-hashes.txt").write_text(
    "\n".join(f"{name} {digest}" for name, digest in {**hashes, **extra}.items())
    + f"\ncontrol_equals_main {files['control'].read_bytes() == files['main'].read_bytes()}\n"
    + f"control_test_roles_equals_repaired {extra['control_test_roles'] == extra['repaired_test_roles']}\n"
    + f"control_test_compare_equals_repaired {extra['control_test_compare'] == extra['repaired_test_compare']}\n"
    + f"main_test_compare_equals_frozen {extra['main_test_compare'] == extra['repaired_test_compare']}\n"
    + f"runtime_equal {extra['control_runtime'] == extra['repaired_runtime']}\n",
    encoding="utf-8",
)

left = files["control"].read_text(encoding="utf-8").splitlines(keepends=True)
right = files["repaired"].read_text(encoding="utf-8").splitlines(keepends=True)
diff = list(
    difflib.unified_diff(
        left,
        right,
        fromfile="control/compare_snapshots.py",
        tofile="repaired/compare_snapshots.py",
        lineterm="\n",
    )
)
# Keep original newlines from the files; rewrite only the header line endings.
text = "".join(diff)
(out / "repair.diff").write_text(text, encoding="utf-8", newline="\n")

pattern = re.compile(r"ignore|skip|exclude|tolerance|allow", re.I)
hits = []
for number, line in enumerate(text.splitlines(), 1):
    if pattern.search(line):
        hits.append(f"{number}:{line}")
(out / "mask-grep.txt").write_text("\n".join(hits) + ("\n" if hits else "NO_MATCHES\n"), encoding="utf-8")
print((out / "provenance-hashes.txt").read_text(encoding="utf-8"))
print("diff_lines", len(text.splitlines()))
print("mask_hits", len(hits))
