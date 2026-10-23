# Diagnostic: enumerate top-level windows of the editor process + capture screen.
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WinEnum {
  public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$p = Get-Process "AudioAlgo Editor" -ErrorAction Stop
$pid32 = [uint32]$p.Id
"editor pid: $pid32  main window: $($p.MainWindowTitle)"

$cb = [WinEnum+EnumProc]{
  param($h, $lp)
  $wpid = [uint32]0
  [WinEnum]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
  if ($wpid -eq $script:target) {
    $txt = New-Object System.Text.StringBuilder 256
    [WinEnum]::GetWindowTextW($h, $txt, 256) | Out-Null
    $cls = New-Object System.Text.StringBuilder 256
    [WinEnum]::GetClassNameW($h, $cls, 256) | Out-Null
    $vis = [WinEnum]::IsWindowVisible($h)
    "  hwnd=$h class='$($cls)' visible=$vis title='$($txt)'"
  }
  return $true
}
$script:target = $pid32
[WinEnum]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save('E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots\diag_state.png')
$bmp.Dispose()
"saved: diag_state.png"
