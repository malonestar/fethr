Second release. Everything in here was worked out on a real desk with the full chain plugged in.

**Chain builder** — new Layout tab in the app. Drag the modules you own into the order they're cabled, using photos of the actual parts. Rotate a module and the app tells the firmware which way is up: joystick direction, panel rotation and the two DualKey keys are all runtime settings now (no more rebuilding for a flipped board). Save to device stores it in flash.

**Four layers on the public firmware** — FETHR, MEDIA, EDIT and MOUSE all ship enabled. The FLOW layer is now called FETHR and the Mono panel shows a small icon per layer (feather, note, pencil, cursor) instead of a letter.

**Protocol 2** — `get_layers`, `set_action`, `set_layer_meta` let the app read and remap every key, plus `swap_keys`, `nav_swap_xy`, `scroll_swap_xy`. Details in `hardware/PROTOCOL.md`.

**Companion display** — the DualKey now pings the AtomS3R every 3 seconds so the link stays up in both directions, and the probe timing no longer phase-locks against the Atom's beacon. Screen rotation set for the Atomic ToChain Base.

**Fixes**
- Chain Key stayed dark when the whole chain was attached: node LEDs now get a guaranteed paint slot so the key poll can't starve them.
- Re-plugging the chain left LEDs unpainted: same-count re-enumeration repaints nodes and reconfigures the Mono panel.
- UART pins were left driven after a port switch, which silently killed receive on the companion link and the chain bus. Pins are reset before every re-open.
- Holding Key 1 or Key 2 no longer lights Key 3 red; only the pressed key changes colour.
- Windows reports the board as a plain "USB Serial Device" with no product string, so the app never found it. It now scans by vendor ID and confirms with a hello.
- README notes that `uv` 0.12 or newer is needed; older versions build a console-subsystem `pythonw.exe` and you get a stray black window.

**Flashing** — prebuilt images attached (`fethr-sidecar-0.2.0-factory.bin`, `fethr-companion-atoms3r-0.1.0-factory.bin`), both flash at `0x0`. The companion image is unchanged from 0.1.0. Steps in `hardware/FLASH.md` and `hardware/COMPANION.md`.

**Tools** — `hardware/tools/serial_peek.py` watches the console, `serial_cmd.py` sends one command. Handy when something on the chain looks wrong.
