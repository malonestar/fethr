# fethr sidecar firmware 0.2.0 — prebuilt images

For the M5Stack **Chain DualKey** (ESP32-S3FN8, 8 MB flash). Flashing instructions
are in [../../FLASH.md](../../FLASH.md); the short version is "write
`firmware.factory.bin` at `0x0`".

| File | Bytes | Flash offset | What it is |
|---|---:|---|---|
| `firmware.factory.bin` | 515,808 | **`0x0`** | All four images below, merged. **Use this one.** |
| `bootloader.bin` | 19,968 | `0x0` | second-stage bootloader |
| `partitions.bin` | 3,072 | `0x8000` | partition table (`default_8MB.csv`) |
| `boot_app0.bin` | 8,192 | `0xE000` | OTA data — points the bootloader at `app0` |
| `firmware.bin` | 450,272 | `0x10000` | the application (`app0`) |

The offsets are the ones the build itself used when it merged the factory image,
and the partition table is Espressif's stock `default_8MB.csv` (`nvs` at `0x9000`,
`otadata` at `0xE000`, `app0` at `0x10000`). `0x10000 + 450,272 = 515,808`, which is
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

## What is new since 0.1.0

**The layout is a setting now.** Up to 0.1.0 the keymap was compile-time data in
`layers.cpp`: changing what Key2 did meant a reflash. 0.2.0 moves the whole layer
table into the persisted runtime config, so the app can rebind any key, rename or
recolour a layer, and change what the stick and the knob do — live, over the same
serial link the colour settings already used. `hello` reports `"proto":2`.

* **`get_layers` / `set_action` / `set_layer_meta`** — the whole table, one slot,
  and one layer's metadata. See [../../PROTOCOL.md](../../PROTOCOL.md).
* **`swap_keys`** — which physical button is Key 1, and which LED goes with it.
  This was the `KEYS_SWAPPED` compile flag; it is a live setting and the compile
  flag is gone.
* **`nav_swap_xy` / `scroll_swap_xy`** — swap a stick's axes. With the four
  existing sign settings that covers all four 90° mountings without a reflash.
* **`layout_changed` event** — emitted after any layout edit so a second client
  showing the keymap knows to re-read it.

**Your saved settings are discarded on first boot.** The NVS blob version went
1 → 2 because the layer table is now part of it, and a v1 blob has no keymap to
carry forward. The device comes up on factory defaults — four layers, the stock
bindings, default colours — and the app can push your preferences back and `save`.
Nothing is lost that the app does not already hold.

```
RAM:   19.0% (62,184 of 327,680 bytes)      (0.1.0: 59,912)
Flash: 13.2% (439,666 of 3,342,336 bytes)   (0.1.0: 433,786)
```

Most of the +2,272 B of RAM is the larger outbound protocol buffer (1 KB → 3 KB),
which `get_layers` needs: the whole table serialises to ~2.2 KB worst case. The
runtime layer table itself is ~200 B.

## Built from

`hardware/firmware/pio/` at version `0.2.0`, with **`FLOW_EXTRA_LAYERS 4`** — a
factory-default device seeds all four layers (FLOW, MEDIA, EDIT, MOUSE) — and the
companion-display link (`FLOW_COMPANION 1`).

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

Flashing does not touch `nvs`, but see the blob-version note above: a 0.1.0 config
is rejected by this build anyway. To start completely clean, either send
`{"cmd":"reset_config"}` over the serial port, use **Reset to defaults** in the
app's Sidecar page, or erase the whole chip before flashing:

```
uv tool run esptool --chip esp32s3 --port COMx erase_flash
```
