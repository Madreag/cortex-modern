param(
    [Parameter(Mandatory = $true)][string]$Out,
    [switch]$FamilyEnded
)

$ErrorActionPreference = 'Stop'
$repo = 'D:/Projects/item5-lifecycle'
$baseline = '6e8a59e1174a8a71c6091ca13e7e2d6706895130'
$branch = 'stage2/menu-readback'
$env:CCCP_HEADLESS = '1'
$env:CL = '/MP6'
$env:GNS_ROOT = 'D:/Projects/stage2_p2/gns_spike/install-win-vcpkg-release'
$env:GNS_DEP_ROOT = 'D:/Projects/stage2_p2/gns_spike/build-win-vcpkg-release/vcpkg_installed/x64-windows'
$env:PYTHONPATH = "$repo/tools"

function Assert-Released {
    if (-not $FamilyEnded) { throw 'Run only after FAMILY ENDED — BUILD, with -FamilyEnded.' }
    if (Test-Path -LiteralPath 'D:/mx/LEAD_FAMILY.lock') { throw 'LEAD_FAMILY.lock exists.' }
    # A two-process harness run owns the machine while it holds the lock, so hold our launches until it releases.
    for ($waited = 0; Test-Path -LiteralPath 'D:/mx/HARNESS_RUNS.lock'; $waited += 30) {
        if ($waited -ge 2700) { throw 'HARNESS_RUNS.lock held for 45 minutes.' }
        Start-Sleep -Seconds 30
    }
}

# A compiling lane is named by its driver MSBuild's solution; cl and link only ever quote a response file.
function Get-ForeignBuildRoots {
    $mine = [System.IO.Path]::GetFullPath($repo).TrimEnd('\')
    $roots = @()
    foreach ($line in (Get-CimInstance Win32_Process -Filter "Name = 'MSBuild.exe'" | ForEach-Object { $_.CommandLine })) {
        foreach ($match in [regex]::Matches([string]$line, '[A-Za-z]:[\\/][^"]*?RTEA\.sln', 'IgnoreCase')) {
            $root = [System.IO.Path]::GetFullPath((Split-Path -Path $match.Value.Replace('/', '\') -Parent)).TrimEnd('\')
            if ($root -ne $mine) { $roots += $root }
        }
    }
    return ($roots | Sort-Object -Unique)
}

function Build-Engine([string]$label) {
    Assert-Released
    # At most two engine builds run on this machine at once.
    for ($waited = 0; @(Get-ForeignBuildRoots).Count -ge 2; $waited += 60) {
        if ($waited -ge 10800) { throw 'Two other worktrees have been compiling for three hours.' }
        Start-Sleep -Seconds 60
        Assert-Released
    }
    $live = Get-CimInstance Win32_Process -Filter "Name = 'Cortex Command.exe'"
    if ($live | Where-Object { $_.ExecutablePath -eq "$repo/Cortex Command.exe".Replace('/', '\') }) {
        throw 'The worktree executable is in use.'
    }
    $vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    & $msbuild "$repo/RTEA.sln" /t:RTEA /p:Configuration=Final /p:Platform=x64 /m /nr:false /nologo /v:minimal 2>&1 | Tee-Object -FilePath "$Out/build-$label.log"
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $label" }
}

function Record-Executable([string]$tree, [string]$label, [string]$revisionBasis) {
    $exe = Get-Item -LiteralPath "$tree/Cortex Command.exe"
    $script:ledger += [ordered]@{
        role = $label; executable = $exe.FullName; sha256 = (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash
        bytes = $exe.Length; file_time = $exe.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss zzz')
        observed_at = (& date '+%Y-%m-%d %H:%M:%S MST'); tree_revision = (& git -C $tree rev-parse HEAD)
        revision_basis = $revisionBasis; detector_sha256 = (Get-FileHash -LiteralPath $detector -Algorithm SHA256).Hash
    }
    ConvertTo-Json -InputObject $script:ledger -Depth 12 | Set-Content -LiteralPath "$Out/exe-ledger.json" -Encoding utf8
}

function Run-Detector([string]$tree, [string]$label, [bool]$red) {
    foreach ($size in @('640x360', '960x540')) {
        Assert-Released
        & python -B $detector --repo $tree --out "$Out/$label-$size" --case all --size $size --port 48270 2>&1 | Tee-Object -FilePath "$Out/$label-$size.log"
        $code = $LASTEXITCODE
        $result = Get-Content -LiteralPath "$Out/$label-$size/result.json" -Raw | ConvertFrom-Json
        if ($red) {
            if ($code -eq 0 -or $result.pass) { throw "Baseline detector passed: $size" }
            foreach ($case in $result.cases) {
                if ($case.pass) { throw "Baseline case passed: $($case.case), $size" }
                if ($case.unknown_commands) { $case.unknown_commands | Add-Content -LiteralPath "$Out/baseline-unknown-commands.txt" }
                # A menu-script case stops at an unknown command, a probe case at the predicate it cannot express.
                $reasons = @($case.unknown_commands)
                foreach ($record in $case.records.PSObject.Properties) { $reasons += @($record.Value.verdict_lines) }
                $reasons = @($reasons | Where-Object { $_ })
                if (-not $reasons) { $reasons = @([string]$case.error) }
                Add-Content -LiteralPath "$Out/baseline-red-reasons.txt" -Value "$size $($case.case): $($reasons -join ' | ')"
            }
        } elseif ($code -ne 0 -or -not $result.pass) { throw "Tip detector failed: $size" }
    }
}

function Run-Driver([string]$name, [string[]]$driverArgs) {
    Assert-Released
    & python -B "$repo/tools/$name.py" --repo $repo --out "$Out/$name" @driverArgs 2>&1 | Tee-Object -FilePath "$Out/$name.log"
    if ($LASTEXITCODE -ne 0) { throw "Regression driver failed: $name" }
}

Assert-Released
$allowedScratch = [System.IO.Path]::GetFullPath('D:/mx/astra-l06-menu-readback-20260914/')
$requestedOut = [System.IO.Path]::GetFullPath($Out)
if (-not $requestedOut.StartsWith($allowedScratch, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Phase B output must remain inside the assigned scratch directory.'
}
if (Test-Path -LiteralPath $Out) { throw 'Use a fresh Phase B output directory.' }
if ((& git -C $repo branch --show-current) -ne $branch) { throw 'Unexpected worktree branch.' }
if (& git -C $repo status --porcelain=v1) { throw 'The worktree must be clean.' }
New-Item -ItemType Directory -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$tip = & git -C $repo rev-parse HEAD
$detector = "$Out/test_menu_readback.py"
Copy-Item -LiteralPath "$repo/tools/test_menu_readback.py" -Destination $detector
$ledger = @()

try {
    # The RED arm only means anything when the baseline executable was built here from the baseline source.
    & git -C $repo switch --detach $baseline
    if ($LASTEXITCODE -ne 0) { throw 'Cannot select the baseline.' }
    Build-Engine 'baseline'
    Record-Executable $repo 'baseline' "Built Final at $baseline in build-baseline.log."
    Run-Detector $repo 'red' $true
} finally {
    if ((& git -C $repo branch --show-current) -ne $branch) {
        & git -C $repo switch $branch
        if ($LASTEXITCODE -ne 0) { throw 'Cannot restore the lane branch; preserve build outputs and report.' }
    }
}
if ((& git -C $repo rev-parse HEAD) -ne $tip) { throw 'The lane tip changed.' }
Build-Engine 'tip'
Record-Executable $repo 'tip' "Built Final at $tip in build-tip.log."
Run-Detector $repo 'green' $false
Run-Driver 'run_selftests' @('--timeout', '300')
Run-Driver 'test_menu_lifecycle' @()
Run-Driver 'test_lobby_input_delay' @('--port', '48276')
Run-Driver 'test_lobby_chat' @('--port', '48273')
Run-Driver 'test_post_match_lobby' @('--port', '48277')
Run-Driver 'test_telemetry_bundle' @('--arm', 'menu', '--port', '48219')
$captures = @()
foreach ($size in @('640x360', '960x540')) {
    $captures += Get-Content -LiteralPath "$Out/green-$size/captures.json" -Raw | ConvertFrom-Json
}
ConvertTo-Json -InputObject $captures -Depth 30 | Set-Content -LiteralPath "$Out/captures.json" -Encoding utf8
Write-Output "Phase B outputs: $Out/exe-ledger.json; $Out/captures.json"
