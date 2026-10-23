# 1024 showcase CLI demo: run both demo projects and verify against independent oracles.
# Usage:  powershell -ExecutionPolicy Bypass -File showcase_1024\run_cli_demo.ps1
# Requires: build\windows-debug\bin\audio_cli.exe (or -BuildDir override) and Python 3.

param(
    [string]$BuildDir = "build\windows-debug\bin",
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$cli = Join-Path $repoRoot "$BuildDir\audio_cli.exe"
if (-not (Test-Path $cli)) {
    Write-Error "audio_cli.exe not found at $cli - build first: cmake --build --preset windows-debug --parallel 4"
}

if ($Python -eq "") {
    foreach ($candidate in @(@("py", "-3"), @("python", ""))) {
        $probe = & $candidate[0] $candidate[1] --version 2>$null
        if ($LASTEXITCODE -eq 0) { $Python = (@($candidate[0]) + @($candidate[1] | Where-Object { $_ -ne "" })) -join " "; break }
    }
    if ($Python -eq "") { Write-Error "no Python 3 interpreter found; pass -Python explicitly" }
}
Write-Host "== python: $Python =="

function Invoke-Run([string]$Project, [string]$Label) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $outputRoot = Join-Path $PSScriptRoot "runs\$Label-$stamp"
    New-Item -ItemType Directory -Force -Path (Split-Path $outputRoot -Parent) | Out-Null
    Write-Host "== [$Label] $cli run $Project =="
    $runOutput = & $cli run $Project $outputRoot 127
    if ($LASTEXITCODE -ne 0) { Write-Error "[$Label] CLI run failed with exit code $LASTEXITCODE" }
    $runOutput | ForEach-Object { Write-Host "  $_" }
    # CLI with a fresh output directory writes run.json/output.wav directly into it;
    # the editor flow (shared runs root) nests run-*/ directories. Handle both.
    $runDir = Get-ChildItem $outputRoot -Directory -Filter "run-*" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -ne $runDir) { $runDir = $runDir.FullName } else { $runDir = $outputRoot }
    if (-not (Test-Path (Join-Path $runDir "run.json"))) { Write-Error "[$Label] no run.json under $runDir" }
    return $runDir
}

# 1) Gain demo: repo example project, +6 dB, verified by the repo's own independent oracle.
$gainProject = Join-Path $repoRoot "examples\gain_demo\gain_demo.aaproj"
$gainRunDir = Invoke-Run $gainProject "gain-demo"
Write-Host "== [$gainRunDir] verify_run_audio.py --gain-db 6.0 =="
$parts = $Python.Split(" ")
& $parts[0] ($parts | Select-Object -Skip 1) (Join-Path $repoRoot "scripts\verify_run_audio.py") --input (Join-Path $repoRoot "examples\gain_demo\input.wav") --run-dir $gainRunDir --gain-db 6.0
if ($LASTEXITCODE -ne 0) { Write-Error "gain demo oracle FAILED" }

# 2) Mixer demo: showcase project, two sources, verified by the showcase oracle.
$mixerProject = Join-Path $PSScriptRoot "mixer_demo\mixer_demo.aaproj"
if (-not (Test-Path $mixerProject)) {
    Write-Host "== mixer demo project missing; generating =="
    $parts = $Python.Split(" ")
    & $parts[0] ($parts | Select-Object -Skip 1) (Join-Path $PSScriptRoot "make_mixer_demo.py")
    if ($LASTEXITCODE -ne 0) { Write-Error "make_mixer_demo.py failed" }
}
$mixerRunDir = Invoke-Run $mixerProject "mixer-demo"
Write-Host "== [$mixerRunDir] verify_mixer_demo.py =="
$parts = $Python.Split(" ")
& $parts[0] ($parts | Select-Object -Skip 1) (Join-Path $PSScriptRoot "verify_mixer_demo.py") --run-dir $mixerRunDir
if ($LASTEXITCODE -ne 0) { Write-Error "mixer demo oracle FAILED" }

Write-Host ""
Write-Host "== ALL DEMO VERIFICATIONS PASSED =="
Write-Host "gain  run dir: $gainRunDir"
Write-Host "mixer run dir: $mixerRunDir"
