Two quality-of-life fixes, no firmware change.

**The sidecar no longer fights your other USB boards.** Until now, whenever the macro-pad wasn't connected, fethr opened every Espressif-looking COM port every two seconds and waited for a hello — which is exactly what an ESP32 in bootloader mode looks like, so flashing anything else on the same PC was a coin toss. The Sidecar page now has a connect mode. **Only when I click Connect** never touches a port on its own; it just remembers which COM port your sidecar was on and takes it back if that exact port reappears. **Connect automatically** (the default for new installs) probes a port once when it first shows up and then leaves it alone. Both wait at most 1.5 s for a hello and skip any port another program holds open.

**The microphone list is one line per microphone.** Windows reports each mic once per audio API, with the oldest one cutting names off at 31 characters. The dropdown now shows the WASAPI entry only, with the full name, and the pseudo-devices ("Sound Mapper", "Primary Sound Capture") are gone. Your saved choice is matched by exact name so it can't land on the wrong twin.

Settings: `sidecar.auto_connect` is replaced by `sidecar.connect_mode` (`auto` / `manual`); old files convert on load. `sidecar.last_port` holds the remembered port.
