param([Parameter(Mandatory=$true)][string]$ArgumentsBase64)
$ErrorActionPreference = 'Stop'
$json = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ArgumentsBase64))
$words = @($json | ConvertFrom-Json)
if ($words.Count -lt 1) { throw 'The peer step needs a command' }
$command = [string]$words[0]
$arguments = @($words | Select-Object -Skip 1)
& $command @arguments
exit $LASTEXITCODE
