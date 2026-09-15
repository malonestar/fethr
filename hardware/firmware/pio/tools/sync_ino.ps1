<#
  sync_ino.ps1 - regenerate the Arduino-IDE sketch folder from the PlatformIO
  project.

  SINGLE SOURCE OF TRUTH: firmware/pio/src + firmware/pio/include.
  firmware/flow_sidecar/ is GENERATED - never edit it by hand, your changes
  will be overwritten the next time this script runs.

  What it does:
    * copies every .h from pio/include and every .cpp from pio/src into the
      sketch folder, FLAT (the Arduino IDE compiles every .cpp/.h that sits
      beside the .ino, and #include "config.h" resolves in both layouts);
    * renames main.cpp to flow_sidecar.ino, which is the one file the Arduino
      IDE insists on;
    * removes stale generated files that no longer exist upstream.

  Usage (from anywhere):
    powershell -NoProfile -ExecutionPolicy Bypass -File <path>\sync_ino.ps1
#>

$ErrorActionPreference = 'Stop'

$pioRoot  = Split-Path -Parent $PSScriptRoot
$srcDir   = Join-Path $pioRoot 'src'
$incDir   = Join-Path $pioRoot 'include'
$inoDir   = Join-Path (Split-Path -Parent $pioRoot) 'flow_sidecar'

if (-not (Test-Path $inoDir)) { New-Item -ItemType Directory -Path $inoDir | Out-Null }

$header = @'
/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/{0}/{1}
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */

'@

$generated = New-Object System.Collections.Generic.List[string]

function Copy-WithBanner($file, $subdir, $destName) {
    $dest = Join-Path $inoDir $destName
    $text = ($header -f $subdir, $file.Name) + (Get-Content -Raw -Path $file.FullName)
    Set-Content -Path $dest -Value $text -Encoding UTF8 -NoNewline
    $script:generated.Add($destName)
}

Get-ChildItem -Path $incDir -Filter *.h | ForEach-Object {
    Copy-WithBanner $_ 'include' $_.Name
}

Get-ChildItem -Path $srcDir -Filter *.cpp | ForEach-Object {
    if ($_.Name -eq 'main.cpp') {
        Copy-WithBanner $_ 'src' 'flow_sidecar.ino'
    } else {
        Copy-WithBanner $_ 'src' $_.Name
    }
}

# Drop anything we generated previously that is no longer produced.
Get-ChildItem -Path $inoDir -Include *.h, *.cpp, *.ino -File | ForEach-Object {
    if ($generated -notcontains $_.Name) {
        $head = Get-Content -Path $_.FullName -TotalCount 3 -ErrorAction SilentlyContinue
        if ($head -and ($head -join "`n") -match 'GENERATED FILE') {
            Write-Host "removing stale $($_.Name)"
            Remove-Item $_.FullName
        } else {
            Write-Host "leaving non-generated file $($_.Name)"
        }
    }
}

Write-Host "sync_ino: wrote $($generated.Count) file(s) to $inoDir"
