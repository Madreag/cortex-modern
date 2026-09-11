import os
import subprocess
from pathlib import Path

log = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w18-hold-pause\msbuild-final.log")
os.environ["GNS_ROOT"] = r"D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release"
os.environ["GNS_DEP_ROOT"] = r"D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows"
os.environ["CL"] = "/MP6"
vswhere = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
msbuild = subprocess.check_output(
    [vswhere, "-latest", "-requires", "Microsoft.Component.MSBuild", "-find", r"MSBuild\**\Bin\MSBuild.exe"],
    text=True,
).strip().splitlines()[0]
cmd = [
    msbuild,
    r"D:\Projects\hold-pause\RTEA.sln",
    "/t:RTEA",
    "/p:Configuration=Final",
    "/p:Platform=x64",
    "/m",
    "/nologo",
    "/v:minimal",
]
print("MSBUILD", msbuild, flush=True)
print("CL", os.environ["CL"], flush=True)
with log.open("w", encoding="utf-8") as handle:
    handle.write("CMD " + " ".join(cmd) + "\n")
    handle.flush()
    proc = subprocess.run(cmd, stdout=handle, stderr=subprocess.STDOUT, text=True)
print("EXIT", proc.returncode, "LOG", log, flush=True)
raise SystemExit(proc.returncode)
