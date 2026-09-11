# Pre-authorise every worktree's engine executable and stop the "new app listening" prompt.
$roots = @(
    'D:\Projects\p4b-interp-validation', 'D:\Projects\takeover-fixes', 'D:\Projects\takeover-build',
    'D:\Projects\hold-pause', 'D:\Projects\h4-secondary', 'D:\Projects\audio-owner-registry',
    'D:\Projects\value-observations', 'D:\Projects\xarch-combat', 'D:\Projects\cccp'
)
$log = 'D:\Projects\reviews\takeover-20260909\grok-workers\firewall_allow_worktrees.log'
"run $(Get-Date -Format o)" | Out-File $log
foreach ($root in $roots) {
    foreach ($exe in @('Cortex Command.exe', 'Cortex Command.debug.release.exe', 'Cortex Command.debug.minimal.exe', 'Cortex Command.debug.full.exe')) {
        $program = Join-Path $root $exe
        $name = "Cortex Command worker $($root.Split('\')[-1]) $exe"
        foreach ($dir in @('Inbound', 'Outbound')) {
            $existing = Get-NetFirewallRule -DisplayName "$name $dir" -ErrorAction SilentlyContinue
            if (-not $existing) {
                New-NetFirewallRule -DisplayName "$name $dir" -Direction $dir -Action Allow -Program $program -Profile Any -Enabled True | Out-Null
                "added $dir $program" | Out-File $log -Append
            }
        }
    }
}
# Notification left ON so unrelated apps still prompt; the allow rules above are what silence our executables.
"NotifyOnListen: " + ((Get-NetFirewallProfile | ForEach-Object { "$($_.Name)=$($_.NotifyOnListen)" }) -join ' ') | Out-File $log -Append
"done" | Out-File $log -Append
