# Registers the sweep as a SYSTEM task every 5 minutes and runs it once now. Needs elevation.
$script = 'D:\Projects\reviews\takeover-20260909\grok-workers\firewall_sweep.ps1'
$pwsh = 'C:\Program Files\PowerShell\7\pwsh.exe'
Start-Transcript -Path 'D:\Projects\reviews\takeover-20260909\grok-workers\firewall_register_task.log' -Force | Out-Null
$action = New-ScheduledTaskAction -Execute $pwsh -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$script`""
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 5)
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -Hidden -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -StartWhenAvailable
Register-ScheduledTask -TaskName 'Cortex firewall sweep' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'Cortex firewall sweep'

