# Packs a built release into out\release\SouthParkRecompiled-v<version>-windows-x64.zip.
#
#   .\tools\package_release.ps1 [-BuildDir build] [-Version 0.9.1]
#
# Everything goes inside one top-level folder, SouthParkRecompiled\, so that unpacking (by hand or
# by a launcher such as RecompLauncher, which names the install folder after it) gives a folder
# with a proper name instead of spilling files or falling back to an internal id.
# The version defaults to version_string in src\main\main.cpp.
param(
    [string]$BuildDir = "build",
    [string]$Version = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

if (-not $Version) {
    $line = Select-String -Path "$root\src\main\main.cpp" -Pattern 'version_string = "([^"]+)"' | Select-Object -First 1
    if (-not $line) { throw "version_string not found in src\main\main.cpp" }
    $Version = $line.Matches[0].Groups[1].Value
}

$folderName = "SouthParkRecompiled"
$out = Join-Path $root "out\release"
$stage = Join-Path $out $folderName
$zip = Join-Path $out "SouthParkRecompiled-v$Version-windows-x64.zip"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($file in "SouthParkRecompiled.exe", "SDL2.dll", "dxcompiler.dll", "dxil.dll") {
    $path = Join-Path $root "$BuildDir\$file"
    if (-not (Test-Path $path)) { throw "$path is missing; build first" }
    Copy-Item $path $stage
}
foreach ($file in "recompcontrollerdb.txt", "README.md", "README.ru.md", "LICENSE") {
    Copy-Item (Join-Path $root $file) $stage
}
Copy-Item -Recurse (Join-Path $root "assets") (Join-Path $stage "assets")

# Passing the folder itself (not its contents) keeps SouthParkRecompiled\ as the archive's root.
Compress-Archive -Path $stage -DestinationPath $zip
"packed v$Version into $zip ($((Get-Item $zip).Length) bytes)"
