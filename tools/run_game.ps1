# Starts the game straight from the command line for a fixed time and prints the log.
#
#   .\tools\run_game.ps1 [-Seconds 20] [-Keep] [-BuildDir build-dbg] [-DumpDl 1]
#
# -Keep leaves the program running instead of closing it when the time is up.
# With a build that has a .pdb (RelWithDebInfo), crash reports get file and line numbers.
param(
    [int]$Seconds = 20,
    [switch]$Keep,
    [int]$Tail = 60,
    [int]$DumpDl = 0,     # write the first N graphics display lists to the log
    [int]$DumpSkip = 0,   # let N display lists pass before dumping starts
    [int]$Mash = 0,       # press START and A in turn for N seconds
    [int]$Watchdog = 0,   # dump all thread stacks if no frame arrives for N seconds
    [string]$BuildDir = "build"
)
$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $root "rom\run.log"
Remove-Item $log -ErrorAction SilentlyContinue
$arguments = @("--rom", "`"$root\rom\southpark.us.z64`"", "--autostart", "--log", "`"$log`"")
if ($DumpDl -gt 0) { $arguments += @("--dump-dl", "$DumpDl") }
if ($DumpSkip -gt 0) { $arguments += @("--dump-dl-skip", "$DumpSkip") }
if ($Mash -gt 0) { $arguments += @("--mash", "$Mash") }
if ($Watchdog -gt 0) { $arguments += @("--watchdog", "$Watchdog") }
$process = Start-Process -FilePath "$root\$BuildDir\SouthParkRecompiled.exe" -ArgumentList $arguments -WorkingDirectory $root -PassThru
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $process.HasExited) { Start-Sleep -Milliseconds 250 }
if ($process.HasExited) {
    "process exited by itself, code 0x{0:X8}" -f $process.ExitCode
} elseif ($Keep) {
    "process still running (pid $($process.Id))"
} else {
    "process still running after $Seconds s; closing it"
    Stop-Process -Id $process.Id -Force
}
Start-Sleep -Milliseconds 500

$symbolArgs = @("--build", $BuildDir)
$local = Join-Path $root "build.local.ps1"
if (Test-Path $local) {
    $Llvm = $null; $LlvmMips = $null; $MakeDir = $null
    . $local
    $symbolizer = Join-Path $Llvm "bin\llvm-symbolizer.exe"
    if (Test-Path $symbolizer) { $symbolArgs += @("--symbolizer", $symbolizer) }
}
& "$root\.venv\Scripts\python.exe" -I "$root\tools\crash_symbols.py" $log @symbolArgs | Select-Object -Last $Tail
