# fethr sidecar firmware 0.1.0 — prebuilt images

For the M5Stack **Chain DualKey** (ESP32-S3FN8, 8 MB flash). Flashing instructions
are in [../../FLASH.md](../../FLASH.md); the short version is "write
`firmware.factory.bin` at `0x0`".

| File | Bytes | Flash offset | What it is |
|---|---:|---|---|
| `firmware.factory.bin` | 509,856 | **`0x0`** | All four images below, merged. **Use this one.** |
| `bootloader.bin` | 19,968 | `0x0` | second-stage bootloader |
| `partitions.bin` | 3,072 | `0x8000` | partition table (`default_8MB.csv`) |
| `boot_app0.bin` | 8,192 | `0xE000` | OTA data — points the bootloader at `app0` |
| `firmware.bin` | 444,320 | `0x10000` | the application (`app0`) |

The offsets are the ones the build itself used when it merged the factory image,
and the partition table is Espressif's stock `default_8MB.csv` (`nvs` at `0x9000`,
`otadata` at `0xE000`, `app0` at `0x10000`). `0x10000 + 444,320 = 509,856`, which is
exactly the factory image's size — the merge is contiguous from offset 0.

Flashing the four separate files at their offsets is equivalent to flashing the
merged one at `0x0`; the merged image only exists so that a browser flasher needs
one file and one address.

## Verifying

```
sha256sum -c SHA256SUMS.txt          # Linux/macOS, or Git Bash on Windows
```

```powershell
Get-FileHash firmware.factory.bin -Algorithm SHA256    # Windows PowerShell
```

## Built from

`hardware/firmware/pio/` at version `0.1.0`, with **`FLOW_EXTRA_LAYERS 1`** — all four
layers (FLOW, MEDIA, EDIT, MOUSE) and the companion-display link (`FLOW_COMPANION 1`).

Two things changed with this rebuild and both are visible from the outside:

* **Four layers, so the Chain Key's long hold now cycles them.** A ≥1 s hold goes
  FLOW → MEDIA → EDIT → MOUSE instead of doing nothing. The earlier image built the FLOW
  layer alone. MEDIA/EDIT/MOUSE compile and are complete but have had far less time on
  hardware than FLOW; set `FLOW_EXTRA_LAYERS` back to `0` in `config.h` for the old
  single-layer behaviour.
* **The companion link speaks the key legend.** The image carries the `legend` field on
  `hello`/`layer` plus the `tap` and `knob` events the
  [companion display](../../COMPANION.md) animates from. The previous image predates all
  of that and would leave a companion showing "no legend yet".

```
RAM:   18.3% (59,912 of 327,680 bytes)
Flash: 13.0% (433,786 of 3,342,336 bytes)
```

| Component | Version |
|---|---|
| PlatformIO Core | 6.2.0 |
| platform `espressif32` (pioarduino) | 55.3.311 |
| arduino-esp32 core | 3.3.11 |
| `M5Chain` | 1.0.10 |
| `Adafruit NeoPixel` | 1.15.5 |
| `ArduinoJson` | 7.4.3 |

Reproduce with `pio run -d hardware/firmware/pio`; the image lands in
`.pio/build/chain_dualkey/`. Note that the build is not bit-for-bit reproducible —
the ELF carries a build timestamp — so a rebuild will not match these checksums.
They are here to check the download, not the toolchain.

## Erasing

Flashing does not touch `nvs`, so device settings (LED colours, Mono brightness,
axis signs) survive an upgrade. To start clean, either send `{"cmd":"reset_config"}`
over the serial port, use **Reset to defaults** in the app's Sidecar page, or erase
the whole chip before flashing:

```
uv tool run esptool --chip esp32s3 --port COMx erase_flash
```
