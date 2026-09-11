# Pre-authorise every Cortex Command executable under the project roots so Windows never shows a firewall prompt for them.
# Reparse points (the runtime Data junctions) are never entered. Idempotent; runs from the scheduled task as SYSTEM.
# Only Cortex trees: the cccp worktrees (live list) plus the Cortex run and evidence roots. Other projects are never scanned.
$roots = @('D:\mx', 'C:\mx-archive', 'D:\Projects\reviews', 'D:\Projects\stage2_p4', 'D:\Projects\stage2_p2', 'D:\Projects\native-fidelity-work')
$worktrees = & git -C 'D:\Projects\cccp' worktree list --porcelain 2>$null | Where-Object { $_ -like 'worktree *' } | ForEach-Object { $_.Substring(9) -replace '/', '\' }
$roots += $worktrees
$log = 'D:\Projects\reviews\takeover-20260909\grok-workers\firewall_sweep.log'
$options = [System.IO.EnumerationOptions]::new()
$options.RecurseSubdirectories = $true
$options.AttributesToSkip = [System.IO.FileAttributes]::ReparsePoint
$options.IgnoreInaccessible = $true
$existing = @{}
foreach ($rule in Get-NetFirewallRule -DisplayName 'Cortex Command*' -ErrorAction SilentlyContinue) {
    foreach ($filter in ($rule | Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue)) {
        if ($filter.Program) { $existing[$filter.Program.ToLowerInvariant() + '|' + $rule.Direction] = $true }
    }
}
$added = 0
foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    foreach ($exe in [System.IO.Directory]::EnumerateFiles($root, 'Cortex Command*.exe', $options)) {
        foreach ($dir in @('Inbound', 'Outbound')) {
            if ($existing.ContainsKey($exe.ToLowerInvariant() + '|' + $dir)) { continue }
            $name = "Cortex Command sweep $dir " + [IO.Path]::GetFileName([IO.Path]::GetDirectoryName($exe)) + ' ' + [guid]::NewGuid().ToString('N').Substring(0, 8)
            New-NetFirewallRule -DisplayName $name -Direction $dir -Action Allow -Program $exe -Profile Any -Enabled True | Out-Null
            $existing[$exe.ToLowerInvariant() + '|' + $dir] = $true
            $added++
            "$(Get-Date -Format s) added $dir $exe" | Out-File $log -Append
        }
    }
}
"$(Get-Date -Format s) sweep done added=$added" | Out-File $log -Append
