# Stage 4: restart editor with the mixer project pre-staged as the auto-load demo.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Auto4b {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int w, int h, bool repaint);
}
'@
[Auto4b]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'
$autoDir = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\automation'
$exe = 'E:\0qyx\dev\0Idea\audioalgo\build\windows-debug\bin\AudioAlgo Editor.exe'
$userProj = "$env:USERPROFILE\Documents\AudioAlgo\Examples\gain_demo\gain_demo.aaproj"
$backup = "$autoDir\gain_demo.aaproj.original"

# 1) stop editor (also dismisses the stuck dialog)
Get-Process "AudioAlgo Editor" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# 2) backup original demo project once, then stage the mixer project with absolute paths
if (-not (Test-Path $backup)) { Copy-Item $userProj $backup }
$mixerJson = Get-Content 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\mixer_demo\mixer_demo.aaproj' -Raw -Encoding UTF8
$mixerJson = $mixerJson.Replace('"tone_primary_440.wav"', '"E:\\0qyx\\dev\\0Idea\\audioalgo\\showcase_1024\\mixer_demo\\tone_primary_440.wav"')
$mixerJson = $mixerJson.Replace('"tone_secondary_1320.wav"', '"E:\\0qyx\\dev\\0Idea\\audioalgo\\showcase_1024\\mixer_demo\\tone_secondary_1320.wav"')
[System.IO.File]::WriteAllText($userProj, $mixerJson, (New-Object System.Text.UTF8Encoding($false)))
"staged mixer project at $userProj"

# 3) relaunch
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = ('"' + $exe + '"') }
$handle = [IntPtr]::Zero
foreach ($attempt in 1..30) {
  Start-Sleep -Seconds 1
  $p = Get-Process -Id $r.ProcessId -ErrorAction SilentlyContinue
  if ($p -and $p.MainWindowHandle -ne 0) { $handle = $p.MainWindowHandle; break }
}
if ($handle -eq [IntPtr]::Zero) { "NO WINDOW"; exit 1 }
[Auto4b]::MoveWindow($handle, 0, 0, 1920, 1040, $true) | Out-Null
[Auto4b]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Seconds 4

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("$shots\stage4_mixer_autoloaded.png")
$bmp.Dispose()
"saved: stage4_mixer_autoloaded.png (pid=$($r.ProcessId))"
