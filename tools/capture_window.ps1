# Launches lmport.exe, waits, and saves a PNG of its window (for checking rendering).
#   powershell -ExecutionPolicy Bypass -File tools\capture_window.ps1 -Seconds 8 -Out build\shot.png [-Args "--foo"]
param(
    [int]$Seconds = 8,
    [string]$Out = "build\shot.png",
    [string]$Args = "",
    [switch]$KeepRunning
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[Win]::SetProcessDPIAware() | Out-Null

$argList = if ($Args) { $Args } else { $null }
$p = if ($argList) { Start-Process .\build\windows-clang\lmport.exe -ArgumentList $argList -PassThru `
        -RedirectStandardOutput build\run.log -RedirectStandardError build\err.log }
     else { Start-Process .\build\windows-clang\lmport.exe -PassThru `
        -RedirectStandardOutput build\run.log -RedirectStandardError build\err.log }
Start-Sleep -Seconds $Seconds
$p.Refresh()
if ($p.HasExited) { throw ("lmport exited early with 0x{0:X}" -f $p.ExitCode) }
$h = $p.MainWindowHandle
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object Win+RECT
[Win]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object Win+POINT
[Win]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = $r.R - $r.L; $hgt = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
$bmp.Save((Join-Path $root $Out), [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out (${w}x$hgt)"
if (-not $KeepRunning) { Stop-Process $p -Force }
