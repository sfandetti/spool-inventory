// Spool Display — round touch display for Spool Inventory.
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466 CO5300 AMOLED, CST9217 touch).
// Left/right: home + one screen per material. Up/down: that material's rolls.
// Wi-Fi + server address are set over USB from the Spool Inventory page ("Round display").
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include "TouchDrvCSTXXX.hpp"
#include <vector>
#include "types.h"

// ---------- pins (Waveshare BSP) ----------
#define LCD_CS 12
#define LCD_SCLK 38
#define LCD_D0 4
#define LCD_D1 5
#define LCD_D2 6
#define LCD_D3 7
#define LCD_RST 39
#define I2C_SDA 15
#define I2C_SCL 14
#define TP_INT 11
#define TP_RST 40
#define BOOT_BTN 0

#define W 466
#define H 466
#define CX 233
#define CY 233

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_CO5300 *panel = new Arduino_CO5300(bus, LCD_RST, 0, W, H, 6, 0, 0, 0);
Arduino_Canvas *g = new Arduino_Canvas(W, H, panel); // full frame in PSRAM, flushed each frame
TouchDrvCST92xx touch;
Preferences prefs;

// ---------- colors (RGB565) ----------
static uint16_t rgb(uint8_t r, uint8_t g8, uint8_t b) { return ((r & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b >> 3); }
const uint16_t BG = rgb(6, 7, 10);
const uint16_t TRACK = rgb(34, 38, 46);
const uint16_t TEXT = rgb(236, 240, 245);
const uint16_t MUTED = rgb(132, 142, 158);
const uint16_t ACCENT = rgb(255, 106, 19);
const uint16_t ACCENT_DIM = rgb(70, 30, 8);
const uint16_t GOOD = rgb(61, 220, 132);
const uint16_t BAD = rgb(255, 90, 90);

static Rgb parseHex(const char *h) {
  if (!h || strlen(h) < 6) return {136, 136, 136};
  uint32_t v = strtoul(String(h).substring(0, 6).c_str(), nullptr, 16);
  return {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
}
static bool isDark(Rgb c) { return (c.r * 3 + c.g * 6 + c.b) / 10 < 45; }

// ---------- data from the server ----------
Feed feed;
SemaphoreHandle_t feedLock;
volatile bool haveData = false, fetchOk = false;
String cfgSsid, cfgPass, cfgServer;

// ---------- UI state ----------
int col = 0, row = 0; // col 0 = home, col N = material N; row 0 = title card, row N = roll N
float offX = 0, offY = 0; // drag offsets in px (only one is non-zero at a time)
enum Axis { AXIS_NONE, AXIS_X, AXIS_Y };
Axis dragAxis = AXIS_NONE;
bool dragging = false;
int16_t touchStartX = 0, lastTouchX = 0, touchStartY = 0, lastTouchY = 0;
uint32_t touchStartMs = 0, lastInteractMs = 0;
bool dimmed = false;
bool touchOk = false;
const uint8_t BRIGHT = 220, DIM = 25;
const uint32_t DIM_AFTER_MS = 90000;

// ---------- text helpers ----------
static void textCenter(const char *s, int x, int y, const uint8_t *font, uint16_t color) {
  g->setFont(font);
  g->setTextColor(color);
  int16_t x1, y1; uint16_t w, h;
  g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  g->setCursor(x - (int)w / 2 - x1, y);
  g->print(s);
}
static int textWidth(const char *s, const uint8_t *font) {
  g->setFont(font);
  int16_t x1, y1; uint16_t w, h;
  g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}
static void pill(const char *s, int x, int y, uint16_t fill, uint16_t color) {
  const uint8_t *f = u8g2_font_helvB12_tf;
  int w = textWidth(s, f) + 28;
  g->fillRoundRect(x - w / 2, y - 17, w, 30, 15, fill);
  textCenter(s, x, y + 3, f, color);
}

// Ring showing how much is left, in the filament's color.
static void ring(int x, int y, int ro, int ri, int pct, Rgb c1, Rgb c2, bool two) {
  g->fillArc(x, y, ro, ri, 0, 360, TRACK);
  float frac = pct < 0 ? 1.0f : pct / 100.0f;
  if (frac <= 0.005f) return;
  float end = 270 + 360 * frac;
  uint16_t a = rgb(c1.r, c1.g, c1.b);
  if (two) {
    float mid = 270 + 180 * frac;
    g->fillArc(x, y, ro, ri, 270, mid, a);
    g->fillArc(x, y, ro, ri, mid, end, rgb(c2.r, c2.g, c2.b));
  } else {
    g->fillArc(x, y, ro, ri, 270, end, a);
  }
  if (isDark(c1)) { // black filament on a black screen: outline the arc so it shows
    g->fillArc(x, y, ro + 2, ro, 270, end, rgb(90, 96, 108));
    g->fillArc(x, y, ri, ri - 2, 270, end, rgb(90, 96, 108));
  }
}

static String stateText(const Printer &p) {
  if (!p.online) return "offline";
  if (p.state == "RUNNING") return "printing " + String(p.progress) + "%";
  if (p.state == "PAUSE") return "paused";
  if (p.state == "FINISH") return "finished";
  if (p.state == "FAILED") return "failed";
  if (p.state == "PREPARE") return "preparing";
  return "idle";
}

// ---------- pages ----------
static void drawOverview(int xo) {
  textCenter("SPOOL INVENTORY", CX + xo, 62, u8g2_font_helvB12_tf, ACCENT);
  textCenter("by Noventia.studio", CX + xo, 82, u8g2_font_helvR10_tf, MUTED);
  char buf[48];
  snprintf(buf, sizeof buf, "%d", feed.rolls);
  textCenter(buf, CX + xo, 150, u8g2_font_logisoso50_tn, TEXT);
  textCenter("rolls on hand", CX + xo, 178, u8g2_font_helvR14_tf, MUTED);
  snprintf(buf, sizeof buf, "%.1f kg left  \xC2\xB7  %d materials", feed.grams / 1000.0f, feed.materials);
  textCenter(buf, CX + xo, 210, u8g2_font_helvB14_tf, TEXT);

  if (feed.printers.empty()) {
    textCenter("No printers connected", CX + xo, 290, u8g2_font_helvR14_tf, MUTED);
  }
  int y = 252;
  for (size_t i = 0; i < feed.printers.size() && i < 2; i++) {
    const Printer &p = feed.printers[i];
    String head = p.name + "  \xC2\xB7  " + stateText(p);
    g->fillCircle(CX + xo - textWidth(head.c_str(), u8g2_font_helvB14_tf) / 2 - 12, y - 5, 4, p.online ? GOOD : BAD);
    textCenter(head.c_str(), CX + xo, y, u8g2_font_helvB14_tf, TEXT);
    int n = p.slots.size();
    if (n == 0) {
      textCenter("no tagged rolls loaded", CX + xo, y + 32, u8g2_font_helvR12_tf, MUTED);
      y += 72;
      continue;
    }
    int show = n > 5 ? 5 : n, gap = 56;
    int x0 = CX + xo - (show - 1) * gap / 2;
    for (int s = 0; s < show; s++) {
      const Slot &sl = p.slots[s];
      ring(x0 + s * gap, y + 36, 21, 14, sl.pct, sl.c, sl.c, false);
      g->fillCircle(x0 + s * gap, y + 36, 11, rgb(sl.c.r, sl.c.g, sl.c.b));
      snprintf(buf, sizeof buf, sl.pct >= 0 ? "%d%%" : "--", sl.pct);
      textCenter(buf, x0 + s * gap, y + 76, u8g2_font_helvR10_tf, MUTED);
    }
    y += 104;
  }
  if (!feed.families.empty()) textCenter("swipe left for materials", CX + xo, 446, u8g2_font_helvR10_tf, MUTED);
}

// Material title card: name, totals, finish breakdown and the colors you have.
static void drawFamily(int idx, int xo) {
  const Family &f = feed.families[idx];
  char buf[64];
  textCenter("MATERIAL", CX + xo, 70, u8g2_font_helvB12_tf, MUTED);
  textCenter(f.name.c_str(), CX + xo, 122, u8g2_font_helvB24_tf, ACCENT);
  snprintf(buf, sizeof buf, "%d roll%s  \xC2\xB7  %.1f kg", f.rolls, f.rolls == 1 ? "" : "s", f.grams / 1000.0f);
  textCenter(buf, CX + xo, 156, u8g2_font_helvB14_tf, TEXT);

  // finish breakdown, two columns
  int n = f.finishes.size();
  int rows = (n + 1) / 2;
  for (int i = 0; i < n; i++) {
    int col = n == 1 ? 0 : (i < rows ? -1 : 1);
    int row = i < rows ? i : i - rows;
    int x = CX + xo + col * 82;
    int y = 204 + row * 30;
    snprintf(buf, sizeof buf, "%s  %d", f.finishes[i].name.c_str(), f.finishes[i].count);
    textCenter(buf, x, y, u8g2_font_helvR14_tf, i == 0 ? TEXT : MUTED);
  }

  // color dots
  int dots = f.dots.size();
  int perRow = dots > 6 ? (dots + 1) / 2 : dots;
  for (int i = 0; i < dots; i++) {
    int row = i < perRow ? 0 : 1;
    int inRow = row == 0 ? perRow : dots - perRow;
    int k = row == 0 ? i : i - perRow;
    int x = CX + xo - (inRow - 1) * 21 + k * 42;
    int y = 330 + row * 42;
    Rgb c = f.dots[i];
    if (isDark(c)) g->fillCircle(x, y, 16, rgb(90, 96, 108));
    g->fillCircle(x, y, isDark(c) ? 14 : 16, rgb(c.r, c.g, c.b));
  }
  textCenter("swipe up for rolls", CX + xo, 440, u8g2_font_helvR10_tf, MUTED);
}

static void drawRoll(int idx, int xo) {
  const Roll &r = feed.list[idx];
  char buf[64];
  // position within this material, e.g. "PETG  ·  3 of 7"
  int first = idx, total = 0;
  while (first > 0 && feed.list[first - 1].fam == r.fam) first--;
  for (int i = first; i < (int)feed.list.size() && feed.list[i].fam == r.fam; i++) total++;
  snprintf(buf, sizeof buf, "%s  \xC2\xB7  %d of %d", r.fam.c_str(), idx - first + 1, total);
  textCenter(buf, CX + xo, 62, u8g2_font_helvB12_tf, ACCENT);
  textCenter(r.name.c_str(), CX + xo, 104, u8g2_font_helvB18_tf, TEXT);

  int ry = 242;
  ring(CX + xo, ry, 124, 96, r.pct, r.c1, r.c2, r.two);
  // a soft swatch of the color inside the ring
  g->fillCircle(CX + xo, ry, 84, BG);
  snprintf(buf, sizeof buf, "%d", r.grams);
  textCenter(buf, CX + xo, ry + 16, u8g2_font_logisoso38_tn, TEXT);
  if (r.qty > 1) snprintf(buf, sizeof buf, "grams  \xC2\xB7  %d rolls", r.qty);
  else if (r.pct >= 0) snprintf(buf, sizeof buf, "grams  \xC2\xB7  %d%%", r.pct);
  else snprintf(buf, sizeof buf, "grams  \xC2\xB7  full roll");
  textCenter(buf, CX + xo, ry + 44, u8g2_font_helvR12_tf, MUTED);

  textCenter(r.color.c_str(), CX + xo, 400, u8g2_font_helvB14_tf, TEXT);
  if (r.qty > 1) snprintf(buf, sizeof buf, "x%d  on the shelf", r.qty);
  if (r.where.length()) pill(r.where.c_str(), CX + xo, 432, ACCENT_DIM, ACCENT);
  else pill(r.qty > 1 ? buf : "on the shelf", CX + xo, 432, TRACK, MUTED);
}

// ---------- navigation grid ----------
// Left/right = home + one column per material. Up/down = that material's title card, then its rolls.
static int colCount() { return feed.families.size() + 1; }
static int rowCount(int c) { return c <= 0 || c > (int)feed.families.size() ? 1 : feed.families[c - 1].count + 1; }

static void drawCell(int c, int r) {
  if (c <= 0) { drawOverview(0); return; }
  if (c > (int)feed.families.size()) return;
  const Family &fa = feed.families[c - 1];
  if (r <= 0) drawFamily(c - 1, 0);
  else if (r <= fa.count) drawRoll(fa.first + r - 1, 0);
}

static void drawStatus() {
  if (WiFi.status() != WL_CONNECTED) textCenter("connecting to Wi-Fi...", CX, 26, u8g2_font_helvR10_tf, BAD);
  else if (!fetchOk) textCenter("can't reach Spool Inventory", CX, 26, u8g2_font_helvR10_tf, BAD);
}

// Small position markers drawn on top: material dots along the top, a scroll bar on the right.
static void drawIndicators() {
  if (WiFi.status() != WL_CONNECTED || !fetchOk) return;
  xSemaphoreTake(feedLock, portMAX_DELAY);
  int cols = colCount(), rows = rowCount(col);
  xSemaphoreGive(feedLock);
  if (cols > 1) {
    int gap = 14, x0 = CX - (cols - 1) * gap / 2;
    for (int i = 0; i < cols; i++)
      g->fillCircle(x0 + i * gap, 24, i == col ? 4 : 3, i == col ? ACCENT : TRACK);
  }
  if (rows > 1) { // right-edge scroll bar for the rolls in this material
    const int top = 150, len = 166, x = 452;
    g->fillRoundRect(x - 2, top, 4, len, 2, TRACK);
    int th = len / rows < 14 ? 14 : len / rows;
    int ty = top + (len - th) * row / (rows - 1);
    g->fillRoundRect(x - 3, ty, 6, th, 3, ACCENT);
  }
}

// ---------- page cache: each screen is drawn once, then just slid around ----------
#define CACHE_SLOTS 5
uint16_t *cacheBuf[CACHE_SLOTS];
int cacheId[CACHE_SLOTS] = {-99, -99, -99, -99, -99};
uint32_t cacheVer[CACHE_SLOTS];
volatile uint32_t dataVersion = 1;
#define STRIP_ROWS 32
uint16_t *strip; // internal-RAM bounce buffer for streaming frames while moving

static uint16_t *cellImage(int c, int r) {
  int id = c * 512 + r;
  uint32_t ver = dataVersion;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cacheId[i] == id && cacheVer[i] == ver) return cacheBuf[i];
  // reuse the slot farthest from the screen being viewed
  int slot = 0, far = -1;
  for (int i = 0; i < CACHE_SLOTS; i++) {
    int d = cacheId[i] < 0 || cacheVer[i] != ver ? 1 << 20 : abs(cacheId[i] / 512 - col) * 1000 + abs(cacheId[i] % 512 - row);
    if (d > far) { far = d; slot = i; }
  }
  g->fillScreen(BG);
  xSemaphoreTake(feedLock, portMAX_DELAY);
  drawCell(c, r);
  xSemaphoreGive(feedLock);
  memcpy(cacheBuf[slot], g->getFramebuffer(), W * H * 2);
  cacheId[slot] = id;
  cacheVer[slot] = ver;
  return cacheBuf[slot];
}

static void render() {
  g->fillScreen(BG);
  if (!haveData) {
    textCenter("SPOOL INVENTORY", CX, 170, u8g2_font_helvB12_tf, ACCENT);
    textCenter("by Noventia.studio", CX, 192, u8g2_font_helvR10_tf, MUTED);
    textCenter(WiFi.status() == WL_CONNECTED ? "Loading rolls..." : "Joining Wi-Fi...", CX, 240, u8g2_font_helvB18_tf, TEXT);
    textCenter(cfgSsid.c_str(), CX, 272, u8g2_font_helvR12_tf, MUTED);
    if (WiFi.status() == WL_CONNECTED && !fetchOk) {
      textCenter("Is Spool Inventory running on", CX, 310, u8g2_font_helvR12_tf, MUTED);
      textCenter(cfgServer.c_str(), CX, 330, u8g2_font_helvR12_tf, MUTED);
    }
    g->flush();
    return;
  }
  xSemaphoreTake(feedLock, portMAX_DELAY);
  int cols = colCount();
  if (col >= cols) { col = cols - 1; row = 0; }
  int rows = rowCount(col);
  if (row >= rows) row = rows - 1;
  // Neighbor shown while dragging: another material's title card (left/right) or the next roll (up/down).
  int nc = -1, nr = -1;
  if (offX < 0 && col + 1 < cols) { nc = col + 1; nr = 0; }
  if (offX > 0 && col > 0) { nc = col - 1; nr = 0; }
  if (offY < 0 && row + 1 < rows) { nc = col; nr = row + 1; }
  if (offY > 0 && row > 0) { nc = col; nr = row - 1; }
  xSemaphoreGive(feedLock);

  // Get finished screen images first (this may draw into the canvas), then slide them into place.
  uint16_t *cur = cellImage(col, row);
  uint16_t *nb = nc >= 0 ? cellImage(nc, nr) : nullptr;
  int xo = (int)offX, yo = (int)offY;

  if (xo == 0 && yo == 0) { // still frame: go through the canvas so overlays can be drawn on top
    memcpy(g->getFramebuffer(), cur, W * H * 2);
    drawIndicators();
    drawStatus();
    g->flush();
    return;
  }

  // Moving: build the frame a strip at a time in fast internal RAM and stream each strip
  // straight to the panel (no full-frame copy through PSRAM).
  for (int y0 = 0; y0 < H; y0 += STRIP_ROWS) {
    int n = H - y0 < STRIP_ROWS ? H - y0 : STRIP_ROWS;
    for (int k = 0; k < n; k++) {
      int y = y0 + k;
      uint16_t *d = strip + k * W;
      if (yo != 0) {
        int sh = yo < 0 ? -yo : yo;
        int sy = yo < 0 ? y + sh : y - sh; // row of the current screen that lands here
        if (sy >= 0 && sy < H) memcpy(d, cur + sy * W, W * 2);
        else if (nb) memcpy(d, nb + (yo < 0 ? sy - H : sy + H) * W, W * 2);
        else for (int x = 0; x < W; x++) d[x] = BG;
      } else {
        int sh = xo < 0 ? -xo : xo;
        if (sh > W) sh = W;
        uint16_t *c = cur + y * W;
        if (xo < 0) {
          memcpy(d, c + sh, (W - sh) * 2);
          if (nb) memcpy(d + W - sh, nb + y * W, sh * 2);
          else for (int x = W - sh; x < W; x++) d[x] = BG;
        } else {
          memcpy(d + sh, c, (W - sh) * 2);
          if (nb) memcpy(d, nb + y * W + W - sh, sh * 2);
          else for (int x = 0; x < sh; x++) d[x] = BG;
        }
      }
    }
    panel->draw16bitRGBBitmap(0, y0, strip, W, n);
  }
}

// Warm the cache for the screens next to the current one while nothing is moving.
static bool cached(int c, int r) {
  for (int i = 0; i < CACHE_SLOTS; i++) if (cacheId[i] == c * 512 + r && cacheVer[i] == dataVersion) return true;
  return false;
}
static void prefetchNeighbors() {
  xSemaphoreTake(feedLock, portMAX_DELAY);
  int cols = colCount(), rows = rowCount(col);
  xSemaphoreGive(feedLock);
  // one at a time, most likely next move first
  if (row + 1 < rows && !cached(col, row + 1)) { cellImage(col, row + 1); return; }
  if (col + 1 < cols && !cached(col + 1, 0)) { cellImage(col + 1, 0); return; }
  if (row > 0 && !cached(col, row - 1)) { cellImage(col, row - 1); return; }
  if (col > 0 && !cached(col - 1, 0)) { cellImage(col - 1, 0); return; }
}

static void renderSetup() {
  g->fillScreen(BG);
  textCenter("SPOOL DISPLAY", CX, 150, u8g2_font_helvB12_tf, ACCENT);
  textCenter("Set up Wi-Fi", CX, 200, u8g2_font_helvB24_tf, TEXT);
  textCenter("Keep me plugged into the PC.", CX, 250, u8g2_font_helvR14_tf, MUTED);
  textCenter("Open Spool Inventory, then", CX, 280, u8g2_font_helvR14_tf, MUTED);
  textCenter("Round display > Set up over USB", CX, 310, u8g2_font_helvB14_tf, TEXT);
  g->flush();
}

// ---------- networking (runs on core 0 so swiping stays smooth) ----------
static void fetchTask(void *) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(5000);
      http.begin("http://" + cfgServer + "/api/device");
      int code = http.GET();
      if (code == 200) {
        static uint32_t lastHash = 0;
        String body = http.getString();
        uint32_t hsh = 2166136261u; // FNV-1a: skip redraws when nothing changed
        for (size_t i = 0; i < body.length(); i++) hsh = (hsh ^ (uint8_t)body[i]) * 16777619u;
        JsonDocument doc;
        if (hsh == lastHash && haveData) { fetchOk = true; }
        else if (!deserializeJson(doc, body)) {
          lastHash = hsh;
          Feed f;
          f.rolls = doc["rolls"] | 0;
          f.grams = doc["grams"] | 0;
          f.materials = doc["materials"] | 0;
          for (JsonObject p : doc["printers"].as<JsonArray>()) {
            Printer pr;
            pr.name = (const char *)(p["name"] | "");
            pr.state = (const char *)(p["state"] | "");
            pr.job = (const char *)(p["job"] | "");
            pr.online = p["online"] | false;
            pr.progress = p["progress"] | 0;
            for (JsonObject s : p["slots"].as<JsonArray>()) {
              pr.slots.push_back({(const char *)(s["where"] | ""), (const char *)(s["type"] | ""), parseHex(s["hex"] | "888888"), s["pct"] | -1});
            }
            f.printers.push_back(pr);
          }
          for (JsonObject r : doc["list"].as<JsonArray>()) {
            Roll rl;
            rl.name = (const char *)(r["name"] | "");
            rl.color = (const char *)(r["color"] | "");
            rl.where = (const char *)(r["where"] | "");
            rl.c1 = parseHex(r["hex"] | "888888");
            const char *h2 = r["hex2"] | "";
            rl.two = h2 && strlen(h2) >= 6;
            rl.c2 = rl.two ? parseHex(h2) : rl.c1;
            rl.grams = r["g"] | 0;
            rl.pct = r["pct"] | -1;
            rl.qty = r["qty"] | 1;
            rl.fam = (const char *)(r["fam"] | "");
            rl.finish = (const char *)(r["finish"] | "");
            f.list.push_back(rl);
          }
          for (JsonObject fo : doc["families"].as<JsonArray>()) {
            Family fa;
            fa.name = (const char *)(fo["name"] | "");
            fa.rolls = fo["rolls"] | 0;
            fa.grams = fo["g"] | 0;
            for (JsonObject fi : fo["finishes"].as<JsonArray>()) fa.finishes.push_back({(const char *)(fi["n"] | ""), fi["c"] | 0});
            for (const char *h : fo["dots"].as<JsonArray>()) fa.dots.push_back(parseHex(h));
            f.families.push_back(fa);
          }
          // Where each material's rolls start in the (material-sorted) list.
          for (Family &fa : f.families) {
            fa.first = -1;
            fa.count = 0;
            for (int i = 0; i < (int)f.list.size(); i++)
              if (f.list[i].fam == fa.name) { if (fa.first < 0) fa.first = i; fa.count++; }
            if (fa.first < 0) fa.first = 0;
          }
          xSemaphoreTake(feedLock, portMAX_DELAY);
          feed = std::move(f);
          dataVersion++;
          xSemaphoreGive(feedLock);
          haveData = true;
          fetchOk = true;
        }
      } else {
        fetchOk = false;
      }
      http.end();
    }
    vTaskDelay(pdMS_TO_TICKS(fetchOk ? 15000 : 5000));
  }
}

// ---------- USB setup: "SETUP {json}" from the Spool Inventory page ----------
static void checkSerialSetup() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line.trim();
      if (line == "DIAG") { // touch diagnostics: chip presence, INT pin, then 15 s of raw readings
        Serial.printf("DIAG touchOk=%d model=%s\r\n", touchOk, touchOk ? touch.getModelName() : "-");
        for (uint8_t a : {0x5A, 0x34, 0x20, 0x6B}) {
          Wire.beginTransmission(a);
          Serial.printf("DIAG i2c 0x%02X %s\r\n", a, Wire.endTransmission() == 0 ? "ACK" : "no-ack");
        }
        uint32_t t0 = millis();
        int reads = 0, hits = 0, lowInt = 0;
        while (millis() - t0 < 15000) {
          int16_t x[1], y[1];
          if (digitalRead(TP_INT) == LOW) lowInt++;
          uint8_t n = touch.getPoint(x, y, 1);
          reads++;
          if (n) { hits++; if (hits % 10 == 1) Serial.printf("DIAG point %d %d\r\n", x[0], y[0]); }
          delay(10);
        }
        Serial.printf("DIAG done reads=%d hits=%d intLow=%d\r\n", reads, hits, lowInt);
      } else if (line == "SCAN") { // list nearby Wi-Fi networks for the setup page
        if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
        int n = WiFi.scanNetworks();
        JsonDocument out;
        JsonArray arr = out.to<JsonArray>();
        for (int i = 0; i < n; i++) {
          String ssid = WiFi.SSID(i);
          if (ssid.isEmpty()) continue;
          bool dup = false;
          for (JsonObject o : arr) if (ssid == (const char *)o["ssid"]) { dup = true; break; }
          if (dup) continue; // same network seen on several access points
          JsonObject o = arr.add<JsonObject>();
          o["ssid"] = ssid;
          o["rssi"] = WiFi.RSSI(i);
          o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        }
        WiFi.scanDelete();
        Serial.print("NETS ");
        serializeJson(out, Serial);
        Serial.println();
      } else if (line.startsWith("SETUP ")) {
        JsonDocument doc;
        if (!deserializeJson(doc, line.substring(6))) {
          prefs.putString("ssid", (const char *)(doc["ssid"] | ""));
          prefs.putString("pass", (const char *)(doc["pass"] | ""));
          prefs.putString("server", (const char *)(doc["server"] | ""));
          Serial.println("SETUP OK");
          Serial.flush();
          delay(400);
          ESP.restart();
        } else {
          Serial.println("SETUP ERROR bad json");
        }
      }
      line = "";
    } else if (line.length() < 400) {
      line += c;
    }
  }
}

// ---------- touch / swipe ----------
static void wake() {
  lastInteractMs = millis();
  if (dimmed) { panel->setBrightness(BRIGHT); dimmed = false; }
}

// Time-based ease-out of whichever axis is moving: always ~170 ms, however fast frames are.
static void animateTo(float target) {
  float *off = dragAxis == AXIS_Y ? &offY : &offX;
  float start = *off;
  const uint32_t dur = 170, t0 = millis();
  for (;;) {
    float t = (millis() - t0) / (float)dur;
    if (t >= 1) break;
    float e = 1 - (1 - t) * (1 - t) * (1 - t);
    *off = start + (target - start) * e;
    render();
  }
  *off = target;
}

static void handleTouch() {
  int16_t x[1], y[1];
  uint8_t n = touch.getPoint(x, y, 1);
  { // touch log on the USB serial port (only when something changes)
    static uint8_t ln = 255; static int16_t lx = -1, ly = -1;
    if (n != ln || (n && (x[0] != lx || y[0] != ly))) {
      Serial.printf("T %u %d %d\r\n", n, n ? x[0] : -1, n ? y[0] : -1);
      ln = n; lx = x[0]; ly = y[0];
    }
  }
  xSemaphoreTake(feedLock, portMAX_DELAY);
  int cols = haveData ? colCount() : 1, rows = haveData ? rowCount(col) : 1;
  xSemaphoreGive(feedLock);
  if (n) {
    wake();
    if (!dragging) {
      dragging = true;
      dragAxis = AXIS_NONE;
      touchStartX = lastTouchX = x[0];
      touchStartY = lastTouchY = y[0];
      touchStartMs = millis();
      return;
    }
    if (x[0] == lastTouchX && y[0] == lastTouchY) return; // no movement, nothing to redraw
    lastTouchX = x[0];
    lastTouchY = y[0];
    float dx = x[0] - touchStartX, dy = y[0] - touchStartY;
    if (dragAxis == AXIS_NONE) { // lock to the direction the finger starts moving
      if (fabsf(dx) < 10 && fabsf(dy) < 10) return;
      dragAxis = fabsf(dx) >= fabsf(dy) ? AXIS_X : AXIS_Y;
    }
    if (dragAxis == AXIS_X) {
      if ((col == 0 && dx > 0) || (col == cols - 1 && dx < 0)) dx *= 0.3f; // rubber-band at the ends
      offX = dx;
    } else {
      if ((row == 0 && dy > 0) || (row == rows - 1 && dy < 0)) dy *= 0.3f;
      offY = dy;
    }
    render();
  } else if (dragging) {
    dragging = false;
    if (dragAxis == AXIS_NONE) return; // a tap
    uint32_t dt = millis() - touchStartMs;
    if (dragAxis == AXIS_X) {
      float dx = lastTouchX - touchStartX;
      bool fling = dt < 250 && fabsf(dx) > 30;
      if ((dx < -W / 5 || (fling && dx < 0)) && col + 1 < cols) { animateTo(-W); col++; row = 0; }
      else if ((dx > W / 5 || (fling && dx > 0)) && col > 0) { animateTo(W); col--; row = 0; }
      else animateTo(0);
    } else {
      float dy = lastTouchY - touchStartY;
      bool fling = dt < 250 && fabsf(dy) > 30;
      if ((dy < -H / 5 || (fling && dy < 0)) && row + 1 < rows) { animateTo(-H); row++; }
      else if ((dy > H / 5 || (fling && dy > 0)) && row > 0) { animateTo(H); row--; }
      else animateTo(0);
    }
    offX = offY = 0;
    dragAxis = AXIS_NONE;
    render();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(TP_INT, INPUT_PULLUP);
  feedLock = xSemaphoreCreateMutex();
  prefs.begin("spooldisp", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgServer = prefs.getString("server", "");

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!g->begin(80000000)) Serial.println("display init failed"); // 80 MHz QSPI
  for (int i = 0; i < CACHE_SLOTS; i++) cacheBuf[i] = (uint16_t *)ps_malloc(W * H * 2);
  strip = (uint16_t *)heap_caps_malloc(W * STRIP_ROWS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g->setUTF8Print(true);
  panel->setBrightness(BRIGHT);

  // Hard-reset the touch chip on every start: a software reboot of the ESP32 doesn't reset it,
  // so a chip that got stuck would otherwise stay stuck until the board loses power.
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(20);
  digitalWrite(TP_RST, HIGH);
  delay(120);
  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(W, H);
    touch.setMirrorXY(true, true);
  } else {
    Serial.println("touch init failed");
  }

  lastInteractMs = millis();
  if (cfgSsid.isEmpty() || cfgServer.isEmpty()) {
    renderSetup();
    for (;;) { checkSerialSetup(); delay(20); }
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 0);
  render();
}

void loop() {
  checkSerialSetup();
  handleTouch();

  static bool lastBtn = true;
  bool btn = digitalRead(BOOT_BTN);
  if (!btn && lastBtn) { // BOOT button: jump back home
    wake();
    col = row = 0;
    offX = offY = 0;
    render();
  }
  lastBtn = btn;

  // Redraw only when the data or connection status changed; otherwise pre-draw one neighbor
  // per pass so a touch is never kept waiting behind a pile of drawing.
  static uint32_t shownVer = 0;
  static int shownStatus = -1;
  int status = (WiFi.status() == WL_CONNECTED ? 1 : 0) | (fetchOk ? 2 : 0) | (haveData ? 4 : 0);
  if (!dragging && (shownVer != dataVersion || shownStatus != status)) {
    shownVer = dataVersion;
    shownStatus = status;
    render();
  } else if (!dragging && haveData) {
    prefetchNeighbors();
  }
  if (!dimmed && millis() - lastInteractMs > DIM_AFTER_MS) { panel->setBrightness(DIM); dimmed = true; }
  if (!dragging) delay(5);
}
