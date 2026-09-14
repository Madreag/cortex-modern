$ErrorActionPreference = 'Stop'
$taskRepo = 'D:/Projects/item4-feel'
$taskRoot = 'D:/mx/astra-f40-root-fix-20260914'
$taskExe = Join-Path $taskRepo 'Cortex Command.exe'
if (-not (Test-Path -LiteralPath "$taskRoot/red/completed.json") -or -not (Test-Path -LiteralPath "$taskRoot/reference/completed.json")) {
    throw 'Run the RED and reference phases before building.'
}
while (Get-Process -Name cl,link -ErrorAction SilentlyContinue) {
    Start-Sleep -Seconds 5
}
if (Get-Process -Name 'Cortex Command*' -ErrorAction SilentlyContinue) {
    throw 'An engine is running; the build lane is not available.'
}
$env:CL = '/MP6'
$env:CCCP_HEADLESS = '1'
$env:GNS_ROOT = 'D:/Projects/stage2_p2/gns_spike/install-win-vcpkg-release'
$env:GNS_DEP_ROOT = 'D:/Projects/stage2_p2/gns_spike/build-win-vcpkg-release/vcpkg_installed/x64-windows'
$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
$beforeHash = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLowerInvariant()
$beforeStamp = date '+%Y-%m-%d %H:%M:%S MST'
python "$taskRepo/tools/seat_facts/build_pin.py" before
if ($LASTEXITCODE -ne 0) { throw 'Build input capture failed.' }
& $msbuild "$taskRepo/RTEA.sln" /t:RTEA /p:Configuration=Final /p:Platform=x64 /m:1 /nr:false /nologo /v:minimal 2>&1 | Tee-Object -FilePath "$taskRoot/build.log"
$buildExit = $LASTEXITCODE
$record = [ordered]@{
    started = $beforeStamp
    finished = (date '+%Y-%m-%d %H:%M:%S MST')
    source_tip = (git -C $taskRepo rev-parse HEAD)
    exe_before_sha256 = $beforeHash
    exe_sha256 = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLowerInvariant()
    exit_code = $buildExit
    CL = $env:CL
}
$record | ConvertTo-Json | Set-Content -LiteralPath "$taskRoot/build.json" -Encoding utf8
if ($buildExit -eq 0) {
    python "$taskRepo/tools/seat_facts/build_pin.py" after
    if ($LASTEXITCODE -ne 0) { throw 'Build input or executable verification failed.' }
}
exit $buildExit
