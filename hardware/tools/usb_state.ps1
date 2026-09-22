# What is the DualKey enumerating as right now?  Run on the PC it is plugged into.
#   bootloader (download mode): "USB JTAG/serial debug unit", VID_303A PID_1001
#   fethr firmware:             "USB Serial Device (COMx)" + HID keyboard/mouse, VID_303A
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
"COM ports: " + ($(if ($ports) { $ports -join ', ' } else { '(none)' }))
$devs = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_303A*' }
if (-not $devs) { "No Espressif (VID 303A) USB device is present."; exit 0 }
foreach ($d in $devs) {
  "{0,-8} {1,-10} {2}  [{3}]" -f $d.Status, $d.Class, $d.FriendlyName, $d.InstanceId
}
if ($devs | Where-Object { $_.InstanceId -like '*PID_1001*' }) { "=> DOWNLOAD MODE (ROM bootloader) - ready to flash." }
elseif ($devs | Where-Object { $_.Class -eq 'HIDClass' }) { "=> running fethr firmware (not in download mode)." }
