[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration,

    [ValidateRange(1, 64)]
    [int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$preset = if ($Configuration -eq 'Debug') { 'windows-debug' } else { 'windows-release' }
$editorPath = Join-Path $repositoryRoot "build/$preset/bin/AudioAlgo Editor.exe"
$cmakeCommand = Get-Command cmake -CommandType Application -All -ErrorAction Stop | Select-Object -First 1
$cmakePath = $cmakeCommand.Source

Push-Location $repositoryRoot
try {
    Write-Host "Configuring AudioAlgo ($Configuration)..."
    & $cmakePath --preset $preset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE."
    }

    Write-Host "Building AudioAlgo Editor ($Configuration, parallel=$Parallel)..."
    & $cmakePath --build --preset $preset --target AudioAlgoEditor --parallel $Parallel
    if ($LASTEXITCODE -ne 0) {
        throw "AudioAlgo Editor build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $editorPath -PathType Leaf)) {
    throw "Build succeeded, but the editor executable was not found: $editorPath"
}

$runningEditors = @(Get-Process -Name 'AudioAlgo Editor' -ErrorAction SilentlyContinue)
if ($runningEditors.Count -gt 0) {
    $processIds = ($runningEditors | ForEach-Object { $_.Id }) -join ', '
    throw "AudioAlgo Editor is already running (PID $processIds). Close the existing editor before starting $Configuration."
}

Write-Host "Starting $editorPath"
$process = Start-Process -FilePath $editorPath -WorkingDirectory (Split-Path -Parent $editorPath) -PassThru
Start-Sleep -Milliseconds 750
if (-not (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) {
    throw "AudioAlgo Editor exited immediately after launch (PID $($process.Id)); check its startup diagnostics."
}
Write-Host "AudioAlgo Editor started (PID $($process.Id))."
