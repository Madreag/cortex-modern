from pathlib import Path
import shutil
import subprocess

out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w3-comparator")
src = Path(r"D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910")
main = Path(r"D:\Projects\p4b-interp-validation")
scratch = out / "scratch-repo"
if scratch.exists():
    raise SystemExit(f"scratch already exists: {scratch}")
(scratch / "tools").mkdir(parents=True)
shutil.copyfile(main / "tools" / "compare_snapshots.py", scratch / "tools" / "compare_snapshots.py")

git_base = [
    "git",
    "-c",
    "core.autocrlf=false",
    "-c",
    "user.name=Erol Germain-Gomuc",
    "-c",
    "user.email=egermain.ua@gmail.com",
    "-c",
    "commit.gpgsign=false",
    "-c",
    "commit.template=",
]


def git(args, **kwargs):
    command = git_base + args
    result = subprocess.run(command, cwd=scratch, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "scratch-git.log").write_text(
        ((out / "scratch-git.log").read_text(encoding="utf-8") if (out / "scratch-git.log").exists() else "")
        + "$ "
        + " ".join(command)
        + "\n"
        + result.stdout
        + f"\n[returncode {result.returncode}]\n\n",
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise SystemExit(result.stdout)
    return result


git(["init"])
git(["add", "tools/compare_snapshots.py"])
git(["commit", "-F", str(out / "baseline-commit-message.txt")])
shutil.copyfile(src / "repaired" / "compare_snapshots.py", scratch / "tools" / "compare_snapshots.py")
shutil.copyfile(src / "repaired" / "test_snapshot_inventory_roles.py", scratch / "tools" / "test_snapshot_inventory_roles.py")
git(["add", "tools/compare_snapshots.py", "tools/test_snapshot_inventory_roles.py"])
git(["commit", "-F", str(out / "commit-message.txt")])
git(["log", "-1", "--format=%B"])
result = git(["format-patch", "-1", "--stdout"])
patch = out / "compare_snapshots-repair.patch"
patch.write_text(result.stdout, encoding="utf-8", newline="\n")
print("wrote", patch, "bytes", patch.stat().st_size)
