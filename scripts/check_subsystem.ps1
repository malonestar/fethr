# Print the PE subsystem of each exe: 2 = GUI (windowless), 3 = console.
param([string[]]$Paths)
foreach ($p in $Paths) {
  $b = [System.IO.File]::ReadAllBytes($p)
  $pe = [BitConverter]::ToInt32($b, 0x3C)
  $sub = [BitConverter]::ToInt16($b, $pe + 4 + 20 + 68)
  $kind = if ($sub -eq 2) { "GUI" } elseif ($sub -eq 3) { "CONSOLE" } else { "other($sub)" }
  Write-Host ("{0}  {1}  {2:N0} bytes" -f $kind, $p, $b.Length)
}
