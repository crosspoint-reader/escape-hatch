// Escape Hatch — the most minimal X3/X4 firmware that can reflash the device.
//
// Boots, auto-detects X3 vs X4, mounts the SD card, and shows a file browser of
// .bin firmware images. Up/Down move the selection, Confirm picks an entry
// (entering folders or, for a .bin, opening a flash-confirm screen). Confirming
// the flash streams the image straight into the inactive OTA partition and
// switches otadata, then reboots into it — the exact SD-flash path from
// crosspoint-reader (FirmwareFlasher + OtaBootSwitch), nothing else.

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkApp.h>
#include <FreeInkUI.h>
#include <FreeInkUIDisplayTarget.h>
#include <InputManager.h>
#include <PowerManager.h>
#include <SDCardManager.h>
#include <SPI.h>
#include <XteinkDetect.h>

#include <esp_chip_info.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <string>
#include <vector>

#include <RecoveryBoot.h>

#include "ButtonHintIcons.h"
#include "FirmwareFlasher.h"
#include "OtaBootSwitch.h"

namespace ui = freeink::ui;

// ---------------------------------------------------------------------------
// Hardware. Display pins are shared by X3 and X4 (BoardConfig XTEINK_X4 == X3).
// ---------------------------------------------------------------------------
EInkDisplay display(BoardConfig::XTEINK_X4.display.sclk, BoardConfig::XTEINK_X4.display.mosi,
                    BoardConfig::XTEINK_X4.display.cs, BoardConfig::XTEINK_X4.display.dc,
                    BoardConfig::XTEINK_X4.display.rst, BoardConfig::XTEINK_X4.display.busy);
InputManager input;

ui::DisplayTarget* g_target = nullptr;
ui::ThemeTokens g_theme;
int16_t g_w = 0, g_h = 0;
int g_visible = 8;
constexpr int16_t kRowHeight = 46;
bool g_firstPaint = true;

// The boot paint must be a clean FULL refresh to seed the panel's differential
// base; everything after is a fast partial refresh. Without this the first
// interactive refresh on the X4 (whose SSD1677 config has no full-sequence
// override) misbehaves and the UI appears to lag a click behind.
void pushDisplay() {
  display.displayBuffer(g_firstPaint ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
  g_firstPaint = false;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum class State {
  Menu,
  Browsing,
  Confirm,
  BootConfirm,
  ButtonTest,
  SdCardTest,
  BatteryInfo,
  EfuseInfo,
  Failed
};
State g_state = State::Menu;

// Top-level menu entries (home screen).
const char* kMenuItems[] = {"Flash Firmware", "Button Test",  "Boot Other Slot",
                            "SD Card Test",   "Battery Info", "EFuse / Security"};
constexpr int kMenuCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
int g_menuSel = 0;

// The inactive OTA app partition we're about to switch the bootloader to,
// captured when the user picks "Boot Other Slot" (only set once validated to
// hold a real app image).
const esp_partition_t* g_bootDest = nullptr;

// Button-test screen: timestamp of the last Back press, to detect a double-tap
// (the test shows every button's reading, so a single Back is a reading too —
// only a quick second tap exits back to the menu).
unsigned long g_lastBackPress = 0;

// Power button: hold it for kPowerSleepHoldMs to deep-sleep. Armed only while the
// button is released, so the wake press that boots us can't immediately re-trigger
// sleep, and g_allowSleepAt blanks the threshold for a moment after boot.
constexpr uint32_t kPowerSleepHoldMs = 1500;
uint32_t g_allowSleepAt = 0;
bool g_powerSleepArmed = true;

std::string g_path = "/";          // current directory
std::vector<std::string> g_names;  // entry display names (folders end in '/')
std::vector<bool> g_isDir;
std::vector<ui::ListItem> g_items;  // labels borrow g_names storage
int g_sel = 0;
int g_top = 0;

std::string g_flashPath;  // full path of the .bin awaiting/within a flash
std::string g_error;
int g_lastPct = -1;

// ---------------------------------------------------------------------------
// OTA partitions
// ---------------------------------------------------------------------------
// The OTA app slot that ISN'T the one we're running from — the "other" firmware.
const esp_partition_t* inactiveAppPartition() { return esp_ota_get_next_update_partition(nullptr); }

// True if `p` begins with a plausible ESP32 app image. We only check the image
// magic (0xE9) — deliberately NOT esp_image_verify(), which on these devices
// rejects the patched X4 image with bogus efuse errors (the very reason the
// flasher bypasses it). 0xE9 also excludes an erased (0xFF) or empty slot, which
// is all we need to avoid pointing the bootloader at a partition with no app.
bool partitionHasApp(const esp_partition_t* p) {
  if (!p) return false;
  uint8_t magic = 0;
  if (esp_partition_read(p, 0, &magic, sizeof(magic)) != ESP_OK) return false;
  return magic == 0xE9;  // ESP_IMAGE_HEADER_MAGIC
}

// ---------------------------------------------------------------------------
// SD enumeration
// ---------------------------------------------------------------------------
bool hasBinExtension(const char* name) {
  const size_t n = strlen(name);
  return n > 4 && strcasecmp(name + n - 4, ".bin") == 0;
}

std::string joinPath(const std::string& base, const std::string& leaf) {
  if (base == "/") return "/" + leaf;
  return base + "/" + leaf;
}

void loadEntries() {
  struct Entry {
    std::string name;
    bool dir;
  };
  std::vector<Entry> entries;

  FsFile dir = SdMan.open(g_path.c_str());
  if (dir && dir.isDirectory()) {
    char name[128];
    for (FsFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
      const bool isdir = f.isDirectory();
      f.getName(name, sizeof(name));
      f.close();
      if (name[0] == '.') continue;  // hidden / system
      if (isdir) {
        entries.push_back({std::string(name) + "/", true});
      } else if (hasBinExtension(name)) {
        entries.push_back({name, false});
      }
    }
    dir.close();
  }

  // Folders first, then files; alphabetical within each group (case-insensitive).
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.dir != b.dir) return a.dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });

  g_names.clear();
  g_isDir.clear();
  for (auto& e : entries) {
    g_names.push_back(e.name);
    g_isDir.push_back(e.dir);
  }
  g_items.clear();
  for (size_t i = 0; i < g_names.size(); ++i) {
    ui::ListItem it{};
    it.label = g_names[i].c_str();
    it.actionValue = static_cast<int16_t>(i);
    g_items.push_back(it);
  }
  g_sel = 0;
  g_top = 0;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Draw the four hardware button hints as icons (replacing the old text labels).
// Reserves the footer band from the screen layout first — same as screen.footer()
// — so the body above never overlaps it, then centres an icon in each of the four
// slots, in hardware order: [Back][Confirm] (left bezel) [Up][Down] (right bezel).
// A blank slot (btnhint::none()) draws nothing.
template <size_t N>
void iconFooter(ui::Screen<N>& screen, ui::BitmapRef i0, ui::BitmapRef i1, ui::BitmapRef i2, ui::BitmapRef i3) {
  const ui::Rect rect = screen.takeBottom(g_theme.footerHeight);
  if (rect.empty()) return;
  ui::DrawTarget& t = screen.target();

  // Divider line along the top edge of the footer band.
  t.fill(ui::Rect{rect.x, rect.y, rect.width, 1}, ui::Paint::solid(ui::Color::Black));

  const int16_t sidePadding = 8;
  const int16_t gap = 4;
  const int16_t contentW = static_cast<int16_t>(rect.width - sidePadding * 2);
  const int16_t slotW = static_cast<int16_t>((contentW - gap * 3) / 4);
  const ui::BitmapRef icons[4] = {i0, i1, i2, i3};

  int16_t x = static_cast<int16_t>(rect.x + sidePadding);
  for (int i = 0; i < 4; ++i) {
    const int16_t w = i == 3 ? static_cast<int16_t>(rect.right() - sidePadding - x) : slotW;
    if (icons[i]) {
      t.bitmap(ui::Rect{x, rect.y, w, rect.height}, icons[i], ui::BitmapMode::Center,
               ui::Paint::solid(ui::Color::Black));
    }
    x = static_cast<int16_t>(x + w + gap);
  }
}

// A full-screen, chrome-less splash: a centered title with a subtitle beneath.
// Used for the boot and sleep screens (no header/footer, like inkdeck's).
void renderSplash(const char* subtitle, EInkDisplay::RefreshMode mode) {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<2> ib;
  ui::InputSnapshot empty;
  ui::Frame<2> frame(*g_target, dev, empty, ib);
  ui::Screen<2> screen(frame, g_theme);

  screen.setContentMargin(ui::Insets{24, 24, 24, 24});
  const ui::Rect body = screen.body();

  ui::TextStyle title = g_theme.titleText;
  title.align = ui::TextAlign::Center;
  title.maxLines = 1;
  ui::TextStyle sub = g_theme.bodyText;
  sub.align = ui::TextAlign::Center;
  sub.maxLines = 2;

  const ui::Rect titleRect{body.x, static_cast<int16_t>(body.y + body.height / 2 - 48), body.width, 32};
  const ui::Rect subRect{body.x, static_cast<int16_t>(titleRect.bottom() + 10), body.width, 56};
  frame.target().text(titleRect, "Escape Hatch", title);
  frame.target().text(subRect, subtitle, sub);

  frame.finish();
  display.displayBuffer(mode);
}

// Render the sleep splash, park the panel, and deep-sleep until the power button
// is pressed again (which resets the chip → a fresh boot). Does not return.
[[noreturn]] void enterDeepSleep() {
  renderSplash("Sleeping\nHold power to wake", EInkDisplay::FULL_REFRESH);
  display.deepSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
  for (;;) {
  }  // unreachable; deepSleepUntilPowerButton never returns
}

void renderMenu() {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<8> ib;
  ui::InputSnapshot empty;
  ui::Frame<8> frame(*g_target, dev, empty, ib);
  ui::Screen<8> screen(frame, g_theme);

  screen.header("Escape Hatch");
  // No Back at the menu root; Confirm selects, Up/Down move.
  iconFooter(screen, btnhint::none(), btnhint::confirm(), btnhint::up(), btnhint::down());

  ui::ListItem items[kMenuCount];
  for (int i = 0; i < kMenuCount; ++i) {
    items[i] = ui::ListItem{};
    items[i].label = kMenuItems[i];
    items[i].actionValue = static_cast<int16_t>(i);
  }
  ui::ListProps props{};
  props.items = items;
  props.count = static_cast<uint16_t>(kMenuCount);
  props.selectedIndex = g_menuSel;
  props.topIndex = 0;
  props.rowHeight = kRowHeight;
  props.selectionMarker = ui::SelectionMarker::Triangle;
  screen.list(props);

  frame.finish();
  pushDisplay();
}

void renderButtonTest(const char* btnName, int adc) {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<4> ib;
  ui::InputSnapshot empty;
  ui::Frame<4> frame(*g_target, dev, empty, ib);
  ui::Screen<4> screen(frame, g_theme);

  screen.header("Button Test");
  // Only Back is meaningful here (double-tap to exit); the other slots stay blank
  // because every button press is consumed as a reading, not navigation.
  iconFooter(screen, btnhint::back(), btnhint::none(), btnhint::none(), btnhint::none());

  std::string body;
  if (btnName) {
    body = std::string(btnName) + "\n\nADC: " + std::to_string(adc);
  } else {
    body = "Press any button to\nshow its name and ADC.";
  }
  body += "\n\n(double-tap Back to exit)";

  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 8;
  frame.target().text(screen.body(), body.c_str(), ts);

  frame.finish();
  pushDisplay();
}

void renderList() {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<32> ib;
  ui::InputSnapshot empty;
  ui::Frame<32> frame(*g_target, dev, empty, ib);
  ui::Screen<32> screen(frame, g_theme);

  std::string title = "Escape Hatch   " + g_path;
  screen.header(title.c_str());

  // Icon hints in hardware order: left-side buttons (Back, Confirm) on the left,
  // right-side buttons (Up, Down) on the right.
  iconFooter(screen, btnhint::back(), btnhint::confirm(), btnhint::up(), btnhint::down());

  if (g_items.empty()) {
    screen.popup("No .bin files in this folder");
  } else {
    ui::ListProps props{};
    props.items = g_items.data();
    props.count = static_cast<uint16_t>(g_items.size());
    props.selectedIndex = g_sel;
    props.topIndex = static_cast<uint16_t>(g_top);
    props.rowHeight = kRowHeight;
    props.selectionMarker = ui::SelectionMarker::Triangle;
    screen.list(props);
  }

  frame.finish();
  pushDisplay();
}

void renderMessage(const char* title, const char* body, const char* hint) {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<4> ib;
  ui::InputSnapshot empty;
  ui::Frame<4> frame(*g_target, dev, empty, ib);
  ui::Screen<4> screen(frame, g_theme);

  screen.header(title);
  if (hint) {
    const ui::FooterAction footer[] = {{.label = hint, .action = ui::NO_ACTION}};
    screen.footer(footer, 1);
  }
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 4;
  frame.target().text(screen.body(), body, ts);

  frame.finish();
  pushDisplay();
}

void renderConfirm() {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<4> ib;
  ui::InputSnapshot empty;
  ui::Frame<4> frame(*g_target, dev, empty, ib);
  ui::Screen<4> screen(frame, g_theme);

  screen.header("Flash firmware?");
  // Four slots matching the browser footer ([Back][Confirm][Up][Down]) so the
  // Cancel/Confirm hints sit under the SAME physical buttons (left side) instead
  // of spreading across the bar — the right two slots (Up/Down) are unused here.
  iconFooter(screen, btnhint::cancel(), btnhint::confirm(), btnhint::none(), btnhint::none());

  std::string body = "Flash this image and reboot into it?\n\n" + g_names[g_sel];
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 6;
  frame.target().text(screen.body(), body.c_str(), ts);

  frame.finish();
  pushDisplay();
}

void renderBootConfirm() {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<4> ib;
  ui::InputSnapshot empty;
  ui::Frame<4> frame(*g_target, dev, empty, ib);
  ui::Screen<4> screen(frame, g_theme);

  screen.header("Boot other slot?");
  iconFooter(screen, btnhint::cancel(), btnhint::confirm(), btnhint::none(), btnhint::none());

  std::string body = "Switch the bootloader to '";
  body += g_bootDest ? g_bootDest->label : "?";
  body += "' and reboot into it?\n\nReturning needs that firmware (or a reflash).";
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 8;
  frame.target().text(screen.body(), body.c_str(), ts);

  frame.finish();
  pushDisplay();
}

// Read-only dump of the chip's security efuses — the facts that decide whether a
// custom-bootloader recovery (rewriting flash 0x0) is even possible, and whether
// a bad bootloader write could be undone. All reads; nothing is burned.
void renderEfuseInfo() {
  const bool secureBoot = esp_efuse_read_field_bit(ESP_EFUSE_SECURE_BOOT_EN);
  uint8_t cryptCnt = 0;  // SPI_BOOT_CRYPT_CNT: flash encryption on when popcount is odd
  esp_efuse_read_field_blob(ESP_EFUSE_SPI_BOOT_CRYPT_CNT, &cryptCnt, 3);
  const bool flashEnc = (__builtin_popcount(cryptCnt) & 1) != 0;
  // These bits mean "interface disabled" when set, so a clear bit = still usable.
  const bool dlDisabled = esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE);
  const bool usbJtagDisabled = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG);
  const bool padJtagDisabled = esp_efuse_read_field_bit(ESP_EFUSE_DIS_PAD_JTAG);

  esp_chip_info_t chip{};
  esp_chip_info(&chip);

  auto yn = [](bool on) { return on ? "ON" : "off"; };
  auto en = [](bool disabled) { return disabled ? "disabled" : "enabled"; };

  std::string body;
  body += "Secure Boot: " + std::string(yn(secureBoot)) + "\n";
  body += "Flash Encryption: " + std::string(yn(flashEnc)) + "\n";
  body += "Serial download: " + std::string(en(dlDisabled)) + "\n";
  body += "USB-JTAG: " + std::string(en(usbJtagDisabled)) + "\n";
  body += "Pad JTAG: " + std::string(en(padJtagDisabled)) + "\n";
  body += "Chip rev: v" + std::to_string(chip.revision / 100) + "." + std::to_string(chip.revision % 100) + "\n\n";

  // One-line verdict for the bootloader-recovery question.
  if (secureBoot || flashEnc) {
    body += "Bootloader rewrite: BLOCKED (would brick).";
  } else if (dlDisabled) {
    body += "Bootloader writable, but NO serial recovery: a bad write is unrecoverable.";
  } else {
    body += "Bootloader writable; serial download still available as a recovery path.";
  }

  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<4> ib;
  ui::InputSnapshot empty;
  ui::Frame<4> frame(*g_target, dev, empty, ib);
  ui::Screen<4> screen(frame, g_theme);

  screen.header("EFuse / Security");
  iconFooter(screen, btnhint::back(), btnhint::none(), btnhint::none(), btnhint::none());

  screen.insetContent(ui::Insets{8, 16, 8, 16});
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Left;
  ts.maxLines = 12;
  frame.target().text(screen.body(), body.c_str(), ts);

  frame.finish();
  pushDisplay();
}

// Exercises the SD card end to end: re-mount the card (hardware/SPI access),
// write a small file, read it back and verify the bytes match, then delete it.
// Each step is reported with OK/FAIL so a user can see exactly where their card
// is failing. Stops at the first failure since later steps depend on earlier ones.
void renderSdCardTest() {
  const char* kTestPath = "/escape_hatch_sdtest.txt";
  // Vary the payload per run so a stale leftover file can't masquerade as a pass.
  const String expected = "escape-hatch SD test " + String(millis());

  std::string body;
  bool ok = true;

  // 1. Hardware / mount: re-run begin() so this reflects the card that's in the
  // slot right now, not just the boot-time state.
  if (SdMan.begin()) {
    body += "Mount card: OK\n";
  } else {
    body += "Mount card: FAIL\n";
    body += "\nCard not detected. Check it is\ninserted and formatted (FAT32).";
    ok = false;
  }

  // 2. Write.
  if (ok) {
    if (SdMan.writeFile(kTestPath, expected)) {
      body += "Write file: OK\n";
    } else {
      body += "Write file: FAIL\n";
      body += "\nCard may be write-protected or full.";
      ok = false;
    }
  }

  // 3. Read back and verify the contents round-tripped.
  if (ok) {
    const String got = SdMan.readFile(kTestPath);
    if (got == expected) {
      body += "Read back: OK\n";
    } else if (got.length() == 0) {
      body += "Read back: FAIL (empty)\n";
      ok = false;
    } else {
      body += "Read back: FAIL (mismatch)\n";
      ok = false;
    }
  }

  // 4. Cleanup: remove the test file (best-effort; report but don't fail the run).
  if (SdMan.exists(kTestPath)) {
    body += SdMan.remove(kTestPath) ? "Cleanup: OK\n" : "Cleanup: FAIL\n";
  }

  body += ok ? "\nSD card is working." : "\nSD card test FAILED.";

  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<4> ib;
  ui::InputSnapshot empty;
  ui::Frame<4> frame(*g_target, dev, empty, ib);
  ui::Screen<4> screen(frame, g_theme);

  screen.header("SD Card Test");
  iconFooter(screen, btnhint::back(), btnhint::none(), btnhint::none(), btnhint::none());

  screen.insetContent(ui::Insets{8, 16, 8, 16});
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Left;
  ts.maxLines = 12;
  frame.target().text(screen.body(), body.c_str(), ts);

  frame.finish();
  pushDisplay();
}

// Live battery telemetry from the SDK's BatteryMonitor. On X4 this is an ADC read
// of the divided LiPo rail (voltage + a polynomial %); on X3 it's the BQ27220 I2C
// fuel gauge (true SoC + voltage). Neither X3 nor X4 has a charge-status line wired
// into their board profile, so `charging` is usually reported as unknown here —
// the field is shown honestly rather than guessed. Confirm re-reads; Back exits.
void renderBatteryInfo() {
  const BatteryMonitor battery;
  const BatteryMonitor::Status st = battery.readStatus();

  std::string body;
  if (!st.supported) {
    body = "No battery telemetry on\nthis board profile.";
  } else {
    // Charge %.
    if (st.percentageKnown) {
      body += "Charge: " + std::to_string(st.percentage) + "%\n";
    } else {
      body += "Charge: unknown\n";
    }

    // Voltage (mV -> V, two decimals).
    if (st.millivoltsKnown) {
      char v[16];
      snprintf(v, sizeof(v), "%.2f V", st.millivolts / 1000.0);
      body += "Voltage: " + std::string(v) + " (" + std::to_string(st.millivolts) + " mV)\n";
    } else {
      body += "Voltage: unknown\n";
    }

    // Charging status.
    if (st.chargingKnown) {
      body += std::string("Charging: ") + (st.charging ? "YES" : "no") + "\n";
    } else {
      body += "Charging: unknown (no charge pin)\n";
    }

    // External power (M5PM1-class boards; usually unknown on X3/X4).
    if (st.externalPowerKnown) {
      body += std::string("External power: ") + (st.externalPower ? "YES" : "no") + "\n";
    }

    // Raw M5PM1 telemetry, only when actually read (-1 sentinel = not available).
    if (st.pm1VinMv >= 0) body += "VIN: " + std::to_string(st.pm1VinMv) + " mV\n";
    if (st.pm1VinOutMv >= 0) body += "5VIN/OUT: " + std::to_string(st.pm1VinOutMv) + " mV\n";
    if (st.pm1PowerSource >= 0) body += "Power source: " + std::to_string(st.pm1PowerSource) + "\n";

    body += "\nConfirm: re-read";
  }

  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<4> ib;
  ui::InputSnapshot empty;
  ui::Frame<4> frame(*g_target, dev, empty, ib);
  ui::Screen<4> screen(frame, g_theme);

  screen.header("Battery Info");
  iconFooter(screen, btnhint::back(), btnhint::confirm(), btnhint::none(), btnhint::none());

  screen.insetContent(ui::Insets{8, 16, 8, 16});
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Left;
  ts.maxLines = 12;
  frame.target().text(screen.body(), body.c_str(), ts);

  frame.finish();
  pushDisplay();
}

void renderProgress(const char* title, int pct) {
  display.clearScreen(0xFF);
  ui::DisplayTarget& t = *g_target;

  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  t.text(ui::Rect{0, static_cast<int16_t>(g_h / 3), g_w, 28}, title, ts);

  const int16_t bx = 40;
  const int16_t bw = static_cast<int16_t>(g_w - 80);
  const int16_t by = static_cast<int16_t>(g_h / 2);
  const int16_t bh = 26;
  t.stroke(ui::Rect{bx, by, bw, bh}, ui::Paint::solid(ui::Color::Black), 2);
  const int16_t fillW = static_cast<int16_t>((bw - 8) * pct / 100);
  if (fillW > 0) {
    t.fill(ui::Rect{static_cast<int16_t>(bx + 4), static_cast<int16_t>(by + 4), fillW, static_cast<int16_t>(bh - 8)},
           ui::Paint::solid(ui::Color::Black));
  }

  char pctbuf[8];
  snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
  t.text(ui::Rect{0, static_cast<int16_t>(by + bh + 10), g_w, 28}, pctbuf, ts);

  display.displayBuffer(EInkDisplay::FAST_REFRESH);
}

// ---------------------------------------------------------------------------
// Flash
// ---------------------------------------------------------------------------
void onFlashProgress(size_t written, size_t total, void*) {
  const int pct = total ? static_cast<int>((written * 100) / total) : 0;
  if (pct >= g_lastPct + 10 || pct >= 100) {
    g_lastPct = pct;
    renderProgress("Flashing...", pct);
  }
}

void doFlash() {
  g_lastPct = -1;
  renderProgress("Validating...", 0);

  const auto res = firmware_flash::flashFromSdPath(g_flashPath.c_str(), onFlashProgress, nullptr);
  if (res == firmware_flash::Result::OK) {
    renderProgress("Done! Rebooting", 100);
    delay(1500);
    ESP.restart();
    return;  // not reached
  }

  g_error = std::string("Flash failed: ") + firmware_flash::resultName(res);
  g_state = State::Failed;
  renderMessage("Update failed", g_error.c_str(), "Any key: back");
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------
void enterDirectory(const std::string& folderWithSlash) {
  std::string leaf = folderWithSlash;
  if (!leaf.empty() && leaf.back() == '/') leaf.pop_back();
  g_path = joinPath(g_path, leaf);
  loadEntries();
  renderList();
}

void goUpDirectory() {
  if (g_path == "/") return;
  const size_t pos = g_path.find_last_of('/');
  g_path = (pos == 0 || pos == std::string::npos) ? "/" : g_path.substr(0, pos);
  loadEntries();
  renderList();
}

// `navDelta` is the net of all queued Up/Down (Left/Right) presses this tick, so
// rapid scrolling collapses to a single render instead of one per press.
void doBrowse(int navDelta, bool confirm, bool back) {
  if (navDelta != 0 && !g_items.empty()) {
    const int n = static_cast<int>(g_items.size());
    int s = (g_sel + navDelta) % n;
    if (s < 0) s += n;  // wrap around both ends
    g_sel = s;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + g_visible) g_top = g_sel - g_visible + 1;
  }

  // A transition (back/confirm) renders the new screen itself, so skip the
  // intermediate list redraw in that case.
  if (back) {
    if (g_path == "/") {
      g_state = State::Menu;
      renderMenu();
    } else {
      goUpDirectory();
    }
    return;
  }
  if (confirm && !g_items.empty()) {
    if (g_isDir[g_sel]) {
      enterDirectory(g_names[g_sel]);
    } else {
      g_flashPath = joinPath(g_path, g_names[g_sel]);
      g_state = State::Confirm;
      renderConfirm();
    }
    return;
  }
  if (navDelta != 0) renderList();
}

void doMenu(int navDelta, bool confirm) {
  if (navDelta != 0) {
    int s = (g_menuSel + navDelta) % kMenuCount;
    if (s < 0) s += kMenuCount;
    g_menuSel = s;
  }
  if (confirm) {
    if (g_menuSel == 0) {  // Flash Firmware
      g_state = State::Browsing;
      g_path = "/";
      loadEntries();
      renderList();
    } else if (g_menuSel == 1) {  // Button Test
      g_state = State::ButtonTest;
      g_lastBackPress = 0;
      renderButtonTest(nullptr, 0);
    } else if (g_menuSel == 2) {  // Boot Other Slot
      g_bootDest = inactiveAppPartition();
      if (!partitionHasApp(g_bootDest)) {
        g_bootDest = nullptr;
        g_state = State::Failed;
        renderMessage("No firmware there", "The other partition has no bootable app to switch to.",
                      "Any key: back");
      } else {
        g_state = State::BootConfirm;
        renderBootConfirm();
      }
    } else if (g_menuSel == 3) {  // SD Card Test
      g_state = State::SdCardTest;
      renderSdCardTest();
    } else if (g_menuSel == 4) {  // Battery Info
      g_state = State::BatteryInfo;
      renderBatteryInfo();
    } else {  // EFuse / Security
      g_state = State::EfuseInfo;
      renderEfuseInfo();
    }
    return;
  }
  if (navDelta != 0) renderMenu();
}

// The ADC ladder splits across two GPIOs: Back/Confirm/Left/Right on pin 1,
// Up/Down on pin 2. Read whichever the pressed button lives on so the reported
// value is the one its resistor divider actually produced.
int adcPinForButton(uint8_t button) {
  return button < InputManager::BTN_UP ? InputManager::BUTTON_ADC_PIN_1 : InputManager::BUTTON_ADC_PIN_2;
}

void doConfirmScreen(bool confirm, bool back) {
  if (confirm) {
    doFlash();  // reboots on success, or drops to Failed
  } else if (back) {
    g_state = State::Browsing;
    renderList();
  }
}

void doBootConfirm(bool confirm, bool back) {
  if (confirm && g_bootDest) {
    renderMessage("Switching...", "Rebooting into the other partition.", nullptr);
    if (ota_boot::switchTo(g_bootDest)) {
      delay(800);
      ESP.restart();
      return;  // not reached
    }
    g_state = State::Failed;
    renderMessage("Switch failed", "Could not update the boot selection.", "Any key: back");
  } else if (back) {
    g_state = State::Menu;
    renderMenu();
  }
}

// Window during which input is ignored, set right after a screen-changing
// action. The buttons are an ADC resistor ladder where Confirm (2090-3100) and
// Back (3100-3900) are adjacent: releasing Confirm ramps its voltage up through
// the Back range, so the fast background poller catches a phantom Back press
// that would otherwise cancel the flash the instant you confirmed it.
unsigned long g_actionIgnoreUntil = 0;

void dispatchAction(int navDelta, bool confirm, bool back) {
  switch (g_state) {
    case State::Menu:
      doMenu(navDelta, confirm);
      break;
    case State::Browsing:
      doBrowse(navDelta, confirm, back);
      break;
    case State::Confirm:
      doConfirmScreen(confirm, back);
      break;
    case State::BootConfirm:
      doBootConfirm(confirm, back);
      break;
    case State::ButtonTest:
      break;  // handled directly in loop()
    case State::SdCardTest:
    case State::EfuseInfo:
      if (back || confirm) {
        g_state = State::Menu;
        renderMenu();
      }
      break;
    case State::BatteryInfo:
      if (back) {
        g_state = State::Menu;
        renderMenu();
      } else if (confirm) {
        renderBatteryInfo();  // re-read live telemetry
      }
      break;
    case State::Failed:
      g_state = State::Menu;
      renderMenu();
      break;
  }
  if (confirm || back) g_actionIgnoreUntil = millis() + 250;  // swallow the release ramp
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(50);

  // Recovery hatch: if Back+Up is held at reset, jump back to the Escape Hatch
  // slot before doing anything else. A no-op here (we ARE that slot) — it earns
  // its keep when the SAME call is the first line of the other firmwares' setup.
  freeink::recovery::checkBootCombo();

  // Pick X3 vs X4 before any peripheral reads the active board profile.
  const bool isX3 = freeink::selectXteinkDevice();

  // X3/X4 put the SD card on the display's SPI bus (the SD profile leaves
  // sclk/mosi unassigned), so claim the shared bus once, with MISO, before SD
  // begin. The display driver's own SPI.begin is then a no-op.
  SPI.begin(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.display.mosi,
            BoardConfig::ACTIVE.display.cs);

  if (isX3) display.setDisplayX3();
  display.begin();
  delay(50);  // let the panel finish powering up so the boot paint isn't dropped
  input.begin();

  // Latch presses on a background task so none are lost while the main loop is
  // blocked in a panel refresh.
  input.beginAsync();

  // The native framebuffer is landscape; DisplayTarget rotates to portrait by
  // default for these (width > height) panels, so work in its logical frame.
  g_target = new ui::DisplayTarget(display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(),
                                   display.getDisplayWidthBytes());
  // Roomier chrome to match the 24px default font.
  g_theme.headerHeight = 52;
  g_theme.footerHeight = 48;
  g_theme.rowHeight = kRowHeight;

  g_w = g_target->logicalWidth();
  g_h = g_target->logicalHeight();
  g_visible = (g_h - g_theme.headerHeight - g_theme.footerHeight) / kRowHeight;
  if (g_visible < 1) g_visible = 1;

  // Boot splash — the seeding FULL refresh that establishes the panel's
  // differential base; stays up through SD bring-up. Everything after is a fast
  // partial refresh, so mark the first paint done and prime the fast pipeline.
  renderSplash("Booting...", EInkDisplay::FULL_REFRESH);
  g_firstPaint = false;
  // X3: prime the fast-refresh pipeline with one fast refresh of the boot frame,
  // so the first interactive screen is a clean fast refresh rather than the
  // (slow, full-looking) first differential after the boot full.
  display.displayBuffer(EInkDisplay::FAST_REFRESH);

  // Bring up the SD card if present. A missing card isn't fatal — only Flash
  // Firmware needs it (and it shows "No .bin files" in context), while Button
  // Test, Boot Other Slot, SD Card Test, and EFuse all work without one — so
  // boot straight to the menu either way rather than parking on an error screen.
  SdMan.begin();
  g_state = State::Menu;
  renderMenu();

  // Power button: drain the wake press so a still-held Power at boot can't
  // immediately satisfy the sleep-hold threshold, then arm hold-to-sleep after a
  // brief settle. Works even on the SD-error screen.
  freeink::PowerManager::waitForPowerButtonRelease();
  g_allowSleepAt = millis() + 2000;
  g_powerSleepArmed = true;
}

void loop() {
  // Power button: hold for kPowerSleepHoldMs to deep-sleep, from any screen. The
  // async input task keeps the level/held-time state fresh. Re-arm only once the
  // button is released so a single long hold can't sleep, wake, and sleep again.
  if (!input.isPowerButtonPressed()) g_powerSleepArmed = true;
  if (g_powerSleepArmed && millis() >= g_allowSleepAt && input.isPowerButtonPressed() &&
      input.getPowerButtonHeldTime() >= kPowerSleepHoldMs) {
    enterDeepSleep();  // does not return
  }

  // Button test: every press is a reading, not navigation. Show the button's
  // name and the live ADC value of the GPIO its divider drives; a quick second
  // Back press (within the double-tap window) returns to the menu.
  if (g_state == State::ButtonTest) {
    uint8_t b;
    while (input.popPress(b)) {
      if (millis() < g_actionIgnoreUntil) continue;  // discard release-ramp artifacts
      const unsigned long now = millis();
      if (b == InputManager::BTN_BACK) {
        if (g_lastBackPress != 0 && now - g_lastBackPress < 700) {
          g_state = State::Menu;
          g_lastBackPress = 0;
          renderMenu();
          g_actionIgnoreUntil = now + 250;
          break;
        }
        g_lastBackPress = now;
      } else {
        g_lastBackPress = 0;  // any other button breaks a pending double-tap
      }
      const int adc = analogRead(adcPinForButton(b));
      renderButtonTest(InputManager::getButtonName(b), adc);
      // Confirm's release ramps up through the Back ADC range; swallow the
      // phantom Back press it would otherwise produce.
      if (b == InputManager::BTN_CONFIRM) g_actionIgnoreUntil = now + 250;
    }
    delay(10);
    return;
  }

  // Drain the latched button events. Navigation coalesces into one delta; a
  // discrete Confirm/Back is dispatched immediately (applying any pending nav
  // first) and ends the drain, so the rest of the queue is re-evaluated next
  // iteration in the possibly-new state — a Confirm meant for the next screen is
  // never consumed by the current one.
  uint8_t b;
  int navDelta = 0;
  bool acted = false;
  while (input.popPress(b)) {
    if (millis() < g_actionIgnoreUntil) continue;  // discard release-ramp artifacts
    if (b == InputManager::BTN_UP || b == InputManager::BTN_LEFT) {
      navDelta--;
    } else if (b == InputManager::BTN_DOWN || b == InputManager::BTN_RIGHT) {
      navDelta++;
    } else if (b == InputManager::BTN_CONFIRM || b == InputManager::BTN_BACK) {
      dispatchAction(navDelta, b == InputManager::BTN_CONFIRM, b == InputManager::BTN_BACK);
      acted = true;
      break;
    }
  }

  if (!acted && navDelta != 0) dispatchAction(navDelta, false, false);

  delay(10);
}
