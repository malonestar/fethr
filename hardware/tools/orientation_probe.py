"""Drive the app's own bridge against a real sidecar: apply an orientation the
way the Layout tab does, then read back what the device holds.

Usage: python orientation_probe.py [module rotation role]   (default joystick 180 nav)
Run with the fethr app CLOSED (it owns the port otherwise)."""
import sys, time
from fethr.core.device import SidecarDevice
from fethr.core.layout import orientation_settings

module = sys.argv[1] if len(sys.argv) > 1 else "joystick"
rotation = int(sys.argv[2]) if len(sys.argv) > 2 else 180
role = sys.argv[3] if len(sys.argv) > 3 else "nav"

dev = SidecarDevice(enabled=True)
ok = dev.connect()
print("connect:", ok, dev.last_error, "| port", dev._port, "| proto", dev.proto, "| fw",
      (dev.info or {}).get("fw"))
if not ok:
    sys.exit(1)

want = orientation_settings(module, rotation, role)
print("plan   :", want)
for path, value in want.items():
    try:
        r = dev.set_config(path, value)
        print(f"set {path}={value!r} -> {r}")
    except Exception as exc:
        print(f"set {path}={value!r} -> EXC {exc}")

cfg = dev.get_config()
keys = [k for k in cfg if "sign" in k or "swap" in k or k == "mono_rotation"]
print("device :", {k: cfg[k] for k in sorted(keys)})
print("save   :", dev.save())
time.sleep(0.3)
dev.disconnect()
