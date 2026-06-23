# Escape Hatch

The most minimal possible firmware for the Xteink **X3 / X4** (ESP32-C3): a tiny
SD-card **firmware flasher**, nothing else. It boots, lets you browse the SD card
for a `.bin`, and reflashes the device with it.

It exists as a recovery / escape hatch — if a main firmware build is broken,
flash this once and you can always reflash from the SD card with three buttons.

## What it does

1. **Auto-detects X3 vs X4** at boot via an I²C fingerprint of the X3-only
   peripherals (BQ27220 gauge / DS3231 RTC / QMI8658 IMU on SDA20/SCL0) and
   selects the matching panel driver — one binary drives both.
2. **Mounts the SD card** and shows a **file browser** of folders and `.bin`
   files (built from FreeInkUI's `list`).
3. **Flashes** the selected image straight into the inactive OTA partition with
   interleaved erase/write, then switches `otadata` and reboots into it.

The flash path is the exact SD-flash process from
[`crosspoint-reader`](../crosspoint-reader-main) — `FirmwareFlasher` (full ESP32
image validation: magic, segment table, XOR checksum, SHA-256 trailer; raw
`esp_partition_erase_range`/`write`) plus `OtaBootSwitch` (raw `otadata` slot
write that bypasses `esp_image_verify`, which rejects the patched X4 image).
Those two files are ported verbatim; only the file-read seam was repointed from
CrossPoint's `HalStorage` to the FreeInk `SDCardManager` (`FsFile`).

## Controls

| Button   | Action                                             |
| -------- | -------------------------------------------------- |
| **Up**   | Move selection up                                  |
| **Down** | Move selection down                                |
| **OK**   | Enter a folder, or pick a `.bin` → flash-confirm   |
| **Back** | Go up a folder / cancel the flash confirm          |

On the confirm screen, **OK** flashes and reboots; **Back** returns to the list.

## Building

The [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) is vendored as a git
submodule at `freeink-sdk/`, and the committed `platformio.ini` builds against it
via relative `symlink://freeink-sdk/libs/...` paths. Clone with the submodule:

```sh
git clone --recurse-submodules <repo-url>
# already cloned without it?
git submodule update --init freeink-sdk
```

Then:

```sh
pio run                 # build (env: default, X3+X4 dual binary)
pio run -t upload       # flash over USB
pio device monitor      # 115200 baud logs
```

### Building against a local SDK working copy

To iterate on the SDK without bumping the submodule, create a
`platformio.local.ini` (gitignored, machine-specific — same pattern as
`crosspoint-reader`) that overrides `[base].lib_deps` with absolute
`symlink:///abs/path/to/freeink-sdk/libs/...` paths. PlatformIO loads it
automatically via `extra_configs`, so the local copy takes precedence over the
submodule. Once changes are committed and pushed to the SDK remote, bump the
pinned version here:

```sh
git -C freeink-sdk pull          # advance the submodule to the new SDK commit
git add freeink-sdk && git commit -m "Bump freeink-sdk submodule"
```

### SDK addition

This project adds one small library to the SDK,
`libs/hardware/XteinkDetect`, providing `freeink::detectXteinkIsX3()` /
`freeink::selectXteinkDevice()` — the canonical Xteink X3/X4 runtime fingerprint
the SDK previously left to each consumer. Any dual X3/X4 app can reuse it.

## Layout

`partitions.csv` matches the device's stock 16 MB dual-OTA layout (`otadata` +
`ota_0` + `ota_1`) so the factory bootloader and `otadata` pick up whatever this
tool writes, and so Escape Hatch can live in one OTA slot while it flashes a full
firmware into the other.
