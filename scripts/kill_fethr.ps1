<#
.SYNOPSIS
    Stop every running fethr process.

.DESCRIPTION
    Quitting from the tray is the polite route; this is the "it wedged and the
    hotkeys are still suppressed" escape hatch. Finds python/pythonw processes
    whose command line contains "fethr" and stops them.

.PARAMETER WhatIf
    Show what would be stopped without stopping it.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\kill_fethr.ps1
#>
[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = 'Stop'

$targets = Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" |
    Where-Object { $_.CommandLine -and $_.CommandLine -match 'fethr' }

if (-not $targets) {
    Write-Host 'No fethr processes are running.'
    return
}

foreach ($process in $targets) {
    Write-Host ("Stopping PID {0}: {1}" -f $process.ProcessId, $process.CommandLine.Trim())
    if ($PSCmdlet.ShouldProcess("PID $($process.ProcessId)", 'Stop-Process')) {
        Stop-Process -Id $process.ProcessId -Force
    }
}

Write-Host ("Stopped {0} process(es)." -f @($targets).Count)
