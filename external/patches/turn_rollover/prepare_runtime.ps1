param([Parameter(Mandatory=$true)][string]$Runtime, [Parameter(Mandatory=$true)][string]$Data)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $Data 'Base.rte'))) { throw 'Data does not contain Base.rte' }
if (Test-Path -LiteralPath (Join-Path $Runtime 'Data')) { throw 'The runtime already has Data' }
New-Item -ItemType Junction -Path (Join-Path $Runtime 'Data') -Target $Data | Out-Null
