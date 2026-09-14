$ErrorActionPreference = "Stop"
$env:GNS_ROOT = "D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release"
$env:GNS_DEP_ROOT = "D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows"
$env:CL = "/MP6"
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
Write-Host "msbuild: $msbuild"
& $msbuild "D:\Projects\item5-lifecycle\RTEA.sln" /t:RTEA /p:Configuration="Final" /p:Platform=x64 /m /nologo /v:minimal
Write-Host "exit: $LASTEXITCODE"
exit $LASTEXITCODE
