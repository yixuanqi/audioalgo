# Stage 3b: click the Open (打开) button in the file dialog.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Auto3b {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
'@
[Auto3b]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'
$p = Get-Process "AudioAlgo Editor" -ErrorAction Stop
$p.Refresh()
[Auto3b]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 800

# Click 打开(O) button at ~(820, 566)
[Auto3b]::SetCursorPos(820, 566) | Out-Null
Start-Sleep -Milliseconds 400
[Auto3b]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 120; [Auto3b]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
"clicked Open button"
Start-Sleep -Seconds 4

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("$shots\stage3b_after_open_click.png")
$bmp.Dispose()
"saved: stage3b_after_open_click.png"
