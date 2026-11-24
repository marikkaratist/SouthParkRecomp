# Starts the game and saves screenshots of its window at the given times.
#
#   .\tools\screenshot.ps1 -At 8,20 [-BuildDir build-dbg] [-Name shot]
#
# Images go to rom\<Name>_<seconds>.png. The game is closed afterwards.
# The picture is taken from the game's window itself (PrintWindow), not from the screen, so
# it never shows other windows and the game does not have to be in front.
param(
    [int[]]$At = @(10),
    [string]$BuildDir = "build",
    [string]$Name = "shot",
    [int]$Burst = 1,              # shots taken in quick succession at each time
    [string[]]$GameArgs = @()     # extra switches for the game, e.g. --no-hints
)
$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $root "rom\run.log"
Remove-Item $log -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SpWindow {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref System.Drawing.Point point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    // Copies the window's own picture, whatever is in front of it (2 = PW_RENDERFULLCONTENT).
    public static void Capture(IntPtr hWnd, System.Drawing.Bitmap bitmap) {
        using (System.Drawing.Graphics g = System.Drawing.Graphics.FromImage(bitmap)) {
            IntPtr hdc = g.GetHdc();
            PrintWindow(hWnd, hdc, 3);
            g.ReleaseHdc(hdc);
        }
    }
}
"@ -ReferencedAssemblies System.Drawing

[SpWindow]::SetProcessDPIAware() | Out-Null
$arguments = @("--rom", "`"$root\rom\southpark.us.z64`"", "--autostart", "--log", "`"$log`"") + $GameArgs
$process = Start-Process -FilePath "$root\$BuildDir\SouthParkRecompiled.exe" -ArgumentList $arguments -WorkingDirectory $root -PassThru
$started = Get-Date
foreach ($second in ($At | Sort-Object)) {
    while (((Get-Date) - $started).TotalSeconds -lt $second -and -not $process.HasExited) { Start-Sleep -Milliseconds 200 }
    if ($process.HasExited) { "process exited before $second s, code 0x{0:X8}" -f $process.ExitCode; break }
    $process.Refresh()
    $handle = $process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { "no window at $second s"; continue }
    $rect = New-Object SpWindow+Rect
    [SpWindow]::GetClientRect($handle, [ref]$rect) | Out-Null
    $origin = New-Object System.Drawing.Point 0, 0
    [SpWindow]::ClientToScreen($handle, [ref]$origin) | Out-Null
    $width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
    for ($shot = 1; $shot -lt $Burst; $shot++) {
        $extra = New-Object System.Drawing.Bitmap $width, $height
        $extraGraphics = [System.Drawing.Graphics]::FromImage($extra)
        [SpWindow]::Capture($handle, $extra)
        $extra.Save((Join-Path $root "rom\${Name}_${second}b$shot.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $extraGraphics.Dispose(); $extra.Dispose()
    }
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    [SpWindow]::Capture($handle, $bitmap)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $path = Join-Path $root "rom\${Name}_$second.png"
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    # Rough content check: how many of a 48x27 grid of sample points are not black.
    $lit = 0; $total = 0
    for ($y = 16; $y -lt $height; $y += [int]($height / 27)) {
        for ($x = 16; $x -lt $width; $x += [int]($width / 48)) {
            $pixel = $bitmap.GetPixel($x, $y); $total++
            if (($pixel.R + $pixel.G + $pixel.B) -gt 24) { $lit++ }
        }
    }
    $graphics.Dispose(); $bitmap.Dispose()
    "saved $path (${width}x$height), non-black samples: $lit of $total"
}
if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
Get-Process SouthParkRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force

# Contact sheet of everything captured in this run: rom\<Name>_sheet.png
$shots = @(Get-ChildItem (Join-Path $root "rom") -Filter "${Name}_*.png" | Where-Object { $_.Name -match "_\d+\.png$" } |
    Sort-Object { [int]($_.BaseName -replace '.*_', '') })
if ($shots.Count -gt 1) {
    $columns = 4; $cellWidth = 480; $cellHeight = 270
    $rows = [int][math]::Ceiling($shots.Count / $columns)
    $sheet = New-Object System.Drawing.Bitmap ($columns * $cellWidth), ($rows * $cellHeight)
    $canvas = [System.Drawing.Graphics]::FromImage($sheet)
    $canvas.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $font = New-Object System.Drawing.Font "Consolas", 14
    for ($i = 0; $i -lt $shots.Count; $i++) {
        $image = [System.Drawing.Image]::FromFile($shots[$i].FullName)
        $x = ($i % $columns) * $cellWidth; $y = [int][math]::Floor($i / $columns) * $cellHeight
        $canvas.DrawImage($image, $x, $y, $cellWidth, $cellHeight)
        $canvas.DrawString(($shots[$i].BaseName -replace '.*_', '') + " s", $font, [System.Drawing.Brushes]::Yellow, $x + 6, $y + 4)
        $image.Dispose()
    }
    $sheetPath = Join-Path $root "rom\${Name}_sheet.png"
    $sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $canvas.Dispose(); $sheet.Dispose()
    "sheet $sheetPath"
}
