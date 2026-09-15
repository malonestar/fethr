# stage_bin.ps1 - copy the freshly built images into hardware/bin/<name>/ and
# write SHA256SUMS.txt beside them.
#
# Not part of the build: run it after `pio run` when cutting a release folder.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stage_bin.ps1 -Version 0.2.0
param(
    [Parameter(Mandatory = $true)][string]$Version
)

$ErrorActionPreference = 'Stop'

$pio   = Split-Path -Parent $PSScriptRoot           # .../firmware/pio
$build = Join-Path $pio '.pio\build\chain_dualkey'
$dest  = Join-Path (Split-Path -Parent (Split-Path -Parent $pio)) "bin\fethr-sidecar-$Version"

New-Item -ItemType Directory -Force -Path $dest | Out-Null

$files = @('firmware.factory.bin', 'bootloader.bin', 'partitions.bin', 'firmware.bin')
foreach ($f in $files) {
    Copy-Item (Join-Path $build $f) (Join-Path $dest $f) -Force
}

# boot_app0.bin comes from the core, not the build dir.
$bootApp0 = Get-ChildItem -Path 'C:\pio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin' -ErrorAction SilentlyContinue
if ($bootApp0) {
    Copy-Item $bootApp0.FullName (Join-Path $dest 'boot_app0.bin') -Force
} else {
    Write-Warning 'boot_app0.bin not found in the core tools/partitions/ - copy it by hand'
}

$lines = @()
foreach ($f in (Get-ChildItem $dest -Filter *.bin | Sort-Object Name)) {
    $h = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
    $lines += "$h  $($f.Name)"
}
Set-Content -Path (Join-Path $dest 'SHA256SUMS.txt') -Value $lines -Encoding ascii

Write-Host "staged -> $dest"
Get-ChildItem $dest | Select-Object Name, Length | Format-Table -AutoSize
