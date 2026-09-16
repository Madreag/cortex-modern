param(
    [string]$Repo = "D:\Projects\fixgroup-1",
    [string[]]$E2eSpawn = @(),
    [string]$OutDir = "",
    [int]$Port = 41120,
    [int]$Ticks = 600,
    [switch]$SkipBuild,
    [switch]$PerturbHost,
    [switch]$FundsCommandHost,
    [switch]$SpawnCommandHost,
    [switch]$DeliverCommandHost,
    [switch]$AIOrderCommandHost,
    [switch]$ScuttleCommandHost,
    [switch]$BrainKillCommandHost,
    [switch]$MismatchSmoke,
    [switch]$StallTest,
    [switch]$RematchTest,
    [switch]$InventoryCommandHost,
    [switch]$BuyCommandHost,
    [switch]$BrainSpawnTest,
    [switch]$PauseTest,
    [switch]$FullLoopTest,
    [switch]$SnapshotTest,
    [switch]$ResyncTest,
    [switch]$ReplayTest,
    [switch]$RealTime,
    [int]$FakeLagMs = 0,
    [int]$FakeLagClientMs = 0,
    [int]$InputDelay = 0,
    [int]$HostInputDelay = -1,
    [string]$ExtraArgs = "",
    [string]$HostExtraArgs = "",
    [string]$ClientExtraArgs = "",
    [switch]$ExactOutDir,
    [switch]$RequirePrediction,
    # The host's previews must have taken at least one resident (a predicted pickup) out of the world overlay.
    [switch]$RequirePredictionTaken,
    # Test-only faults for the fault battery: they act on the harness itself, never on the game.
    [string]$FaultSkipCheck = "",
    [switch]$FaultNewerSource,
    [string]$MatchPreset = "",
    [string]$MatchMode = "",
    [string]$HostUiScript = "",
    [string]$ClientUiScript = ""
)

$ErrorActionPreference = "Stop"

# The buy case's arrival (4500ms delivery delay) and tick-700 funds probe need the longer run.
if ($BuyCommandHost -and $Ticks -lt 900) { $Ticks = 900 }
# The pause case spends ~360 ticks paused (180 held + 180 countdown); run long enough to play after.
if ($PauseTest -and $Ticks -lt 900) { $Ticks = 900 }
# The full loop needs the buy arrival (~tick 567) and the tick-700 funds probe.
if ($FullLoopTest -and $Ticks -lt 900) { $Ticks = 900 }

$repo = $Repo
$gnsRoot = "D:\Projects\stage2_p2\gns_spike\install-win-vcpkg-release"
$gnsDepRoot = "D:\Projects\stage2_p2\gns_spike\build-win-vcpkg-release\vcpkg_installed\x64-windows"

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = "D:\Projects\stage2_p4\menu_service_e2e_$stamp"
}

# Every run gets its own directory: earlier evidence is never overwritten.
if (-not $ExactOutDir) {
    $OutDir = "$OutDir" + "_" + (Get-Date -Format "yyyyMMdd_HHmmss") + "_" + $PID
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Set-Location $repo
# The peers run from a private runtime built by the repo's own tools/run_sim_test.py, never from the repo itself.
$runtime = (& python -c "import sys; sys.path.insert(0, r'$repo\tools'); from run_sim_test import prepare_runtime; print(prepare_runtime(r'$repo', sys.argv[1]))" "$OutDir\private").Trim()
if (-not (Test-Path $runtime)) { throw "the private runtime was not created: $runtime" }
Write-Host "PRIVATE RUNTIME: $runtime"
Write-Host "RUN DIR: $OutDir"

# ---- structured result: one named check per property, provenance of what was actually tested ----
. "D:\Projects\stage2_p4\harness_common.ps1"
$script:Checks = New-Object System.Collections.Generic.List[object]
$script:HarnessVersion = 3
$script:ProvenanceStart = $null
$script:EngineWait = [ordered]@{ waited = $false; polls = 0; waited_s = 0; pids = @() }
$script:BuildStartedUtc = [datetime]::MinValue
function Add-Check([string]$Name, [string]$Status, [string]$Detail = "", [bool]$Required = $true) {
    if ($FaultSkipCheck -and $Name -eq $FaultSkipCheck) {
        Write-Host "[fault] FaultSkipCheck: the '$Name' check was NOT recorded"
        return
    }
    $script:Checks.Add([pscustomobject]@{ name = $Name; status = $Status; detail = $Detail; required = $Required })
}
# The scripts and fixtures whose content decides the verdict, hashed into the provenance.
function Get-HarnessScriptPaths {
    $paths = @($PSCommandPath, "D:\Projects\stage2_p4\harness_common.ps1", "D:\Projects\stage2_p4\compare_e2e_simgated.py")
    if ($SnapshotTest) { $paths += "D:\Projects\stage2_p4\compare_p5_snapshots.py" }
    foreach ($argText in @($ExtraArgs, $HostExtraArgs, $ClientExtraArgs)) {
        if (-not $argText) { continue }
        $parts = $argText.Split(" ")
        for ($k = 0; $k -lt $parts.Length - 1; $k++) {
            if ($parts[$k] -eq "-input-script") { $paths += $parts[$k + 1] }
        }
    }
    if ($HostUiScript) { $paths += $HostUiScript }
    if ($ClientUiScript) { $paths += $ClientUiScript }
    return [string[]]$paths
}
function Capture-Provenance([bool]$Built) {
    return Get-Provenance -Repo $repo -BuiltByThisRun $Built -ScriptPaths (Get-HarnessScriptPaths) -BuildStartedUtc $script:BuildStartedUtc -FaultNewerSource:$FaultNewerSource
}
# Which named checks a passing run of this mode must have recorded; a flow that skipped one cannot pass.
function Get-RequiredChecks {
    $req = @("provenance_stable", "binary_matches_source")
    if ($MismatchSmoke) { return $req + "mismatch_smoke" }
    if ($RematchTest) { return $req + "rematch" }
    if ($ResyncTest) { return $req + "resync" }
    if ($PerturbHost) { return $req + "positive_control" }
    $req += @("process_exit", "liveness", "config_agreement", "content_identity_agreement", "prediction_executed", "controller_boundary", "simgated")
    $predictionOn = ($ExtraArgs -notmatch "-net-local-prediction off")
    if ($predictionOn -and (($InputDelay -gt 0) -or ($HostInputDelay -gt 0) -or ($FakeLagMs -gt 0) -or ($FakeLagClientMs -gt 0))) { $req += "prediction_isolation" }
    if ($RequirePredictionTaken) { $req += "prediction_taken" }
    foreach ($mode in $script:Settings.modes) { if ($mode -notin $script:ModeCheckExclusions) { $req += "mode_" + $mode.ToLower() } }
    if ($ReplayTest) { $req += @("replay_recording_intact", "replay_playback", "replay_compare") }
    if ($SnapshotTest) { $req += "snapshot" }
    return $req
}
$script:ModeCheckExclusions = @("SkipBuild", "ExactOutDir", "RequirePrediction", "RequirePredictionTaken", "ReplayTest", "FaultNewerSource")
$script:Settings = [pscustomobject]@{
    ticks = $Ticks; port = $Port; input_delay = $InputDelay; host_input_delay = $HostInputDelay
    fake_lag_ms = $FakeLagMs; fake_lag_client_ms = $FakeLagClientMs; extra_args = $ExtraArgs; host_extra_args = $HostExtraArgs; client_extra_args = $ClientExtraArgs; require_prediction = [bool]$RequirePrediction
    skip_build = [bool]$SkipBuild; fault_skip_check = $FaultSkipCheck; fault_newer_source = [bool]$FaultNewerSource; require_prediction_taken = [bool]$RequirePredictionTaken
    match_preset = $MatchPreset; match_mode = $MatchMode
    modes = [string[]]@($PSBoundParameters.Keys | Where-Object { ($PSBoundParameters[$_] -is [System.Management.Automation.SwitchParameter]) -and [bool]$PSBoundParameters[$_] })
}
function Finish-Run([bool]$Pass) {
    if ($null -eq $script:ProvenanceStart) {
        # The run failed before the build finished: record what is on disk, labelled as not built by this run.
        $script:ProvenanceStart = Capture-Provenance $false
        $script:ProvenanceStart | ConvertTo-Json -Depth 5 | Out-File -Encoding utf8 "$OutDir\provenance.json"
    }
    $provenanceEnd = Capture-Provenance $script:ProvenanceStart.built_by_this_run
    $stable = (Get-ProvenanceIdentity $provenanceEnd) -eq (Get-ProvenanceIdentity $script:ProvenanceStart)
    Add-Check "provenance_stable" $(if ($stable) { "pass" } else { "fail" }) $(if ($stable) { "binary, source (committed, staged, unstaged, untracked) and scripts unchanged for the whole run" } else { "the binary, source or scripts changed under the run; its provenance is void" })
    $binary = Get-BinaryMatchesSource $script:ProvenanceStart
    Add-Check "binary_matches_source" $(if ($binary.pass) { "pass" } else { "fail" }) $binary.detail
    if ($Pass) {
        $recorded = @($script:Checks | ForEach-Object { $_.name })
        $missing = @(Get-RequiredChecks | Where-Object { $_ -notin $recorded })
        if ($missing.Count -gt 0) {
            Add-Check "required_checks_present" "fail" ("required checks never recorded: " + ($missing -join ", "))
        } else {
            Add-Check "required_checks_present" "pass" ("all " + (@(Get-RequiredChecks)).Count + " required checks recorded")
        }
    }
    $requiredFailed = @($script:Checks | Where-Object { $_.required -and $_.status -ne "pass" -and $_.status -ne "n/a" })
    $finalPass = $Pass -and ($requiredFailed.Count -eq 0)
    $contentIdentity = $null
    $hostReportPath = "$OutDir\host_menu_service_e2e.json"
    if (Test-Path $hostReportPath) {
        try {
            $hj = Get-Content $hostReportPath -Raw | ConvertFrom-Json
            $li = $hj.service.runner.session.local_identity
            $contentIdentity = [pscustomobject]@{
                match_config_hash = $hj.service.runner.match_config_hash; activity_preset = $hj.activity_preset
                scene_name = $hj.service.runner.match_config.scene_name; session_id = $hj.service.runner.match_config.session_id
                module_manifest_hash = $li.module_manifest_hash; deterministic_config_hash = $li.deterministic_config_hash
                session_rules_hash = $li.session_rules_hash; num_lua_states = $li.num_lua_states; build_id = $li.build_id
            }
        } catch {}
    }
    $result = [ordered]@{}
    $result["harness_version"] = $script:HarnessVersion
    $result["pass"] = [bool]$finalPass
    $result["out_dir"] = [string]$OutDir
    $result["settings"] = $script:Settings
    $result["checks"] = $script:Checks.ToArray()
    $result["failed_required"] = [string[]]@($requiredFailed | ForEach-Object { $_.name })
    $result["provenance_start"] = $script:ProvenanceStart
    $result["provenance_end"] = $provenanceEnd
    $result["content_identity"] = $contentIdentity
    $result["engine_wait"] = $script:EngineWait
    try {
        $result | ConvertTo-Json -Depth 6 | Out-File -Encoding utf8 "$OutDir\result.json"
    } catch {
        Write-Host "result.json write failed: $($_.Exception.Message) at $($_.InvocationInfo.PositionMessage)"
        throw
    }
    if ($finalPass) {
        Write-Host "RESULT: PASS ($($script:Checks.Count) checks) $OutDir"
        exit 0
    }
    Write-Host "RESULT: FAIL [$($requiredFailed.name -join ', ')] $OutDir"
    exit 1
}
function Fail-Run([string]$Name, [string]$Message) {
    Add-Check $Name "fail" $Message
    Write-Host "FAIL [$Name]: $Message"
    Finish-Run $false
}

# Wait for a foreign engine to exit before this launch starts.
$script:EngineWait = [ordered]@{ waited = $false; polls = 0; waited_s = 0; pids = @() }
$waitDeadline = (Get-Date).AddHours(1)
$waitStart = Get-Date
while ($true) {
    $foreignEngines = @(Get-Process | Where-Object { $_.ProcessName -like "Cortex Command*" } | ForEach-Object { $_.Id })
    if ($foreignEngines.Count -eq 0) { break }
    if ((Get-Date) -ge $waitDeadline) {
        Fail-Run "foreign_engine" "Cortex Command still running after 1h: $($foreignEngines -join ',')"
    }
    Write-Host ("WAIT foreign Cortex Command pids=[{0}] (30s)" -f ($foreignEngines -join ','))
    $script:EngineWait.waited = $true
    $script:EngineWait.polls++
    $script:EngineWait.pids = @($foreignEngines)
    Start-Sleep -Seconds 30
}
$script:EngineWait.waited_s = [int]((Get-Date) - $waitStart).TotalSeconds
Write-Host ("engine wait done waited={0} polls={1} waited_s={2}" -f $script:EngineWait.waited, $script:EngineWait.polls, $script:EngineWait.waited_s)

$env:GNS_ROOT = $gnsRoot
$env:GNS_DEP_ROOT = $gnsDepRoot
$env:PATH = "$gnsDepRoot\bin;$env:PATH"

git status --short --branch | Out-File -Encoding utf8 "$OutDir\git_status.txt"
git rev-parse --abbrev-ref HEAD | Out-File -Encoding utf8 "$OutDir\branch.txt"
git rev-parse HEAD | Out-File -Encoding utf8 "$OutDir\head.txt"

if (-not $SkipBuild) {
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($msbuild)) {
        Fail-Run "build" "MSBuild was not found by vswhere."
    }
    $script:BuildStartedUtc = (Get-Date).ToUniversalTime()
    & $msbuild ".\RTEA.sln" /m:8 /p:Configuration=Final /p:Platform=x64 /t:Build /nologo /v:minimal 2>&1 |
        Tee-Object -FilePath "$OutDir\build_gns.txt" |
        Out-Host
    if ($LASTEXITCODE -ne 0) {
        Fail-Run "build" "MSBuild failed with exit code $LASTEXITCODE; refusing to test a stale executable."
    }
}
# The identity of what is about to be tested: the binary as built (or found) and the source it claims to come from.
$script:ProvenanceStart = Capture-Provenance (-not $SkipBuild)
$script:ProvenanceStart | ConvertTo-Json -Depth 5 | Out-File -Encoding utf8 "$OutDir\provenance.json"

$hostReport = "$OutDir\host_menu_service_e2e.json"
$clientReport = "$OutDir\client_menu_service_e2e.json"
$hostTrace = "$OutDir\host_trace.json"
$clientTrace = "$OutDir\client_trace.json"
$hostOut = "$OutDir\host.out.txt"
$hostErr = "$OutDir\host.err.txt"
$clientOut = "$OutDir\client.out.txt"
$clientErr = "$OutDir\client.err.txt"

# -tick-hashes/-out emit the per-tick trace; -max-ticks caps both uniformly for the host/client sim-gated compare.
# The rematch and resync cases run WITHOUT tracing: the activity restart reuses tick numbers, and
# the LIVE runtime desync exchange polices the rounds instead.
$hostArgs = @(
    "-net-match-service-e2e",
    "-net-host",
    "-net-port", "$Port",
    "-net-match-ticks", "$Ticks",
    "-net-match-report", $hostReport
)
$clientArgs = @(
    "-net-match-service-e2e",
    "-net-join", "127.0.0.1",
    "-net-port", "$Port",
    "-net-match-ticks", "$Ticks",
    "-net-match-report", $clientReport
)
if (-not $RematchTest -and -not $ResyncTest) {
    $hostArgs += @("-tick-hashes", "-max-ticks", "$Ticks", "-out", $hostTrace)
    $clientArgs += @("-tick-hashes", "-max-ticks", "$Ticks", "-out", $clientTrace)
}
# Input delay: the host picks it (the client adopts the host's via the lobby config sync). -HostInputDelay
# overrides the host's value only, so a run can prove the client-adopts-host path (host D>0, client 0).
$effectiveHostDelay = if ($HostInputDelay -ge 0) { $HostInputDelay } else { $InputDelay }
if ($effectiveHostDelay -gt 0) { $hostArgs += @("-net-match-input-delay", "$effectiveHostDelay") }
if ($InputDelay -gt 0) { $clientArgs += @("-net-match-input-delay", "$InputDelay") }
# Passthrough for one-off gates (e.g. "-net-local-prediction off"), applied to both peers.
if ($ExtraArgs) { $hostArgs += $ExtraArgs.Split(" "); $clientArgs += $ExtraArgs.Split(" ") }
if ($HostExtraArgs) { $hostArgs += $HostExtraArgs.Split(" ") }
if ($ClientExtraArgs) { $clientArgs += $ClientExtraArgs.Split(" ") }
if ($MatchPreset) {
    $hostArgs += @("-net-match-service-preset", $MatchPreset)
    $clientArgs += @("-net-match-service-preset", $MatchPreset)
}
if ($MatchMode) {
    $hostArgs += @("-net-match-mode", $MatchMode)
    $clientArgs += @("-net-match-mode", $MatchMode)
}

# Positive control: perturb only the host's sim at tick 50; the sim-gated compare must then catch it.
if ($PerturbHost) { $hostArgs += "-determinism-selftest-perturb" }
# M4 command-channel control: the host issues a synced SetTeamFunds command at tick 50; both peers must stay identical.
if ($FundsCommandHost) { $hostArgs += "-net-match-e2e-funds-command" }
# M4.2 control: the host spawns an actor via command at tick 50; both peers must clone the identical actor.
if ($SpawnCommandHost) { $hostArgs += "-net-match-e2e-spawn-command" }
foreach ($spec in @($E2eSpawn | Where-Object { $_ })) {
    $hostArgs += @("-net-match-e2e-spawn", $spec)
}
# M4.3 control: the host delivers a craft + cargo via command at tick 50; both peers must build the identical delivery.
if ($DeliverCommandHost) { $hostArgs += "-net-match-e2e-deliver-command" }
# AI orders: the host sends go-to / follow / squad / disband orders for its units at ticks 50/200/400/600;
# both peers must hold the identical AI modes, waypoints and squad (dump-checked by the gameplay battery).
if ($AIOrderCommandHost) { $hostArgs += "-net-match-e2e-ai-order-command" }
# #16 control: the host scuttles the delivered craft at tick 100; both peers must gib it identically.
# The scuttle targets the tick-50 delivery, so it implies the deliver command.
if ($ScuttleCommandHost) {
    $hostArgs += "-net-match-e2e-scuttle-command"
    if (-not $DeliverCommandHost) { $hostArgs += "-net-match-e2e-deliver-command" }
}
# #18 control: the host delivers + scuttles a craft onto the enemy brain; the win condition must end
# the match identically on both peers, and the sim traces through the game-over to the cap.
if ($BrainKillCommandHost) {
    $hostArgs += "-net-match-e2e-brain-kill-command"
}
# #20 gate: a deliberate identity mismatch (nls 4 vs 8 -> different deterministic config hash) must be
# rejected with the rich reason surfaced in the service error text, not a bare "state=Rejected".
if ($MismatchSmoke) {
    $hostArgs += @("-num-lua-states", "4")
    $clientArgs += @("-num-lua-states", "8")
}
# #21 gate: the client fakes a hung peer (8s frame stall at tick 300); the host must ride it out within
# the 20s missing-frame grace and both peers must still finish 600/600 sim-gated identical.
if ($StallTest) {
    $clientArgs += "-net-match-e2e-stall"
}
# #15 gate: host-issued inventory ops (reorder/swap/reload/drop) at ticks 210-300; both peers must
# apply them identically, with the drop feeding the per-item hash directly.
if ($InventoryCommandHost) {
    $hostArgs += "-net-match-e2e-inventory-command"
}
# M4.4 gate: the host grants 5000 funds at tick 50 then places a REAL buy order through the
# CreateDelivery confirm seam at tick 80; both peers must queue the identical arrival and deduct
# the identical cost (host funds probe at tick 700).
if ($BuyCommandHost) {
    $hostArgs += "-net-match-e2e-buy-command"
}
# M4.6 gate: the CLIENT spawns a second brain for its own team (authority), then the host runs the
# brain-kill on the original; the match must NOT end because the win condition counts every brain.
if ($BrainSpawnTest) {
    $clientArgs += "-net-match-e2e-brain-spawn-command"
    $hostArgs += "-net-match-e2e-brain-kill-command"
}
# M5 gate: the host pauses at tick 250 and unpauses at 430; both sims must stop and resume on the
# same frame with sim time frozen across the whole gap.
if ($PauseTest) {
    $hostArgs += "-net-match-e2e-pause-command"
}
# P5-1 gate: BOTH peers save the full game at the same synced tick 300; the snapshots' sim payloads
# must be byte-identical AND the match must stay sim-gated identical through the save.
if ($SnapshotTest) {
    $hostArgs += "-net-match-e2e-snapshot"
    $clientArgs += "-net-match-e2e-snapshot"
    Remove-Item "$repo\Userdata\UserSavedGames.rte\p5snap_p*.ccsave" -Force -ErrorAction SilentlyContinue
}
# P5-2 gate: the host perturbs its sim at tick 50; the runtime desync detection must fire, BOTH
# peers must resync from the host's snapshot over the live session, and the reloaded round must
# run to the cap with the live desync exchange clean.
if ($ResyncTest) {
    $hostArgs += @("-determinism-selftest-perturb", "-net-match-e2e-resync")
    $clientArgs += "-net-match-e2e-resync"
    Remove-Item "$repo\Userdata\UserSavedGames.rte\p5resync*.ccsave" -Force -ErrorAction SilentlyContinue
}
# P5-4 gate: the host records the match; the offline playback must reproduce it sim-identical.
if ($ReplayTest) {
    $hostArgs += @("-net-replay-out", "$OutDir\match.ccreplay")
    Remove-Item "$OutDir\match.ccreplay" -Force -ErrorAction SilentlyContinue
}
# P7 gate: a simulated high-ping wire (both peers add fake lag) with the host auto-picking the
# input delay from the measured RTT — the match must run stall-free and sim-gated identical.
# Correctness runs free-run the match (one tick per loop iteration, no frame draw); anything that
# measures time (stalls, lag, pace) keeps real-time pacing.
# A UI probe drives placement from the draw path, so those rows keep real-time frames.
$uiProbe = -not [string]::IsNullOrWhiteSpace($HostUiScript) -or -not [string]::IsNullOrWhiteSpace($ClientUiScript)
if (-not $RealTime -and -not $StallTest -and $FakeLagMs -eq 0 -and $FakeLagClientMs -eq 0 -and -not $uiProbe) {
    $hostArgs += "-free-run-sim"
    $clientArgs += "-free-run-sim"
}
if ($FakeLagMs -gt 0) {
    $hostArgs += @("-net-fake-lag", "$FakeLagMs", "-net-match-auto-delay")
    $clientArgs += @("-net-fake-lag", "$FakeLagMs")
}
# P8-1 gate: lag on the CLIENT process only — its own round trip self-pays as its own input
# delay while the host keeps a near-LAN delay; still stall-free and sim-gated identical.
if ($FakeLagClientMs -gt 0) {
    $hostArgs += "-net-match-auto-delay"
    $clientArgs += @("-net-fake-lag", "$FakeLagClientMs")
}
# M7 full loop: economy + combat + result in ONE match — the host buys a real delivery through the
# CreateDelivery seam AND runs the brain-kill to a win, in the same run.
if ($FullLoopTest) {
    $hostArgs += "-net-match-e2e-buy-command"
    $hostArgs += "-net-match-e2e-brain-kill-command"
}
# #19 gate: the host brain-kill ends match 1; both peers return to the lobby over the LIVE session and
# run a full second match (the tick-50/70 kill re-arms after the restart, so round 2 also ends with a win).
if ($RematchTest) {
    $hostArgs += "-net-match-e2e-brain-kill-command"
    $hostArgs += "-net-match-e2e-rematch"
    $clientArgs += "-net-match-e2e-rematch"
}

function Start-CortexCommand {
    param(
        [string[]]$Arguments,
        [string]$StdOutPath,
        [string]$StdErrPath,
        [string]$UiScript = ""
    )

    # Every engine launch goes through tools/isolated_launch.py, the same runner every Python harness uses: a
    # private desktop the user never sees, SW_HIDE, a job that dies with this process, the fullscreen hold and
    # the memory cap. Started directly, the engine put its window, GL context and foreground activation on the
    # user's desktop. The engine's combined output comes back on the wrapper's stdout, so StdOutTask still
    # carries what it carried before; the runner's own record is under <OutDir>\isolated\<peer>\.
    $recordDir = Join-Path (Split-Path -Parent $StdOutPath) ("isolated\" + [System.IO.Path]::GetFileNameWithoutExtension($StdOutPath))
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo.FileName = "python"
    $process.StartInfo.WorkingDirectory = $runtime
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.CreateNoWindow = $true
    $wrapped = @((Join-Path $repo "tools\isolated_launch.py"), "--out", $recordDir, "--cwd", $runtime, "--timeout", "900", "--", (Join-Path $repo "Cortex Command.exe")) + $Arguments
    foreach ($argument in $wrapped) {
        [void]$process.StartInfo.ArgumentList.Add($argument)
    }
    $process.StartInfo.Environment["CCCP_HEADLESS"] = "1"
    if ($UiScript) {
        $process.StartInfo.Environment["CC_TEST_NET_UI_SCRIPT"] = $UiScript
    }
    if (-not $process.Start()) {
        Fail-Run "launch" "Failed to start Cortex Command."
    }
    return [pscustomobject]@{
        Process = $process
        StdOutTask = $process.StandardOutput.ReadToEndAsync()
        StdErrTask = $process.StandardError.ReadToEndAsync()
        StdOutPath = $StdOutPath
        StdErrPath = $StdErrPath
    }
}

# No engine launch while the user is in a fullscreen application: hold here, before the deadline starts counting.
& python (Join-Path $repo "tools\isolated_launch.py") --hold | Out-Null
$hostRun = Start-CortexCommand -Arguments $hostArgs -StdOutPath $hostOut -StdErrPath $hostErr -UiScript $HostUiScript
Start-Sleep -Milliseconds 750
$clientRun = Start-CortexCommand -Arguments $clientArgs -StdOutPath $clientOut -StdErrPath $clientErr -UiScript $ClientUiScript
$hostProcess = $hostRun.Process
$clientProcess = $clientRun.Process

# 150s is the menu-service default; a 900-tick gameplay dump plus editor_script needs more wall.
$waitSeconds = [Math]::Max(150, 90 + [int]($Ticks / 2))
$deadline = (Get-Date).AddSeconds($waitSeconds)
foreach ($process in @($hostProcess, $clientProcess)) {
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
    }
}

$timedOut = $false
foreach ($process in @($hostProcess, $clientProcess)) {
    $process.Refresh()
    if (-not $process.HasExited) {
        $timedOut = $true
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}
foreach ($run in @($hostRun, $clientRun)) {
    $run.Process.WaitForExit()
    [System.IO.File]::WriteAllText($run.StdOutPath, $run.StdOutTask.Result, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($run.StdErrPath, $run.StdErrTask.Result, [System.Text.UTF8Encoding]::new($false))
}
if ($timedOut) {
    Fail-Run "timeout" "P4A menu-service E2E timed out. Evidence: $OutDir"
}

# #20: both peers must fail setup AND both must carry the rich mismatch reason in the report.
if ($MismatchSmoke) {
    if ($hostProcess.ExitCode -eq 0 -or $clientProcess.ExitCode -eq 0) {
        Fail-Run "mismatch_smoke" "MISMATCH SMOKE FAILED: a mismatched pair got past setup (host=$($hostProcess.ExitCode) client=$($clientProcess.ExitCode)). Evidence: $OutDir"
    }
    if (-not (Test-Path $hostReport) -or -not (Test-Path $clientReport)) {
        Fail-Run "mismatch_smoke" "MISMATCH SMOKE did not write both reports. Evidence: $OutDir"
    }
    $mustContain = @("deterministic config hash does not match", "deterministic_config_hash:")
    $hostJson = Get-Content $hostReport -Raw | ConvertFrom-Json
    $clientJson = Get-Content $clientReport -Raw | ConvertFrom-Json
    # The rejected client fails setup with the reason. The host keeps listening for a replacement, so it stays
    # Starting and carries the reason in service.error; its setup_error holds the reason only under
    # -net-match-e2e-join-rejection and is the 60s wait's timeout string otherwise.
    foreach ($needle in $mustContain) {
        if (-not $clientJson.setup_error.Contains($needle)) {
            Fail-Run "mismatch_smoke" "MISMATCH SMOKE FAILED: client setup_error lacks '$needle': '$($clientJson.setup_error)'. Evidence: $OutDir"
        }
        if (-not $clientJson.service.error.Contains($needle)) {
            Fail-Run "mismatch_smoke" "MISMATCH SMOKE FAILED: client service.error lacks '$needle': '$($clientJson.service.error)'. Evidence: $OutDir"
        }
        if (-not $hostJson.service.error.Contains($needle)) {
            Fail-Run "mismatch_smoke" "MISMATCH SMOKE FAILED: host service.error lacks '$needle': '$($hostJson.service.error)'. Evidence: $OutDir"
        }
    }
    if ($clientJson.service.state -ne "Failed") {
        Fail-Run "mismatch_smoke" "MISMATCH SMOKE FAILED: client service_state=$($clientJson.service.state) (expected Failed). Evidence: $OutDir"
    }
    if ($hostJson.service.state -ne "Starting") {
        Fail-Run "mismatch_smoke" "MISMATCH SMOKE FAILED: host service_state=$($hostJson.service.state) (expected Starting, it keeps listening for a replacement). Evidence: $OutDir"
    }
    Write-Host "MISMATCH SMOKE host:   $($hostJson.service.error)"
    Write-Host "MISMATCH SMOKE client: $($clientJson.setup_error)"
    Write-Host "MISMATCH SMOKE PASS: the client failed with the rich reason and the host kept listening carrying it."
    Add-Check "mismatch_smoke" "pass" "client Failed with the rich reason, host still Starting with it in service.error"
    Finish-Run $true
}

# #19: both peers must have completed match 1, reconvened in the lobby, and run round 2 to the cap with
# the LIVE desync exchange clean and the round-2 winner agreed. No traces in this mode; exit here.
if ($RematchTest) {
    $failures = New-Object System.Collections.Generic.List[string]
    if ($hostProcess.ExitCode -ne 0) { $failures.Add("host exit=$($hostProcess.ExitCode)") }
    if ($clientProcess.ExitCode -ne 0) { $failures.Add("client exit=$($clientProcess.ExitCode)") }
    foreach ($run in @(
        [pscustomobject]@{ Name = "host"; Path = $hostReport },
        [pscustomobject]@{ Name = "client"; Path = $clientReport }
    )) {
        if (-not (Test-Path $run.Path)) { $failures.Add("$($run.Name) report missing"); continue }
        $json = Get-Content $run.Path -Raw | ConvertFrom-Json
        if ($json.rematches -ne 1) { $failures.Add("$($run.Name) rematches=$($json.rematches) (expected 1)") }
        if ($json.running_ticks -lt $Ticks) { $failures.Add("$($run.Name) round-2 running_ticks=$($json.running_ticks)") }
        if ($json.runtime_error) { $failures.Add("$($run.Name) runtime_error=$($json.runtime_error)") }
        if ($json.setup_error) { $failures.Add("$($run.Name) setup_error=$($json.setup_error)") }
        if ($json.winner_team -ne 0) { $failures.Add("$($run.Name) round-2 winner_team=$($json.winner_team) (expected 0)") }
        if ($json.service.state -ne "Running") { $failures.Add("$($run.Name) service_state=$($json.service.state)") }
    }
    if ($failures.Count -gt 0) {
        $failures | Out-File -Encoding utf8 "$OutDir\failures.txt"
        Fail-Run "rematch" "REMATCH TEST FAILED: $($failures -join '; '). Evidence: $OutDir"
    }
    Write-Host "REMATCH TEST PASS: match 1 -> lobby -> match 2 to cap, live desync exchange clean, round-2 winner agreed."
    Add-Check "rematch" "pass" "round 2 ran to cap with the winner agreed"
    Finish-Run $true
}

# P5-2: both peers must have detected the desync, resynced from the host's snapshot over the LIVE
# session, and run the reloaded round to the cap with the live desync exchange clean.
if ($ResyncTest) {
    $failures = New-Object System.Collections.Generic.List[string]
    if ($hostProcess.ExitCode -ne 0) { $failures.Add("host exit=$($hostProcess.ExitCode)") }
    if ($clientProcess.ExitCode -ne 0) { $failures.Add("client exit=$($clientProcess.ExitCode)") }
    foreach ($run in @(
        [pscustomobject]@{ Name = "host"; Path = $hostReport; Out = $hostOut },
        [pscustomobject]@{ Name = "client"; Path = $clientReport; Out = $clientOut }
    )) {
        if (-not (Test-Path $run.Path)) { $failures.Add("$($run.Name) report missing"); continue }
        $json = Get-Content $run.Path -Raw | ConvertFrom-Json
        if ($json.resyncs -lt 1) { $failures.Add("$($run.Name) resyncs=$($json.resyncs) (expected >=1)") }
        if ($json.running_ticks -lt $Ticks) { $failures.Add("$($run.Name) post-resync running_ticks=$($json.running_ticks)") }
        if ($json.runtime_error) { $failures.Add("$($run.Name) runtime_error=$($json.runtime_error)") }
        if ($json.setup_error) { $failures.Add("$($run.Name) setup_error=$($json.setup_error)") }
        if ($json.service.state -ne "Running") { $failures.Add("$($run.Name) service_state=$($json.service.state)") }
        # The census: the healed round loads 4 actors (2 brains + 2 dummies) and combat may kill
        # some, but MORE than 4 means a resumed-save respawn doubled them SIM-CONSISTENTLY —
        # invisible to the divergence gates.
        if ($json.actors -gt 4 -or $json.actors -lt 1) { $failures.Add("$($run.Name) actors=$($json.actors) (expected 1..4 - resumed-save respawn?)") }
        # The peak catches a double-spawn that combat later sheds back to <=4 before the final census.
        if ($null -ne $json.actors_peak -and $json.actors_peak -gt 4) { $failures.Add("$($run.Name) actors_peak=$($json.actors_peak) (>4 - transient heal double-spawn)") }
        if ((Get-Content $run.Out -Raw) -notmatch "\[net-match\] resync: match relaunched from the snapshot") {
            $failures.Add("$($run.Name) never relaunched from the snapshot")
        }
    }
    # Both peers must agree on the census (per-run outcomes vary; cross-peer they cannot).
    if ((Test-Path $hostReport) -and (Test-Path $clientReport)) {
        $hostActors = (Get-Content $hostReport -Raw | ConvertFrom-Json).actors
        $clientActors = (Get-Content $clientReport -Raw | ConvertFrom-Json).actors
        if ($hostActors -ne $clientActors) { $failures.Add("census mismatch: host actors=$hostActors client actors=$clientActors") }
    }
    if ($failures.Count -gt 0) {
        $failures | Out-File -Encoding utf8 "$OutDir\failures.txt"
        Fail-Run "resync" "RESYNC TEST FAILED: $($failures -join '; '). Evidence: $OutDir"
    }
    Write-Host "RESYNC TEST PASS: desync detected -> both peers reloaded the host snapshot over the live session -> reloaded round ran to cap clean."
    Add-Check "resync" "pass" "heal reloaded and the round ran to cap"
    Finish-Run $true
}

# Positive control: the injected divergence must be CAUGHT. Since the win condition landed, a diverged
# match can end itself mid-run (win/draw fires differently per peer -> actor-state apply errors), which
# is detection too. Otherwise both peers run to cap and the sim-gated compare below must fail.
if ($PerturbHost -and ($hostProcess.ExitCode -ne 0 -or $clientProcess.ExitCode -ne 0)) {
    $divergencePattern = "apply failed|Desync|diverged|desync"
    $errs = @()
    foreach ($rep in @($hostReport, $clientReport)) {
        if (Test-Path $rep) { $errs += (Get-Content $rep -Raw | ConvertFrom-Json).runtime_error }
    }
    if (($errs -join " ") -match $divergencePattern) {
        Write-Host "POSITIVE CONTROL PASS: the injected host divergence was caught at runtime ($($errs -join ' | '))."
        Add-Check "positive_control" "pass" "runtime detection: $($errs -join ' | ')"
        Finish-Run $true
    }
    Fail-Run "positive_control" "POSITIVE CONTROL run failed for a non-divergence reason: $($errs -join ' | '). Evidence: $OutDir"
}

if ($hostProcess.ExitCode -ne 0 -or $clientProcess.ExitCode -ne 0) {
    Fail-Run "process_exit" "P4A menu-service E2E process failure. host=$($hostProcess.ExitCode) client=$($clientProcess.ExitCode). Evidence: $OutDir"
}
if (-not (Test-Path $hostReport) -or -not (Test-Path $clientReport)) {
    Fail-Run "reports_present" "P4A menu-service E2E did not write both reports. Evidence: $OutDir"
}

$hostJson = Get-Content $hostReport -Raw | ConvertFrom-Json
$clientJson = Get-Content $clientReport -Raw | ConvertFrom-Json

$failures = New-Object System.Collections.Generic.List[string]
foreach ($run in @(
    [pscustomobject]@{ Name = "host"; Json = $hostJson },
    [pscustomobject]@{ Name = "client"; Json = $clientJson }
)) {
    $name = $run.Name
    $json = $run.Json
    if ($json.exit_code -ne 0) { $failures.Add("$name exit_code=$($json.exit_code)") }
    # A UI-scripted placement is supposed to enter the editor; leftover editor (cap, no leave) stays a fail.
    $leftEditor = [int]$json.running_ticks -gt 0 -and @('Running', 'Over') -contains [string]$json.activity_state
    $peerLog = if ($name -eq 'host') { $hostOut } else { $clientOut }
    $peerText = if (Test-Path $peerLog) { Get-Content $peerLog -Raw } else { '' }
    $placed = $peerText -match 'via=editor' -or $peerText -match 'placed[: ].*uid='
    if ($json.entered_editor -and -not ($uiProbe -and $leftEditor -and $placed)) {
        $failures.Add("$name entered unsynchronized editor")
    }
    if ($json.running_ticks -lt $Ticks) { $failures.Add("$name running_ticks=$($json.running_ticks)") }
    if ($json.service.state -ne "Running") { $failures.Add("$name service_state=$($json.service.state)") }
    if ($json.service.runner.state -ne "Running") { $failures.Add("$name runner_state=$($json.service.runner.state)") }
    $postCapPeerShutdown = $json.exit_code -eq 0 -and $json.running_ticks -ge $Ticks -and
        ($json.service.runner.lockstep.timeout_reason -like "MissingFrameTimeout:missing lockstep frame*" -or
         $json.service.runner.lockstep.timeout_reason -like "PeerDisconnected:*" -or
         $json.service.runner.lockstep.timeout_reason -like "PeerLeft:*")
    if ($json.service.runner.lockstep.timeout_reason -and -not $json.service.runner.lockstep.timeout_reason.StartsWith("Complete:") -and -not $postCapPeerShutdown) {
        $failures.Add("$name lockstep_timeout=$($json.service.runner.lockstep.timeout_reason)")
    }
    # P8-3: matches must hold the pinned dt's true pace; stall and lag cases legitimately stretch wall time.
    if (-not $StallTest -and $FakeLagMs -eq 0 -and $FakeLagClientMs -eq 0 -and [double]$json.pace.wall_tps -lt 55) {
        $failures.Add("$name wall_tps=$($json.pace.wall_tps) (expected >=55: pace regression)")
    }
}

if ($hostJson.service.runner.match_config_hash -ne $clientJson.service.runner.match_config_hash) {
    $failures.Add("match config hashes differ")
}

# #15: all four inventory ops must have been issued and none may have failed to apply on either peer.
if ($InventoryCommandHost) {
    $hostOutText = Get-Content $hostOut -Raw
    $opCount = ([regex]::Matches($hostOutText, "\[net-match-service-e2e\] inventory op ")).Count
    if ($opCount -ne 4) { $failures.Add("host issued $opCount inventory ops (expected 4)") }
    foreach ($rep in @(@{n="host"; p=$hostOut}, @{n="client"; p=$clientOut})) {
        if ((Get-Content $rep.p -Raw) -match "inventory command did not apply") {
            $failures.Add("$($rep.n) had an inventory command that did not apply")
        }
    }
}

# M4.4: the buy order must be issued through the real CreateDelivery seam, queue on BOTH peers with the
# identical funds transition, and deduct exactly its cost from the granted 5000.
if ($BuyCommandHost) {
    $hostOutText = Get-Content $hostOut -Raw
    $clientOutText = Get-Content $clientOut -Raw
    if ($hostOutText -notmatch "\[net-match-service-e2e\] buy order placed: ok") {
        $failures.Add("host never placed the buy order (CreateDelivery failed?)")
    }
    if ($hostOutText -notmatch "\[net-match\] buy order issued: team 0 cost (\d+(?:\.\d+)?) items 2") {
        $failures.Add("host never issued the buy order through the confirm seam")
    } else {
        $cost = [double]$Matches[1]
        if ($cost -le 0) { $failures.Add("buy order cost=$cost (expected > 0)") }
        $hostQueued = [regex]::Match($hostOutText, "\[net-match\] buy order queued: team 0 cost [^`r`n]+")
        $clientQueued = [regex]::Match($clientOutText, "\[net-match\] buy order queued: team 0 cost [^`r`n]+")
        if (-not $hostQueued.Success) { $failures.Add("host never queued the buy order") }
        if (-not $clientQueued.Success) { $failures.Add("client never queued the buy order") }
        if ($hostQueued.Success -and $clientQueued.Success -and $hostQueued.Value -ne $clientQueued.Value) {
            $failures.Add("buy order funds transition differs: host='$($hostQueued.Value)' client='$($clientQueued.Value)'")
        }
        if ($hostOutText -notmatch "\[net-match-service-e2e\] team 0 funds at tick 700: (-?\d+(?:\.\d+)?)") {
            $failures.Add("host never logged the tick-700 funds probe")
        } elseif ([math]::Abs([double]$Matches[1] - (5000 - $cost)) -gt 0.01) {
            $failures.Add("host funds at 700 = $($Matches[1]) (expected $(5000 - $cost))")
        }
    }
    foreach ($rep in @(@{n="host"; t=$hostOutText}, @{n="client"; t=$clientOutText})) {
        if ($rep.t -match "buy order (rejected|did not queue|item skipped|target not found)") {
            $failures.Add("$($rep.n) had a buy order failure line")
        }
    }
}

# M4.6: the spawned second brain must keep the match alive through the original brain's death.
# Every capped run reports Over after the e2e teardown; the discriminator is the winner: a wrong
# early end here would be a team-0 WIN, so both peers must finish winnerless.
if ($BrainSpawnTest) {
    $clientOutText = Get-Content $clientOut -Raw
    if ($clientOutText -notmatch "\[net-match-service-e2e\] brain spawn") {
        $failures.Add("client never spawned the second brain")
    }
    if ($hostJson.winner_team -ne -1) { $failures.Add("host winner_team=$($hostJson.winner_team) (expected -1: no winner)") }
    if ($clientJson.winner_team -ne -1) { $failures.Add("client winner_team=$($clientJson.winner_team) (expected -1: no winner)") }
}

# M7 full loop: the buy must deduct identically AND the brain-kill must produce the agreed winner in
# the same match — economy and result together.
if ($FullLoopTest) {
    $hostOutText = Get-Content $hostOut -Raw
    $clientOutText = Get-Content $clientOut -Raw
    $hostQueued = [regex]::Match($hostOutText, "\[net-match\] buy order queued: team 0 cost [^`r`n]+")
    $clientQueued = [regex]::Match($clientOutText, "\[net-match\] buy order queued: team 0 cost [^`r`n]+")
    if (-not $hostQueued.Success -or -not $clientQueued.Success) { $failures.Add("full-loop: a peer never queued the buy order") }
    elseif ($hostQueued.Value -ne $clientQueued.Value) { $failures.Add("full-loop: buy funds transition differs") }
    if ($hostJson.winner_team -ne 0) { $failures.Add("full-loop: host winner_team=$($hostJson.winner_team) (expected 0)") }
    if ($clientJson.winner_team -ne 0) { $failures.Add("full-loop: client winner_team=$($clientJson.winner_team) (expected 0)") }
}

# M5: both peers must pause and resume on the SAME tick with sim time frozen across the gap.
if ($PauseTest) {
    $hostOutText = Get-Content $hostOut -Raw
    $clientOutText = Get-Content $clientOut -Raw
    $hp = [regex]::Match($hostOutText, "\[net-match\] match paused at tick (\d+) sim ms (\d+)")
    $cp = [regex]::Match($clientOutText, "\[net-match\] match paused at tick (\d+) sim ms (\d+)")
    $hr = [regex]::Match($hostOutText, "\[net-match\] match resumed at tick (\d+) sim ms (\d+)")
    $cr = [regex]::Match($clientOutText, "\[net-match\] match resumed at tick (\d+) sim ms (\d+)")
    if (-not $hp.Success -or -not $cp.Success) { $failures.Add("a peer never paused") }
    elseif ($hp.Groups[1].Value -ne $cp.Groups[1].Value) { $failures.Add("pause tick differs: host=$($hp.Groups[1].Value) client=$($cp.Groups[1].Value)") }
    if (-not $hr.Success -or -not $cr.Success) { $failures.Add("a peer never resumed") }
    elseif ($hr.Groups[1].Value -ne $cr.Groups[1].Value) { $failures.Add("resume tick differs: host=$($hr.Groups[1].Value) client=$($cr.Groups[1].Value)") }
    if ($hp.Success -and $hr.Success -and $hp.Groups[2].Value -ne $hr.Groups[2].Value) {
        $failures.Add("host sim time moved while paused: $($hp.Groups[2].Value) -> $($hr.Groups[2].Value)")
    }
    if ($cp.Success -and $cr.Success -and $cp.Groups[2].Value -ne $cr.Groups[2].Value) {
        $failures.Add("client sim time moved while paused: $($cp.Groups[2].Value) -> $($cr.Groups[2].Value)")
    }
}

# P7/P8-1: at simulated high ping the auto-picked delays must cover the wire — no per-tick
# stalls at all — and the sim-gated compare below proves the lag never touched the sim. The
# pick is per SENDER: the lagged side pays its own round trip; an unlagged host stays near-LAN.
if ($FakeLagMs -gt 0 -or $FakeLagClientMs -gt 0) {
    $hostOutText = Get-Content $hostOut -Raw
    $clientOutText = Get-Content $clientOut -Raw
    # Both-process lag stacks per leg (send half + recv half each way); client-only lag is its RTT.
    $expectedRtt = if ($FakeLagMs -gt 0) { 2 * $FakeLagMs } else { $FakeLagClientMs }
    $autoDelay = [regex]::Match($hostOutText, "\[net-match\] auto input delay: peer 2 rtt (\d+)ms -> (\d+) frames")
    if (-not $autoDelay.Success) {
        $failures.Add("fake-lag: the host never auto-picked the client's input delay")
    } else {
        if ([int]$autoDelay.Groups[1].Value -lt ($expectedRtt - 80)) {
            $failures.Add("fake-lag: measured rtt $($autoDelay.Groups[1].Value)ms is under the injected $expectedRtt ms (did the fake lag apply?)")
        }
        if ([int]$clientJson.service.runner.lockstep.input_delay_frames -ne [int]$autoDelay.Groups[2].Value) {
            $failures.Add("fake-lag: client runs delay $($clientJson.service.runner.lockstep.input_delay_frames) but the host picked $($autoDelay.Groups[2].Value)")
        }
    }
    if ([int]$hostJson.service.runner.lockstep.input_delay_frames -gt 2) {
        $failures.Add("fake-lag: host input delay $($hostJson.service.runner.lockstep.input_delay_frames) is not near-LAN (expected <=2; the lag must self-pay on the sender)")
    }
    if ([int]$hostJson.service.runner.lockstep.peer_input_delays.'2' -ne [int]$clientJson.service.runner.lockstep.peer_input_delays.'2') {
        $failures.Add("fake-lag: peers disagree on the per-peer delay set")
    }
    foreach ($peer in @(@{n="host"; t=$hostOutText}, @{n="client"; t=$clientOutText})) {
        if ($peer.t -match "\[net-match\] waiting on peer frames") {
            $failures.Add("fake-lag: $($peer.n) stalled mid-match despite the auto delay")
        }
    }
}

# #21: the host must have actually stalled (>1.5s marker) and recovered; liveness + the sim-gated
# compare below prove the stall never touched the sim.
if ($StallTest) {
    $hostOutText = Get-Content $hostOut -Raw
    if ($hostOutText -notmatch "\[net-match\] waiting on peer frames") {
        $failures.Add("host never logged the peer-frame stall marker (did the stall happen?)")
    }
    if ($hostOutText -notmatch "\[net-match\] peer stall recovered after (\d+)ms") {
        $failures.Add("host never logged stall recovery")
    } elseif ([int]$Matches[1] -lt 6000) {
        $failures.Add("host stall recovered after only $($Matches[1])ms (expected ~8s)")
    }
}

# #18: the brain kill must actually end the match, with both peers agreeing on the winner (team 0 killed team 1's brain).
if ($BrainKillCommandHost) {
    if ($hostJson.activity_state -ne "Over") { $failures.Add("host activity_state=$($hostJson.activity_state) (expected Over)") }
    if ($clientJson.activity_state -ne "Over") { $failures.Add("client activity_state=$($clientJson.activity_state) (expected Over)") }
    if ($hostJson.winner_team -ne $clientJson.winner_team) { $failures.Add("winner_team mismatch host=$($hostJson.winner_team) client=$($clientJson.winner_team)") }
    if ($hostJson.winner_team -ne 0) { $failures.Add("winner_team=$($hostJson.winner_team) (expected 0)") }
}

if ($failures.Count -gt 0) {
    $failures | Out-File -Encoding utf8 "$OutDir\failures.txt"
    foreach ($failure in $failures) { Add-Check "liveness" "fail" $failure }
    Fail-Run "liveness" "P4A menu-service E2E failed: $($failures -join '; '). Evidence: $OutDir"
}
Add-Check "process_exit" "pass" "host=$($hostProcess.ExitCode) client=$($clientProcess.ExitCode)"
Add-Check "liveness" "pass" "host ticks=$($hostJson.running_ticks) client ticks=$($clientJson.running_ticks) of $Ticks"
Add-Check "config_agreement" "pass" $hostJson.service.runner.match_config_hash
$hostIdentity = $hostJson.service.runner.session.local_identity
$clientIdentity = $clientJson.service.runner.session.local_identity
if ($hostIdentity.module_manifest_hash -and $hostIdentity.module_manifest_hash -eq $clientIdentity.module_manifest_hash -and $hostIdentity.deterministic_config_hash -eq $clientIdentity.deterministic_config_hash) {
    Add-Check "content_identity_agreement" "pass" "module manifest $($hostIdentity.module_manifest_hash) and deterministic config $($hostIdentity.deterministic_config_hash) agree"
} else {
    Add-Check "content_identity_agreement" "fail" "host manifest=$($hostIdentity.module_manifest_hash) config=$($hostIdentity.deterministic_config_hash) client manifest=$($clientIdentity.module_manifest_hash) config=$($clientIdentity.deterministic_config_hash)"
}
foreach ($mode in $script:Settings.modes) {
    if ($mode -notin $script:ModeCheckExclusions) {
        if ($mode -eq "DeliverCommandHost") {
            $deliverText = ''
            if ($hostOut -and (Test-Path $hostOut)) { $deliverText = Get-Content $hostOut -Raw }
            if ($deliverText -match '\[net-match\] deliver command host applied tick=(\d+) team=(\d+) order=(\d+) items=(\d+) peer=(\d+)') {
                Add-Check "mode_delivercommandhost" "pass" $Matches[0]
            } else {
                Add-Check "mode_delivercommandhost" "fail" "missing applied line [net-match] deliver command host applied tick= team= order= items= peer="
            }
        } else {
            Add-Check ("mode_" + $mode.ToLower()) "pass" "mode-specific assertions held"
        }
    }
}

# Prediction: a run with an input delay must have actually previewed on the delayed peers; D=0 cannot validate prediction.
$predictionOff = $ExtraArgs -match "-net-local-prediction off"
$delayedPeers = @()
foreach ($run in @([pscustomobject]@{ Name = "host"; Json = $hostJson }, [pscustomobject]@{ Name = "client"; Json = $clientJson })) {
    $delayFrames = [int]$run.Json.service.runner.lockstep.input_delay_frames
    if ($delayFrames -gt 0) {
        $delayedPeers += $run.Name
        $lp = $run.Json.local_prediction
        $previews = if ($null -ne $lp) { [int]$lp.previews } else { -1 }
        if (-not $predictionOff -and $previews -le 0) {
            Add-Check "prediction_executed" "fail" "$($run.Name) ran with input delay $delayFrames but previewed $previews frames"
        } elseif (-not $predictionOff) {
            Add-Check "prediction_executed" "pass" "$($run.Name) delay=$delayFrames previews=$previews actor_ticks=$($lp.actor_ticks) ms_total=$($lp.ms_total) shadows=$($lp.shadows) taken=$($lp.taken)"
            # Isolation: speculative execution never wrote to the world; the report's violation counter is the tripwire.
            $violations = if ($null -ne $lp.violations) { [int]$lp.violations } else { -1 }
            if ($violations -eq 0) {
                Add-Check "prediction_isolation" "pass" "$($run.Name) previews=$previews shadows=$($lp.shadows) taken=$($lp.taken) violations=0"
            } else {
                Add-Check "prediction_isolation" "fail" "$($run.Name) speculative execution wrote to the world $violations time(s) (previews=$previews taken=$($lp.taken))"
            }
        }
    }
}
# Controller boundary: the AI pass must cross only as commands and intents; a direct write on the canonical actor fails.
foreach ($run in @([pscustomobject]@{ Name = "host"; Json = $hostJson }, [pscustomobject]@{ Name = "client"; Json = $clientJson })) {
    $cb = $run.Json.controller_boundary
    if ($null -eq $cb) {
        Add-Check "controller_boundary" "fail" "$($run.Name) report has no controller_boundary block"
    } elseif ([int]$cb.direct_writes -eq 0) {
        Add-Check "controller_boundary" "pass" "$($run.Name) equip_commands=$($cb.equip_commands) aim_intents=$($cb.aim_intents) flip_intents=$($cb.flip_intents) direct_writes=0"
    } else {
        Add-Check "controller_boundary" "fail" "$($run.Name) AI pass wrote the canonical actor directly $($cb.direct_writes) time(s)"
    }
}
if ($RequirePredictionTaken) {
    $hostTaken = if ($null -ne $hostJson.local_prediction) { [int]$hostJson.local_prediction.taken } else { -1 }
    if ($predictionOff -or $hostTaken -lt 1) {
        Add-Check "prediction_taken" "fail" "the host's previews took $hostTaken resident(s); a predicted pickup was required"
    } else {
        Add-Check "prediction_taken" "pass" "the host's previews took $hostTaken resident(s) out of the world overlay"
    }
}
if ($predictionOff -and $RequirePrediction) {
    Add-Check "prediction_executed" "fail" "-RequirePrediction was set but prediction is disabled by -net-local-prediction off"
} elseif ($predictionOff) {
    Add-Check "prediction_executed" "n/a" "prediction disabled by -net-local-prediction off (control run)" $false
} elseif ($delayedPeers.Count -eq 0) {
    if ($RequirePrediction) { Add-Check "prediction_executed" "fail" "no peer ran with an input delay, so prediction never executed" }
    else { Add-Check "prediction_executed" "n/a" "D=0: regression-only run; prediction not exercised" $false }
}

# Sim-gated determinism: host vs client per-tick, controller excluded. The liveness checks above do not prove this.
if (-not (Test-Path $hostTrace) -or -not (Test-Path $clientTrace)) {
    Fail-Run "traces_present" "P4A menu-service E2E did not write both determinism traces. Evidence: $OutDir"
}
$compareScript = "D:\Projects\stage2_p4\compare_e2e_simgated.py"
# UI-probe rows hash PAUSED_CORE through this tree's paused-tolerant comparer. Both compares of one row share this path.
if ($uiProbe) {
    $rowCompare = Join-Path $repo "tools\compare_sim_traces.py"
    $rowCompareArgs = @("--min-ticks", "1")
    $minTicks = 1
} else {
    $rowCompare = $compareScript
    $rowCompareArgs = @("--expected-ticks", "$Ticks")
    $minTicks = $Ticks
}
& python $rowCompare $hostTrace $clientTrace @rowCompareArgs 2>&1 |
    Tee-Object -FilePath "$OutDir\simgated_compare.txt" | Out-Host
$compareExit = $LASTEXITCODE
if ($PerturbHost) {
    if ($compareExit -eq 0) {
        Fail-Run "positive_control" "POSITIVE CONTROL FAILED: gate did NOT catch the injected host divergence. Evidence: $OutDir"
    }
    Write-Host "POSITIVE CONTROL PASS: sim-gated compare correctly caught the injected host divergence."
    Add-Check "positive_control" "pass" "the sim-gated compare caught the injected divergence"
    Finish-Run $true
}
if ($compareExit -ne 0) {
    Fail-Run "simgated" "P4A menu-service E2E SIM-GATED DETERMINISM FAILED (host vs client diverged). Evidence: $OutDir"
}
$simGatedLine = (Get-Content "$OutDir\simgated_compare.txt" -Raw).Trim()
$simGatedTicks = ([regex]::Match($simGatedLine, "(\d+) overlapping ticks identical")).Groups[1].Value
if (-not $uiProbe) {
    if (-not $simGatedTicks -or [int]$simGatedTicks -lt $minTicks) {
        Fail-Run "simgated" "SIM-GATED compare covered only '$simGatedTicks' ticks (need >= $minTicks). Evidence: $OutDir"
    }
} elseif (-not $simGatedTicks -or [int]$simGatedTicks -lt 1) {
    Fail-Run "simgated" "SIM-GATED unpaused compare covered only '$simGatedTicks' ticks. Evidence: $OutDir"
}
Add-Check "simgated" "pass" "$simGatedTicks ticks identical host==client, controller excluded"

# P5-1: both snapshots must exist, have logged their save, and carry byte-identical sim payloads.
if ($SnapshotTest) {
    foreach ($rep in @(@{n="host"; p=$hostOut}, @{n="client"; p=$clientOut})) {
        if ((Get-Content $rep.p -Raw) -notmatch "\[net-match\] snapshot saved: p5snap_p\d in (\d+)ms") {
            Fail-Run "snapshot" "SNAPSHOT TEST FAILED: $($rep.n) never logged a completed save. Evidence: $OutDir"
        }
        Write-Host "SNAPSHOT $($rep.n): saved in $($Matches[1])ms"
    }
    $snapA = "$repo\Userdata\UserSavedGames.rte\p5snap_p1.ccsave"
    $snapB = "$repo\Userdata\UserSavedGames.rte\p5snap_p2.ccsave"
    if (-not (Test-Path $snapA) -or -not (Test-Path $snapB)) {
        Fail-Run "snapshot" "SNAPSHOT TEST FAILED: a .ccsave is missing ($snapA / $snapB). Evidence: $OutDir"
    }
    Copy-Item $snapA, $snapB $OutDir
    $env:CCCP_TOOLS_DIR = Join-Path $repo "tools"
    $snapshotCompare = Join-Path $repo "tools\compare_snapshots.py"
    & python $snapshotCompare $snapA $snapB --peer-report-a $hostReport --peer-report-b $clientReport --cross-process 2>&1 |
        Tee-Object -FilePath "$OutDir\snapshot_compare.txt" | Out-Host
    if ($LASTEXITCODE -ne 0) {
        Fail-Run "snapshot" "SNAPSHOT TEST FAILED: the two peers' snapshots differ in the sim payload. Evidence: $OutDir"
    }
    Add-Check "snapshot" "pass" "both peers saved at the synced tick and the sim payloads are byte-identical"
}

# P5-4: play the recording back offline; the deterministic sim must reproduce the recorded match
# (sim-gated identical to the recording peer's own trace).
if ($ReplayTest) {
    if (-not (Test-Path "$OutDir\match.ccreplay")) {
        Fail-Run "replay_recording_intact" "REPLAY TEST FAILED: no recording was written. Evidence: $OutDir"
    }
    # The recording itself must be complete: every record decodes, the end marker is present, no gaps.
    Push-Location $runtime
    & python "$repo\tools\isolated_launch.py" --out "$OutDir\isolated\replay_verify" --cwd $runtime -- "$repo\Cortex Command.exe" -net-replay-verify "$OutDir\match.ccreplay" -out "$OutDir\replay_verify.json" 2>&1 | Out-File -Encoding utf8 "$OutDir\replay_verify.txt"
    $verifyExit = $LASTEXITCODE
    Pop-Location
    $verify = $null
    if (Test-Path "$OutDir\replay_verify.json") { $verify = Get-Content "$OutDir\replay_verify.json" -Raw | ConvertFrom-Json }
    if ($verifyExit -ne 0 -or $null -eq $verify -or -not $verify.ok -or [int]$verify.last_frame -lt $Ticks) {
        Fail-Run "replay_recording_intact" "REPLAY TEST FAILED: the recording failed its integrity scan (exit=$verifyExit): $(if ($verify) { $verify | ConvertTo-Json -Compress } else { 'no verify report' }). Evidence: $OutDir"
    }
    Add-Check "replay_recording_intact" "pass" "frames=$($verify.frames) last_frame=$($verify.last_frame) end_marker=$($verify.end_marker)"
    $replayTrace = "$OutDir\replay_trace.json"
    $replayLog = "$OutDir\replay.out.txt"
    Push-Location $runtime
    # The playback's viewer seat follows the recorded human only through the same presentation inputs, so the
    # host's input script rides along; its actor inputs are overridden by the recorded wire frames.
    $replayExtra = @()
    if ($HostExtraArgs) {
        $hostParts = $HostExtraArgs.Split(" ")
        for ($k = 0; $k -lt $hostParts.Length - 1; $k++) {
            if ($hostParts[$k] -eq "-input-script") { $replayExtra += @("-input-script", $hostParts[$k + 1]) }
        }
    }
    & python "$repo\tools\isolated_launch.py" --out "$OutDir\isolated\replay" --cwd $runtime -- "$repo\Cortex Command.exe" -net-replay "$OutDir\match.ccreplay" -tick-hashes -max-ticks $Ticks -out $replayTrace @replayExtra 2>&1 |
        Out-File -Encoding utf8 $replayLog
    $replayExit = $LASTEXITCODE
    Pop-Location
    $replayOutcome = "missing"
    $replayLastTick = -1
    if (Test-Path $replayTrace) {
        $rt = Get-Content $replayTrace -Raw | ConvertFrom-Json
        $replayOutcome = [string]$rt.runs[0].strings.replay_outcome
        $replayLastTick = [int]$rt.runs[0].numeric.replay_last_tick
    }
    if ($replayExit -ne 0 -or ($replayOutcome -ne "completed" -and $replayOutcome -ne "tick_cap") -or $replayLastTick -lt $minTicks) {
        Fail-Run "replay_playback" "REPLAY TEST FAILED: playback exit=$replayExit outcome=$replayOutcome last_tick=$replayLastTick (need completed/tick_cap and >= $minTicks). Evidence: $replayLog"
    }
    Add-Check "replay_playback" "pass" "exit=$replayExit outcome=$replayOutcome last_tick=$replayLastTick"
    # Same $rowCompare / $rowCompareArgs as the sim-gated compare above. A split here is the humans_vs_cpu red.
    Set-Content -Path "$OutDir\compare_route.txt" -Value "$rowCompare $($rowCompareArgs -join ' ')"
    & python $rowCompare $hostTrace $replayTrace @rowCompareArgs 2>&1 |
        Tee-Object -FilePath "$OutDir\replay_compare.txt" | Out-Host
    if ($LASTEXITCODE -ne 0) {
        Fail-Run "replay_compare" "REPLAY TEST FAILED: playback diverged from the recorded match. Evidence: $OutDir"
    }
    $replayCompareLine = (Get-Content "$OutDir\replay_compare.txt" -Raw).Trim()
    $comparedTicks = ([regex]::Match($replayCompareLine, "(\d+) overlapping ticks identical")).Groups[1].Value
    if (-not $comparedTicks -or [int]$comparedTicks -lt $minTicks) {
        Fail-Run "replay_compare" "REPLAY TEST FAILED: only '$comparedTicks' ticks compared (need >= $minTicks). Evidence: $OutDir"
    }
    Add-Check "replay_compare" "pass" "$comparedTicks ticks identical to the host trace"
    $playbackMs = ([regex]::Match((Get-Content $replayLog -Raw), "playback finished in (\d+)ms")).Groups[1].Value
    Write-Host "REPLAY TEST PASS: offline playback reproduced the recorded match sim-identical ($playbackMs ms for $Ticks ticks, outcome=$replayOutcome)."
}

$summary = @"
# P4A Menu-Service E2E

OutDir: $OutDir
Port: $Port
Ticks: $Ticks
Host exit: $($hostProcess.ExitCode)
Client exit: $($clientProcess.ExitCode)
Host running_ticks: $($hostJson.running_ticks)
Client running_ticks: $($clientJson.running_ticks)
Match config hash: $($hostJson.service.runner.match_config_hash)
Editor entered: host=$($hostJson.entered_editor) client=$($clientJson.entered_editor)
Sim-gated determinism: $simGatedLine

PASS (liveness + no-editor + config-agreement + sim-gated host==client)
"@
$summary | Out-File -Encoding utf8 "$OutDir\SUMMARY.md"
Write-Host $summary
Finish-Run $true
