# Sends a key press to the game's window only (PostMessage), for testing in-game keys without
# touching other windows.
#
#   .\tools\press_key.ps1 -Key 0x78      (0x78 = F9, 0x2D = Insert, 0x24 = Home)
param(
    [int]$Key = 0x78
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SpKeys {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$process = Get-Process SouthParkRecompiled -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $process -or $process.MainWindowHandle -eq 0) { "game window not found"; exit 1 }
$scan = [SpKeys]::MapVirtualKey($Key, 0)
$down = [IntPtr](1 -bor ($scan -shl 16))
$up = [IntPtr]([int64](1 -bor ($scan -shl 16) -bor (3 -shl 30)))
[void][SpKeys]::PostMessage($process.MainWindowHandle, 0x100, [IntPtr]$Key, $down)
Start-Sleep -Milliseconds 300
[void][SpKeys]::PostMessage($process.MainWindowHandle, 0x101, [IntPtr]$Key, $up)
"sent key 0x{0:X2} to window {1}" -f $Key, $process.MainWindowHandle
