<#
.SYNOPSIS
  Flash the prebuilt fethr sidecar firmware onto a Chain DualKey in download mode.

.DESCRIPTION
  Finds the serial port that appeared when the board entered download mode, runs
  esptool against it, and prints what to check afterwards. See FLASH.md for the
  download-mode procedure and for doing this by hand instead.

  esptool is located in this order:
    1. an `esptool` already on PATH
    2. `uv tool run esptool`  (nothing installed permanently)
    3. `python -m esptool`    (pip install esptool)

.PARAMETER Port
  Serial port to flash, e.g. COM7. Default: the most recently added port, which
  is the one the download-mode board just created.

.PARAMETER Image
  Firmware image. Default: the newest bin\fethr-sidecar-*\firmware.factory.bin
  beside this script.

.PARAMETER Baud
  Upload speed. Default 921600. Drop to 460800 or 115200 on a flaky cable.

.PARAMETER Erase
  Erase the whole flash first, discarding saved device settings (NVS).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File hardware\flash.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File hardware\flash.ps1 -Port COM7 -Baud 460800
#>

[CmdletBinding()]
param(
    [string] $Port,
    [string] $Image,
    [int]    $Baud = 921600,
    [switch] $Erase
)

$ErrorActionPreference = 'Stop'

function Find-Image {
    $binRoot = Join-Path $PSScriptRoot 'bin'
    if (-not (Test-Path $binRoot)) {
        throw "No bin\ folder beside this script - pass -Image with the path to firmware.factory.bin."
    }
    $candidate = Get-ChildItem -Path $binRoot -Recurse -Filter 'firmware.factory.bin' |
                 Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $candidate) {
        throw "No firmware.factory.bin under $binRoot - pass -Image explicitly."
    }
    return $candidate.FullName
}

# The ROM bootloader's port is the one that was not there a moment ago. Windows
# has no "added at" timestamp for a COM port, so use the registry order: the
# serial device most recently enumerated is last in SERIALCOMM.
function Find-Port {
    $key = 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM'
    $ports = @()
    if (Test-Path $key) {
        $item = Get-ItemProperty -Path $key
        $ports = $item.PSObject.Properties |
                 Where-Object { $_.Name -notlike 'PS*' } |
                 ForEach-Object { $_.Value }
    }
    if (-not $ports) { $ports = [System.IO.Ports.SerialPort]::GetPortNames() }
    if (-not $ports) {
        throw "No serial ports at all. Is the board in download mode? (FLASH.md step 2)"
    }
    $chosen = $ports[-1]
    if ($ports.Count -gt 1) {
        Write-Host "Ports present: $($ports -join ', ')  ->  using $chosen" -ForegroundColor Yellow
        Write-Host "If that is wrong, re-run with -Port COMx." -ForegroundColor Yellow
    }
    return $chosen
}

# Returns the argv prefix that runs esptool, or $null.
function Find-Esptool {
    if (Get-Command esptool -ErrorAction SilentlyContinue) { return @('esptool') }
    if (Get-Command esptool.py -ErrorAction SilentlyContinue) { return @('esptool.py') }
    if (Get-Command uv -ErrorAction SilentlyContinue) { return @('uv', 'tool', 'run', 'esptool') }
    if (Get-Command python -ErrorAction SilentlyContinue) { return @('python', '-m', 'esptool') }
    return $null
}

function Invoke-Esptool {
    param([string[]] $Prefix, [string[]] $Arguments)
    $exe  = $Prefix[0]
    $argv = @()
    if ($Prefix.Count -gt 1) { $argv += $Prefix[1..($Prefix.Count - 1)] }
    $argv += $Arguments
    Write-Host ""
    Write-Host "> $exe $($argv -join ' ')" -ForegroundColor DarkGray
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "esptool exited with $LASTEXITCODE" }
}

if (-not $Image) { $Image = Find-Image }
if (-not (Test-Path $Image)) { throw "Image not found: $Image" }
if (-not $Port)  { $Port  = Find-Port }

$tool = Find-Esptool
if (-not $tool) {
    throw @'
esptool not found. Install one of:
  uv          https://docs.astral.sh/uv/    (this script then uses: uv tool run esptool)
  pip install esptool
Or flash in the browser instead - see FLASH.md option A.
'@
}

Write-Host ""
Write-Host "fethr sidecar flasher" -ForegroundColor Cyan
Write-Host "  image : $Image"
Write-Host "  port  : $Port"
Write-Host "  baud  : $Baud"
Write-Host ""
Write-Host "The board must be in DOWNLOAD MODE: side switch middle, unplug," -ForegroundColor Yellow
Write-Host "hold Key 2 (the key farther from the lanyard hole), plug in, release. (FLASH.md step 2)" -ForegroundColor Yellow

if ($Erase) {
    Write-Host ""
    Write-Host "Erasing flash - saved device settings will be lost." -ForegroundColor Yellow
    Invoke-Esptool -Prefix $tool -Arguments @('--chip', 'esp32s3', '--port', $Port, 'erase_flash')
}

Invoke-Esptool -Prefix $tool -Arguments @(
    '--chip', 'esp32s3', '--port', $Port, '--baud', "$Baud",
    'write_flash', '0x0', $Image
)

Write-Host ""
Write-Host "Flashed." -ForegroundColor Green
Write-Host ""
Write-Host "Next:"
Write-Host "  1. Unplug and plug back in WITHOUT holding Key 2 (switch stays middle)."
Write-Host "  2. Windows should show a keyboard named 'fethr sidecar'."
Write-Host "  3. Key 1 lights dim blue, Key 2 dim violet; a Mono panel scrolls FETHR then shows the feather."
Write-Host "  4. Start fethr, open Notepad, hold Key 1 and talk."
Write-Host ""
Write-Host "Full checklist: hardware/FLASH.md, section 4."
