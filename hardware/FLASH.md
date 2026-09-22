# Flashing the fethr sidecar

> Flashing the optional **companion display** (an AtomS3R) is a different board and a
> different image — see [COMPANION.md](COMPANION.md). Everything below is the DualKey.

The M5Stack Chain DualKey ships with M5Stack's own firmware. This replaces it with
fethr's. You do not need a toolchain, a compiler, or a serial terminal — one file
and a browser is enough. Ten minutes, most of which is reading.

The DualKey is an ESP32-S3. Flashing it cannot brick it: the ROM bootloader lives in
silicon and the download-mode procedure below always works, whatever is (or isn't) in
flash. If you want M5Stack's firmware back afterwards, M5Burner has it under device
type "Chain DualKey".

## 1. What to download

From this repository, the folder [`bin/fethr-sidecar-0.2.1/`](bin/fethr-sidecar-0.2.1/).
You only need one file:

**`firmware.factory.bin`** — everything merged into one image, written at offset `0x0`.

(The individual `bootloader.bin` / `partitions.bin` / `boot_app0.bin` / `firmware.bin`
are there too, with their offsets, for anyone who prefers them. `SHA256SUMS.txt` lets
you check the download.)

If you are reading this on GitHub: open the file and use the **Download raw file**
button. Do not copy it out of the web view — it is binary.

## 2. Put the board in download mode

The DualKey has **no reset button**, and once fethr's firmware is on it the board is a
USB keyboard, which means the usual auto-reset-into-bootloader handshake does not
exist. So you do this by hand, **before every flash**:

1. Move the 3-position side switch to the **middle** position.
2. Unplug the USB-C cable.
3. Press and **hold Key 2** — the key farther from the lanyard hole (it doubles as
   the chip's boot button). With the cable pointing away from you that's the right-hand key.
4. Plug USB-C back in, still holding Key 2.
5. Release Key 2.

A **new COM port appears** (Windows: Device Manager ▸ Ports; Linux: `/dev/ttyACM*`;
macOS: `/dev/cu.usbmodem*`). That port is the ROM bootloader. If no new port shows up,
you missed a step — unplug and do it again, holding Key 1 a moment longer.

To leave download mode without flashing: unplug and plug back in **without** holding
Key 1.

## 3. Flash it — pick one

### A. In the browser (no install)

Espressif host a WebSerial flasher: **<https://espressif.github.io/esptool-js/>**
It needs Chrome or Edge (Firefox and Safari have no WebSerial).

1. Set **Baudrate** to `921600`, click **Connect**, and pick the port that appeared
   in step 2.
2. Under **Program**, choose `firmware.factory.bin` and set its **Flash Address** to
   `0x0`.
3. Click **Program** and wait — about 15 seconds. It prints `Leaving...` when done.

### B. Command line

[esptool](https://github.com/espressif/esptool) is Espressif's own flasher.

With [uv](https://docs.astral.sh/uv/) (nothing to install permanently):

```
uv tool run esptool --chip esp32s3 --port COM7 --baud 921600 write_flash 0x0 firmware.factory.bin
```

Or with pip:

```
pip install esptool
esptool --chip esp32s3 --port COM7 --baud 921600 write_flash 0x0 firmware.factory.bin
```

Replace `COM7` with your port (`/dev/ttyACM0`, `/dev/cu.usbmodem1101`, …). On Windows
there is also [`flash.ps1`](flash.ps1) in this folder, which finds the newest COM
port, runs esptool for you and prints what to do next:

```powershell
powershell -ExecutionPolicy Bypass -File hardware\flash.ps1
```

If the flash fails part-way, redo step 2 and run it again. A half-written flash is not
a problem — the ROM bootloader is untouched.

### C. Build it yourself

The source is a PlatformIO project in [`firmware/pio/`](firmware/pio/):

```
pio run -d hardware/firmware/pio            # build
pio run -d hardware/firmware/pio -t upload  # build + flash (do step 2 first)
```

Run `pio` from PowerShell or cmd on Windows, **not** Git Bash — esp-idf's tooling
refuses to run under MSYS. Details, pinned versions and the reasoning behind the
build flags are in [firmware/flow_sidecar/FIRMWARE_NOTES.md](firmware/flow_sidecar/FIRMWARE_NOTES.md).

## 4. After flashing

Unplug the board and plug it back in **without** holding Key 1, side switch still in
the middle.

**What should happen within a second or two:**

* Windows enumerates a keyboard named **fethr sidecar** (Device Manager ▸ Keyboards,
  or Bluetooth & devices ▸ Devices). It is a plain HID keyboard + mouse — no driver.
* **Key 1 lights dim blue**, **Key 2 dim violet**. Those are the two dictation
  functions, not the layer colour.
* If you have a Chain Mono panel attached, it scrolls `FLOW` once and then shows a
  steady **F**.
* Other Chain nodes light up too: the Chain Key cyan, the joysticks dim blue, the
  Angle knob somewhere on a green-to-red scale depending on where it is turned.

**Then test it.** Start the fethr app (the tray feather), open Notepad, and:

| Do this | Expect |
|---|---|
| Hold Key 1 and talk | Key 1 breathes red, the panel shows a microphone. Let go: the LED goes back to blue, the panel shows a spinner while the server works, then a **✓** as the text appears in Notepad. |
| Hold Key 2 and talk | Same, with the mic-plus-sparkle glyph — this one runs the cleanup pass, so the spinner lasts a little longer. |
| Tap the Chain Key | The last transcript is pasted again. (There is a deliberate ~350 ms delay, so the device can tell a single tap from a double.) |
| Tap the Chain Key twice | Undo. |
| Turn the Angle knob | System volume, with a bar on the panel. |

With the app **not** running, the keys still send F8/F9/F7 like any keyboard, and the
panel shows its own ✓ when you release a key instead of waiting for a paste that is
never coming.

The app's **Sidecar** page shows the firmware version, which nodes enumerated, and
lets you change LED colours, panel brightness and timings. Turn on **Enable sidecar**
there first — with it off, fethr never opens a serial port.

## Troubleshooting

**No new COM port in download mode.** The switch must be in the middle, and Key 1 must
be held *as* the cable goes in. Try a different cable — some USB-C cables are
charge-only.

**esptool says "Wrong boot mode detected".** The board booted normally instead of into
the bootloader. Redo step 2.

**The board enumerates as a keyboard but nothing happens when you hold a key.** The
app needs to be running, and **Enable sidecar** is only for the settings channel —
the keystrokes themselves are plain HID. Check that F8 is still fethr's hotkey on the
Dictation page.

**Nothing on the Mono panel / no chained nodes.** Check that the arrow moulded into
each Chain connector points **away** from the DualKey, and that everything is on the
**right-hand** port. Plug in over serial and press `?` for a status dump listing every
node the firmware found.
