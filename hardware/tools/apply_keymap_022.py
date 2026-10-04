"""Apply the 0.2.2 FETHR-layer defaults to a connected sidecar without reflashing:
Chain Key tap -> Enter, nav-stick push -> F7 (re-paste).  Run with the fethr app CLOSED."""
import sys, time
from fethr.core.device import SidecarDevice

dev = SidecarDevice(enabled=True)
if not dev.connect():
    print("connect failed:", dev.last_error); sys.exit(1)
print("connected", dev._port, "fw", (dev.info or {}).get("fw"), "proto", dev.proto)
before = dev.get_layers()[0]["actions"]
print("before: chain_key=", before["chain_key"], " nav_click=", before["nav_click"])
print(dev.set_action(0, "chain_key", {"type": "key_tap", "key": "enter", "mods": [], "fn": "enter"}))
print(dev.set_action(0, "nav_click", {"type": "key_tap", "key": "f7", "mods": [], "fn": "repaste"}))
after = dev.get_layers()[0]["actions"]
print("after:  chain_key=", after["chain_key"], " nav_click=", after["nav_click"])
print("save:", dev.save())
time.sleep(0.3)
dev.disconnect()
