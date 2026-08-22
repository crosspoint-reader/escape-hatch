// Escape Hatch — the most minimal X3/X4/X4-Pro firmware that can reflash the
// device.
//
// Boots, detects the device variant, mounts the SD card, and shows a file
// browser of .bin firmware images. Up/Down (or the X4 Pro's Left/Right keys)
// move the selection, Confirm picks an entry (entering folders or, for a .bin,
// opening a flash-confirm screen). Confirming the flash streams the image
// straight into the inactive OTA partition and switches otadata, then reboots
// into it — the exact SD-flash path from crosspoint-reader (FirmwareFlasher +
// OtaBootSwitch), nothing else.
//
// UI runs on the FreeInkUI FreeInkApp runtime: every screen registers its
// interactive elements (list rows, footer icons) in the app's interaction
// table, so on touch boards (X4 Pro's GT911) taps route straight to the same
// actions the buttons drive. The GT911 capacitive Home key acts as Back.

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

// The C3 X3/X4 dual binary picks its profile at runtime; single-profile builds
// (X4 Pro) compile exactly one device in.
#if defined(FREEINK_DEVICE_X3) || defined(FREEINK_DEVICE_X4)
#define HATCH_XTEINK_C3 1
#else
#define HATCH_XTEINK_C3 0
#endif

// ---------------------------------------------------------------------------
// Hardware. DEFAULT_DEVICE is the compile-time boot profile: X4 for the C3
// dual binary (X3 shares its display pins; runtime detect swaps the profile),
// the X4 Pro profile for the S3 build.
// ---------------------------------------------------------------------------
EInkDisplay display(BoardConfig::DEFAULT_DEVICE.display.sclk, BoardConfig::DEFAULT_DEVICE.display.mosi,
                    BoardConfig::DEFAULT_DEVICE.display.cs, BoardConfig::DEFAULT_DEVICE.display.dc,
                    BoardConfig::DEFAULT_DEVICE.display.rst, BoardConfig::DEFAULT_DEVICE.display.busy);
InputManager input;

// UI runtime: owns the interaction table taps route against. 32 interactions
// covers the busiest screen (browser list rows + footer icons).
using App = ui::FreeInkApp<32>;
using AppScreen = App::ScreenType;
App* g_app = nullptr;

ui::DisplayTarget* g_target = nullptr;
ui::ThemeTokens g_theme;
int16_t g_w = 0, g_h = 0;
int g_visible = 8;
constexpr int16_t kRowHeight = 46;
bool g_firstPaint = true;

// Semantic actions the screens register. Footer icons mirror the physical
// buttons so a touch board can tap them; list rows carry their index in the
// event value.
enum : ui::ActionId {
  ACT_MENU_ROW = 1,  // home-menu row (value = menu index)
  ACT_FILE_ROW,      // browser row (value = entry index)
  ACT_BACK,
  ACT_CONFIRM,
  ACT_UP,
  ACT_DOWN,
};

// The boot paint must be a clean FULL refresh to seed the panel's differential
// base; everything after is a fast partial refresh. Without this the first
// interactive refresh on the X4 (whose SSD1677 config has no full-sequence
// override) misbehaves and the UI appears to lag a click behind.
void pushDisplay() {
  display.displayBuffer(g_firstPaint ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
  g_firstPaint = false;
}

// Repaint the active screen through the app runtime and push it to the panel.
void paint() {
  display.clearScreen(0xFF);
  g_app->render();
  pushDisplay();
}

void showScreen(App::ScreenFn fn) {
  g_app->setScreen(fn, nullptr, ui::RefreshHint::Fast);
  paint();
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
  HwDetect,
  EfuseInfo,
  Failed
};
State g_state = State::Menu;

// Top-level menu entries (home screen).
const char* kMenuItems[] = {"Flash Firmware", "Button Test",  "Boot Other Slot",  "SD Card Test",
                            "Screen Wipe",    "Battery Info", "Hardware Detect",  "EFuse / Security"};
constexpr int kMenuCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
int g_menuSel = 0;

#if HATCH_XTEINK_C3
// Boot-time Xteink fingerprint results, captured before display bring-up so the
// Hardware Detect screen can show them without re-running the panel probe (which
// pulses the panel's reset line and would leave the live display needing a
// re-begin()).
freeink::XteinkVerdict g_xtVerdict = freeink::XteinkVerdict::Inconclusive;
uint8_t g_xtScore1 = 0, g_xtScore2 = 0;  // I2C chip hits per pass (0-3)
#endif
// Panel-controller probe results live in the SDK's diagnostics snapshot
// (freeink::getXteinkDisplayProbeDiag()), populated by
// applyXteinkDisplayController() in setup().

// The inactive OTA app partition we're about to switch the bootloader to,
// captured when the user picks "Boot Other Slot" (only set once validated to
// hold a real app image).
const esp_partition_t* g_bootDest = nullptr;

// Button-test screen: timestamp of the last Back press, to detect a double-tap
// (the test shows every button's reading, so a single Back is a reading too —
// only a quick second tap exits back to the menu). A tap on the footer's Back
// icon exits immediately on touch boards.
unsigned long g_lastBackPress = 0;
const char* g_btnName = nullptr;  // last pressed button (getButtonName static string)
int g_btnAdc = -1;                // its ADC reading, -1 on digital-button boards

// Power button: hold it for kPowerSleepHoldMs to deep-sleep. Armed only while the
// button is released, so the wake press that boots us can't immediately re-trigger
// sleep, and g_allowSleepAt blanks the threshold for a moment after boot.
constexpr uint32_t kPowerSleepHoldMs = 1500;
uint32_t g_allowSleepAt = 0;
bool g_powerSleepArmed = true;

// GT911 capacitive Home key = Back. The home-key flags are per-update
// one-shots owned by the async input task, so a slow main-loop iteration can
// observe the same tap twice — this timestamp dedupes it.
unsigned long g_lastHomeTap = 0;

std::string g_path = "/";          // current directory
std::vector<std::string> g_names;  // entry display names (folders end in '/')
std::vector<bool> g_isDir;
std::vector<ui::ListItem> g_items;  // labels borrow g_names storage
int g_sel = 0;
int g_top = 0;

std::string g_flashPath;  // full path of the .bin awaiting/within a flash
std::string g_error;
int g_lastPct = -1;

// Message screen contents (owned copies: the screen may repaint after the
// caller's buffers are gone).
std::string g_msgTitle, g_msgBody, g_msgHint;

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

// A footer slot: the hardware-button hint icon plus the action a tap on that
// slot dispatches (NO_ACTION = decorative only, no hit rect).
struct FooterSlot {
  ui::BitmapRef icon;
  ui::ActionId action = ui::NO_ACTION;
};

// Draw the four hardware button hints as icons. Reserves the footer band from
// the screen layout first — same as screen.footer() — so the body above never
// overlaps it, then centres an icon in each of the four slots, in hardware
// order: [Back][Confirm] (left bezel) [Up][Down] (right bezel). Each slot with
// an action also registers a touch hit rect, so on touch boards the icons are
// tappable buttons. A blank slot (btnhint::none()) draws nothing.
void iconFooter(AppScreen& screen, FooterSlot s0, FooterSlot s1, FooterSlot s2, FooterSlot s3) {
  const ui::Rect rect = screen.takeBottom(screen.theme().footerHeight);
  if (rect.empty()) return;
  ui::DrawTarget& t = screen.target();

  // Divider line along the top edge of the footer band.
  t.fill(ui::Rect{rect.x, rect.y, rect.width, 1}, ui::Paint::solid(ui::Color::Black));

  const int16_t sidePadding = 8;
  const int16_t gap = 4;
  const int16_t contentW = static_cast<int16_t>(rect.width - sidePadding * 2);
  const int16_t slotW = static_cast<int16_t>((contentW - gap * 3) / 4);
  const FooterSlot slots[4] = {s0, s1, s2, s3};

  int16_t x = static_cast<int16_t>(rect.x + sidePadding);
  for (int i = 0; i < 4; ++i) {
    const int16_t w = i == 3 ? static_cast<int16_t>(rect.right() - sidePadding - x) : slotW;
    const ui::Rect slot{x, rect.y, w, rect.height};
    if (slots[i].icon) {
      t.bitmap(slot, slots[i].icon, ui::BitmapMode::Center, ui::Paint::solid(ui::Color::Black));
    }
    if (slots[i].icon && slots[i].action != ui::NO_ACTION) {
      screen.frame().hit(slot, slots[i].action);
    }
    x = static_cast<int16_t>(x + w + gap);
  }
}

// A full-screen, chrome-less splash: a centered title with a subtitle beneath.
// Used for the boot and sleep screens (no header/footer, like inkdeck's).
// Drawn outside the app runtime — nothing on it is interactive.
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

void scrMenu(AppScreen& screen, void*) {
  screen.header("Escape Hatch");
  // No Back at the menu root; Confirm selects, Up/Down move.
  iconFooter(screen, {btnhint::none()}, {btnhint::confirm(), ACT_CONFIRM}, {btnhint::up(), ACT_UP},
             {btnhint::down(), ACT_DOWN});

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
  props.action = ACT_MENU_ROW;  // rows are tappable; value = menu index
  screen.list(props);
}
void renderMenu() { showScreen(scrMenu); }

void scrButtonTest(AppScreen& screen, void*) {
  screen.header("Button Test");
  // Only Back is meaningful here (double-tap, or tap the icon, to exit); the
  // other slots stay blank because every button press is consumed as a
  // reading, not navigation.
  iconFooter(screen, {btnhint::back(), ACT_BACK}, {btnhint::none()}, {btnhint::none()}, {btnhint::none()});

  std::string body;
  if (g_btnName) {
    body = std::string(g_btnName);
    if (g_btnAdc >= 0) {
      body += "\n\nADC: " + std::to_string(g_btnAdc);
    } else {
      body += "\n\n(digital GPIO button)";
    }
  } else {
    body = "Press any button to\nshow its reading.";
  }
  body += "\n\n(double-tap Back to exit)";

  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 8;
  screen.target().text(screen.body(), body.c_str(), ts);
}
void renderButtonTest(const char* btnName, int adc) {
  g_btnName = btnName;
  g_btnAdc = adc;
  showScreen(scrButtonTest);
}

void scrList(AppScreen& screen, void*) {
  std::string title = "Escape Hatch   " + g_path;
  screen.header(title.c_str());

  // Icon hints in hardware order: left-side buttons (Back, Confirm) on the left,
  // right-side buttons (Up, Down) on the right.
  iconFooter(screen, {btnhint::back(), ACT_BACK}, {btnhint::confirm(), ACT_CONFIRM}, {btnhint::up(), ACT_UP},
             {btnhint::down(), ACT_DOWN});

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
    props.action = ACT_FILE_ROW;  // rows are tappable; value = entry index
    screen.list(props);
  }
}
void renderList() { showScreen(scrList); }

void scrMessage(AppScreen& screen, void*) {
  screen.header(g_msgTitle.c_str());
  if (!g_msgHint.empty()) {
    // The hint doubles as a touch target: tapping it acts like Back (the
    // message states treat Back/Confirm alike).
    const ui::FooterAction footer[] = {{.label = g_msgHint.c_str(), .action = ACT_BACK}};
    screen.footer(footer, 1);
  }
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 4;
  screen.target().text(screen.body(), g_msgBody.c_str(), ts);
}
void renderMessage(const char* title, const char* body, const char* hint) {
  g_msgTitle = title ? title : "";
  g_msgBody = body ? body : "";
  g_msgHint = hint ? hint : "";
  showScreen(scrMessage);
}

void scrConfirm(AppScreen& screen, void*) {
  screen.header("Flash firmware?");
  // Four slots matching the browser footer ([Back][Confirm][Up][Down]) so the
  // Cancel/Confirm hints sit under the SAME physical buttons (left side) instead
  // of spreading across the bar — the right two slots (Up/Down) are unused here.
  iconFooter(screen, {btnhint::cancel(), ACT_BACK}, {btnhint::confirm(), ACT_CONFIRM}, {btnhint::none()},
             {btnhint::none()});

  std::string body = "Flash this image and reboot into it?\n\n" + g_names[g_sel];
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 6;
  screen.target().text(screen.body(), body.c_str(), ts);
}
void renderConfirm() { showScreen(scrConfirm); }

void scrBootConfirm(AppScreen& screen, void*) {
  screen.header("Boot other slot?");
  iconFooter(screen, {btnhint::cancel(), ACT_BACK}, {btnhint::confirm(), ACT_CONFIRM}, {btnhint::none()},
             {btnhint::none()});

  std::string body = "Switch the bootloader to '";
  body += g_bootDest ? g_bootDest->label : "?";
  body += "' and reboot into it?\n\nReturning needs that firmware (or a reflash).";
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 8;
  screen.target().text(screen.body(), body.c_str(), ts);
}
void renderBootConfirm() { showScreen(scrBootConfirm); }

// Read-only dump of the chip's security efuses — the facts that decide whether a
// custom-bootloader recovery (rewriting flash 0x0) is even possible, and whether
// a bad bootloader write could be undone. All reads; nothing is burned.
void scrEfuseInfo(AppScreen& screen, void*) {
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

  screen.header("EFuse / Security");
  iconFooter(screen, {btnhint::back(), ACT_BACK}, {btnhint::none()}, {btnhint::none()}, {btnhint::none()});

  screen.insetContent(ui::Insets{8, 16, 8, 16});
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Left;
  ts.maxLines = 12;
  screen.target().text(screen.body(), body.c_str(), ts);
}
void renderEfuseInfo() { showScreen(scrEfuseInfo); }

// Shows the boot-time hardware fingerprint: on the C3 builds the X3-vs-X4 I2C
// verdict with its per-pass chip-hit scores, plus (everywhere) the
// panel-controller probe (original controller vs the UltraChip sibling newer
// batches carry) with the raw VER/FLG bytes from the SDK's diagnostics
// snapshot. All values were captured in setup() before display init.
void scrHwDetect(AppScreen& screen, void*) {
  auto hex2 = [](uint8_t v) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", v);
    return std::string(buf);
  };

  std::string body = "Profile: " + std::string(BoardConfig::ACTIVE.name) + "\n\n";

#if HATCH_XTEINK_C3
  const char* verdict = g_xtVerdict == freeink::XteinkVerdict::X3Confirmed   ? "X3 confirmed"
                        : g_xtVerdict == freeink::XteinkVerdict::X4Confirmed ? "X4 confirmed"
                                                                             : "Inconclusive (running as X4)";
  body += "I2C fingerprint: " + std::string(verdict) + "\n";
  body += "Chip hits: " + std::to_string(g_xtScore1) + "/3 + " + std::to_string(g_xtScore2) + "/3";
  body += " (gauge, RTC, IMU)\n\n";
#else
  body += std::string("Touch: ") + (input.hasTouch() ? "present" : "none") + "\n\n";
#endif

  const freeink::XteinkDisplayProbeDiag& diag = freeink::getXteinkDisplayProbeDiag();
  if (diag.valid) {
    const auto v = static_cast<freeink::DisplayControllerVerdict>(diag.verdict);
    const char* panel = v == freeink::DisplayControllerVerdict::Uc81xxConfirmed  ? "UltraChip confirmed"
                        : v == freeink::DisplayControllerVerdict::PrimaryAssumed ? "Default (assumed)"
                                                                                 : "Inconclusive (using default)";
    body += "Panel controller: " + std::string(panel);
    if (diag.promoted) body += " [driver promoted]";
    body += "\nVER:";
    for (uint8_t b : diag.ver) body += " " + hex2(b);
    body += "\nFLG: " + hex2(diag.flg) + "\n";
    if (diag.mtpValid) {
      body += "MTP:";
      for (size_t i = 0; i < 8; i++) body += " " + hex2(diag.mtp[i]);
      body += " ...\n";
    }
  } else {
    body += "Panel probe: not run\n";
  }

  screen.header("Hardware Detect");
  iconFooter(screen, {btnhint::back(), ACT_BACK}, {btnhint::none()}, {btnhint::none()}, {btnhint::none()});

  screen.insetContent(ui::Insets{8, 16, 8, 16});
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Left;
  ts.maxLines = 12;
  screen.target().text(screen.body(), body.c_str(), ts);
}
void renderHwDetect() { showScreen(scrHwDetect); }

// Exercises the SD card end to end: re-mount the card (hardware access), write
// a small file, read it back and verify the bytes match, then delete it. Each
// step is reported with OK/FAIL so a user can see exactly where their card is
// failing. Stops at the first failure since later steps depend on earlier ones.
void scrSdCardTest(AppScreen& screen, void*) {
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

  screen.header("SD Card Test");
  iconFooter(screen, {btnhint::back(), ACT_BACK}, {btnhint::none()}, {btnhint::none()}, {btnhint::none()});

  screen.insetContent(ui::Insets{8, 16, 8, 16});
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Left;
  ts.maxLines = 12;
  screen.target().text(screen.body(), body.c_str(), ts);
}
void renderSdCardTest() { showScreen(scrSdCardTest); }

// Live battery telemetry from the SDK's BatteryMonitor. On X4 this is an ADC read
// of the divided LiPo rail (voltage + a polynomial %); on X3 it's the BQ27220 I2C
// fuel gauge (true SoC + voltage); on X4 Pro the CW2017 I2C gauge. No profile
// wires a charge-status line, so `charging` is usually reported as unknown here —
// the field is shown honestly rather than guessed. Confirm re-reads; Back exits.
void scrBatteryInfo(AppScreen& screen, void*) {
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

  screen.header("Battery Info");
  iconFooter(screen, {btnhint::back(), ACT_BACK}, {btnhint::confirm(), ACT_CONFIRM}, {btnhint::none()},
             {btnhint::none()});

  screen.insetContent(ui::Insets{8, 16, 8, 16});
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Left;
  ts.maxLines = 12;
  screen.target().text(screen.body(), body.c_str(), ts);
}
void renderBatteryInfo() { showScreen(scrBatteryInfo); }

// Progress bar, drawn outside the app runtime (nothing interactive, and it
// repaints at a cadence the state machine drives).
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

  // The two wrong-device rejections deserve plain language — they are the
  // user picking the wrong file, not a broken card or corrupt download.
  if (res == firmware_flash::Result::BAD_CHIP) {
    g_error = "This image is built for a different chip family and would brick this device.";
  } else if (res == firmware_flash::Result::WRONG_BOARD) {
    g_error = "This image is tagged for a different board, not this device.";
  } else {
    g_error = std::string("Flash failed: ") + firmware_flash::resultName(res);
  }
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

// Drop every input event latched while a blocking operation held the loop —
// presses AND touch taps — so none replay as phantom actions afterwards.
void drainPendingInput() {
  uint8_t b;
  while (input.popPress(b)) {}
  float nx, ny;
  while (input.popTouchTap(nx, ny)) {}
}

// Ghosting cleanup: alternate all-black / all-white FULL refreshes. A full
// waveform drives every pixel through its complete transition, and flipping
// polarity between passes shakes out the residual charge that accumulated
// partial refreshes leave behind (the ghost outlines of previous screens).
// Runs synchronously — the flashing panel IS the progress indicator — then
// re-seeds the UI exactly like boot: one FULL menu paint to establish a clean
// differential base, plus a FAST pass to prime the X3 fast-refresh pipeline.
void runScreenWipe() {
  constexpr int kWipeCycles = 3;
  for (int i = 0; i < kWipeCycles; ++i) {
    display.clearScreen(0x00);  // all black
    display.displayBuffer(EInkDisplay::FULL_REFRESH);
    display.clearScreen(0xFF);  // all white
    display.displayBuffer(EInkDisplay::FULL_REFRESH);
  }
  g_firstPaint = true;  // next pushDisplay (the menu render) refreshes FULL
  renderMenu();
  display.displayBuffer(EInkDisplay::FAST_REFRESH);
  // Events latched by the async input task while the wipe blocked the loop
  // (including the launching Confirm's release ramp) would replay as phantom
  // menu actions — drop them.
  drainPendingInput();
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
      renderButtonTest(nullptr, -1);
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
    } else if (g_menuSel == 4) {  // Screen Wipe — runs in place, stays on the menu
      runScreenWipe();
    } else if (g_menuSel == 5) {  // Battery Info
      g_state = State::BatteryInfo;
      renderBatteryInfo();
    } else if (g_menuSel == 6) {  // Hardware Detect
      g_state = State::HwDetect;
      renderHwDetect();
    } else {  // EFuse / Security
      g_state = State::EfuseInfo;
      renderEfuseInfo();
    }
    return;
  }
  if (navDelta != 0) renderMenu();
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
// action. The X3/X4 buttons are an ADC resistor ladder where Confirm
// (2090-3100) and Back (3100-3900) are adjacent: releasing Confirm ramps its
// voltage up through the Back range, so the fast background poller catches a
// phantom Back press that would otherwise cancel the flash the instant you
// confirmed it. The same window swallows touch taps queued mid-transition.
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
    case State::HwDetect:
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

// Translate a routed UI action (a tap on a list row or footer icon) into the
// same state-machine inputs the physical buttons produce. A row tap both
// selects the row and confirms it — the natural touch gesture.
void handleUiAction(const ui::ActionEvent& ev) {
  if (g_state == State::ButtonTest) {
    // Every physical press is a reading here, but a tap on the footer's Back
    // icon exits immediately (a touch board may have no Back button at all).
    if (ev.action == ACT_BACK) {
      g_state = State::Menu;
      g_lastBackPress = 0;
      renderMenu();
      g_actionIgnoreUntil = millis() + 250;
    }
    return;
  }
  switch (ev.action) {
    case ACT_MENU_ROW:
      if (ev.value >= 0 && ev.value < kMenuCount) {
        g_menuSel = ev.value;
        dispatchAction(0, true, false);
      }
      break;
    case ACT_FILE_ROW:
      if (ev.value >= 0 && ev.value < static_cast<int>(g_items.size())) {
        g_sel = ev.value;
        dispatchAction(0, true, false);
      }
      break;
    case ACT_BACK:
      dispatchAction(0, false, true);
      break;
    case ACT_CONFIRM:
      dispatchAction(0, true, false);
      break;
    case ACT_UP:
      dispatchAction(-1, false, false);
      break;
    case ACT_DOWN:
      dispatchAction(+1, false, false);
      break;
    default:
      break;
  }
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
  // (On the X4 Pro there is no Back GPIO, so the combo never fires — harmless.)
  freeink::recovery::checkBootCombo();

#if HATCH_XTEINK_C3
  // Pick X3 vs X4 before any peripheral reads the active board profile. This is
  // selectXteinkDevice() unrolled through the verdict API so the per-pass scores
  // land in globals for the Hardware Detect screen.
  g_xtVerdict = freeink::detectXteinkVerdict(&g_xtScore1, &g_xtScore2);
  const bool isX3 = g_xtVerdict == freeink::XteinkVerdict::X3Confirmed;
  BoardConfig::selectDevice(isX3 ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);
  // Panel selection must precede the controller probe: setDisplayX3() re-selects
  // the X3 profile (a full ACTIVE overwrite), which would wipe a promoted
  // controller if it ran after.
  if (isX3) display.setDisplayX3();
#endif
  // Fingerprint the panel controller on the live display bus and promote
  // ACTIVE.displayController to the UltraChip sibling this unit actually
  // carries (X3: UC8253 -> UC8279d; X4: SSD1677 -> UC8179/UC8279; X4 Pro
  // batches vary the same way), so display.begin() selects the matching
  // driver. Results land in the SDK's diagnostics snapshot for the Hardware
  // Detect screen.
  const bool promoted = freeink::applyXteinkDisplayController();
  Serial.printf("[detect] promoted=%d -> %s (controller=%d)\n", promoted, BoardConfig::ACTIVE.name,
                static_cast<int>(BoardConfig::ACTIVE.displayController));

#if HATCH_XTEINK_C3
  // X3/X4 put the SD card on the display's SPI bus (the SD profile leaves
  // sclk/mosi unassigned), so claim the shared bus once, with MISO, before SD
  // begin. The display driver's own SPI.begin is then a no-op. (The X4 Pro's
  // SD is native SDMMC on its own pins — no shared bus to claim there.)
  SPI.begin(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.display.mosi,
            BoardConfig::ACTIVE.display.cs);
#endif

  display.begin();
  delay(50);  // let the panel finish powering up so the boot paint isn't dropped
  input.begin();

  // Latch presses (and, on touch boards, completed taps) on a background task
  // so none are lost while the main loop is blocked in a panel refresh.
  input.beginAsync();

  // The native framebuffer is landscape; DisplayTarget rotates to portrait by
  // default for these (width > height) panels, so work in its logical frame.
  g_target = new ui::DisplayTarget(display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(),
                                   display.getDisplayWidthBytes());
  // Roomier chrome to match the 24px default font.
  g_theme.headerHeight = 52;
  g_theme.footerHeight = 48;
  g_theme.rowHeight = kRowHeight;

  // UI runtime: owns the interaction table so taps queued during a refresh can
  // be routed against the last-rendered screen (FreeInkApp::route).
  g_app = new App(*g_target, g_target->deviceContext());
  g_app->setTheme(g_theme);

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

  // Touch: drain the taps the async task queued and route each against the
  // interactions the last-rendered frame registered. Taps arrive normalized in
  // the panel-native frame (the board profile's swap/flip already applied);
  // touchToLogical maps them into the UI's rotated logical frame.
  float nx = 0.0f, ny = 0.0f;
  while (input.popTouchTap(nx, ny)) {
    if (millis() < g_actionIgnoreUntil) continue;  // mid-transition leftovers
    ui::InputSnapshot snap;
    const ui::Point p = ui::touchToLogical(g_app->device(), nx, ny);
    snap.touchReleased = true;
    snap.touchX = p.x;
    snap.touchY = p.y;
    const ui::ActionEvent ev = g_app->route(snap);
    if (ev) handleUiAction(ev);
  }

  // GT911 capacitive Home key = Back (the X4 Pro has no Back GPIO). The flag
  // is a per-update one-shot owned by the async task, so debounce re-reads.
  if (input.hasTouch() && input.wasHomeKeyTapped() && millis() - g_lastHomeTap > 400) {
    g_lastHomeTap = millis();
    if (millis() >= g_actionIgnoreUntil) {
      if (g_state == State::ButtonTest) {
        g_state = State::Menu;
        renderMenu();
        g_actionIgnoreUntil = millis() + 250;
      } else {
        dispatchAction(0, false, true);
      }
    }
  }

  // Button test: every press is a reading, not navigation. Show the button's
  // name and (on the ADC-ladder boards) the live reading of the GPIO its
  // divider drives; a quick second Back press (within the double-tap window)
  // returns to the menu.
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
      // Board-aware sample: raw ADC + classification on the Xteink ladder,
      // raw = -1 on digital-button boards (X4 Pro), where the name suffices.
      InputManager::ButtonAdcSample group1{}, group2{};
      input.readButtonAdc(group1, group2);
      const int adc = (b == InputManager::BTN_UP || b == InputManager::BTN_DOWN) ? group2.raw : group1.raw;
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
