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
2. Boots to a small **menu** — **Flash Firmware** and **Button Test**.
3. **Flash Firmware** mounts the SD card and shows a **file browser** of folders
   and `.bin` files (built from FreeInkUI's `list`), then **flashes** the selected
   image straight into the inactive OTA partition with interleaved erase/write,
   switches `otadata`, and reboots into it.
4. **Button Test** is a hardware diagnostic: press any button to see its name and
   the live ADC value of the resistor-ladder GPIO it drives. Double-tap **Back**
   to return to the menu.
5. **Boot Other Slot** points the bootloader at the *other* OTA app partition and
   reboots into it (e.g. back into your main firmware) — without reflashing. It
   first checks that slot actually starts with a valid app image (`0xE9` magic)
   and refuses if it's empty, so it can't strand you on a blank partition.
6. **EFuse / Security** reads (never burns) the chip's security efuses — Secure
   Boot, flash encryption, serial-download / JTAG disables, chip revision — and
   gives a one-line verdict on whether a custom-bootloader recovery is possible
   and recoverable on this device. See
   [`docs/recovery-bootloader-feasibility.md`](docs/recovery-bootloader-feasibility.md).

The button hints along the bottom of every screen are drawn as icons (the
Lucide-derived set shared with the inkdeck firmware) rather than text labels.

A short **boot splash** seeds the panel on power-up. **Hold the power button**
(~1.5 s) from any screen to show a **sleep screen** and enter deep sleep; pressing
the power button again wakes the device (it resets into a fresh boot). The wake
source is configured per-SoC by the SDK's `PowerManager`.

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
| **OK**   | Select a menu item / enter a folder / pick a `.bin`|
| **Back** | Go up a folder; at the root, return to the menu    |

On the confirm screen, **OK** flashes and reboots; **Back** returns to the list.
In **Button Test**, every press shows that button's name and ADC reading;
double-tap **Back** to exit to the menu.

**Hold Power** (~1.5 s) on any screen to deep-sleep; tap **Power** again to wake.

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
firmware into the other. Escape Hatch is uploaded to **`ota_0`** (`offset_address
= 0x10000`); a flashed firmware lands in **`ota_1`**.

## Recovery combo (boot back into Escape Hatch)

The SDK lib **`RecoveryBoot`** provides `freeink::recovery::checkBootCombo()`:
held at reset, the combo **Back + Up** repoints `otadata` at `ota_0` and reboots
into Escape Hatch. It's called as the first line of `setup()` here.

The stock second-stage bootloader can't read buttons — only the firmware that
boots can — so this check has to run inside *each* firmware, as the first line of
`setup()`. Calling it in Escape Hatch itself is a no-op (it's already `ota_0`);
the payoff is calling the **same function from your main firmware(s)** (it lives
in the shared SDK so any firmware can `#include <RecoveryBoot.h>`), so a held
combo there bounces you back here. It's safe to call unconditionally: it does
nothing unless the combo is held, `ota_0` holds a valid app, and you're not
already running from `ota_0`.

Why **Back + Up** specifically: the buttons are an ADC resistor ladder with
Back/Confirm/Left/Right on GPIO1 and Up/Down on GPIO2. Two buttons on the *same*
pin (e.g. Back+Right) collapse to one reading and can't be told apart, so a
detectable combo must take one button from each pin.

Two limits worth knowing:

- A firmware that crashes in ROM / early SDK init *before* reaching the check
  can't be escaped this way. A corrupt app *image*, though, is caught for free:
  the bootloader falls back to the other OTA slot on its own.
- Truly unconditional GPIO recovery (independent of the running app) would need a
  custom second-stage bootloader — which Escape Hatch deliberately never
  reflashes, since a bad bootloader is the one thing it couldn't recover from.
