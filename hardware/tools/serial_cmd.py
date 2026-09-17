"""Send one JSON command to the sidecar and print the reply line(s).
Usage: python serial_cmd.py COMx '{"cmd":"get_config"}' [seconds]"""
import sys, time, json
import serial

port, cmd = sys.argv[1], sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
# A bare word is shorthand for {"cmd": word} (shell quoting is unreliable over ssh).
if not cmd.strip().startswith("{"):
    cmd = json.dumps({"cmd": cmd.strip()})
with serial.Serial(port, 115200, timeout=0.2) as s:
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write((cmd.strip() + "\n").encode())
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
for line in buf.decode("utf-8", "replace").splitlines():
    if line.startswith("{"):
        try:
            print(json.dumps(json.loads(line), indent=1)[:3000])
        except Exception:
            print(line[:3000])
