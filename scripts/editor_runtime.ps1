# Native permission inspection only. Never changes a file or directory ACL.
if (-not ('AudioAlgo.RuntimePermissions' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AudioAlgo {
public static class RuntimePermissions {
 [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] static extern uint GetNamedSecurityInfo(string name,int kind,uint info,out IntPtr owner,out IntPtr group,out IntPtr dacl,out IntPtr sacl,out IntPtr descriptor);
 [DllImport("advapi32.dll")] static extern bool GetAce(IntPtr acl,uint index,out IntPtr ace);
 [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
 [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid,uint index);
 [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
 [DllImport("advapi32.dll",SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
 [DllImport("advapi32.dll",SetLastError=true)] static extern bool GetTokenInformation(IntPtr token,int kind,IntPtr data,int size,out int required);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 static int Rid(IntPtr sid) { byte n=Marshal.ReadByte(GetSidSubAuthorityCount(sid)); return n==0 ? 0 : Marshal.ReadInt32(GetSidSubAuthority(sid,(uint)(n-1))); }
 public static int FileIntegrity(string path) {
   IntPtr owner,group,dacl,sacl,sd;
   uint error=GetNamedSecurityInfo(path,1,0x10,out owner,out group,out dacl,out sacl,out sd);
   if(error!=0) throw new System.ComponentModel.Win32Exception((int)error);
   try {
     if(sacl==IntPtr.Zero) return 0x2000;
     int count=(ushort)Marshal.ReadInt16(sacl,4);
     for(uint i=0;i<count;i++) { IntPtr ace; if(!GetAce(sacl,i,out ace)) throw new InvalidOperationException("Cannot inspect mandatory label");
       if(Marshal.ReadByte(ace)==0x11) return Rid(IntPtr.Add(ace,8)); }
     return 0x2000;
   } finally { LocalFree(sd); }
 }
 public static int ProcessIntegrity(int pid) {
   IntPtr process=OpenProcess(0x1000,false,pid), token=IntPtr.Zero, data=IntPtr.Zero;
   if(process==IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
   try {
     if(!OpenProcessToken(process,8,out token)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
     int size; GetTokenInformation(token,25,IntPtr.Zero,0,out size); data=Marshal.AllocHGlobal(size);
     if(!GetTokenInformation(token,25,data,size,out size)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
     return Rid(Marshal.ReadIntPtr(data));
   } finally { if(data!=IntPtr.Zero) Marshal.FreeHGlobal(data); if(token!=IntPtr.Zero) CloseHandle(token); CloseHandle(process); }
 }
}
}
'@
}

function Get-AudioAlgoFileHash {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '') }
    finally { $hash.Dispose(); $stream.Dispose() }
}

function Resolve-AudioAlgoRuntime {
    param([string]$SourceDirectory, [string]$RuntimeDirectory)
    if ([AudioAlgo.RuntimePermissions]::ProcessIntegrity($PID) -lt 0x2000) {
        throw 'The launcher itself has Low Integrity. Run the launcher from a normal user Explorer/PowerShell session; no administrator or ACL changes are required.'
    }
    $sourceRoot = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\')
    $runtimeRoot = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\')
    if ($runtimeRoot.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or $runtimeRoot.StartsWith($sourceRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Choose a separate runtime directory, outside the build output.'
    }
    [IO.Directory]::CreateDirectory($runtimeRoot) | Out-Null
    if ([AudioAlgo.RuntimePermissions]::FileIntegrity($runtimeRoot) -lt 0x2000) {
        throw "Runtime directory inherits Low Integrity: $runtimeRoot. Choose a normal user directory with -RunDirectory. Existing permissions were not changed."
    }
    $inventory = @()
    foreach ($file in (Get-ChildItem -LiteralPath $sourceRoot -File -Recurse | Sort-Object FullName)) {
        if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Linked runtime inputs are not supported.' }
        if ($file.Extension -eq '.pdb' -or ($file.Extension -eq '.exe' -and $file.Name -ne 'AudioAlgo Editor.exe')) { continue }
        $relative = $file.FullName.Substring($sourceRoot.Length + 1)
        $inventory += [ordered]@{ path = $relative; sha256 = (Get-AudioAlgoFileHash $file.FullName) }
    }
    if (-not ($inventory | Where-Object { $_.path -eq 'AudioAlgo Editor.exe' })) { throw 'Editor executable is missing from the build output.' }
    $json = ConvertTo-Json -InputObject $inventory -Depth 4 -Compress
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($json))).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose() }
    $destination = Join-Path $runtimeRoot $digest
    $stage = Join-Path $runtimeRoot ('.staging-' + [guid]::NewGuid().ToString('N'))
    if (-not (Test-Path -LiteralPath $destination)) {
        [IO.Directory]::CreateDirectory($stage) | Out-Null
        try {
            foreach ($entry in $inventory) {
                $inputPath = Join-Path $sourceRoot $entry.path
                $outputPath = Join-Path $stage $entry.path
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
                # New files inherit the chosen runtime directory's security. Do
                # not copy the development workspace's security descriptors.
                $inputStream = [IO.File]::OpenRead($inputPath)
                try {
                    $outputStream = [IO.File]::Open($outputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
                    try { $inputStream.CopyTo($outputStream); $outputStream.Flush() }
                    finally { $outputStream.Dispose() }
                } finally { $inputStream.Dispose() }
                if ((Get-AudioAlgoFileHash $outputPath) -ne $entry.sha256) { throw 'Runtime copy verification failed; source may have changed.' }
            }
            if ([AudioAlgo.RuntimePermissions]::FileIntegrity((Join-Path $stage 'AudioAlgo Editor.exe')) -lt 0x2000) { throw 'Copied runtime still has Low Integrity; nothing will be launched.' }
            [IO.File]::WriteAllText((Join-Path $stage 'deployment.json'), $json, [Text.Encoding]::UTF8)
            Move-Item -LiteralPath $stage -Destination $destination -ErrorAction Stop
        } finally {
            # Only this invocation's unique, verified child may be removed.
            $resolvedStage = [IO.Path]::GetFullPath($stage)
            if (-not $resolvedStage.StartsWith($runtimeRoot + '\.staging-', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe staging cleanup target.' }
            if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
        }
    }
    foreach ($entry in $inventory) {
        $deployed = Join-Path $destination $entry.path
        if (-not (Test-Path -LiteralPath $deployed -PathType Leaf) -or (Get-AudioAlgoFileHash $deployed) -ne $entry.sha256) { throw 'Existing runtime is incomplete or modified; choose another runtime directory. No files were overwritten.' }
    }
    $executable = Join-Path $destination 'AudioAlgo Editor.exe'
    if ([AudioAlgo.RuntimePermissions]::FileIntegrity($executable) -lt 0x2000) { throw 'Runtime executable has Low Integrity; nothing will be launched.' }
    return $executable
}
