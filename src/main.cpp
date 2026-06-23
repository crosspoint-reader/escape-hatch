// Escape Hatch — the most minimal X3/X4 firmware that can reflash the device.
//
// Boots, auto-detects X3 vs X4, mounts the SD card, and shows a file browser of
// .bin firmware images. Up/Down move the selection, Confirm picks an entry
// (entering folders or, for a .bin, opening a flash-confirm screen). Confirming
// the flash streams the image straight into the inactive OTA partition and
// switches otadata, then reboots into it — the exact SD-flash path from
// crosspoint-reader (FirmwareFlasher + OtaBootSwitch), nothing else.

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkApp.h>
#include <FreeInkUI.h>
#include <FreeInkUIDisplayTarget.h>
#include <InputManager.h>
#include <SDCardManager.h>
#include <SPI.h>
#include <XteinkDetect.h>

#include <algorithm>
#include <string>
#include <vector>

#include "FirmwareFlasher.h"

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
enum class State { Browsing, Confirm, Failed };
State g_state = State::Browsing;

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
void renderList() {
  display.clearScreen(0xFF);
  ui::DeviceContext dev = g_target->deviceContext();
  ui::InteractionBuffer<32> ib;
  ui::InputSnapshot empty;
  ui::Frame<32> frame(*g_target, dev, empty, ib);
  ui::Screen<32> screen(frame, g_theme);

  std::string title = "Escape Hatch   " + g_path;
  screen.header(title.c_str());

  // Hints in hardware order: left-side buttons (Back, Confirm) on the left,
  // right-side buttons (Up, Down) on the right.
  const ui::FooterAction footer[] = {
      {.label = "Back", .action = ui::NO_ACTION},
      {.label = "Confirm", .action = ui::NO_ACTION},
      {.label = "Up", .action = ui::NO_ACTION},
      {.label = "Down", .action = ui::NO_ACTION},
  };
  screen.footer(footer, 4);

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
  const ui::FooterAction footer[] = {
      {.label = "Cancel", .action = ui::NO_ACTION},
      {.label = "Confirm", .action = ui::NO_ACTION},
      {.label = "", .action = ui::NO_ACTION},
      {.label = "", .action = ui::NO_ACTION},
  };
  screen.footer(footer, 4);

  std::string body = "Flash this image and reboot into it?\n\n" + g_names[g_sel];
  ui::TextStyle ts{};
  ts.align = ui::TextAlign::Center;
  ts.maxLines = 6;
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
    goUpDirectory();
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

void doConfirmScreen(bool confirm, bool back) {
  if (confirm) {
    doFlash();  // reboots on success, or drops to Failed
  } else if (back) {
    g_state = State::Browsing;
    renderList();
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
    case State::Browsing:
      doBrowse(navDelta, confirm, back);
      break;
    case State::Confirm:
      doConfirmScreen(confirm, back);
      break;
    case State::Failed:
      g_state = State::Browsing;
      renderList();
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

  if (!SdMan.begin()) {
    g_state = State::Failed;
    g_error = "SD card not detected";
    renderMessage("SD card error", "Insert an SD card with a .bin and reboot.", nullptr);
    return;
  }

  loadEntries();
  g_state = State::Browsing;
  renderList();  // boot paint: FULL, seeds the differential base

  // X3: prime the fast-refresh pipeline with one fast refresh of the same frame,
  // so the first interactive press is a clean fast refresh rather than the
  // (slow, full-looking) first differential after the boot full.
  display.displayBuffer(EInkDisplay::FAST_REFRESH);
}

void loop() {
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
