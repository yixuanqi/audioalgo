# Stage 2: click Run, wait for render, capture full window + crops.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Auto2 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
'@
[Auto2]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'

$p = Get-Process "AudioAlgo Editor" -ErrorAction Stop
$p.Refresh()
if ($p.MainWindowHandle -eq 0) { "NO WINDOW"; exit 1 }
[Auto2]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 800

# Click Run (toolbar button at ~(588, 59) physical)
[Auto2]::SetCursorPos(588, 59) | Out-Null
Start-Sleep -Milliseconds 400
[Auto2]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 120; [Auto2]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
"clicked Run"
Start-Sleep -Seconds 8

function Capture([string]$path) {
  $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
  $g.Dispose(); return $bmp
}

$bmp = Capture "full"
$bmp.Save("$shots\stage2_gain_run_full.png")

# Crop status strip (bottom of 1920x1040 window)
$status = $bmp.Clone([System.Drawing.Rectangle]::new(0, 990, 1920, 30), $bmp.PixelFormat)
$status.Save("$shots\stage2_gain_status_strip.png")
$status.Dispose()

# Crop right inspector panel
$panel = $bmp.Clone([System.Drawing.Rectangle]::new(1620, 60, 300, 240), $bmp.PixelFormat)
$panel.Save("$shots\stage2_gain_panel.png")
$panel.Dispose()
$bmp.Dispose()
"saved: stage2_gain_run_full.png / stage2_gain_status_strip.png / stage2_gain_panel.png"
