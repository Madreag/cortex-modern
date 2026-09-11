import shutil
from pathlib import Path

repo = Path(r"D:\Projects\takeover-build")
gns_bin = Path(r"D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows\bin")
settings_src = Path(
    r"D:\Projects\reviews\takeover-20260909\native-positive20-verified-0244\positive-gcc-attempt20\Settings.input.ini"
)
userdata = repo / "Userdata"
userdata.mkdir(parents=True, exist_ok=True)
dest_settings = userdata / "Settings.ini"
if not dest_settings.exists():
    shutil.copy2(settings_src, dest_settings)
    print(f"copied settings {settings_src} -> {dest_settings}")
else:
    print(f"settings already present {dest_settings}")

copies = [repo / "external" / "lib" / "win" / "fmod.dll"]
for name in (
    "abseil_dll.dll",
    "legacy.dll",
    "libcrypto-3-x64.dll",
    "libprotobuf-lite.dll",
    "libprotobuf.dll",
    "libprotoc.dll",
    "libssl-3-x64.dll",
):
    copies.append(gns_bin / name)
for src in copies:
    dest = repo / src.name
    if dest.exists():
        print(f"already {dest.name} {dest.stat().st_size}")
        continue
    shutil.copy2(src, dest)
    print(f"copied {src} -> {dest} {dest.stat().st_size}")
