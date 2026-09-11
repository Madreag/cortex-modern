import os
import subprocess
import sys
import time
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w12-windows-compile")
LOG = OUT / "build.log"
META = OUT / "build-meta.txt"
SLN = r"D:\Projects\takeover-build\RTEA.sln"
VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

os.environ["GNS_ROOT"] = r"D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release"
os.environ["GNS_DEP_ROOT"] = r"D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows"
os.environ["CL"] = "/MP6"

vs = subprocess.check_output(
    [VSWHERE, "-latest", "-requires", "Microsoft.Component.MSBuild", "-find", r"MSBuild\**\Bin\MSBuild.exe"],
    text=True,
).strip().splitlines()
if not vs:
    raise SystemExit("vswhere found no MSBuild")
msbuild = vs[0].strip()

cmd = [
    msbuild,
    SLN,
    "/t:RTEA",
    "/p:Configuration=Final",
    "/p:Platform=x64",
    "/m",
    "/nologo",
    "/v:minimal",
]

header = [
    f"msbuild={msbuild}",
    f"sln={SLN}",
    f"cmd={' '.join(cmd)}",
    f"GNS_ROOT={os.environ['GNS_ROOT']}",
    f"GNS_DEP_ROOT={os.environ['GNS_DEP_ROOT']}",
    f"CL={os.environ['CL']}",
    f"cwd={os.getcwd()}",
]
print("\n".join(header), flush=True)

start = time.time()
with LOG.open("w", encoding="utf-8", errors="replace") as f:
    f.write("\n".join(header) + "\n\n")
    f.flush()
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        f.write(line)
        f.flush()
        sys.stdout.write(line)
        sys.stdout.flush()
    rc = proc.wait()
elapsed = time.time() - start

meta = (
    f"exit_code={rc}\n"
    f"duration_seconds={elapsed:.3f}\n"
    f"msbuild={msbuild}\n"
    f"sln={SLN}\n"
    f"log={LOG}\n"
)
META.write_text(meta, encoding="utf-8")
print(meta, flush=True)
sys.exit(rc)
