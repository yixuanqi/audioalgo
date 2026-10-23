# Stage 5: Run the mixer graph, capture the three showcase screenshots.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Auto5 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
'@
[Auto5]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'
$p = Get-Process "AudioAlgo Editor" -ErrorAction Stop
$p.Refresh()
[Auto5]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 800

# Click Run (~(574, 56))
[Auto5]::SetCursorPos(574, 56) | Out-Null
Start-Sleep -Milliseconds 400
[Auto5]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 120; [Auto5]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
"clicked Run"
Start-Sleep -Seconds 8

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()

# Screenshot 1: full window (graph + waveform + status)
$bmp.Save("$shots\screenshot1_mixer_overview.png")

# Screenshot 2: mixer parameter panel (right side)
$panel = $bmp.Clone([System.Drawing.Rectangle]::new(1620, 60, 300, 280), $bmp.PixelFormat)
$panel.Save("$shots\screenshot2_mixer_parameters.png")
$panel.Dispose()

# Screenshot 3: status strip with run id / frames / SHA
$status = $bmp.Clone([System.Drawing.Rectangle]::new(0, 990, 1920, 30), $bmp.PixelFormat)
$status.Save("$shots\screenshot3_status_sha.png")
$status.Dispose()
$bmp.Dispose()
"saved: screenshot1_mixer_overview.png / screenshot2_mixer_parameters.png / screenshot3_status_sha.png"
