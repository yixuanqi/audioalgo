# Stage 3d: retype full path in filename edit, then click the real Open button (corrected coords).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Auto3d {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
'@
[Auto3d]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'
$p = Get-Process "AudioAlgo Editor" -ErrorAction Stop
$p.Refresh()
[Auto3d]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 800

function Click-At([int]$x, [int]$y) {
  [Auto3d]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 300
  [Auto3d]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 100
  [Auto3d]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
}

# 1) focus filename edit (~(460, 501)), select all, retype full path
Click-At 460 501
Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait('^a')
Start-Sleep -Milliseconds 200
[System.Windows.Forms.SendKeys]::SendWait('E:\0qyx\dev\0Idea\audioalgo\showcase_1024\mixer_demo\mixer_demo.aaproj')
Start-Sleep -Milliseconds 500
"full path typed"

# 2) click Open (打开) button at corrected position (~(801, 531))
Click-At 801 531
"clicked Open"
Start-Sleep -Seconds 4

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("$shots\stage3d_mixer_loaded.png")
$bmp.Dispose()
"saved: stage3d_mixer_loaded.png"
