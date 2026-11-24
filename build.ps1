# Builds South Park: Recompiled on Windows.
#
#   .\build.ps1              configure (first time) and build
#   .\build.ps1 -Recompile   rerun N64Recomp and RSPRecomp first
#   .\build.ps1 -Target RecompiledFuncs
#
# Needs: Visual Studio Build Tools with the C++ workload, LLVM 20 or newer
# (clang-cl), an LLVM with the MIPS target for the patch binary, and make.
# Tool locations come from the parameters below or from build.local.ps1.
param(
    [string]$Config = "Release",
    [string]$BuildDir = "build",             # e.g. -Config RelWithDebInfo -BuildDir build-dbg
    [string]$Target = "",
    [switch]$Recompile,
    [string]$Llvm = $env:SP_LLVM,            # clang-cl for the host build
    [string]$LlvmMips = $env:SP_LLVM_MIPS,   # clang with the MIPS target
    [string]$MakeDir = $env:SP_MAKE_DIR      # folder containing make.exe
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$local = Join-Path $root "build.local.ps1"
if (Test-Path $local) { . $local }
foreach ($pair in @(@("Llvm", $Llvm), @("LlvmMips", $LlvmMips), @("MakeDir", $MakeDir))) {
    if (-not $pair[1]) { throw "Set -$($pair[0]) or define it in build.local.ps1" }
}

# Import the MSVC environment.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ build tools not found" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>nul && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = "$Llvm\bin;$env:PATH;$MakeDir"

Push-Location $root
try {
    $recomp = Join-Path $root "external\N64Recomp\build"
    if ($Recompile -or -not (Test-Path "RecompiledFuncs\funcs.h")) {
        & "$recomp\N64Recomp.exe" southpark.us.toml | Select-String -NotMatch "^\[Info\]"
        if ($LASTEXITCODE) { throw "N64Recomp failed" }
        & "$recomp\RSPRecomp.exe" aspMain.us.toml
        if ($LASTEXITCODE) { throw "RSPRecomp failed" }
    }
    if (-not (Test-Path "$BuildDir\build.ninja")) {
        cmake -S . -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
            -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
            "-DPATCHES_C_COMPILER=$($LlvmMips -replace '\\','/')/bin/clang.exe" `
            "-DPATCHES_LD=$($Llvm -replace '\\','/')/bin/ld.lld.exe" `
            "-DN64RECOMP_EXE=$($recomp -replace '\\','/')/N64Recomp.exe"
        if ($LASTEXITCODE) { throw "CMake configure failed" }
    }
    $buildArgs = @("--build", $BuildDir, "--config", $Config)
    if ($Target) { $buildArgs += @("--target", $Target) }
    cmake @buildArgs
    if ($LASTEXITCODE) { throw "Build failed" }
} finally {
    Pop-Location
}
