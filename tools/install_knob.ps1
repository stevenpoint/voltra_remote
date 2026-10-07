# Flash the Voltra remote onto a Waveshare ESP32-S3-Knob-Touch-LCD-1.8 from Windows, then
# take screenshots of every screen for design work. Run it from PowerShell:
#
#   irm https://raw.githubusercontent.com/stevenpoint/voltra_remote/watch206/tools/install_knob.ps1 | iex
#
# Installs Python and Git (with winget) if they are missing, and PlatformIO in a folder of its own
# (%USERPROFILE%\voltra_remote_tools). Safe to run again: it fetches the latest code each time.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Branch = 'watch206'
$ZipUrl = "https://github.com/stevenpoint/voltra_remote/archive/refs/heads/$Branch.zip"
$Home_ = [Environment]::GetFolderPath('UserProfile')
$Tools = Join-Path $Home_ 'voltra_remote_tools'
$Venv = Join-Path $Tools 'venv'
$VenvPy = Join-Path $Venv 'Scripts\python.exe'
$Src = Join-Path $Home_ 'voltra_remote'

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
# Run with "irm | iex", so throw rather than exit: exit would close the PowerShell window.
function Fail($msg) {
    Write-Host "`n$msg" -ForegroundColor Red
    throw 'Stopped.'
}

function Find-Python {
    # The py launcher comes with python.org and winget installs, and unlike "python" it is
    # never the Microsoft Store stub.
    foreach ($cmd in @(@('py', '-3'), @('python'))) {
        try {
            $exe = $cmd[0]; $rest = @($cmd | Select-Object -Skip 1)
            $out = & $exe @rest -c 'import sys; print(sys.version_info >= (3, 9))' 2>$null
            if ($LASTEXITCODE -eq 0 -and $out -eq 'True') { return , $cmd }
        } catch {}
    }
    return $null
}

# --- 1. Python ---------------------------------------------------------------------------
Step 'Checking for Python'
if (-not (Test-Path $VenvPy)) {
    $py = Find-Python
    if (-not $py) {
        if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
            Fail ("Python is not installed and winget is not available to install it.`n" +
                  "Install Python from https://www.python.org/downloads/ (tick 'Add python.exe to PATH'), then run this again.")
        }
        Write-Host 'Installing Python (a few minutes)...'
        winget install -e --id Python.Python.3.12 --scope user --silent --accept-source-agreements --accept-package-agreements
        $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                    [Environment]::GetEnvironmentVariable('Path', 'User')
        $py = Find-Python
        if (-not $py) { Fail 'Python installed, but this window cannot see it yet. Close PowerShell, open a new one and run this again.' }
    }
    Write-Host 'Setting up PlatformIO (a few minutes)...'
    New-Item -ItemType Directory -Force -Path $Tools | Out-Null
    $exe = $py[0]; $rest = @($py | Select-Object -Skip 1)
    & $exe @rest -m venv $Venv
    if ($LASTEXITCODE -ne 0) { Fail 'Could not create the Python environment.' }
}
& $VenvPy -m pip install --quiet --upgrade platformio pyserial
if ($LASTEXITCODE -ne 0) { Fail 'Could not install PlatformIO. Check the internet connection and run this again.' }
Write-Host 'Python and PlatformIO ready.'

# --- Git: the ESP32 platform will not build without it on PATH ----------------------------
Step 'Checking for Git'
function Add-GitPath {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path', 'User')
    foreach ($d in @("$env:ProgramFiles\Git\cmd", "$env:LOCALAPPDATA\Programs\Git\cmd")) {
        if ((Test-Path "$d\git.exe") -and ($env:Path -notlike "*$d*")) { $env:Path = "$d;$env:Path" }
    }
}
Add-GitPath
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Fail ("Git is not installed and winget is not available to install it.`n" +
              "Install Git from https://git-scm.com/download/win, then run this again.")
    }
    Write-Host 'Installing Git (a few minutes; Windows may ask for permission)...'
    winget install -e --id Git.Git --silent --accept-source-agreements --accept-package-agreements
    Add-GitPath
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Fail 'Git installed, but this window cannot see it yet. Close PowerShell, open a new one and run this again.'
    }
}
Write-Host 'Git ready.'

# --- 2. Code -----------------------------------------------------------------------------
Step 'Downloading the latest code'
$zip = Join-Path $env:TEMP 'voltra_remote.zip'
$unpack = Join-Path $env:TEMP 'voltra_remote_unpack'
Invoke-WebRequest -Uri $ZipUrl -OutFile $zip -UseBasicParsing
if (Test-Path $unpack) { Remove-Item -Recurse -Force $unpack }
Expand-Archive -Path $zip -DestinationPath $unpack
if (Test-Path $Src) {
    # Keep the build cache (.pio) so a re-run builds faster; replace everything else.
    Get-ChildItem -Force $Src | Where-Object { $_.Name -ne '.pio' } | Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $Src | Out-Null
}
Copy-Item -Recurse -Force (Join-Path $unpack "voltra_remote-$Branch\*") $Src
Remove-Item -Recurse -Force $unpack, $zip
Set-Location $Src
Write-Host "Code in $Src"

# --- 3. Flash ----------------------------------------------------------------------------
Step 'Flashing the knob'
Write-Host 'Plug the knob in over USB-C.'
Write-Host 'The first build downloads the ESP32 toolchain, which takes several minutes.'
while ($true) {
    & $VenvPy -m platformio run -e remote -t upload
    if ($LASTEXITCODE -eq 0) { break }
    Write-Host "`nThe upload did not work." -ForegroundColor Yellow
    Write-Host 'If the build itself finished, the knob was probably not found: unplug it, flip the'
    Write-Host 'USB-C plug over (the knob has two USB chips, one per side), and plug it back in.'
    $again = Read-Host 'Try again? (Y/n)'
    if ($again -match '^[nN]') { Fail 'Stopped before flashing.' }
}
Write-Host 'Knob flashed.' -ForegroundColor Green

# --- 4. Screenshots ----------------------------------------------------------------------
$take = Read-Host "`nTake screenshots of every screen now? Leave the knob plugged in. (Y/n)"
if ($take -notmatch '^[nN]') {
    Step 'Taking screenshots'
    Start-Sleep -Seconds 5   # let the knob finish restarting after the upload
    $shots = Join-Path $Src 'shots'
    if (Test-Path $shots) { Remove-Item -Recurse -Force $shots }
    & $VenvPy tools\screenshot.py --all -o $shots
    if ($LASTEXITCODE -ne 0) { Fail 'The screenshots did not work. Close anything else using the knob (a serial monitor) and run this again.' }
    $out = Join-Path ([Environment]::GetFolderPath('Desktop')) 'knob_screenshots.zip'
    if (Test-Path $out) { Remove-Item -Force $out }
    Compress-Archive -Path (Join-Path $shots '*') -DestinationPath $out
    Write-Host "`nScreenshots saved to $out - please send that file." -ForegroundColor Green
    explorer.exe "/select,$out"
}

Write-Host "`nAll done. To flash again later, run the same command." -ForegroundColor Green
