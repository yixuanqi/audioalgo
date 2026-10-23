# Stage 3c: fill filename via WM_SETTEXT and press Open via BM_CLICK (no pixel guessing).
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Dlg {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
  public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, string lp);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$dlg = [Dlg]::FindWindowW($null, 'Open AudioAlgo project')
if ($dlg -eq [IntPtr]::Zero) { "DIALOG NOT FOUND"; exit 1 }
"dialog found: $dlg"

$edits = New-Object System.Collections.ArrayList
$openBtn = [IntPtr]::Zero
$cb = [Dlg+EnumProc]{
  param($h, $lp)
  $cls = New-Object System.Text.StringBuilder 256
  [Dlg]::GetClassNameW($h, $cls, 256) | Out-Null
  $txt = New-Object System.Text.StringBuilder 256
  [Dlg]::GetWindowTextW($h, $txt, 256) | Out-Null
  $r = New-Object Dlg+RECT
  [Dlg]::GetWindowRect($h, [ref]$r) | Out-Null
  $info = "class=$($cls) text='$($txt)' rect=$($r.Left),$($r.Top)-$($r.Right),$($r.Bottom)"
  $script:children += $info
  if ($cls.ToString() -eq 'Edit') { $script:edits += ,@($h, $r.Top) }
  if ($cls.ToString() -eq 'Button' -and $txt.ToString() -like '*打开*') { $script:openBtn = $h }
  return $true
}
$script:children = New-Object System.Collections.ArrayList
$script:edits = New-Object System.Collections.ArrayList
$script:openBtn = [IntPtr]::Zero
[Dlg]::EnumChildWindows($dlg, $cb, [IntPtr]::Zero) | Out-Null
$script:children | ForEach-Object { "  $_" }

# Filename edit = the lowest Edit control
$edit = ($script:edits | Sort-Object { $_[1] } -Descending | Select-Object -First 1)[0]
if ($edit -eq $null) { "NO EDIT FOUND"; exit 1 }
"path edit: $edit"
$path = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\mixer_demo\mixer_demo.aaproj'
[Dlg]::SendMessageW($edit, 0x000C, [IntPtr]::Zero, $path) | Out-Null   # WM_SETTEXT
Start-Sleep -Milliseconds 500

if ($script:openBtn -eq [IntPtr]::Zero) { "NO OPEN BUTTON"; exit 1 }
[Dlg]::SendMessageW($script:openBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK
"BM_CLICK sent to Open button"
Start-Sleep -Seconds 4

$shots = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots'
$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("$shots\stage3c_after_dialog_confirm.png")
$bmp.Dispose()
"saved: stage3c_after_dialog_confirm.png"
