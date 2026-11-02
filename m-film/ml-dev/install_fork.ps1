# Install the film-mode ML fork on the camera card.
#   powershell -File install_fork.ps1 -Card G:
# First run backs up the official ML (autoexec.bin + ML\MODULES) to
# ml-dev\card_backup\official\ so restore_official.ps1 can put it back.
param([Parameter(Mandatory = $true)][string]$Card)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$fork = Join-Path $here "out\fork"
$backup = Join-Path $here "card_backup\official"
$card = $Card.TrimEnd('\') + '\'

if (-not (Test-Path (Join-Path $card "autoexec.bin"))) { throw "No autoexec.bin on $card - is this the camera card?" }
if (-not (Test-Path (Join-Path $fork "autoexec.bin"))) { throw "Build the fork first (build_fork.sh)" }

# one-time backup of the official install
if (-not (Test-Path (Join-Path $backup "autoexec.bin"))) {
    New-Item -ItemType Directory -Force (Join-Path $backup "ML\MODULES") | Out-Null
    Copy-Item (Join-Path $card "autoexec.bin") $backup
    Copy-Item (Join-Path $card "ML\MODULES\*") (Join-Path $backup "ML\MODULES")
    Write-Output "Backed up official ML to $backup"
}

# the fork's core and the modules/symbols built against it
Copy-Item (Join-Path $fork "autoexec.bin") $card -Force
Copy-Item (Join-Path $fork "ML\modules\*") (Join-Path $card "ML\MODULES") -Force
# the film OS fonts (not inside autoexec.bin, which has to stay small)
New-Item -ItemType Directory -Force (Join-Path $card "ML\DATA") | Out-Null
Copy-Item (Join-Path $fork "ML\data\FOSFONT.BIN") (Join-Path $card "ML\DATA") -Force
# the films beyond the built-in ones, and the donation codes if there are any
Copy-Item (Join-Path $fork "ML\data\FOSFILMS.BIN") (Join-Path $card "ML\DATA") -Force
$qr = Join-Path $fork "ML\data\FOSQR.BIN"
if (Test-Path $qr) { Copy-Item $qr (Join-Path $card "ML\DATA") -Force }
# the films' colour tables (the darkroom's prints stay as they are)
New-Item -ItemType Directory -Force (Join-Path $card "ML\DARKROOM") | Out-Null
Copy-Item (Join-Path $fork "ML\DARKROOM\LOOK*.LUT") (Join-Path $card "ML\DARKROOM") -Force
# the shelf's samples (EOS M builds only)
$samples = Join-Path $fork "ML\SAMPLES"
if (Test-Path $samples) {
    New-Item -ItemType Directory -Force (Join-Path $card "ML\SAMPLES") | Out-Null
    Copy-Item (Join-Path $samples "F*.BMP") (Join-Path $card "ML\SAMPLES") -Force
}

# filmroll is built in now; lua.mo was built for the official core
foreach ($m in "filmroll.mo", "lua.mo") {
    $p = Join-Path $card "ML\MODULES\$m"
    if (Test-Path $p) { Remove-Item $p; Write-Output "Removed ML\MODULES\$m" }
}

$a = (Get-FileHash (Join-Path $fork "autoexec.bin")).Hash
$b = (Get-FileHash (Join-Path $card "autoexec.bin")).Hash
Write-Output ("Installed fork autoexec.bin: " + $(if ($a -eq $b) { "verified" } else { "HASH MISMATCH" }))
