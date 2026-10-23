# Stage 1: launch editor, position window, capture initial state.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Auto1 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int w, int h, bool repaint);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
[Auto1]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'
New-Item -ItemType Directory -Force -Path $shots | Out-Null
$exe = 'E:\0qyx\dev\0Idea\audioalgo\build\windows-debug\bin\AudioAlgo Editor.exe'

Get-Process "AudioAlgo Editor" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = ('"' + $exe + '"') }
if ($r.ReturnValue -ne 0) { "LAUNCH FAILED rc=$($r.ReturnValue)"; exit 1 }

$handle = [IntPtr]::Zero
foreach ($attempt in 1..30) {
  Start-Sleep -Seconds 1
  $p = Get-Process -Id $r.ProcessId -ErrorAction SilentlyContinue
  if ($p -and $p.MainWindowHandle -ne 0) { $handle = $p.MainWindowHandle; break }
}
if ($handle -eq [IntPtr]::Zero) { "NO WINDOW after 30s"; exit 1 }

[Auto1]::MoveWindow($handle, 0, 0, 1920, 1040, $true) | Out-Null
[Auto1]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Seconds 4

$rect = New-Object Auto1+RECT
[Auto1]::GetWindowRect($handle, [ref]$rect) | Out-Null
"window rect: $($rect.Left),$($rect.Top) - $($rect.Right),$($rect.Bottom)"

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("$shots\stage1_launch.png")
$bmp.Dispose()
"saved: stage1_launch.png (pid=$($r.ProcessId))"
