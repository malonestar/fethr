Small release, two things you'll notice.

**Key 1 is the left key.** The numbering now assumes the DualKey sits with its USB-C cable pointing away from you: Key 1 on the left (raw dictation), Key 2 on the right (dictation plus cleanup). 0.2.0 had them the other way round, which meant a board on a normal desk came up backwards. If your cable points toward you instead, turn the DualKey 180° in the app's Layout tab and it swaps them on the device. One knock-on: the chip's boot button is a hardware fact, and it is now Key 2, so the download-mode step in `hardware/FLASH.md` says hold Key 2 (the right key, farther from the lanyard hole).

**Cleanup edits your words instead of replying to them.** Dictate "explain to me what multiplexing is" and the cleanup pass used to hand back an explanation of multiplexing. The model is now told, in so many words, that it is a transcript editor and the text is never a question for it; three worked examples ride along with every request; and a size check throws away any result that is nowhere near the length of what you said and pastes the raw transcript instead. Filler words and self-corrections are still tidied.

**Fixes**
- `hardware/flash.ps1` chose its default image by file name, and every candidate is called `firmware.factory.bin`, so it could pick an old version. It now takes the newest version folder.
- New `hardware/tools/usb_state.ps1` says whether the board is in download mode, running fethr, or not enumerating at all — useful before you run the flasher.
- New `hardware/tools/cleanup_probe.py` runs a handful of dictated phrases through your cleanup server so you can see what it does before trusting it.

**Flashing** — `fethr-sidecar-0.2.1-factory.bin` attached, flashes at `0x0`. The companion image is unchanged (`fethr-companion-atoms3r-0.1.0-factory.bin`, also attached). Your saved device settings survive the flash.
