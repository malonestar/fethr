# Which windowed processes are running elevated?  A non-elevated fethr cannot
# type into an elevated window (Windows UIPI drops the keystrokes silently).
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Tok {
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr h, uint access, out IntPtr tok);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool GetTokenInformation(IntPtr tok, int cls, out uint info, uint len, out uint ret);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
function Is-Elevated($p) {
  try {
    $tok = [IntPtr]::Zero
    if (-not [Tok]::OpenProcessToken($p.Handle, 0x0008, [ref]$tok)) { return "?" }
    $v = [uint32]0; $r = [uint32]0
    [Tok]::GetTokenInformation($tok, 20, [ref]$v, 4, [ref]$r) | Out-Null   # TokenElevation
    [Tok]::CloseHandle($tok) | Out-Null
    return [bool]$v
  } catch { return "?" }
}
Get-Process | Where-Object { $_.MainWindowTitle -or $_.ProcessName -match "notepad|firefox|pythonw|chrome|code" } | ForEach-Object {
  "{0,-6} {1,-22} elevated={2,-5} {3}" -f $_.Id, $_.ProcessName, (Is-Elevated $_), $_.MainWindowTitle
}
