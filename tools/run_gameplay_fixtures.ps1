# Gameplay fixtures under lockstep with input delay and local prediction: each case is a 2-peer e2e
# (host==client sim-gated, replay identical, controller boundary clean) plus a semantic assertion read
# off both peers' dumps or the harness's own mode assertions. Every case keeps its own evidence dir.
# One script for every case, the three 3b ones included; -Only picks the subset a caller wants.
param(
    [string]$Repo = 'D:\Projects\control-build',
    [string]$OutDir = "D:\mx\gameplay_$(Get-Date -Format yyyyMMdd_HHmmss)_$PID",
    [int]$Delay = 3,
    [int]$PortBase = 47661,
    [string[]]$Only = @(),
    [string]$Lane = 'D:\Projects\reviews\takeover-20260909\grok-workers\fg6d-battery',
    [string]$HarnessCommon = 'D:\Projects\stage2_p4\harness_common.ps1'
)
$ErrorActionPreference = 'Continue'
. $HarnessCommon
# -File passes a comma list as one string; accept both forms.
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
New-Item -ItemType Directory -Force $OutDir | Out-Null
$checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$Name, [bool]$Pass, [string]$Detail) {
    $checks.Add([pscustomobject]@{ name = $Name; status = $(if ($Pass) { 'pass' } else { 'fail' }); detail = $Detail })
    Write-Host ("[{0}] {1}: {2}" -f $(if ($Pass) { 'PASS' } else { 'FAIL' }), $Name, $Detail)
}
$prov = Get-Provenance -Repo $Repo -ScriptPaths @($PSCommandPath, (Join-Path $Lane 'check_fixture.py'), (Join-Path $Lane 'run_interp_e2e.ps1'))
($prov | ConvertTo-Json -Depth 5) | Out-File -Encoding utf8 (Join-Path $OutDir 'provenance.json')
$bin = Get-BinaryMatchesSource $prov
Add-Check 'binary_matches_source' $bin.pass $bin.detail

$port = $PortBase
function Wait-NoEngine([string]$For) {
    $deadline = (Get-Date).AddHours(1)
    while ($true) {
        $procs = @(Get-Process "Cortex Command*" -ErrorAction SilentlyContinue)
        if ($procs.Count -eq 0) { return }
        if ((Get-Date) -ge $deadline) { throw "Cortex Command still running after 1 h: $($procs.Id -join ',')" }
        $script:engineWaits.Add([pscustomobject]@{ at = (Get-Date).ToUniversalTime().ToString('o'); pids = ($procs.Id -join ','); waiting_for = $For })
        Write-Host ("WAIT Cortex Command pids=[{0}] before {1} (30s)" -f ($procs.Id -join ','), $For)
        Start-Sleep -Seconds 30
    }
}
$script:engineWaits = New-Object System.Collections.Generic.List[object]
function Run-Case([string]$Name, [string[]]$HarnessArgs, [string]$Dump, [string]$Script) {
    if ($Only.Count -gt 0 -and $Name -notin $Only) { return $null }
    Wait-NoEngine $Name
    $script:port += 0
    $dir = Join-Path $OutDir $Name
    $args = @('-Repo', $Repo, '-SkipBuild', '-ReplayTest', '-InputDelay', "$Delay", '-Port', "$script:port") + $HarnessArgs
    if ($Script) { $args += "-HostExtraArgs:-input-script $Script" }
    if ($Dump) { $env:CC_SIM_DUMP = $Dump }
    & pwsh -NoProfile -File (Join-Path $Lane 'run_interp_e2e.ps1') @args -OutDir $dir -ExactOutDir 2>&1 | Out-File -Encoding utf8 (Join-Path $OutDir "$Name.txt")
    if ($Dump) { Remove-Item Env:CC_SIM_DUMP }
    $r = $null
    if (Test-Path (Join-Path $dir 'result.json')) { $r = Get-Content (Join-Path $dir 'result.json') -Raw | ConvertFrom-Json }
    $failed = @()
    if ($r) { $failed = @($r.checks | Where-Object { $_.status -eq 'fail' } | ForEach-Object { $_.name }) }
    Add-Check "$Name.e2e" ($null -ne $r -and $r.pass) "$(if ($r) { "checks=$($r.checks.Count) failed=[$($failed -join ',')]" } else { 'no result.json' }) $dir"
    return $dir
}
function Check-Case([string]$Name, [string]$Dir, [string[]]$Extra = @()) {
    if (-not $Dir) { return }
    & python (Join-Path $Lane 'check_fixture.py') $Name (Join-Path $Dir 'host_trace.json.simdump.txt') (Join-Path $Dir 'client_trace.json.simdump.txt') --delay $Delay @Extra 2>&1 | Tee-Object -FilePath (Join-Path $OutDir "$Name.check.txt") | Out-Host
    Add-Check "$Name.semantic" ($LASTEXITCODE -eq 0) (Get-Content (Join-Path $OutDir "$Name.check.txt") -Raw).Trim().Split("`n")[-1]
}

# F60 humans-vs-CPU: Co-op PvE on stock Skirmish Defense. Each human seat is driven through the setup editor.
$hostUi = ''
$clientUi = ''
if ($Only.Count -eq 0 -or 'humans_vs_cpu' -in $Only) {
    $uiRoot = Join-Path $OutDir 'humans_vs_cpu_ui'
    & python (Join-Path $Lane 'write_editor_scripts.py') $uiRoot
    $hostUi = Join-Path $uiRoot 'host-ui\ui-script.json'
    $clientUi = Join-Path $uiRoot 'client-ui\ui-script.json'
}

# Every gameplay case in one table: name, the harness switches, the sim dump window, the input script,
# and the extra arguments its semantic check needs. Each case is its own e2e dir under OutDir.
$cases = @(
    @{ name = 'fire_reload'; harness = @(); dump = '27:520'; script = 'D:/Projects/stage2_p4/fixtures/fire_reload.txt'; check = $null }
    @{ name = 'weapon_switch'; harness = @(); dump = '27:210'; script = 'D:/Projects/stage2_p4/fixtures/weapon_switch.txt'; check = $null }
    @{ name = 'jetpack'; harness = @(); dump = '27:145'; script = 'D:/Projects/stage2_p4/fixtures/jetpack.txt'; check = $null }
    @{ name = 'terrain_fire'; harness = @(); dump = '27:225'; script = 'D:/Projects/stage2_p4/fixtures/terrain_fire.txt'; check = { param($dir) @('--host-trace', (Join-Path $dir 'host_trace.json')) } }
    @{ name = 'pie_reload'; harness = @(); dump = '27:240'; script = 'D:/Projects/stage2_p4/fixtures/pie_reload.txt'; check = $null }
    @{ name = 'ai_orders'; harness = @('-AIOrderCommandHost', '-Ticks', '900'); dump = '27:640'; script = ''; check = $null }
    @{ name = 'door_pass'; harness = @('-Ticks', '520', '-E2eSpawn', 'ADoor:Door Slide Short:Base.rte:980:715:40:0'); dump = '27:500'; script = 'D:/Projects/stage2_p4/fixtures/door_pass.txt'; check = $null }
    @{ name = 'crab_ai_order'; harness = @('-Ticks', '450', '-E2eSpawn', 'ACrab:Crab:Base.rte:920:760:20'); dump = '27:400'; script = 'D:/Projects/stage2_p4/fixtures/crab_ai_order.txt'; check = $null }
    @{ name = 'craft_cargo'; harness = @('-DeliverCommandHost', '-Ticks', '700'); dump = '27:640'; script = ''; check = $null }
    @{ name = 'humans_vs_cpu'; harness = @('-Ticks', '900', '-MatchPreset', 'Skirmish Defense', '-MatchMode', 'coop-pve', '-HostUiScript', $hostUi, '-ClientUiScript', $clientUi); dump = '27:890'; script = ''; check = { param($dir) @('--host-log', (Join-Path $dir 'host.out.txt'), '--client-log', (Join-Path $dir 'client.out.txt')) } }
)

foreach ($case in $cases) {
    $d = Run-Case $case.name $case.harness $case.dump $case.script
    if (-not $d) { continue }
    $extra = @()
    if ($case.check) { $extra = @(& $case.check $d) }
    Check-Case $case.name $d $extra
}

$pass = -not ($checks | Where-Object { $_.status -ne 'pass' })
$result = [ordered]@{ pass = $pass; out_dir = $OutDir; delay = $Delay; checks = $checks.ToArray(); provenance_identity = (Get-ProvenanceIdentity $prov); engine_waits = $script:engineWaits.ToArray() }
($result | ConvertTo-Json -Depth 5) | Out-File -Encoding utf8 (Join-Path $OutDir 'result.json')
Write-Host ("GAMEPLAY FIXTURES: {0} ({1} checks) evidence {2}" -f $(if ($pass) { 'PASS' } else { 'FAIL' }), $checks.Count, $OutDir)
exit $(if ($pass) { 0 } else { 1 })
