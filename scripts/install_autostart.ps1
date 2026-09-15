<#
.SYNOPSIS
    Add (or remove) a fethr shortcut in the current user's Startup folder.

.DESCRIPTION
    Creates "fethr.lnk" in shell:startup pointing at pythonw.exe -m fethr, with
    the repository as the working directory and the generated .ico as the icon.
    Uses pythonw so no console window appears at login.

.PARAMETER Python
    Path to a pythonw.exe. Defaults to .venv\Scripts\pythonw.exe beside the
    repository, falling back to the first pythonw.exe on PATH.

.PARAMETER Remove
    Delete the shortcut instead of creating it.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\install_autostart.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\install_autostart.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$Python,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$startup = [Environment]::GetFolderPath('Startup')
$linkPath = Join-Path $startup 'fethr.lnk'

if ($Remove) {
    if (Test-Path $linkPath) {
        Remove-Item $linkPath -Force
        Write-Host "Removed $linkPath"
    } else {
        Write-Host "Nothing to remove at $linkPath"
    }
    return
}

if (-not $Python) {
    $venv = Join-Path $root '.venv\Scripts\pythonw.exe'
    if (Test-Path $venv) {
        $Python = $venv
    } else {
        $cmd = Get-Command pythonw.exe -ErrorAction SilentlyContinue
        if (-not $cmd) { throw 'No pythonw.exe found. Pass -Python <path>.' }
        $Python = $cmd.Source
    }
}

if (-not (Test-Path $Python)) { throw "Python not found: $Python" }

$icon = Join-Path $root 'fethr\assets\fethr.ico'

$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($linkPath)
$link.TargetPath       = $Python
$link.Arguments        = '-m fethr'
$link.WorkingDirectory = $root
$link.Description      = 'fethr - local-first dictation'
if (Test-Path $icon) { $link.IconLocation = $icon }
$link.Save()

Write-Host "Installed $linkPath"
Write-Host "  target : $Python -m fethr"
Write-Host "  workdir: $root"
