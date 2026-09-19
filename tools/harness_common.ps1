# Shared by the stage2_p4 harnesses: what binary and what source (committed, staged, unstaged,
# untracked) and which test scripts a run actually exercised.

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Get-TextSha256([string]$Text) {
    if ([string]::IsNullOrEmpty($Text)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace("-", "")
}

# The newest file among the tracked build inputs and any untracked file under Source that the build
# reads (data files such as the state inventory CSV are not compiled in).
$script:BuildInputExtensions = @(".cpp", ".c", ".h", ".hpp", ".inl", ".rc", ".vcxproj", ".props", ".build", ".txt", ".lua")
function Get-NewestSourceMtime([string]$Repo) {
    $tracked = @(git -C $Repo ls-files -- Source RTEA.vcxproj RTEA.common.props meson.build)
    $untracked = @(git -C $Repo ls-files --others --exclude-standard -- Source)
    $newest = [datetime]::MinValue
    $newestPath = ""
    foreach ($rel in ($tracked + $untracked)) {
        if ([string]::IsNullOrWhiteSpace($rel)) { continue }
        if ([IO.Path]::GetExtension($rel).ToLower() -notin $script:BuildInputExtensions) { continue }
        $full = Join-Path $Repo $rel
        if (Test-Path -LiteralPath $full) {
            $t = [System.IO.File]::GetLastWriteTimeUtc($full)
            if ($t -gt $newest) { $newest = $t; $newestPath = $rel }
        }
    }
    return [pscustomobject]@{ mtime_utc = $newest; path = $newestPath }
}

function Get-Provenance {
    param(
        [string]$Repo,
        [bool]$BuiltByThisRun = $false,
        [string[]]$ScriptPaths = @(),
        [datetime]$BuildStartedUtc = [datetime]::MinValue,
        [switch]$FaultNewerSource
    )
    $exe = Join-Path $Repo "Cortex Command.exe"
    $exeInfo = Get-Item -LiteralPath $exe
    $dirty = @(git -C $Repo status --porcelain --untracked-files=no)
    $untrackedSource = @(git -C $Repo ls-files --others --exclude-standard -- Source RTEA.vcxproj RTEA.common.props meson.build)
    $unstaged = (git -C $Repo diff) -join "`n"
    $staged = (git -C $Repo diff --cached) -join "`n"
    $untrackedText = ""
    foreach ($rel in $untrackedSource) {
        if ([string]::IsNullOrWhiteSpace($rel)) { continue }
        $untrackedText += "== $rel`n" + (Get-Content -LiteralPath (Join-Path $Repo $rel) -Raw)
    }
    $newest = Get-NewestSourceMtime $Repo
    if ($FaultNewerSource) {
        # Test-only fault: pretend a source file was saved after the binary was linked.
        $newest = [pscustomobject]@{ mtime_utc = (Get-Date).ToUniversalTime(); path = "(fault: FaultNewerSource)" }
    }
    $scripts = New-Object System.Collections.Generic.List[object]
    foreach ($p in $ScriptPaths) {
        if ([string]::IsNullOrWhiteSpace($p)) { continue }
        $hash = if (Test-Path -LiteralPath $p) { Get-FileSha256 $p } else { "missing" }
        $scripts.Add([pscustomobject]@{ path = $p; sha256 = $hash })
    }
    return [pscustomobject]@{
        exe = $exe
        exe_sha256 = Get-FileSha256 $exe
        exe_mtime_utc = $exeInfo.LastWriteTimeUtc.ToString("o")
        exe_bytes = $exeInfo.Length
        git_head = (git -C $Repo rev-parse HEAD).Trim()
        git_branch = (git -C $Repo rev-parse --abbrev-ref HEAD).Trim()
        git_dirty_files = $dirty
        git_unstaged_diff_sha256 = Get-TextSha256 $unstaged
        git_staged_diff_sha256 = Get-TextSha256 $staged
        git_untracked_source = $untrackedSource
        git_untracked_source_sha256 = Get-TextSha256 $untrackedText
        newest_source_mtime_utc = $newest.mtime_utc.ToString("o")
        newest_source_path = $newest.path
        exe_newer_than_source = ($exeInfo.LastWriteTimeUtc -ge $newest.mtime_utc)
        built_by_this_run = $BuiltByThisRun
        build_started_utc = $(if ($BuiltByThisRun) { $BuildStartedUtc.ToString("o") } else { "" })
        exe_linked_after_build_start = $(if ($BuiltByThisRun) { $exeInfo.LastWriteTimeUtc -ge $BuildStartedUtc.AddSeconds(-2) } else { $false })
        build_config = "Final x64"
        scripts = $scripts.ToArray()
        captured_at = (Get-Date).ToString("o")
    }
}

# Everything that identifies what was tested; two captures with equal identities tested the same thing.
function Get-ProvenanceIdentity($p) {
    $scriptHashes = @($p.scripts | ForEach-Object { "$($_.path)=$($_.sha256)" }) -join ","
    return "$($p.exe_sha256)|$($p.git_head)|$($p.git_unstaged_diff_sha256)|$($p.git_staged_diff_sha256)|$($p.git_untracked_source_sha256)|$scriptHashes"
}

# The binary must be the one the source describes: either this run built it, or it is newer than every build input.
function Get-BinaryMatchesSource($p) {
    if ($p.built_by_this_run) {
        if ($p.exe_linked_after_build_start -or $p.exe_newer_than_source) {
            return [pscustomobject]@{ pass = $true; detail = "built by this run (linked $($p.exe_mtime_utc); newest source $($p.newest_source_path) at $($p.newest_source_mtime_utc))" }
        }
        return [pscustomobject]@{ pass = $false; detail = "the build reported success but the exe ($($p.exe_mtime_utc)) predates both the build start ($($p.build_started_utc)) and $($p.newest_source_path) ($($p.newest_source_mtime_utc))" }
    }
    if ($p.exe_newer_than_source) {
        return [pscustomobject]@{ pass = $true; detail = "SkipBuild: exe ($($p.exe_mtime_utc)) is newer than every build input (newest $($p.newest_source_path) at $($p.newest_source_mtime_utc))" }
    }
    return [pscustomobject]@{ pass = $false; detail = "stale binary: $($p.newest_source_path) ($($p.newest_source_mtime_utc)) is newer than the exe ($($p.exe_mtime_utc)); rebuild before testing" }
}
