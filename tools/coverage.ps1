# coverage.ps1 [-GameDir DIR] - runs scripted sessions of the game in the interpreter and collects
# the CS:IP of every executed instruction into work\cov.txt (the input of tools\ughrec.py).
# Script events are "frame:key:down" with set-1 scancodes in hex; 1xx = an E0 (grey) key.
param([string]$GameDir = "_references\dos")
$ErrorActionPreference = "Continue"   # (the test program writes its report to stderr)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root "build\ughtest.exe"
if (-not (Test-Path $exe)) { throw "build\ughtest.exe not found (build.cmd test)" }
$cov = Join-Path $root "work\cov_new.txt"
if (Test-Path $cov) { Remove-Item $cov }
$save = Join-Path $root "work\covsave"
if (Test-Path $save) { Remove-Item -Recurse $save }
New-Item -ItemType Directory -Force $save | Out-Null

function Keys([int]$from, [int]$to, [string[]]$keys, [int]$step, [int]$hold) {
    $ev = @(); $i = 0
    for ($f = $from; $f -lt $to; $f += $step) {
        $k = $keys[$i % $keys.Count]; $i++
        $ev += "$($f):$($k):1"; $ev += "$($f + $hold):$($k):0"
    }
    return $ev
}
function Typed([int]$from, [string]$text) {
    $sc = @{}; $q = "QWERTYUIOP"; $a = "ASDFGHJKL"; $z = "ZXCVBNM"
    for ($i = 0; $i -lt $q.Length; $i++) { $sc[$q[$i]] = 0x10 + $i }
    for ($i = 0; $i -lt $a.Length; $i++) { $sc[$a[$i]] = 0x1E + $i }
    for ($i = 0; $i -lt $z.Length; $i++) { $sc[$z[$i]] = 0x2C + $i }
    $ev = @(); $f = $from
    foreach ($c in $text.ToCharArray()) {
        $h = "{0:x}" -f $sc[[char]::ToUpper($c)]
        $ev += "$($f):$($h):1"; $ev += "$($f + 8):$($h):0"; $f += 20
    }
    $ev += "$($f + 100):1c:1"; $ev += "$($f + 108):1c:0"
    return $ev
}
$fly = @("148", "148", "14d", "148", "11d", "14b", "148", "150", "14d", "11d")
$sessions = @(
    @{ frames = 4000; script = @() },
    @{ frames = 14000; script = @("2700:01:1", "2704:01:0", "3000:3b:1", "3004:3b:0") + (Keys 3300 14000 $fly 30 22) },
    @{ frames = 9000; script = @("2700:01:1", "2704:01:0", "3000:3c:1", "3004:3c:0") + (Typed 3400 "RESURRECTION") + @("4300:3b:1", "4308:3b:0") + (Keys 4500 9000 $fly 25 20) },
    @{ frames = 5200; script = @("2700:01:1", "2704:01:0", "2900:3d:1", "2904:3d:0", "3000:3e:1", "3004:3e:0", "3100:3f:1", "3104:3f:0", "3300:3f:1", "3304:3f:0", "3500:3e:1", "3504:3e:0", "3700:3b:1", "3704:3b:0", "4600:19:1", "4604:19:0", "4700:19:1", "4704:19:0", "5000:01:1", "5004:01:0") },
    @{ frames = 40000; script = @("2700:01:1", "2704:01:0", "3000:3b:1", "3004:3b:0") + (Keys 3300 40000 @("148", "14b", "14d", "150") 40 25) + (Typed 39000 "TESTER") }
)
$env:UGH_INTERP = "1"; $env:UGH_COV = $cov; $env:UGH_SAVE = $save
try {
    $n = 0
    foreach ($s in $sessions) {
        $n++
        Write-Host "session $n ($($s.frames) frames)"
        & $exe $GameDir $s.frames "-" ($s.script -join ",") 2>&1 | Select-Object -Last 1 | ForEach-Object { "  $_" }
    }
} finally {
    Remove-Item env:UGH_INTERP, env:UGH_COV, env:UGH_SAVE
}
# merge with what was collected before
$old = Join-Path $root "work\cov.txt"
$all = @()
if (Test-Path $old) { $all += Get-Content $old }
$all += Get-Content $cov
$all | Sort-Object -Unique | Set-Content -Encoding ascii $old
Write-Host "work\cov.txt: $((Get-Content $old).Count) addresses"
