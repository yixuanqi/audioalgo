param([string]$RepositoryRoot, [string]$ProofFile, [string]$SourceDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $RepositoryRoot 'scripts/editor_runtime.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('AudioAlgo-runtime-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$source = if ($SourceDirectory) { [IO.Path]::GetFullPath($SourceDirectory) } else { Join-Path $RepositoryRoot 'build/windows-debug/bin' }
$sourceExecutable = Join-Path $source 'AudioAlgo Editor.exe'
if ([AudioAlgo.RuntimePermissions]::FileIntegrity($sourceExecutable) -ge 0x2000) { throw 'This regression requires the actual low-labelled workspace executable' }
$runtime = Resolve-AudioAlgoRuntime -SourceDirectory $source -RuntimeDirectory $testRoot
if ([AudioAlgo.RuntimePermissions]::FileIntegrity($runtime) -lt 0x2000) { throw 'Runtime still inherits the Low label; directly launching Release/Debug does not fix storage writes' }
if ((Get-AudioAlgoFileHash $runtime) -ne (Get-AudioAlgoFileHash $sourceExecutable)) { throw 'Deployment changed the executable bytes' }
$again = Resolve-AudioAlgoRuntime -SourceDirectory $source -RuntimeDirectory $testRoot
if ($again -ne $runtime) { throw 'Unchanged builds must reuse the same verified runtime' }
$lowRejected = $false
try { Resolve-AudioAlgoRuntime -SourceDirectory $source -RuntimeDirectory (Join-Path $RepositoryRoot 'build/reports/loops/NA0026-runtime-permissions/low-target') | Out-Null }
catch { if ($_.Exception.Message -like '*inherits Low Integrity*') { $lowRejected = $true } else { throw } }
if (-not $lowRejected) { throw 'A low-labelled runtime directory was accepted' }
$stream = [IO.File]::Open($runtime, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
try { $originalByte = $stream.ReadByte(); $stream.Position = 0; $stream.WriteByte(($originalByte -bxor 1)) }
finally { $stream.Dispose() }
$tamperRejected = $false
try { Resolve-AudioAlgoRuntime -SourceDirectory $source -RuntimeDirectory $testRoot | Out-Null }
catch { if ($_.Exception.Message -like '*modified*') { $tamperRejected = $true } else { throw } }
finally {
    $stream = [IO.File]::Open($runtime, [IO.FileMode]::Open, [IO.FileAccess]::Write)
    try { $stream.WriteByte($originalByte) } finally { $stream.Dispose() }
}
if (-not $tamperRejected) { throw 'Modified deployments must be rejected without overwriting them' }
if ($ProofFile) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($ProofFile))) | Out-Null
    [ordered]@{ source = $sourceExecutable; runtime = $runtime; source_rid = [AudioAlgo.RuntimePermissions]::FileIntegrity($sourceExecutable); runtime_rid = [AudioAlgo.RuntimePermissions]::FileIntegrity($runtime); process_rid = [AudioAlgo.RuntimePermissions]::ProcessIntegrity($PID); sha256 = (Get-AudioAlgoFileHash $runtime); low_target_rejected = $lowRejected; modified_target_rejected = $tamperRejected; window_launched = $false } | ConvertTo-Json | Set-Content -LiteralPath $ProofFile -Encoding UTF8
}
Write-Output ('Verified normal runtime label without launching a window: ' + $runtime)
