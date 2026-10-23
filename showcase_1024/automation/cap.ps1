# Capture-only helper.
param([string]$Name = 'cap')
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class AutoCap {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
'@
[AutoCap]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$p = Get-Process "AudioAlgo Editor" -ErrorAction Stop
$p.Refresh()
[AutoCap]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 800
$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots\$Name.png")
$bmp.Dispose()
"saved: $Name.png"
