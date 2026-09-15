"""Print what the sidecar says over USB CDC: boot banner, chain enumeration,
then the '?' status dump. Usage: python serial_peek.py [COMx] [seconds]"""
import sys, time
import serial
from serial.tools import list_ports

port = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].upper().startswith("COM") else None
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0

for p in list_ports.comports():
    print(f"port {p.device}: {p.description} | vid={p.vid:#06x} pid={p.pid:#06x} | {p.product} | {p.manufacturer}"
          if p.vid else f"port {p.device}: {p.description}")
    if port is None and p.vid == 0x303A:
        port = p.device

if not port:
    print("no Espressif port found")
    sys.exit(1)

print(f"--- opening {port} ---")
with serial.Serial(port, 115200, timeout=0.2) as s:
    time.sleep(0.3)
    s.write(b"?\n")
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
    sys.stdout.write(buf.decode("utf-8", "replace"))
    print(f"\n--- {len(buf)} bytes ---")
