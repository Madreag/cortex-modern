@echo off
schtasks /create /tn CortexFirewallSweep /tr "\"C:\Program Files\PowerShell\7\pwsh.exe\" -NoProfile -ExecutionPolicy Bypass -File \"D:\Projects\reviews\takeover-20260909\grok-workers\firewall_sweep.ps1\"" /sc minute /mo 5 /ru SYSTEM /rl HIGHEST /f > "D:\Projects\reviews\takeover-20260909\grok-workers\schtasks_create.log" 2>&1
schtasks /run /tn CortexFirewallSweep >> "D:\Projects\reviews\takeover-20260909\grok-workers\schtasks_create.log" 2>&1
