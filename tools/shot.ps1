# shot.ps1 -Exe EXE -ArgList ARGS SECONDS OUT.png [KEYS] - starts the program, waits, saves its window as a PNG, closes it.
# KEYS: optional SendKeys string sent after half the wait (e.g. "{F1}").
param([string]$Exe, [string]$ArgList, [int]$Seconds = 8, [string]$Out = "shot.png", [string]$Keys = "")
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int n);
  public struct RECT { public int L, T, R, B; }
}
"@
if ($ArgList) { $p = Start-Process -FilePath $Exe -ArgumentList $ArgList -PassThru }
else { $p = Start-Process -FilePath $Exe -PassThru }
$half = [int]($Seconds / 2)
Start-Sleep -Seconds $half
$p.Refresh()
$h = $p.MainWindowHandle
Start-Sleep -Seconds ($Seconds - $half)
$p.Refresh()
$h = $p.MainWindowHandle
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[W]::PrintWindow($h, $dc, 2) | Out-Null   # PW_RENDERFULLCONTENT: this window only, never the screen
$g.ReleaseHdc($dc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force
"saved $Out ${w}x$hh"
