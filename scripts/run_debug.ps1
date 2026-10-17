[CmdletBinding()]
param(
    [ValidateRange(1, 64)]
    [int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'build_and_run_editor.ps1'
& $runner -Configuration Debug -Parallel $Parallel
