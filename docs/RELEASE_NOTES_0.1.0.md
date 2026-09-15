First release.

**App** — Windows tray app with a settings window. Hold F8 to dictate (raw), F9 for the cleaned-up version, tap F7 to re-paste. Speech recognition on your own whisper.cpp server, optional cleanup through your own Ollama. Settings in `%APPDATA%\fethr\settings.json`.

**Sidecar firmware** — for an M5Stack Chain DualKey with Key / Joystick / Angle / Mono modules. Four layers (FLOW, MEDIA, EDIT, MOUSE), per-key function colours, panel indicators, USB settings channel to the app. Finds the chain on either port automatically.

**Companion display** — optional AtomS3R on an Atomic ToChain Base, cabled to the DualKey's spare port: shows the active layer's key legend, a "talk!" animation while recording, and cycles layers with its button.

**Flashing** — prebuilt images attached (`fethr-sidecar-0.1.0-factory.bin`, `fethr-companion-atoms3r-0.1.0-factory.bin`), both flash at `0x0`. Steps in `hardware/FLASH.md` and `hardware/COMPANION.md`.

Known: only tested on one desk so far. Joystick axis direction, panel rotation and which key is which depend on how you mount the modules; all three are settings in the app's Sidecar page.
