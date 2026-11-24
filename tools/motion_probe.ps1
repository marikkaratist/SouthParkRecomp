# Measures how smoothly the picture moves: starts the game, waits, then grabs a
# narrow horizontal strip of the window as fast as it can and saves each strip
# with its capture time. tools\motion_probe.py turns the strips into a list of
# frame-to-frame shifts.
#
#   .\tools\motion_probe.ps1 -At 58 -Count 150 [-Row 0.45] [-GameArgs --no-hints] [-Name probe]
param(
    [int]$At = 58,
    [int]$Count = 150,
    [double]$Row = 0.45,          # vertical position of the strip, 0 = top, 1 = bottom
    [int]$Height = 8,
    [string]$Name = "probe",
    [string]$BuildDir = "build",
    [string[]]$GameArgs = @()
)
$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $root "rom\run.log"
$out = Join-Path $root "rom\$Name"
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item $out -Recurse -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SpProbe {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref System.Drawing.Point point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@ -ReferencedAssemblies System.Drawing
[SpProbe]::SetProcessDPIAware() | Out-Null

$arguments = @("--rom", "`"$root\rom\southpark.us.z64`"", "--autostart", "--log", "`"$log`"") + $GameArgs
$process = Start-Process -FilePath "$root\$BuildDir\SouthParkRecompiled.exe" -ArgumentList $arguments -WorkingDirectory $root -PassThru
Start-Sleep -Seconds $At
$process.Refresh()
$handle = $process.MainWindowHandle
[SpProbe]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Milliseconds 300
$rect = New-Object SpProbe+Rect
[SpProbe]::GetClientRect($handle, [ref]$rect) | Out-Null
$origin = New-Object System.Drawing.Point 0, 0
[SpProbe]::ClientToScreen($handle, [ref]$origin) | Out-Null
$width = $rect.Right - $rect.Left
$top = $origin.Y + [int](($rect.Bottom - $rect.Top) * $Row)

$strips = New-Object System.Collections.Generic.List[object]
$times = New-Object System.Collections.Generic.List[double]
$watch = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Count; $i++) {
    $bitmap = New-Object System.Drawing.Bitmap $width, $Height, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($origin.X, $top, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    $times.Add($watch.Elapsed.TotalMilliseconds)
    $strips.Add($bitmap)
}
if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
for ($i = 0; $i -lt $strips.Count; $i++) {
    $strips[$i].Save((Join-Path $out ("{0:D4}.bmp" -f $i)), [System.Drawing.Imaging.ImageFormat]::Bmp)
    $strips[$i].Dispose()
}
($times | ForEach-Object { "{0:F1}" -f $_ }) -join "`n" | Set-Content (Join-Path $out "times.txt") -Encoding ascii
"captured $Count strips of ${width}x$Height in {0:F0} ms -> $out" -f $times[$times.Count - 1]
