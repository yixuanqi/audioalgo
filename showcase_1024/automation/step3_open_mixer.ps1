# Stage 3: open the mixer demo project via the Open dialog (type path + Enter).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Auto3 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
'@
[Auto3]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'

$p = Get-Process "AudioAlgo Editor" -ErrorAction Stop
$p.Refresh()
[Auto3]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 800

# Click Open (toolbar at ~(292, 59))
[Auto3]::SetCursorPos(292, 59) | Out-Null
Start-Sleep -Milliseconds 400
[Auto3]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 120; [Auto3]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
"clicked Open"
Start-Sleep -Seconds 3

# Type the project path into the file dialog and confirm
$path = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\mixer_demo\mixer_demo.aaproj'
[System.Windows.Forms.SendKeys]::SendWait($path)
Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
"path typed + Enter"
Start-Sleep -Seconds 4

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("$shots\stage3_mixer_loaded.png")
$bmp.Dispose()
"saved: stage3_mixer_loaded.png"
