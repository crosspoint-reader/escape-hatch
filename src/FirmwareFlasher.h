#pragma once

#include <cstddef>
#include <cstdint>

// Flash a firmware image from an SD-card path into the next OTA app partition,
// then switch otadata so the X3/X4 stock bootloader picks it up on next boot.
// Mirrors the web flasher: raw esp_partition_erase_range + esp_partition_write
// + ota_boot::switchTo (no Arduino Update class, no esp_image_verify — those
// reject our patched image on X4 silicon).
//
// Ported from crosspoint-reader (src/network/FirmwareFlasher.*); the only change
// is the file I/O seam — it reads through the FreeInk SDCardManager (FsFile)
// instead of CrossPoint's HalStorage wrapper.

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,  // segment table malformed or runs past EOF
  BAD_CHECKSUM,  // ESP image XOR checksum mismatch
  BAD_SHA,       // SHA256 trailer mismatch (hash_appended images)
  BAD_CHIP,      // image chip_id doesn't match the running MCU family
  WRONG_BOARD,   // image carries a board tag naming a different board
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Open `sdPath`, validate it looks like an ESP32 image, then stream it into the
// next OTA app partition with interleaved 64 KiB erase + sector writes. On
// success switches otadata via ota_boot::switchTo. Caller is responsible for
// ESP.restart() afterwards.
//
// `alreadyValidated` lets callers that have just run `validateImageFile()`
// themselves skip the redundant second pass. Defaults to false so callers
// without prior validation keep the defense-in-depth check.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated = false);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, segment table walk, XOR checksum, and SHA256 trailer (when
// hash_appended == 1). Also rejects an image built for a different MCU family
// (chip_id vs the running slot's) and scans for the embedded board tag (see
// FirmwareBoardTag.h), rejecting an image tagged for a different board — the
// S3 boards share a chip_id, so the tag is what tells an x4pro image from a
// sticky or papermono one. Run this before flashing a candidate firmware so a
// truncated/corrupted/wrong-device .bin never reaches otadata.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to skip
// the size-fits-partition check.
Result validateImageFile(const char* sdPath, size_t partitionSize);

const char* resultName(Result r);

// Returns the chip_id (esp_image_header_t offset 12) of the currently-running
// image, or 0xFFFF if it cannot be read. Because the running slot booted
// successfully, its chip_id is authoritative for the current CPU, so a
// candidate image must match it to be safe to flash.
uint16_t runningPartitionChipId();

}  // namespace firmware_flash
