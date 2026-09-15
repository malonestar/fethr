# fethr companion display firmware 0.1.0 — prebuilt images

For the M5Stack **AtomS3R** (ESP32-S3-PICO-1-N8R8, 8 MB flash + 8 MB octal PSRAM) on an
**Atomic ToChain Base**. What it is and how it is wired: [../../COMPANION.md](../../COMPANION.md).
The short version of flashing is "write `firmware.factory.bin` at `0x0`".

**This is not the DualKey firmware.** The sidecar's own images are in
[`../fethr-sidecar-0.1.0/`](../fethr-sidecar-0.1.0/) and the two are not
interchangeable — different board, different flash layout, different USB mode.

| File | Bytes | Flash offset | What it is |
|---|---:|---|---|
| `firmware.factory.bin` | 672,864 | **`0x0`** | All four images below, merged. **Use this one.** |
| `bootloader.bin` | 19,968 | `0x0` | second-stage bootloader |
| `partitions.bin` | 3,072 | `0x8000` | partition table (`default_8MB.csv`) |
| `boot_app0.bin` | 8,192 | `0xE000` | OTA data — points the bootloader at `app0` |
| `firmware.bin` | 607,328 | `0x10000` | the application (`app0`) |

The offsets are the ones the build itself used when it merged the factory image, and the
partition table is Espressif's stock `default_8MB.csv`. `0x10000 + 607,328 = 672,864`,
which is exactly the factory image's size — the merge is contiguous from offset 0.

`bootloader.bin` is byte-identical to the DualKey's. That is expected rather than a
mix-up: on the ESP32-S3 the second-stage bootloader is selected by *flash* mode (`qio`
at 80 MHz for both boards), and PSRAM is brought up later by the application, so the
AtomS3R's octal PSRAM does not change it. What the PSRAM setting *does* change is which
static libraries get linked — this build pulls `esp32s3/qio_opi/libesp_psram.a` where the
DualKey pulls `qio_qspi/`.

## Getting into download mode

The AtomS3R has a real reset button (the small side button, **not** the screen):

> Press and hold the reset button for about **2 seconds**, until the internal **green LED
> lights up**, then release.
>
> — [M5Stack AtomS3R documentation](https://docs.m5stack.com/en/core/AtomS3R), *Download Mode*

Then flash as in [../../FLASH.md](../../FLASH.md) §3 — Espressif's browser flasher, or:

```
uv tool run esptool --chip esp32s3 --port COM8 --baud 921600 write_flash 0x0 firmware.factory.bin
```

## Verifying

```
sha256sum -c SHA256SUMS.txt          # Linux/macOS, or Git Bash on Windows
```

```powershell
Get-FileHash firmware.factory.bin -Algorithm SHA256    # Windows PowerShell
```

## Built from

`hardware/firmware/companion-atoms3r/` at version `0.1.0`, rebuilt for the **key-legend
screen** — the per-layer legend, the recording takeover, the tap/layer/knob animations
(described in [../../COMPANION.md](../../COMPANION.md)). The version string does not move:
the project's rule is that it changes when an image is cut for a release, and this is
still the companion's first one. Flash it together with a DualKey image built from the
current `hardware/firmware/pio/` — an older sidecar sends no `legend` field and this
screen would sit on "no legend yet".

```
RAM:    8.8% (28,916 of 327,680 bytes)
Flash: 17.5% (584,791 of 3,342,336 bytes)
```

The 32 KB display sprite is allocated at run time and is not in the static RAM figure.

| Component | Version |
|---|---|
| PlatformIO Core | 6.2.0 |
| platform `espressif32` (pioarduino) | 55.3.311 |
| arduino-esp32 core | 3.3.11 |
| `M5Unified` | 0.2.22 |
| `M5GFX` (pulled in by M5Unified) | bundled with the above |
| `ArduinoJson` | 7.4.3 |

Reproduce with `pio run -d hardware/firmware/companion-atoms3r`; the image lands in
`.pio/build/companion_atoms3r/`. The build is not bit-for-bit reproducible — the ELF
carries a build timestamp — so a rebuild will not match these checksums. They are here
to check the download, not the toolchain.

## Erasing

This firmware stores nothing: there is no NVS blob and no settings. Everything it shows
comes from the sidecar over the link, so re-flashing is the only state it has. To wipe
the chip anyway:

```
uv tool run esptool --chip esp32s3 --port COMx erase_flash
```
