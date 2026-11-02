# Put the official ML (backed up by install_fork.ps1) back on the card.
#   powershell -File restore_official.ps1 -Card G:
param([Parameter(Mandatory = $true)][string]$Card)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$backup = Join-Path $here "card_backup\official"
$card = $Card.TrimEnd('\') + '\'

if (-not (Test-Path (Join-Path $backup "autoexec.bin"))) { throw "No backup in $backup" }

Copy-Item (Join-Path $backup "autoexec.bin") $card -Force
Copy-Item (Join-Path $backup "ML\MODULES\*") (Join-Path $card "ML\MODULES") -Force
Write-Output "Official ML restored on $card (the filmroll module from ml-dev\out can be copied back if wanted)"
