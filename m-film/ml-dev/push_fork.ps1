# Push the ML fork (WSL: ~/ml/magiclantern_simplified, branch film-mode) to
# its private GitHub repo. Git in WSL has no GitHub login, so the branch goes
# through a bare repo on the Windows side and is pushed from there with the
# gh credentials.
#   powershell -File push_fork.ps1
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bare = Join-Path $here "out\mlfork.git"
$remote = "https://github.com/HNGSeries/magiclantern-eosm-film.git"

if (-not (Test-Path $bare)) { git init -q --bare $bare }

$bareWsl = (wsl -d Ubuntu -u root -- wslpath -a "$bare") -replace "`0", ""
wsl -d Ubuntu -u root -- git -C /root/ml/magiclantern_simplified push -q --force "$bareWsl" film-mode:film-mode
if ($LASTEXITCODE) { throw "WSL -> bare repo push failed" }

git -C $bare push $remote film-mode:film-mode
if ($LASTEXITCODE) { throw "push to GitHub failed" }
Write-Output "film-mode pushed to $remote"
