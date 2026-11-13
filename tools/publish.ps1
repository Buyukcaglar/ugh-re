# publish.ps1 -Message "..." - copies the committed sources (HEAD) without the generated code
# (src/gen/gen_all.c, made from the game) into the public repository ugh-re and pushes a commit.
# The public history is its own: it never contains gen_all.c or any game file.
param([Parameter(Mandatory = $true)][string]$Message, [string]$Repo = "Buyukcaglar/ugh-re")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$pub = Join-Path $root "work\public"
if (-not (Test-Path (Join-Path $pub ".git"))) {
    if (Test-Path $pub) { Remove-Item -Recurse -Force $pub }
    gh repo clone $Repo $pub
    if ($LASTEXITCODE -ne 0) { throw "cannot clone $Repo" }
}
git -C $pub pull --ff-only 2>$null
Get-ChildItem -Force $pub | Where-Object { $_.Name -ne ".git" } | Remove-Item -Recurse -Force
$tar = Join-Path $root "work\public.tar"
git -C $root archive --format=tar -o $tar HEAD
tar -x -f $tar -C $pub --exclude "src/gen/gen_all.c"
Remove-Item $tar
foreach ($bad in @("src/gen/gen_all.c", "_references", "work")) {
    if (Test-Path (Join-Path $pub $bad)) { throw "$bad must not be published" }
}
git -C $pub add -A
git -C $pub diff --cached --quiet
if ($LASTEXITCODE -eq 0) { Write-Host "nothing to publish"; exit 0 }
$msg = $Message + "`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git -C $pub commit -q -m $msg
git -C $pub push origin HEAD
Write-Host "published to $Repo"
