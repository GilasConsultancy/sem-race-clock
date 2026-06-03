#include <WiFi.h>
#include <lwip/sockets.h>   // setsockopt / SO_SNDTIMEO for SSE write timeout
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ezTime.h>
#include <HTTPUpdate.h>

// ESP-IDF direct call — lets us rename the mDNS hostname after MDNS.begin()
// without re-initialising the stack (calling MDNS.begin() twice would fail).
extern "C" { esp_err_t mdns_hostname_set(const char* hostname); }

// ─── Hardware configuration ──────────────────────────────────
// Uncomment a line when that hardware is physically connected.
// Each flag pulls in the required library and replaces the
// Serial-only display stubs with real hardware calls.
//
#define HAS_RTC      // DS3231 real-time clock  (needs: RTClib by Adafruit)
//                   //   SDA → GPIO21,  SCL → GPIO22  (hardware I²C)
//
#define HAS_P10      // P10 HUB12 LED panels    (uses DMD32, vendored in src/DMD32/)
//                   //   DATA → GPIO23 (MOSI)  CLK  → GPIO18 (SCK)
//                   //   LATCH→ GPIO5  (SS)    OE   → GPIO4
//                   //   A    → GPIO16         B    → GPIO17
//                   //   2 panels wide × 1 tall = 64 × 16 px total

// Status LEDs — two separate 5 mm LEDs (always compiled in)
#define LED_RED    25
#define LED_GREEN  26

// P10 control pins — documented here for reference; actual values live in
// src/DMD32/DMD32.h (vendored) so the library and firmware stay in sync.
#define P10_PANELS_WIDE  2
#define P10_PANELS_TALL  1
#define P10_PIN_OE       4   // Output-enable, active low
#define P10_PIN_A        16  // Row-select A
#define P10_PIN_B        17  // Row-select B
#define P10_PIN_LATCH    5   // Shift-register latch (VSPI SS)
#define P10_WIDTH       (P10_PANELS_WIDE * 32)  // 64 px — always available

#ifdef HAS_RTC
#include <Wire.h>
#include <RTClib.h>
#endif

#ifdef HAS_P10
// DMD32 — ESP32-native fork of the DMD library, vendored in src/DMD32/
// Pins are set in src/DMD32/DMD32.h.  Scanning is driven by a hardware timer.
#include "src/DMD32/DMD32.h"
#include "src/DMD32/fonts/Arial14.h"
#endif
// ─────────────────────────────────────────────────────────────

// --- Version ---
#define FIRMWARE_VERSION "0.6.0"
#define GITHUB_OWNER     "gilasconsultancy"
#define GITHUB_REPO      "sem-race-clock"

// --- Configuration ---
static const char* BASE_HOSTNAME = "raceclock";  // base mDNS name shared by all devices

#define MAX_SESSIONS 20

// NVS schema version — increment when the NVS key layout changes and add
// migration logic in setup().  Devices with an older schema are migrated
// automatically on first boot after an update.
#define NVS_SCHEMA 1

// --- Session structure ---
struct Session {
  String type;        // "Prototype" or "Urban Concept"
  int startH, startM;
  int lastStartH, lastStartM;
  int endH, endM;
};

// --- WiFi network list ---
struct WifiNetwork { String ssid, pass; };
static const int WIFI_MAX = 10;
WifiNetwork wifiNets[WIFI_MAX];
int         wifiNetCount  = 0;
int         wifiActiveIdx = -1;   // index of currently-connected network (-1 = none)

// --- Peer discovery ---
static int    deviceNum         = 1;             // persisted in NVS key "device_num"
static String effectiveHostname = "raceclock";   // BASE_HOSTNAME + deviceNum (>1 appends number)
static uint32_t lastPeerScan   = 0;

struct PeerDevice { String hostname; String ip; uint16_t port; };
static const int PEER_MAX = 9;
static PeerDevice peers[PEER_MAX];
static int        peerCount = 0;

// Manual (operator-added) peers — persisted in NVS, visually distinct from mDNS ones.
struct ManualPeer { String ip; uint16_t port; };
static const int MANUAL_PEER_MAX = 5;
static ManualPeer manualPeers[MANUAL_PEER_MAX];
static int        manualPeerCount = 0;

// ── Background mDNS scan (FreeRTOS task, core 1) ──────────────────────────────
// MDNS.queryService() blocks for ~2 s; running it in a task keeps loop() free.
// mdnsScanTrigger() / mdnsScanTaskFn() are defined after resolveAddress() below
// so all their dependencies are already in scope.
static volatile bool      mdnsScanRunning   = false;
static volatile bool      mdnsScanDone      = false;
static volatile bool      peersDirty        = false;
static portMUX_TYPE       peersMux          = portMUX_INITIALIZER_UNLOCKED;
// Forward declarations (bodies follow resolveAddress)
static void mdnsScanTaskFn(void*);
static void mdnsScanTrigger();

// --- Unpaired devices (connected to our AP, waiting to be provisioned) ---
struct UnpairedDevice { String hostname; String ip; uint32_t seenAt; };
static const int UNPAIRED_MAX = 4;
static UnpairedDevice unpaired[UNPAIRED_MAX];
static int            unpairedCount = 0;

// --- Clock states ---
enum ClockState {
  STATE_NO_SESSION,
  STATE_PRE_SESSION,
  STATE_COUNTDOWN,
  STATE_WARNING,
  STATE_LAST_START,
  STATE_TRACK_CLOSED
};

// --- Globals ---
WebServer    server(80);
WiFiServer   sseServer(81);    // SSE event stream on port 81
DNSServer    dnsServer;        // captive portal — redirects all DNS to AP IP
Preferences  prefs;
Timezone     localTZ;

Session      sessions[MAX_SESSIONS];
int          sessionCount   = 0;
int          activeSession  = -1;
ClockState   clockState     = STATE_NO_SESSION;
int          warnMinutes    = 5;
String       currentTZName  = "Europe/Warsaw";
bool         apMode         = false;
String       overrideText   = "";    // if non-empty, bypasses the clock state machine
bool         overrideDirty  = false; // set when overrideText changes; loop acts once then clears
bool         updateRequested      = false; // set by /api/doupdate;    acted on in loop()
bool         fsUpdateRequested    = false; // set by /api/doupdatefs;  acted on in loop()
bool         rebootRequested      = false; // set by /api/wifi/restart; acted on in loop()

#ifdef HAS_RTC
RTC_DS3231   rtc;
bool         rtcAvailable  = false;
bool         rtcSynced     = false;   // true once NTP has written the RTC
#endif

// ── ElegantOTA credentials (loaded from NVS; default admin/raceclock) ────────
static String otaUser = "admin";
static String otaPass = "raceclock";

// ── In-memory diagnostic log ─────────────────────────────────────────────────
// 40 lines × 96 chars = 3 840 bytes.  Exposed via GET /api/log so field
// failures can be diagnosed without a USB cable.
// NOT thread-safe — call logf() only from the main loop task (core 1).
#define LOG_LINES 40
#define LOG_WIDTH 96
static char logBuf[LOG_LINES][LOG_WIDTH];
static int  logHead  = 0;
static int  logCount = 0;

void logf(const char* fmt, ...) {
  char msg[LOG_WIDTH - 12];   // reserve space for the timestamp prefix
  va_list a; va_start(a, fmt); vsnprintf(msg, sizeof(msg), fmt, a); va_end(a);
  Serial.println(msg);
  uint32_t ms = millis();
  snprintf(logBuf[logHead], LOG_WIDTH, "%6lu.%03lu %s", ms / 1000, ms % 1000, msg);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

// ============================================================
//  STATUS LED
// ============================================================
enum LedMode { LED_MODE_OFF, LED_GREEN_SOLID, LED_GREEN_BLINK, LED_RED_SOLID };
static LedMode   ledMode       = LED_MODE_OFF;
static uint32_t  ledLastToggle = 0;
static bool      ledBlinkOn    = false;

void updateLed() {
  LedMode desired;
  if (apMode || WiFi.status() != WL_CONNECTED)
    desired = LED_RED_SOLID;
  else if (timeStatus() == timeSet)
    desired = LED_GREEN_SOLID;
  else
    desired = LED_GREEN_BLINK;

  if (desired != ledMode) {
    ledMode       = desired;
    ledBlinkOn    = true;
    ledLastToggle = millis();
  }

  switch (ledMode) {
    case LED_GREEN_SOLID:
      digitalWrite(LED_RED, LOW);  digitalWrite(LED_GREEN, HIGH); break;
    case LED_RED_SOLID:
      digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, LOW);  break;
    case LED_GREEN_BLINK:
      if (millis() - ledLastToggle >= 1000) {
        ledLastToggle = millis();
        ledBlinkOn    = !ledBlinkOn;
        digitalWrite(LED_GREEN, ledBlinkOn ? HIGH : LOW);
        digitalWrite(LED_RED,   LOW);
      }
      break;
    default:
      digitalWrite(LED_RED, LOW);  digitalWrite(LED_GREEN, LOW);  break;
  }
}
// ============================================================

// ============================================================
//  DISPLAY LAYER
// ============================================================
#ifdef HAS_P10
// Hardware SPI: DATA=GPIO23(MOSI), CLK=GPIO18(SCK), LATCH=GPIO5(SS)
DMD dmd(P10_PANELS_WIDE, P10_PANELS_TALL);

// Hardware timer drives the display scan at ~300 µs intervals (≈333 Hz ÷ 4 rows
// = ≈83 Hz refresh), well above the flicker threshold.
static hw_timer_t* p10Timer = NULL;
void IRAM_ATTR p10ScanISR() { dmd.scanDisplayBySPI(); }

// DMD32 omits stringWidth — implement it from the font header.
// Font layout: size(2) fixedWidth(1) height(1) firstChar(1) charCount(1)
//              charWidths[charCount]  then bitmap data.
static int p10StringWidth(const char* str) {
  const uint8_t* f = Arial_14;   // pointer avoids typeof(array[]) in pgm_read macros
  int w = 0;
  uint16_t fsize  = pgm_read_word(f);
  uint8_t  fFirst = pgm_read_byte(f + 4);
  uint8_t  fCount = pgm_read_byte(f + 5);
  for (; *str; str++) {
    uint8_t c = (uint8_t)*str;
    if (c < fFirst || c >= fFirst + fCount) continue;
    if (fsize == 0)                              // fixed-width
      w += pgm_read_byte(f + 2);
    else                                         // variable-width
      w += pgm_read_byte(f + 6 + (c - fFirst));
  }
  return w;
}

static String   p10ScrollText;
static bool     p10Scrolling  = false;
static int16_t  p10ScrollX    = 0;
static uint32_t p10LastScroll = 0;

static void p10StartScroll(const String& msg) {
  p10ScrollText = msg;
  p10Scrolling  = true;
  p10ScrollX    = P10_WIDTH;    // start off the right edge
  p10LastScroll = millis();
  dmd.clearScreen(true);
}

// Call every loop iteration — advances scroll by 2 px every 50 ms.
void p10UpdateScroll() {
  if (!p10Scrolling) return;
  if (millis() - p10LastScroll < 50) return;
  p10LastScroll = millis();
  dmd.clearScreen(true);
  dmd.selectFont(Arial_14);
  // y=1: centres the 14-tall font in the 16-row panel
  dmd.drawString(p10ScrollX, 1, p10ScrollText.c_str(), p10ScrollText.length(), GRAPHICS_NORMAL);
  int tw = p10StringWidth(p10ScrollText.c_str());
  p10ScrollX -= 2;
  if (p10ScrollX < -tw) p10ScrollX = P10_WIDTH;   // loop
}

// ── Custom pixel fonts ────────────────────────────────────────────────────────

// Large digit font: 6 wide × 9 tall.
// Each byte encodes one row; bit 5 = leftmost pixel, bit 0 = rightmost.
static const uint8_t DIGIT_FONT[10][9] = {
  { 0x1E, 0x3F, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x1E }, // 0
  { 0x1C, 0x3C, 0x2C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x3F }, // 1
  { 0x1E, 0x3F, 0x33, 0x03, 0x06, 0x0C, 0x18, 0x3F, 0x3F }, // 2
  { 0x1E, 0x3F, 0x33, 0x0F, 0x0F, 0x03, 0x33, 0x3F, 0x1E }, // 3
  { 0x30, 0x33, 0x33, 0x33, 0x3F, 0x3F, 0x03, 0x03, 0x03 }, // 4
  { 0x3F, 0x3F, 0x30, 0x3E, 0x3F, 0x03, 0x33, 0x3F, 0x1E }, // 5
  { 0x1E, 0x3F, 0x30, 0x3E, 0x3F, 0x33, 0x33, 0x3F, 0x1E }, // 6
  { 0x3E, 0x3E, 0x06, 0x06, 0x0F, 0x0F, 0x06, 0x06, 0x06 }, // 7
  { 0x1E, 0x3F, 0x33, 0x3F, 0x3F, 0x33, 0x33, 0x3F, 0x1E }, // 8
  { 0x1E, 0x3F, 0x33, 0x33, 0x3F, 0x1F, 0x03, 0x3F, 0x1E }, // 9
};

// Draw one large digit at pixel position (x, y).
static void drawCustomDigit(int x, int y, char d) {
  if (d < '0' || d > '9') return;
  const uint8_t* rows = DIGIT_FONT[d - '0'];
  for (int row = 0; row < 9; row++) {
    uint8_t bits = rows[row];
    for (int col = 0; col < 6; col++)
      dmd.writePixel(x + col, y + row, GRAPHICS_NORMAL,
                     (bits >> (5 - col)) & 1);
  }
}

// Draw colon glyph at (x, y): two 2×2 dot groups, each offset 1 px from the
// left edge of the 4-wide colon gap.  Upper dots at row+2..+3, lower at +5..+6.
static void drawCustomColon(int x, int y) {
  dmd.writePixel(x + 1, y + 2, GRAPHICS_NORMAL, true);
  dmd.writePixel(x + 2, y + 2, GRAPHICS_NORMAL, true);
  dmd.writePixel(x + 1, y + 3, GRAPHICS_NORMAL, true);
  dmd.writePixel(x + 2, y + 3, GRAPHICS_NORMAL, true);
  dmd.writePixel(x + 1, y + 5, GRAPHICS_NORMAL, true);
  dmd.writePixel(x + 2, y + 5, GRAPHICS_NORMAL, true);
  dmd.writePixel(x + 1, y + 6, GRAPHICS_NORMAL, true);
  dmd.writePixel(x + 2, y + 6, GRAPHICS_NORMAL, true);
}

// Small label font: 3 wide × 5 tall.  Bit 2 = leftmost pixel.
struct LabelGlyph { char ch; uint8_t rows[5]; };
static const LabelGlyph LABEL_FONT[] = {
  { 'P', { 0x7, 0x5, 0x7, 0x4, 0x4 } },  // Prototype
  { 'r', { 0x0, 0x7, 0x4, 0x4, 0x4 } },
  { 'o', { 0x0, 0x7, 0x5, 0x5, 0x7 } },
  { 't', { 0x2, 0x7, 0x2, 0x2, 0x3 } },
  { 'y', { 0x0, 0x5, 0x7, 0x1, 0x3 } },
  { 'p', { 0x0, 0x7, 0x5, 0x7, 0x4 } },
  { 'e', { 0x0, 0x7, 0x5, 0x6, 0x7 } },
  { 'U', { 0x5, 0x5, 0x5, 0x5, 0x7 } },  // Urban Concept
  { 'b', { 0x4, 0x4, 0x7, 0x5, 0x7 } },
  { 'a', { 0x0, 0x7, 0x3, 0x5, 0x7 } },
  { 'n', { 0x0, 0x7, 0x5, 0x5, 0x5 } },
  { 'C', { 0x7, 0x5, 0x4, 0x5, 0x7 } },
  { 'c', { 0x0, 0x7, 0x4, 0x4, 0x7 } },
};
static const int LABEL_FONT_COUNT = (int)(sizeof(LABEL_FONT) / sizeof(LABEL_FONT[0]));

// Pixel width of a label string.
// Between letters: 1 px gap.  Space character: 4 px gap (replaces letter + gap).
static int labelPixelWidth(const String& text) {
  int w = 0;
  bool first = true;
  for (int i = 0; i < (int)text.length(); i++) {
    char c = text[i];
    if (c == ' ') { w += 4; first = true; }
    else          { if (!first) w += 1; w += 3; first = false; }
  }
  return w;
}

// Draw a label string with its top-left corner at (x, y).
static void drawCustomLabel(const String& text, int x, int y) {
  int cx = x;
  bool first = true;
  for (int i = 0; i < (int)text.length(); i++) {
    char ch = text[i];
    if (ch == ' ') { cx += 4; first = true; continue; }
    if (!first) cx += 1;
    first = false;
    const LabelGlyph* g = nullptr;
    for (int k = 0; k < LABEL_FONT_COUNT; k++)
      if (LABEL_FONT[k].ch == ch) { g = &LABEL_FONT[k]; break; }
    if (g) {
      for (int row = 0; row < 5; row++) {
        uint8_t bits = g->rows[row];
        for (int col = 0; col < 3; col++)
          dmd.writePixel(cx + col, y + row, GRAPHICS_NORMAL,
                         (bits >> (2 - col)) & 1);
      }
    }
    cx += 3;
  }
}

// Draw countdown string "HH:MM:SS" using the large digit font.
// Display rows 0–4: label (caller draws separately).
// Display rows 5–6: blank separator.
// Display rows 7–15: time glyphs (9 rows tall).
//
// Horizontal layout (total 64 px):
//   8px margin | D0(6) 1px | D1(6) | 4px colon | D2(6) 1px | D3(6) | 4px colon | D4(6) 1px | D5(6) | 9px margin
static void drawCustomTime(const String& t) {
  if ((int)t.length() < 8) return;
  const int y = 7;
  drawCustomDigit( 8, y, t[0]);   // hours tens
  drawCustomDigit(15, y, t[1]);   // hours units
  drawCustomColon(21, y);         // first ':'  (4-wide gap at x=21..24)
  drawCustomDigit(25, y, t[3]);   // minutes tens
  drawCustomDigit(32, y, t[4]);   // minutes units
  drawCustomColon(38, y);         // second ':' (4-wide gap at x=38..41)
  drawCustomDigit(42, y, t[6]);   // seconds tens
  drawCustomDigit(49, y, t[7]);   // seconds units
}

#endif  // HAS_P10

void displayMessage(const String& msg) {
#ifdef HAS_P10
  p10Scrolling = false;
  dmd.selectFont(Arial_14);
  int tw = p10StringWidth(msg.c_str());
  if (tw > P10_WIDTH) {          // too wide — scroll instead
    p10StartScroll(msg);
    return;
  }
  dmd.clearScreen(true);
  // Arial_14 is 14 px tall; centre vertically in 16-row panel → y=1
  dmd.drawString((P10_WIDTH - tw) / 2, 1, msg.c_str(), msg.length(), GRAPHICS_NORMAL);
#else
  Serial.println("[DISPLAY] " + msg);
#endif
}

void displayCountdown(const String& timeStr, const String& sessionType) {
#ifdef HAS_P10
  p10Scrolling = false;
  dmd.clearScreen(true);
  int lw = labelPixelWidth(sessionType);
  int lx = (P10_WIDTH - lw) / 2;
  drawCustomLabel(sessionType, lx, 0);
  drawCustomTime(timeStr);
#else
  Serial.println("[DISPLAY] " + sessionType + " | " + timeStr);
#endif
}

// showTime=true  → label + "00:00:00" (LAST_START on-phase)
// showTime=false → label only, time area blank (WARNING off-phase / LAST_START off-phase)
void displayBlink(bool showTime, const String& sessionType) {
#ifdef HAS_P10
  p10Scrolling = false;
  dmd.clearScreen(true);
  int lw = labelPixelWidth(sessionType);
  int lx = (P10_WIDTH - lw) / 2;
  drawCustomLabel(sessionType, lx, 0);
  if (showTime) drawCustomTime("00:00:00");
#else
  Serial.println(showTime
    ? "[DISPLAY] " + sessionType + " | 00:00:00"
    : "[DISPLAY] " + sessionType + " |        ");
#endif
}

void displayScroll(const String& msg) {
#ifdef HAS_P10
  p10StartScroll(msg);
#else
  Serial.println("[DISPLAY SCROLL] " + msg);
#endif
}
// ============================================================

// --- Time helpers ---
int toMins(int h, int m) { return h * 60 + m; }

bool parseTime(const String& s, int& h, int& m) {
  if (s.length() < 5) return false;
  h = s.substring(0, 2).toInt();
  m = s.substring(3, 5).toInt();
  if (h > 23 || m > 59) return false;
  return true;
}

void sortSessions() {
  for (int i = 0; i < sessionCount - 1; i++) {
    for (int j = i + 1; j < sessionCount; j++) {
      if (toMins(sessions[j].startH, sessions[j].startM) <
          toMins(sessions[i].startH, sessions[i].startM)) {
        Session tmp = sessions[i];
        sessions[i] = sessions[j];
        sessions[j] = tmp;
      }
    }
  }
}

String formatCountdown(int totalSeconds) {
  if (totalSeconds < 0) totalSeconds = 0;
  int h = totalSeconds / 3600;
  int m = (totalSeconds % 3600) / 60;
  int s = totalSeconds % 60;
  char buf[12];
  sprintf(buf, "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

// --- Determine active session ---
void updateActiveSession() {
  if (sessionCount == 0) { activeSession = -1; return; }
  // Use ezTime's timezone-aware accessors — localtime() on ESP32 is always UTC
  int nowSecs = localTZ.hour() * 3600 + localTZ.minute() * 60 + localTZ.second();

  activeSession = -1;
  for (int i = 0; i < sessionCount; i++) {
    int endSecs = sessions[i].endH * 3600 + sessions[i].endM * 60;
    if (nowSecs < endSecs) {
      activeSession = i;
      break;
    }
  }
}

// --- Determine clock state ---
ClockState computeState(int& remainingSeconds) {
  if (sessionCount == 0) { remainingSeconds = 0; return STATE_NO_SESSION; }
  if (activeSession < 0) { remainingSeconds = 0; return STATE_TRACK_CLOSED; }

  Session& s = sessions[activeSession];
  // Use ezTime's timezone-aware accessors — localtime() on ESP32 is always UTC
  int nowSecs       = localTZ.hour() * 3600 + localTZ.minute() * 60 + localTZ.second();
  int startSecs     = s.startH * 3600 + s.startM * 60;
  int lastStartSecs = s.lastStartH * 3600 + s.lastStartM * 60;
  int endSecs       = s.endH * 3600 + s.endM * 60;

  if (nowSecs < startSecs) {
    remainingSeconds = lastStartSecs - nowSecs;
    if (remainingSeconds < 0) remainingSeconds += 86400;
    return STATE_PRE_SESSION;
  }
  if (nowSecs < lastStartSecs) {
    remainingSeconds = lastStartSecs - nowSecs;
    if (remainingSeconds <= warnMinutes * 60) return STATE_WARNING;
    return STATE_COUNTDOWN;
  }
  if (nowSecs < endSecs) {
    remainingSeconds = 0;
    return STATE_LAST_START;
  }
  remainingSeconds = 0;
  return STATE_TRACK_CLOSED;
}

// --- Save/load sessions — stored in NVS (survives LittleFS uploads) ---
void saveSessions() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < sessionCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["type"]      = sessions[i].type;
    char buf[6];
    sprintf(buf, "%02d:%02d", sessions[i].startH,     sessions[i].startM);
    o["start"]     = buf;
    sprintf(buf, "%02d:%02d", sessions[i].lastStartH, sessions[i].lastStartM);
    o["lastStart"] = buf;
    sprintf(buf, "%02d:%02d", sessions[i].endH,        sessions[i].endM);
    o["end"]       = buf;
  }
  String json;
  serializeJson(doc, json);
  prefs.putString("sessions", json);
}

void loadSessions() {
  sessionCount = 0;
  String json = prefs.getString("sessions", "[]");
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject o : arr) {
    if (sessionCount >= MAX_SESSIONS) break;
    sessions[sessionCount].type = o["type"].as<String>();
    parseTime(o["start"].as<String>(),
              sessions[sessionCount].startH,     sessions[sessionCount].startM);
    parseTime(o["lastStart"].as<String>(),
              sessions[sessionCount].lastStartH, sessions[sessionCount].lastStartM);
    parseTime(o["end"].as<String>(),
              sessions[sessionCount].endH,        sessions[sessionCount].endM);
    sessionCount++;
  }
  sortSessions();
}

// --- Apply timezone (non-blocking — NTP is already running) ---
bool applyTimezone(const String& ianaName) {
  if (!localTZ.setLocation(ianaName)) return false;
  currentTZName = ianaName;
  prefs.putString("tz", ianaName);
  // Cache the POSIX rule so DST offsets are correct on the next cold boot
  // even if the network is not yet available (pairs with the load in setup).
  String posix = localTZ.getPosix();
  if (posix.length() > 0) prefs.putString("posix", posix);
  return true;
}

// --- Serve static files from LittleFS ---
void serveFile(const String& path, const String& contentType) {
  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "Not found: " + path);
    return;
  }
  File f = LittleFS.open(path, "r");
  server.streamFile(f, contentType);
  f.close();
}

// --- File upload (replace a web file in LittleFS over HTTP) ---
static File uploadFile;

void handleFileUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String name = upload.filename;
    if (!name.startsWith("/")) name = "/" + name;
    // Whitelist: only known web files can be overwritten
    if (name != "/index.html" && name != "/style.css" && name != "/webonly.html") {
      Serial.println("Upload rejected (unknown file): " + name);
      return;
    }
    Serial.println("Upload start: " + name);
    uploadFile = LittleFS.open(name, "w");

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);

  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload complete: %u bytes\n", upload.totalSize);
    }
  }
}

// --- SEM schedule import ---
// Fetches a URL over HTTPS and returns the body. Certificate validation is
// skipped (setInsecure) — acceptable for a known public read-only API.
bool fetchURL(const String& url, String& body) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(8000);
  bool ok = (http.GET() == 200);
  if (ok) body = http.getString();
  http.end();
  return ok;
}

// Fetch current event + its full schedule. Populates eventName and schDoc.
bool fetchSEMSchedule(String& eventName, JsonDocument& schDoc) {
  // Step 1 — current event
  String evJson;
  if (!fetchURL("https://results.sem-app.com/events/current.json", evJson)) return false;
  JsonDocument evDoc;
  if (deserializeJson(evDoc, evJson)) return false;
  if (!evDoc["success"] || evDoc["totalCount"].as<int>() == 0) return false;
  eventName      = evDoc["data"][0]["name"].as<String>();
  String eventId = evDoc["data"][0]["eventId"].as<String>();

  // Step 2 — full schedule for that event
  String schJson;
  if (!fetchURL("https://results.sem-app.com/" + eventId + "/schedule.json", schJson)) return false;
  return !deserializeJson(schDoc, schJson);
}

// Returns true if a schedule item is a track session we care about:
// must mention "Prototype" or "Urban Concept" AND have "Last Start:" in notes.
bool isSEMTrackSession(const String& activity, const String& notes) {
  return notes.indexOf("Last Start:") >= 0 &&
         (activity.indexOf("Prototype") >= 0 || activity.indexOf("Urban Concept") >= 0);
}

// GET /api/sem/schedule — returns event name + all track-session days in one fetch.
// Replaces the old /api/sem/days + /api/sem/sessions pair (which fetched the
// external SEM API twice).  The browser selects the desired day client-side.
//
// Response: { eventName, days: [ { date, day, sessions: [{type,start,lastStart,end}] } ] }
// Only days that contain at least one track session are included.
//
// The result is cached in RAM for the lifetime of the boot — the current event
// and its schedule don't change during a race day.  Pass ?refresh=1 to force
// a fresh fetch (e.g. after midnight when a new event day begins).
static String semScheduleCache;   // empty = not yet fetched this boot

void handleSEMSchedule() {
  // Require an NTP sync as proof of internet reachability.
  // WiFi can be connected to a local network with no WAN — in that case
  // fetchSEMSchedule would block for up to 16 s before failing.
  if (apMode || WiFi.status() != WL_CONNECTED || timeStatus() != timeSet) {
    server.send(503, "application/json", "{\"error\":\"No internet — cannot reach SEM API\"}");
    return;
  }

  bool refresh = (server.arg("refresh") == "1");
  if (!semScheduleCache.isEmpty() && !refresh) {
    server.send(200, "application/json", semScheduleCache);
    return;
  }

  String eventName;
  JsonDocument schDoc;
  if (!fetchSEMSchedule(eventName, schDoc)) {
    server.send(503, "application/json", "{\"error\":\"Could not fetch SEM schedule\"}");
    return;
  }

  JsonDocument out;
  out["eventName"] = eventName;
  JsonArray days = out["days"].to<JsonArray>();

  for (JsonObject day : schDoc["data"].as<JsonArray>()) {
    // Collect track sessions for this day first
    JsonDocument tmp;
    JsonArray tmpSessions = tmp.to<JsonArray>();
    for (JsonObject item : day["schedule"].as<JsonArray>()) {
      String activity = item["activity"] | "";
      String notes    = item["notes"]    | "";
      if (!isSEMTrackSession(activity, notes)) continue;
      String type = (activity.indexOf("Prototype") >= 0) ? "Prototype" : "Urban Concept";
      int lsIdx        = notes.indexOf("Last Start:") + 12;
      String lastStart = notes.substring(lsIdx, lsIdx + 5);
      lastStart.trim();
      // Guard: skip this session if the extracted time doesn't look like HH:MM.
      // Protects against SEM API format changes silently producing garbage data.
      if (lastStart.length() != 5 || lastStart[2] != ':') {
        Serial.println("[SEM] Skipping session — malformed lastStart: '" + lastStart + "'");
        continue;
      }
      JsonObject s  = tmpSessions.add<JsonObject>();
      s["type"]      = type;
      s["start"]     = item["timeStart"];
      s["lastStart"] = lastStart;
      s["end"]       = item["timeEnd"];
    }
    if (tmpSessions.size() == 0) continue;   // skip days with no track sessions

    JsonObject d       = days.add<JsonObject>();
    d["date"]          = day["date"];
    d["day"]           = day["day"];
    JsonArray outSess  = d["sessions"].to<JsonArray>();
    for (JsonObject s : tmpSessions) outSess.add(s);
  }

  serializeJson(out, semScheduleCache);   // store for subsequent calls this boot
  server.send(200, "application/json", semScheduleCache);
}

void handleGetTime() {
  JsonDocument doc;
  doc["epoch"]  = (long)UTC.now();   // must be UTC — JS applies the timezone offset itself
  doc["synced"] = (timeStatus() == timeSet);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// --- API: GET /api/peers ---
// Returns a JSON array of discovered peer clocks on the local network.
// Uses a 30-second cache to avoid blocking the main thread too often.
void handleGetPeers() {
  // Trigger a background scan if the cache is stale — returns immediately.
  // The browser will get fresh results on the next poll after the task completes.
  if (millis() - lastPeerScan > 30000 && !apMode && WiFi.status() == WL_CONNECTED)
    mdnsScanTrigger();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < peerCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["hostname"] = peers[i].hostname;
    o["ip"]       = peers[i].ip;
    o["port"]     = peers[i].port;
    o["manual"]   = false;
  }
  for (int i = 0; i < manualPeerCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["hostname"] = "";
    o["ip"]       = manualPeers[i].ip;
    o["port"]     = manualPeers[i].port;
    o["manual"]   = true;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// --- API: POST /api/peers/push ---
// Pushes this device's full session list to every known peer clock.
// The browser calls this same-origin endpoint; the firmware does the
// peer-to-peer HTTP so no cross-origin CORS preflight is needed.
void handlePeersPush() {
  int totalPeers = peerCount + manualPeerCount;
  if (totalPeers == 0) {
    server.send(200, "application/json", "{\"pushed\":0,\"skipped\":0}");
    return;
  }

  int pushed = 0, skipped = 0;
  WiFiClient client;

  for (int p = 0; p < totalPeers; p++) {
    String ip   = (p < peerCount) ? peers[p].ip             : manualPeers[p - peerCount].ip;
    uint16_t pt = (p < peerCount) ? peers[p].port           : manualPeers[p - peerCount].port;
    String base = "http://" + ip + ":" + String(pt);
    HTTPClient http;
    http.setTimeout(3000);   // peers are on the local network — 3 s is generous

    // Step 1 — clear sessions on peer
    http.begin(client, base + "/api/sessions/clear");
    http.addHeader("Content-Type", "application/json");
    int rc = http.POST("");
    http.end();
    if (rc < 200 || rc > 299) { skipped++; continue; }

    // Step 2 — push each session individually
    bool ok = true;
    for (int i = 0; i < sessionCount && ok; i++) {
      JsonDocument body;
      body["index"] = -1;
      JsonObject s  = body["session"].to<JsonObject>();
      s["type"]     = sessions[i].type;
      char buf[6];
      sprintf(buf, "%02d:%02d", sessions[i].startH,     sessions[i].startM);     s["start"]     = buf;
      sprintf(buf, "%02d:%02d", sessions[i].lastStartH, sessions[i].lastStartM); s["lastStart"] = buf;
      sprintf(buf, "%02d:%02d", sessions[i].endH,        sessions[i].endM);       s["end"]       = buf;
      String bodyStr; serializeJson(body, bodyStr);

      http.begin(client, base + "/api/sessions");
      http.addHeader("Content-Type", "application/json");
      rc = http.POST(bodyStr);
      http.end();
      if (rc < 200 || rc > 299) ok = false;
    }
    if (ok) pushed++; else skipped++;
  }

  server.send(200, "application/json",
    "{\"pushed\":" + String(pushed) + ",\"skipped\":" + String(skipped) + "}");
}

// ── Zero-config device provisioning ──────────────────────────────────────────
// An unconfigured device joins this device's AP, announces itself via
// POST /api/provision, and waits.  The operator clicks "Adapt" in the web UI
// which calls POST /api/adapt — this device pushes full config to the new device.

// POST /api/provision  { hostname, ip }  — called by the unconfigured device
void handleProvision() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  String hostname = doc["hostname"] | "raceclock";
  String ip       = doc["ip"]       | "";
  if (ip.isEmpty()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ip required\"}"); return;
  }
  // Update existing entry or add new one
  for (int i = 0; i < unpairedCount; i++) {
    if (unpaired[i].ip == ip) {
      unpaired[i].hostname = hostname;
      unpaired[i].seenAt   = millis();
      server.send(200, "application/json", "{\"ok\":true}");
      ssePush("unpaired_changed");
      return;
    }
  }
  if (unpairedCount < UNPAIRED_MAX) {
    unpaired[unpairedCount] = { hostname, ip, millis() };
    unpairedCount++;
    ssePush("unpaired_changed");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// GET /api/unpaired — returns list of unconfigured devices waiting to be adapted
void handleGetUnpaired() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < unpairedCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["hostname"] = unpaired[i].hostname;
    o["ip"]       = unpaired[i].ip;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/adapt  { ip }  — push full config to an unpaired device then restart it
void handleAdapt() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}"); return;
  }
  String targetIp = doc["ip"] | "";
  if (targetIp.isEmpty()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ip required\"}"); return;
  }

  WiFiClient client;
  HTTPClient  http;
  // setTimeout persists across begin()/end() cycles on the same HTTPClient instance.
  http.setTimeout(3000);   // peers are on the local network — 3 s is generous
  String base = "http://" + targetIp;
  int errors = 0;

  // 1. WiFi networks
  for (int i = 0; i < wifiNetCount; i++) {
    JsonDocument body;
    body["ssid"]     = wifiNets[i].ssid;
    body["password"] = wifiNets[i].pass;
    String s; serializeJson(body, s);
    http.begin(client, base + "/api/wifi/add");
    http.addHeader("Content-Type", "application/json");
    int rc = http.POST(s); http.end();
    if (rc < 200 || rc > 299) {
      Serial.printf("[adapt] wifi/add failed for SSID %s — rc=%d\n",
                    wifiNets[i].ssid.c_str(), rc);
      errors++;
    }
  }

  // 2. Settings (timezone + warn threshold)
  { JsonDocument body;
    body["tz"]          = currentTZName;
    body["warnMinutes"] = warnMinutes;
    String s; serializeJson(body, s);
    http.begin(client, base + "/api/settings");
    http.addHeader("Content-Type", "application/json");
    int rc = http.POST(s); http.end();
    if (rc < 200 || rc > 299) {
      Serial.printf("[adapt] settings failed — rc=%d\n", rc);
      errors++;
    }
  }

  // 3. Sessions — only push if WiFi credentials were delivered successfully.
  // Skipping here avoids sending sessions to a device that may never connect.
  if (errors == 0) {
    http.begin(client, base + "/api/sessions/clear");
    http.addHeader("Content-Type", "application/json");
    int rc = http.POST(""); http.end();
    if (rc < 200 || rc > 299) {
      Serial.printf("[adapt] sessions/clear failed — rc=%d\n", rc);
      errors++;
    }
    for (int i = 0; i < sessionCount && errors == 0; i++) {
      JsonDocument body;
      body["index"] = -1;
      JsonObject s  = body["session"].to<JsonObject>();
      s["type"]     = sessions[i].type;
      char buf[6];
      sprintf(buf, "%02d:%02d", sessions[i].startH,     sessions[i].startM);     s["start"]     = buf;
      sprintf(buf, "%02d:%02d", sessions[i].lastStartH, sessions[i].lastStartM); s["lastStart"] = buf;
      sprintf(buf, "%02d:%02d", sessions[i].endH,       sessions[i].endM);       s["end"]       = buf;
      String bodyStr; serializeJson(body, bodyStr);
      http.begin(client, base + "/api/sessions");
      http.addHeader("Content-Type", "application/json");
      rc = http.POST(bodyStr); http.end();
      if (rc < 200 || rc > 299) {
        Serial.printf("[adapt] session %d push failed — rc=%d\n", i, rc);
        errors++;
      }
    }
  }

  if (errors > 0) {
    server.send(500, "application/json",
      "{\"ok\":false,\"error\":\"" + String(errors) + " step(s) failed — see serial log\"}");
    return;
  }

  // 4. Restart — device will reboot with new credentials and join main WiFi
  http.begin(client, base + "/api/wifi/restart");
  http.addHeader("Content-Type", "application/json");
  http.POST(""); http.end();   // best-effort: device may reboot before responding

  // Remove from unpaired list
  for (int i = 0; i < unpairedCount; i++) {
    if (unpaired[i].ip == targetIp) {
      for (int j = i; j < unpairedCount - 1; j++) unpaired[j] = unpaired[j+1];
      unpairedCount--;
      break;
    }
  }
  ssePush("unpaired_changed");
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- API: GET /api/settings ---
void handleGetSettings() {
  JsonDocument doc;
  doc["tz"]          = currentTZName;
  doc["warnMinutes"] = warnMinutes;
  doc["maxSessions"] = MAX_SESSIONS;
  doc["ntpSynced"]   = (timeStatus() == timeSet);
  doc["apMode"]      = apMode;
  doc["hostname"]    = effectiveHostname;
  doc["deviceNum"]   = deviceNum;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// --- API: POST /api/settings ---
void handlePostSettings() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}");
    return;
  }
  String newTZ = doc["tz"].as<String>();
  newTZ.trim();
  int newWarn  = doc["warnMinutes"] | 5;
  if (newWarn < 1)  newWarn = 1;
  if (newWarn > 30) newWarn = 30;

  // Validate timezone: must be non-empty and have IANA form ("Region/City").
  if (newTZ.isEmpty() || newTZ.indexOf('/') < 0) {
    server.send(400, "application/json",
      "{\"ok\":false,\"error\":\"Invalid timezone — expected IANA name e.g. Europe/Warsaw\"}");
    return;
  }

  bool tzChanged = (newTZ != currentTZName);

  // Persist both values before anything else so they survive the reboot.
  prefs.putString("tz", newTZ);
  prefs.putInt("warn", newWarn);
  currentTZName = newTZ;
  warnMinutes   = newWarn;

  if (tzChanged) {
    // Timezone change has too many downstream effects (NTP resync, session
    // comparison, display state).  Reboot cleanly so everything starts fresh.
    server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    displayMessage("REBOOT...");
    delay(500);
    ESP.restart();
    return;
  }

  server.send(200, "application/json", "{\"ok\":true,\"reboot\":false}");
}

// --- API: GET /api/sessions ---
void handleGetSessions() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < sessionCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["type"] = sessions[i].type;
    char buf[6];
    sprintf(buf, "%02d:%02d", sessions[i].startH, sessions[i].startM);
    o["start"] = buf;
    sprintf(buf, "%02d:%02d", sessions[i].lastStartH, sessions[i].lastStartM);
    o["lastStart"] = buf;
    sprintf(buf, "%02d:%02d", sessions[i].endH, sessions[i].endM);
    o["end"] = buf;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ── Server-Sent Events (port 81) ─────────────────────────────────────────────
// A separate WiFiServer so the connection can be held open indefinitely
// without interfering with WebServer's request/response cycle on port 80.
// One client at a time; browser EventSource reconnects automatically.

// ── Server-Sent Events — up to SSE_MAX simultaneous browser connections ───────
// Supports multiple open browsers (e.g. two operators, or management + monitor).
// When all slots are occupied, the oldest client is evicted to make room.
static const int  SSE_MAX = 4;
static WiFiClient sseClients[SSE_MAX];
static bool       sseActive[SSE_MAX]   = {};
static uint32_t   sseLastPing[SSE_MAX] = {};

void sseBegin() { sseServer.begin(); }

// Push a named event to every connected browser (fire-and-forget).
// The 200 ms SO_SNDTIMEO set on accept prevents a stalled client
// from blocking the main loop for more than 200 ms.
void ssePush(const char* type) {
  String msg = String("data: {\"type\":\"") + type + "\"}\n\n";
  for (int i = 0; i < SSE_MAX; i++) {
    if (!sseActive[i]) continue;
    if (!sseClients[i].connected()) { sseActive[i] = false; sseClients[i].stop(); continue; }
    sseClients[i].print(msg);
    sseLastPing[i] = millis();
  }
}

// Call from loop() — accepts new connections and maintains all active clients.
void sseLoop() {
  WiFiClient incoming = sseServer.accept();
  if (incoming) {
    // Find a free slot; if full, evict the oldest (slot 0) rather than
    // blocking new connections behind a potentially stale client.
    int slot = -1;
    for (int i = 0; i < SSE_MAX; i++) if (!sseActive[i]) { slot = i; break; }
    if (slot < 0) { sseClients[0].stop(); slot = 0; }

    sseClients[slot]  = incoming;
    sseActive[slot]   = true;
    sseLastPing[slot] = millis();

    int fd = sseClients[slot].fd();
    if (fd >= 0) {
      struct timeval tv = { 0, 200000 };   // 200 ms send timeout
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    sseClients[slot].print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "\r\n"
    );
    sseClients[slot].print("data: {\"type\":\"connected\"}\n\n");
  }

  // Maintain all active slots: drop dead connections, send keepalives.
  for (int i = 0; i < SSE_MAX; i++) {
    if (!sseActive[i]) continue;
    if (!sseClients[i].connected()) { sseActive[i] = false; sseClients[i].stop(); continue; }
    if (millis() - sseLastPing[i] > 15000) {
      sseClients[i].print(": ping\n\n");
      sseLastPing[i] = millis();
    }
  }
}

// --- API: POST /api/sessions (add or edit) ---
void handlePostSession() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}");
    return;
  }
  int idx = doc["index"] | -1;
  JsonObject o = doc["session"];

  Session s;
  s.type = o["type"].as<String>();
  if (!parseTime(o["start"].as<String>(),     s.startH,     s.startM)     ||
      !parseTime(o["lastStart"].as<String>(), s.lastStartH, s.lastStartM) ||
      !parseTime(o["end"].as<String>(),       s.endH,       s.endM)) {
    server.send(400, "application/json",
      "{\"ok\":false,\"error\":\"Invalid time format\"}");
    return;
  }

  // Reject midnight-crossing sessions — same-day time arithmetic doesn't support them
  if (toMins(s.endH, s.endM) <= toMins(s.startH, s.startM)) {
    server.send(400, "application/json",
      "{\"ok\":false,\"error\":\"Sessions cannot cross midnight — end must be later than start on the same day\"}");
    return;
  }

  // Validate time order
  if (toMins(s.startH, s.startM) >= toMins(s.lastStartH, s.lastStartM) ||
      toMins(s.lastStartH, s.lastStartM) >= toMins(s.endH, s.endM)) {
    server.send(400, "application/json",
      "{\"ok\":false,\"error\":\"Times must be: start < last start < end\"}");
    return;
  }

  // Check for overlaps with existing sessions
  int newStart = toMins(s.startH, s.startM);
  int newEnd   = toMins(s.endH,   s.endM);
  for (int i = 0; i < sessionCount; i++) {
    if (i == idx) continue;
    if (newStart < toMins(sessions[i].endH,   sessions[i].endM) &&
        newEnd   > toMins(sessions[i].startH, sessions[i].startM)) {
      server.send(400, "application/json",
        "{\"ok\":false,\"error\":\"Overlaps an existing session\"}");
      return;
    }
  }

  if (idx >= 0 && idx < sessionCount) {
    sessions[idx] = s;   // edit
  } else {
    if (sessionCount >= MAX_SESSIONS) {
      server.send(400, "application/json",
        "{\"ok\":false,\"error\":\"Maximum sessions reached\"}");
      return;
    }
    sessions[sessionCount++] = s;   // add
  }

  sortSessions();
  saveSessions();
  updateActiveSession();
  ssePush("sessions_changed");
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- API: POST /api/sessions/delete ---
void handleDeleteSession() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}"); return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= sessionCount) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  for (int i = idx; i < sessionCount - 1; i++) sessions[i] = sessions[i+1];
  sessionCount--;
  saveSessions();
  updateActiveSession();
  ssePush("sessions_changed");
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- API: POST /api/sessions/clear ---
void handleClearSessions() {
  sessionCount = 0;
  activeSession = -1;
  saveSessions();
  ssePush("sessions_changed");
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- API: GET /api/override ---
void handleGetOverride() {
  JsonDocument doc;
  doc["text"] = overrideText;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// --- API: POST /api/override  { "text": "..." }  (empty string clears) ---
void handlePostOverride() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad JSON\"}"); return;
  }
  overrideText = doc["text"].as<String>();
  overrideText.trim();
  overrideDirty = true;
  ssePush("override_changed");
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- API: GET /api/display ---
void handleGetDisplay() {
  // Override takes priority over the clock state machine
  if (overrideText.length() > 0) {
    bool scroll = (overrideText.length() * 6 > P10_WIDTH); // approx: 6 px/char
    JsonDocument doc;
    doc["state"]   = "OVERRIDE";
    doc["line1"]   = overrideText;
    doc["line2"]   = "";
    doc["blinkMs"] = 0;
    doc["scroll"]  = scroll;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
    return;
  }

  int remaining = 0;
  ClockState state = computeState(remaining);

  String line1, line2, stateStr;
  int blinkMs = 0;

  String sType = (activeSession >= 0) ? sessions[activeSession].type : "";

  switch (state) {
    case STATE_NO_SESSION:
      line1 = "NO SESSION"; stateStr = "NO_SESSION"; break;
    case STATE_PRE_SESSION:
      line1 = "TRACK CLOSED"; stateStr = "PRE_SESSION"; break;
    case STATE_COUNTDOWN:
      line1 = sType; line2 = formatCountdown(remaining);
      stateStr = "COUNTDOWN"; break;
    case STATE_WARNING:
      line1 = sType; line2 = formatCountdown(remaining);
      blinkMs = 750; stateStr = "WARNING"; break;
    case STATE_LAST_START:
      line1 = sType; line2 = "00:00:00";
      blinkMs = 1000; stateStr = "LAST_START"; break;
    case STATE_TRACK_CLOSED:
      line1 = "TRACK CLOSED"; stateStr = "TRACK_CLOSED"; break;
    default:
      stateStr = "UNKNOWN"; break;
  }

  JsonDocument doc;
  doc["state"]   = stateStr;
  doc["line1"]   = line1;
  doc["line2"]   = line2;
  doc["blinkMs"] = blinkMs;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ============================================================
//  FIRMWARE UPDATE  (GitHub Releases via HTTPUpdate)
// ============================================================

// GET /api/version — current compiled-in version
void handleGetVersion() {
  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// GET /api/checkupdate — compare compiled version against version.json on GitHub main branch
void handleCheckUpdate() {
  String raw;
  String url = "https://raw.githubusercontent.com/"
               GITHUB_OWNER "/" GITHUB_REPO "/main/version.json";
  if (!fetchURL(url, raw)) {
    server.send(503, "application/json", "{\"error\":\"Could not reach GitHub\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, raw) || !doc["version"]) {
    server.send(503, "application/json", "{\"error\":\"Invalid version response\"}");
    return;
  }
  String latest  = doc["version"].as<String>();
  String current = FIRMWARE_VERSION;

  // Simple semver comparison: parse X.Y.Z and compare numerically
  int cMaj=0, cMin=0, cPat=0, lMaj=0, lMin=0, lPat=0;
  sscanf(current.c_str(), "%d.%d.%d", &cMaj, &cMin, &cPat);
  sscanf(latest.c_str(),  "%d.%d.%d", &lMaj, &lMin, &lPat);
  bool newer = (lMaj > cMaj) ||
               (lMaj == cMaj && lMin > cMin) ||
               (lMaj == cMaj && lMin == cMin && lPat > cPat);

  JsonDocument out;
  out["current"]         = current;
  out["latest"]          = latest;
  out["updateAvailable"] = newer;
  String result; serializeJson(out, result);
  server.send(200, "application/json", result);
}

// POST /api/doupdate — queue a firmware download + OTA install from GitHub Releases
// The actual update runs in loop() so this response can be sent first.
void handleDoUpdate() {
  server.send(200, "application/json", "{\"ok\":true}");
  updateRequested = true;
}

void handleDoUpdateFS() {
  server.send(200, "application/json", "{\"ok\":true}");
  fsUpdateRequested = true;
}

// Called from loop() when updateRequested is set.
void performFirmwareUpdate() {
  updateRequested = false;

  // Fetch latest version so we can build the asset URL
  String raw;
  if (!fetchURL("https://raw.githubusercontent.com/"
                GITHUB_OWNER "/" GITHUB_REPO "/main/version.json", raw)) {
    Serial.println("[OTA] Could not fetch version.json");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, raw)) { Serial.println("[OTA] Bad version.json"); return; }
  String latest = doc["version"].as<String>();

  String url = "https://github.com/" GITHUB_OWNER "/" GITHUB_REPO
               "/releases/download/v" + latest + "/firmware.bin";
  Serial.println("[OTA] Downloading: " + url);
  ssePush("update_start");

#ifdef HAS_P10
  displayMessage("OTA...");
#endif

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = HTTP_UPDATE_FAILED;
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (attempt > 1) {
      logf("[OTA] Attempt 1 failed — retrying in 5 s");
      ssePush("update_retry");
      delay(5000);
    }
    ret = httpUpdate.update(client, url);
    if (ret != HTTP_UPDATE_FAILED) break;
    logf("[OTA] Attempt %d failed: %s", attempt,
         httpUpdate.getLastErrorString().c_str());
  }

  switch (ret) {
    case HTTP_UPDATE_OK:
      logf("[OTA] Success — rebooting");
      break;                        // device reboots; update_done implicit
    case HTTP_UPDATE_NO_UPDATES:
      logf("[OTA] No update available");
      ssePush("update_failed");
      break;
    case HTTP_UPDATE_FAILED:
      logf("[OTA] Failed after 2 attempts: %s",
           httpUpdate.getLastErrorString().c_str());
      ssePush("update_failed");
      break;
  }
}

// Called from loop() when fsUpdateRequested is set.
void performFilesystemUpdate() {
  fsUpdateRequested = false;

  String raw;
  if (!fetchURL("https://raw.githubusercontent.com/"
                GITHUB_OWNER "/" GITHUB_REPO "/main/version.json", raw)) {
    Serial.println("[FSOTA] Could not fetch version.json");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, raw)) { Serial.println("[FSOTA] Bad version.json"); return; }
  String latest = doc["version"].as<String>();

  String url = "https://github.com/" GITHUB_OWNER "/" GITHUB_REPO
               "/releases/download/v" + latest + "/littlefs.bin";
  Serial.println("[FSOTA] Downloading: " + url);
  ssePush("update_start");

#ifdef HAS_P10
  displayMessage("FS OTA...");
#endif

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = HTTP_UPDATE_FAILED;
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (attempt > 1) {
      logf("[FSOTA] Attempt 1 failed — retrying in 5 s");
      ssePush("update_retry");
      delay(5000);
    }
    ret = httpUpdate.updateSpiffs(client, url);
    if (ret != HTTP_UPDATE_FAILED) break;
    logf("[FSOTA] Attempt %d failed: %s", attempt,
         httpUpdate.getLastErrorString().c_str());
  }

  switch (ret) {
    case HTTP_UPDATE_OK:
      logf("[FSOTA] Success — rebooting");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      logf("[FSOTA] No update available");
      ssePush("update_failed");
      break;
    case HTTP_UPDATE_FAILED:
      logf("[FSOTA] Failed after 2 attempts: %s",
           httpUpdate.getLastErrorString().c_str());
      ssePush("update_failed");
      break;
  }
}
// ── Diagnostic log ───────────────────────────────────────────────────────────
// GET /api/log — returns the in-memory log as a JSON array of strings,
// oldest entry first.  Call from a browser or curl to diagnose field issues
// without needing a USB cable attached to the device.
void handleGetLog() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  int start = (logCount < LOG_LINES) ? 0 : logHead;  // oldest entry
  for (int i = 0; i < logCount; i++)
    arr.add(logBuf[(start + i) % LOG_LINES]);
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ── ElegantOTA credential management ─────────────────────────────────────────
// GET /api/ota-credentials — returns current username (never the password)
void handleGetOtaCreds() {
  JsonDocument doc;
  doc["user"]      = otaUser;
  doc["isDefault"] = (otaUser == "admin" && otaPass == "raceclock");
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/ota-credentials { user, pass } — persists new credentials and reboots
// New credentials take effect after the reboot (ElegantOTA is initialised at boot).
void handleSetOtaCreds() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}"); return;
  }
  String newUser = doc["user"] | "";
  String newPass = doc["pass"] | "";
  newUser.trim();
  if (newUser.isEmpty()) {
    server.send(400, "application/json",
      "{\"ok\":false,\"error\":\"Username cannot be empty\"}"); return;
  }
  prefs.putString("ota_user", newUser);
  prefs.putString("ota_pass", newPass);
  logf("[OTA] Credentials updated for user '%s' — rebooting", newUser.c_str());
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  displayMessage("REBOOT...");
  delay(500);
  ESP.restart();
}

// ── Peer helpers ─────────────────────────────────────────────────────────────
// MDNS.address(i) returns 0.0.0.0 when the A record wasn't included as an
// additional record in the SRV/PTR response (common with non-ESP mDNS
// responders such as Python zeroconf).  Fall back to an explicit A-record
// query for the hostname in that case.
static IPAddress resolveAddress(int i) {
  IPAddress addr = MDNS.address(i);
  if (addr != IPAddress(0, 0, 0, 0)) return addr;
  String h = MDNS.hostname(i);          // e.g. "raceclock2"
  addr = MDNS.queryHost(h.c_str());     // explicit A-record lookup
  if (addr == IPAddress(0, 0, 0, 0)) {
    // Try with ".local" suffix — some stacks need it
    addr = MDNS.queryHost((h + ".local").c_str());
  }
  return addr;
}

// ── mDNS scan task — defined here so resolveAddress() is already in scope ─────
static void mdnsScanTaskFn(void*) {
  PeerDevice tmp[PEER_MAX];
  int        tmpCount = 0;

  int n = MDNS.queryService("raceclock", "tcp");
  for (int i = 0; i < n && tmpCount < PEER_MAX; i++) {
    IPAddress addr = resolveAddress(i);
    if (addr == WiFi.localIP()) continue;
    tmp[tmpCount].hostname = MDNS.hostname(i) + ".local";
    tmp[tmpCount].ip       = addr.toString();
    tmp[tmpCount].port     = MDNS.port(i);
    tmpCount++;
  }
  lastPeerScan = millis();

  // Brief critical section — copy results into the shared array
  taskENTER_CRITICAL(&peersMux);
  peersDirty = (tmpCount != peerCount);
  peerCount  = tmpCount;
  for (int i = 0; i < tmpCount; i++) peers[i] = tmp[i];
  taskEXIT_CRITICAL(&peersMux);

  mdnsScanRunning = false;
  mdnsScanDone    = true;   // loop() picks this up to push SSE if needed
  vTaskDelete(NULL);        // one-shot task: delete self
}

// Trigger a scan if one isn't already running.
static void mdnsScanTrigger() {
  if (mdnsScanRunning) return;
  mdnsScanRunning = true;
  mdnsScanDone    = false;
  // 4 KB stack; priority 1; pinned to core 1 (same as loop()) so FreeRTOS
  // serialises access to MDNS state — no cross-core cache coherence needed.
  xTaskCreatePinnedToCore(mdnsScanTaskFn, "mdnsScan", 4096, NULL, 1, NULL, 1);
}

// ── Peer device-number negotiation ──────────────────────────────────────────
// Scans for other race clocks via mDNS and claims a unique device number.
// Called once during setup() after WiFi connects, before the web server starts.
// Populates the peers[] cache as a side-effect.
void negotiateDeviceNumber() {
  Serial.println("[Peers] Scanning…");
  int n = MDNS.queryService("raceclock", "tcp");
  lastPeerScan = millis();
  bool taken[PEER_MAX + 2] = {};  // index = device number, true = in use
  peerCount = 0;
  for (int i = 0; i < n; i++) {
    IPAddress peerIP = resolveAddress(i);
    if (peerIP == WiFi.localIP()) continue;  // skip self
    String h = MDNS.hostname(i);
    // "raceclock" → 1, "raceclock2" → 2, "raceclock3" → 3, …
    int num = 1;
    if (h.startsWith("raceclock") && h.length() > 9) {
      num = h.substring(9).toInt();
      if (num < 2) num = 1;
    }
    if (num >= 1 && num <= PEER_MAX + 1) taken[num] = true;
    if (peerCount < PEER_MAX) {
      peers[peerCount].hostname = h + ".local";
      peers[peerCount].ip       = peerIP.toString();
      peers[peerCount].port     = MDNS.port(i);
      peerCount++;
    }
  }
  if (!n) Serial.println("[Peers] None found.");

  if (taken[deviceNum]) {
    int prev = deviceNum;
    for (int i = 1; i <= PEER_MAX + 1; i++) {
      if (!taken[i]) { deviceNum = i; break; }
    }
    effectiveHostname = (deviceNum == 1) ? String(BASE_HOSTNAME)
                                         : String(BASE_HOSTNAME) + String(deviceNum);
    Serial.printf("[Peers] Number %d taken — reassigned to %d (%s)\n",
                  prev, deviceNum, effectiveHostname.c_str());
  } else {
    Serial.printf("[Peers] Claiming number %d (%s)\n",
                  deviceNum, effectiveHostname.c_str());
  }
}

// ── WiFi network list helpers ────────────────────────────────────────────────

void saveWifiNetworks() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < wifiNetCount; i++) {
    JsonObject net = arr.add<JsonObject>();
    net["ssid"] = wifiNets[i].ssid;
    net["pass"] = wifiNets[i].pass;
  }
  String out; serializeJson(doc, out);
  prefs.putString("wifi_networks", out);
}

void loadWifiNetworks() {
  wifiNetCount  = 0;
  wifiActiveIdx = -1;
  String raw = prefs.getString("wifi_networks", "");
  if (raw.length() > 0) {
    JsonDocument doc;
    if (!deserializeJson(doc, raw)) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject net : arr) {
        if (wifiNetCount >= WIFI_MAX) break;
        wifiNets[wifiNetCount].ssid = net["ssid"] | "";
        wifiNets[wifiNetCount].pass = net["pass"] | "";
        wifiNetCount++;
      }
    }
  } else {
    // Migrate from v0.4.2 single-network keys
    String oldSsid = prefs.getString("wifi_ssid", "");
    String oldPass = prefs.getString("wifi_password", "");
    if (!oldSsid.isEmpty()) {
      wifiNets[0].ssid = oldSsid;
      wifiNets[0].pass = oldPass;
      wifiNetCount = 1;
      saveWifiNetworks();  // commit to new format
      Serial.println("Migrated single-network credentials to wifi_networks");
    }
    prefs.remove("wifi_ssid");
    prefs.remove("wifi_password");
  }
}

// ── Manual peer persistence ───────────────────────────────────────────────────
void saveManualPeers() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < manualPeerCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ip"]   = manualPeers[i].ip;
    o["port"] = manualPeers[i].port;
  }
  String out; serializeJson(doc, out);
  prefs.putString("manual_peers", out);
}

void loadManualPeers() {
  manualPeerCount = 0;
  String raw = prefs.getString("manual_peers", "[]");
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (manualPeerCount >= MANUAL_PEER_MAX) break;
    String ip = o["ip"] | "";
    if (ip.isEmpty()) continue;
    manualPeers[manualPeerCount].ip   = ip;
    manualPeers[manualPeerCount].port = o["port"] | 80;
    manualPeerCount++;
  }
}

// POST /api/peers/manual — add a peer by IP (persists across reboots)
void handleAddManualPeer() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}"); return;
  }
  String ip = doc["ip"] | "";
  ip.trim();
  if (ip.isEmpty()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ip required\"}"); return;
  }
  for (int i = 0; i < manualPeerCount; i++) {
    if (manualPeers[i].ip == ip) {
      server.send(200, "application/json", "{\"ok\":true}"); return;   // already present
    }
  }
  if (manualPeerCount >= MANUAL_PEER_MAX) {
    server.send(400, "application/json",
      "{\"ok\":false,\"error\":\"Maximum " + String(MANUAL_PEER_MAX) + " manual peers\"}"); return;
  }
  manualPeers[manualPeerCount].ip   = ip;
  manualPeers[manualPeerCount].port = 80;
  manualPeerCount++;
  saveManualPeers();
  ssePush("peers_changed");
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/peers/manual/remove — remove a manual peer by IP
void handleRemoveManualPeer() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  String ip = doc["ip"] | "";
  for (int i = 0; i < manualPeerCount; i++) {
    if (manualPeers[i].ip == ip) {
      for (int j = i; j < manualPeerCount - 1; j++) manualPeers[j] = manualPeers[j + 1];
      manualPeerCount--;
      saveManualPeers();
      ssePush("peers_changed");
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
}

// GET /api/wifi — network list + connection status (passwords never sent)
void handleGetWifi() {
  JsonDocument doc;
  doc["connected"] = (WiFi.status() == WL_CONNECTED);
  doc["ip"]        = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  doc["apMode"]    = apMode;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < wifiNetCount; i++) {
    JsonObject net = arr.add<JsonObject>();
    net["ssid"]   = wifiNets[i].ssid;
    net["active"] = (i == wifiActiveIdx);
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/wifi/add — { ssid, password }  (updates password if SSID already exists)
void handleWifiAdd() {
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"Bad JSON\"}");
    return;
  }
  String ssid = doc["ssid"] | "";
  String pass = doc["password"] | "";
  ssid.trim();
  if (ssid.isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"SSID cannot be empty\"}");
    return;
  }
  // Update existing entry if SSID already present
  for (int i = 0; i < wifiNetCount; i++) {
    if (wifiNets[i].ssid == ssid) {
      wifiNets[i].pass = pass;
      saveWifiNetworks();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  if (wifiNetCount >= WIFI_MAX) {
    server.send(400, "application/json", "{\"error\":\"Maximum 10 networks stored\"}");
    return;
  }
  wifiNets[wifiNetCount].ssid = ssid;
  wifiNets[wifiNetCount].pass = pass;
  wifiNetCount++;
  saveWifiNetworks();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/wifi/remove — { index }
void handleWifiRemove() {
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"Bad JSON\"}");
    return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= wifiNetCount) {
    server.send(400, "application/json", "{\"error\":\"Invalid index\"}");
    return;
  }
  for (int i = idx; i < wifiNetCount - 1; i++) wifiNets[i] = wifiNets[i + 1];
  wifiNetCount--;
  if      (wifiActiveIdx == idx)  wifiActiveIdx = -1;
  else if (wifiActiveIdx > idx)   wifiActiveIdx--;
  saveWifiNetworks();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/wifi/move — { index, direction: "up"|"down" }
void handleWifiMove() {
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"Bad JSON\"}");
    return;
  }
  int    idx    = doc["index"]     | -1;
  String dir    = doc["direction"] | "";
  int    newIdx = (dir == "up") ? idx - 1 : idx + 1;
  if (idx < 0 || idx >= wifiNetCount || newIdx < 0 || newIdx >= wifiNetCount) {
    server.send(400, "application/json", "{\"error\":\"Invalid move\"}");
    return;
  }
  WifiNetwork tmp  = wifiNets[idx];
  wifiNets[idx]    = wifiNets[newIdx];
  wifiNets[newIdx] = tmp;
  if      (wifiActiveIdx == idx)    wifiActiveIdx = newIdx;
  else if (wifiActiveIdx == newIdx) wifiActiveIdx = idx;
  saveWifiNetworks();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/wifi/restart — reboot the device (restores WiFi from stored network list)
void handleReboot() {
  server.send(200, "application/json", "{\"ok\":true}");
  rebootRequested = true;
}

// ============================================================

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Mount filesystem
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  prefs.begin("semclock", false);

  // NVS schema migration — increment NVS_SCHEMA and add cases here when
  // the stored key layout changes.
  int savedSchema = prefs.getInt("schema", 0);
  if (savedSchema != NVS_SCHEMA) {
    // No data to migrate yet (first versioned schema).
    prefs.putInt("schema", NVS_SCHEMA);
    logf("[NVS] Schema initialised at v%d (was v%d)", NVS_SCHEMA, savedSchema);
  }

  // Load ElegantOTA credentials; fall back to hardcoded defaults if not set.
  otaUser = prefs.getString("ota_user", "admin");
  otaPass = prefs.getString("ota_pass", "raceclock");

  currentTZName = prefs.getString("tz", "Europe/Warsaw");
  warnMinutes   = prefs.getInt("warn", 5);
  // device_num is NOT loaded from NVS — always start from 1 and negotiate at
  // boot.  Persisting the number caused "stuck" states when the mDNS cache
  // had stale entries from a previous boot, making the board think its own
  // hostname was already taken by another device.
  prefs.remove("device_num");   // clear any leftover value from v0.4.6

  // Restore cached POSIX rule so DST is correct before the network comes up.
  // applyTimezone() will overwrite this with a fresh fetch once WiFi connects.
  String cachedPosix = prefs.getString("posix", "");
  if (cachedPosix.length() > 0) {
    localTZ.setPosix(cachedPosix);
    Serial.println("DST rules restored from cache: " + cachedPosix);
  }

  loadSessions();

  // Status LEDs — brief self-test, then off until state is known
  pinMode(LED_RED,   OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_RED,   HIGH);
  digitalWrite(LED_GREEN, HIGH);
  delay(300);
  digitalWrite(LED_RED,   LOW);
  digitalWrite(LED_GREEN, LOW);

  // DS3231 RTC — read before WiFi so time is available immediately
#ifdef HAS_RTC
  if (!rtc.begin()) {
    Serial.println("DS3231 not found");
  } else {
    rtcAvailable = true;
    DateTime t = rtc.now();
    if (t.year() >= 2024) {
      setTime(t.unixtime());   // give ezTime a starting point; NTP overwrites later
      Serial.println("RTC loaded: " + t.timestamp());
    } else {
      Serial.println("RTC time invalid (year " + String(t.year()) + ")");
    }
  }
#endif

  // P10 display — initialise before first displayScroll call
#ifdef HAS_P10
  dmd.clearScreen(true);
  // Hardware timer drives the scan ISR at ~300 µs intervals.
  // ESP32 Arduino core 3.x API: timerBegin(hz), timerAlarm(t, ticks, reload, count).
  // 1 MHz timer, alarm at 300 ticks → fires every 300 µs.
  p10Timer = timerBegin(1000000);
  timerAttachInterrupt(p10Timer, &p10ScanISR);
  timerAlarm(p10Timer, 300, true, 0);
#endif

  loadWifiNetworks();
  loadManualPeers();

  // Helper: start our own AP (fallback when no main WiFi available)
  String ownApSsid = (deviceNum > 1) ? "RaceClock" + String(deviceNum) : "RaceClock";
  auto startOwnAP = [&]() {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ownApSsid.c_str(), "raceclock1");
    dnsServer.start(53, "*", WiFi.softAPIP());  // captive portal: all DNS → us
    displayScroll("AP:" + ownApSsid + " pw:raceclock1");
    Serial.println("AP mode — 192.168.4.1  SSID: " + ownApSsid);
  };

    displayScroll("CONNECTING...");
    WiFi.setHostname(BASE_HOSTNAME);
    WiFi.mode(WIFI_AP_STA);   // AP_STA so we can run the pairing AP alongside STA
    bool connected = false;
    for (int i = 0; i < wifiNetCount && !connected; i++) {
      Serial.printf("Trying network %d: %s\n", i, wifiNets[i].ssid.c_str());
      WiFi.begin(wifiNets[i].ssid.c_str(), wifiNets[i].pass.c_str());
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500); Serial.print(".");
        attempts++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        connected     = true;
        wifiActiveIdx = i;
        Serial.printf("\nConnected to \"%s\" — %s\n",
                      wifiNets[i].ssid.c_str(),
                      WiFi.localIP().toString().c_str());
      } else {
        WiFi.disconnect(true);
        delay(200);
        Serial.printf("\nNetwork %d failed, trying next\n", i);
      }
    }
    if (!connected) {
      // Before starting our own AP: scan for another raceclock acting as a host.
      // If found, join it as a client and wait to be provisioned.
      Serial.println("All WiFi networks failed — scanning for raceclock AP...");
      displayScroll("SEEKING PEER...");
      WiFi.mode(WIFI_STA);
      int n = WiFi.scanNetworks();
      String foundAP = "";
      for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i).startsWith("RaceClock")) { foundAP = WiFi.SSID(i); break; }
      }
      if (foundAP.length() > 0) {
        Serial.println("Found " + foundAP + " — joining for provisioning");
        WiFi.begin(foundAP.c_str(), "raceclock1");
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
          delay(500); attempts++;
        }
        if (WiFi.status() == WL_CONNECTED) {
          // Announce to the host device (always at 192.168.4.1)
          HTTPClient http; WiFiClient wc;
          http.begin(wc, "http://192.168.4.1/api/provision");
          http.addHeader("Content-Type", "application/json");
          String body = "{\"hostname\":\"" + effectiveHostname +
                        "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
          http.POST(body); http.end();
          apMode = false;   // we are a client, not in own-AP mode
          displayScroll("WAITING FOR CONFIG...");
          Serial.println("Announced — waiting to be provisioned by host");
          // Fall through to server.begin() / loop — host will push config
        } else {
          startOwnAP();     // couldn't connect to raceclock AP either
        }
      } else {
        Serial.println("No raceclock AP found — starting own AP");
        startOwnAP();
      }
    } else {
      // STA connected — also start our AP so unconfigured devices can find us
      WiFi.softAP(ownApSsid.c_str(), "raceclock1");
      dnsServer.start(53, "*", WiFi.softAPIP());  // captive portal for AP clients
      Serial.println("Pairing AP started: " + ownApSsid + " (192.168.4.1)");
      // mDNS: start with the base name, negotiate a unique number, then rename if needed
      MDNS.begin(BASE_HOSTNAME);
      negotiateDeviceNumber();
      if (deviceNum > 1)
        mdns_hostname_set(effectiveHostname.c_str());
      MDNS.addService("raceclock", "tcp", 80);
      WiFi.setHostname(effectiveHostname.c_str());
      Serial.println("mDNS: http://" + effectiveHostname + ".local");

      displayMessage("SYNC...");
      applyTimezone(currentTZName);
      waitForSync(10);
      if (timeStatus() != timeSet) displayMessage("NO TIME");
      else displayMessage(WiFi.localIP().toString());
      Serial.println("Ready — http://" + effectiveHostname + ".local");
    }

  updateActiveSession();

  // --- ArduinoOTA (firmware upload over WiFi from Arduino IDE) ---
  // Works in both STA and AP mode. Password prevents accidental flashing.
  ArduinoOTA.setHostname(effectiveHostname.c_str());
  // No password — Arduino IDE 2.x has no UI to enter one and silently fails.
  // Network isolation (private LAN) is the security boundary for OTA.
  // ElegantOTA (/update) still requires browser credentials.
  ArduinoOTA.onStart([]() {
    Serial.println("OTA: starting update");
    displayMessage("OTA UPDATE");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA: complete — rebooting");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r", progress * 100 / total);
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("OTA error[%u]\n", err);
  });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA ready");

  // Routes
  server.on("/",                   HTTP_GET,  []() { serveFile("/index.html", "text/html"); });
  server.on("/tv",                 HTTP_GET,  []() { serveFile("/webonly.html", "text/html"); });
  server.on("/style.css",          HTTP_GET,  []() { serveFile("/style.css",  "text/css");  });
  server.on("/api/settings",       HTTP_GET,  handleGetSettings);
  server.on("/api/settings",       HTTP_POST, handlePostSettings);
  server.on("/api/sessions",       HTTP_GET,  handleGetSessions);
  server.on("/api/sessions",       HTTP_POST, handlePostSession);
  server.on("/api/sessions/delete",HTTP_POST, handleDeleteSession);
  server.on("/api/sessions/clear", HTTP_POST, handleClearSessions);
  server.on("/api/time",           HTTP_GET,  handleGetTime);
  server.on("/api/display",        HTTP_GET,  handleGetDisplay);
  server.on("/api/sem/schedule",   HTTP_GET,  handleSEMSchedule);
  server.on("/api/override",       HTTP_GET,  handleGetOverride);
  server.on("/api/override",       HTTP_POST, handlePostOverride);
  server.on("/api/wifi",           HTTP_GET,  handleGetWifi);
  server.on("/api/wifi/add",       HTTP_POST, handleWifiAdd);
  server.on("/api/wifi/remove",    HTTP_POST, handleWifiRemove);
  server.on("/api/wifi/move",      HTTP_POST, handleWifiMove);
  server.on("/api/wifi/restart",   HTTP_POST, handleReboot);
  server.on("/api/peers",               HTTP_GET,  handleGetPeers);
  server.on("/api/peers/push",          HTTP_POST, handlePeersPush);
  server.on("/api/peers/manual",        HTTP_POST, handleAddManualPeer);
  server.on("/api/peers/manual/remove", HTTP_POST, handleRemoveManualPeer);
  server.on("/api/provision",      HTTP_POST, handleProvision);
  server.on("/api/unpaired",       HTTP_GET,  handleGetUnpaired);
  server.on("/api/adapt",          HTTP_POST, handleAdapt);
  server.on("/api/version",        HTTP_GET,  handleGetVersion);
  server.on("/api/checkupdate",    HTTP_GET,  handleCheckUpdate);
  server.on("/api/doupdate",       HTTP_POST, handleDoUpdate);
  server.on("/api/doupdatefs",     HTTP_POST, handleDoUpdateFS);
  server.on("/api/log",            HTTP_GET,  handleGetLog);
  server.on("/api/ota-credentials",HTTP_GET,  handleGetOtaCreds);
  server.on("/api/ota-credentials",HTTP_POST, handleSetOtaCreds);
  server.on("/upload",             HTTP_POST,
    []() { server.send(200, "application/json", "{\"ok\":true}"); },
    handleFileUpload
  );
  server.onNotFound([]() {
    // API paths get a proper 404
    if (server.uri().startsWith("/api/")) {
      server.send(404, "text/plain", "Not found."); return;
    }
    // Everything else — captive portal redirect to the management UI.
    // Using a relative Location so it works regardless of which interface
    // (192.168.4.1 AP or raceclock.local STA) the request arrived on.
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  // --- ElegantOTA (browser-based firmware + filesystem upload at /update) ---
  ElegantOTA.begin(&server, otaUser.c_str(), otaPass.c_str());
  server.begin();
  sseBegin();   // SSE event stream on port 81
}

// --- Loop ---
void loop() {
  events();
  dnsServer.processNextRequest();   // captive portal DNS
  server.handleClient();
  sseLoop();
  ArduinoOTA.handle();
  ElegantOTA.loop();

  if (updateRequested)      performFirmwareUpdate();
  if (fsUpdateRequested)    performFilesystemUpdate();
  if (rebootRequested)      { delay(200); ESP.restart(); }

#ifdef HAS_P10
  p10UpdateScroll();
#endif
  updateLed();

  // Wi-Fi watchdog (STA mode only)
  static unsigned long lastWifiCheck    = 0;
  static bool          wifiWasConnected = true;
  if (!apMode && millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiWasConnected) {
        wifiWasConnected = false;
        displayMessage("RECONNECTING");
        logf("[wifi] connection lost — reconnecting");
      }
      WiFi.reconnect();
    } else if (!wifiWasConnected) {
      wifiWasConnected = true;
      logf("[wifi] restored (%s)", WiFi.localIP().toString().c_str());
      ssePush("wifi_reconnected");
      updateNTP();
#ifdef HAS_RTC
      rtcSynced = false;   // update RTC once NTP re-syncs
#endif
    }
  }

  // Notify browser the moment NTP first achieves a lock (with or without RTC)
  { static bool ntpWasSynced = false;
    bool ntpNow = (timeStatus() == timeSet);
    if (ntpNow && !ntpWasSynced) { ntpWasSynced = true; ssePush("ntp_synced"); }
    if (!ntpNow)                    ntpWasSynced = false;   // reset on loss
  }

  // Write RTC once each time NTP achieves a fresh lock
#ifdef HAS_RTC
  if (rtcAvailable && !rtcSynced && timeStatus() == timeSet) {
    rtcSynced = true;
    rtc.adjust(DateTime((uint32_t)UTC.now()));
    Serial.println("RTC updated from NTP");
  }
#endif

  // Re-announce to host if we are waiting for provisioning (joined a raceclock AP)
  { static uint32_t lastAnnounce = 0;
    static bool waitingProvision = false;
    // Set on first loop after joining a raceclock AP in setup() (apMode=false, STA connected,
    // but gateway is 192.168.4.1 which means we are on a soft-AP network, not the main LAN)
    IPAddress gw = WiFi.gatewayIP();
    // 192.168.4.1 is the fixed IP of our own softAP — matches only that subnet,
    // not the common home-router addresses (192.168.0.1, 192.168.1.1, etc.).
    bool onRaceclockAP = (!apMode && WiFi.status() == WL_CONNECTED &&
                          gw[0] == 192 && gw[1] == 168 && gw[2] == 4 && gw[3] == 1);
    if (onRaceclockAP && millis() - lastAnnounce > 30000) {
      lastAnnounce = millis();
      HTTPClient http; WiFiClient wc;
      http.begin(wc, "http://192.168.4.1/api/provision");
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(2000);   // never block the loop for more than 2 s
      String body = "{\"hostname\":\"" + effectiveHostname + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
      http.POST(body); http.end();
    }
  }

  // Expire unpaired devices that haven't re-announced in 2 minutes
  { static uint32_t lastExpiry = 0;
    if (millis() - lastExpiry > 15000) {
      lastExpiry = millis();
      int prev = unpairedCount;
      for (int i = 0; i < unpairedCount; ) {
        if (millis() - unpaired[i].seenAt > 120000) {
          for (int j = i; j < unpairedCount - 1; j++) unpaired[j] = unpaired[j+1];
          unpairedCount--;
        } else { i++; }
      }
      if (unpairedCount != prev) ssePush("unpaired_changed");
    }
  }

  // Background peer scan — trigger every 60 s; the task does the blocking work.
  { static uint32_t lastBgScan = 0;
    if (!apMode && WiFi.status() == WL_CONNECTED && millis() - lastBgScan > 60000) {
      lastBgScan = millis();
      mdnsScanTrigger();
    }
    // Pick up results from the task when it finishes
    if (mdnsScanDone) {
      mdnsScanDone = false;
      if (peersDirty) { peersDirty = false; ssePush("peers_changed"); }
    }
  }

  // Re-evaluate active session periodically
  static unsigned long lastSessionCheck = 0;
  if (millis() - lastSessionCheck > 30000) {
    lastSessionCheck = millis();
    updateActiveSession();
  }

  // Heap watchdog — reboot before heap exhaustion causes a silent crash.
  // After days of String allocations the heap fragments; 15 KB is the safety
  // floor below which JSON/HTTP operations start failing in unpredictable ways.
  { static uint32_t lastHeapCheck = 0;
    if (millis() - lastHeapCheck > 60000) {
      lastHeapCheck = millis();
      uint32_t free = ESP.getFreeHeap();
      logf("[heap] free=%u  min=%u", free, ESP.getMinFreeHeap());
      if (free < 15000) {
        logf("[heap] critically low (%u) — rebooting", free);
        displayMessage("REBOOT...");
        delay(500);
        ESP.restart();
      }
    }
  }

  // Daily safety-net reboot at 03:00 local time (track is always closed then).
  // Clears any accumulated heap fragmentation or stale socket state.
  { static bool rebootArmed = true;
    int h = localTZ.hour();
    int m = localTZ.minute();
    // Arm at 03:01 (so a reboot at 03:00 doesn't immediately re-trigger)
    if (h == 3 && m == 1)  rebootArmed = true;
    if (h == 3 && m == 0 && rebootArmed) {
      rebootArmed = false;
      logf("[watchdog] Daily reboot at 03:00");
      displayMessage("REBOOT...");
      delay(500);
      ESP.restart();
    }
  }

  // Clock state machine
  static int  lastRemaining = -1;
  static bool blinkState    = false;
  static unsigned long lastBlink = 0;

  // Override: apply immediately when text changes, then bypass state machine
  if (overrideDirty) {
    overrideDirty = false;
    if (overrideText.length() > 0) {
#ifdef HAS_P10
      if (p10StringWidth(overrideText.c_str()) > P10_WIDTH)
        displayScroll(overrideText);
      else
        displayMessage(overrideText);
#else
      Serial.println("[OVERRIDE] " + overrideText);
#endif
    } else {
      // Cleared — force state machine to redraw on next iteration
      lastRemaining = -1;
      clockState    = (ClockState)(-1);
    }
  }

  if (!overrideText.isEmpty()) {
    delay(100);
    return; // skip state machine and blink logic while override is active
  }

  int remaining = 0;
  ClockState state = computeState(remaining);

  // Update display when state or time changes
  if (state != clockState || remaining != lastRemaining) {
    clockState    = state;
    lastRemaining = remaining;

    String sType = (activeSession >= 0)
      ? sessions[activeSession].type : "";

    switch (state) {
      case STATE_NO_SESSION:
        displayMessage("NO SESSION");
        break;
      case STATE_PRE_SESSION:
        displayMessage("TRACK CLOSED");
        break;
      case STATE_COUNTDOWN:
        displayCountdown(formatCountdown(remaining), sType);
        break;
      case STATE_WARNING:
        // Sync blink to second boundary: reset to "on" each time the
        // countdown ticks so the new value is always visible first,
        // then goes dark 750 ms later — no stutter at the second change.
        blinkState = true;
        lastBlink  = millis();
        displayCountdown(formatCountdown(remaining), sType);
        break;
      case STATE_LAST_START:
        // Reset phase on entry (remaining stays 0, so this fires once).
        blinkState = true;
        lastBlink  = millis();
        displayBlink(true, sType);
        break;
      case STATE_TRACK_CLOSED:
        displayMessage("TRACK CLOSED");
        break;
    }
  }

  // Blink timing:
  //   WARNING    — 750 ms on / 250 ms off; type stays solid, only countdown blinks
  //   LAST_START — 500 ms on / 500 ms off; type stays solid, 00:00:00 blinks
  if (state == STATE_WARNING || state == STATE_LAST_START) {
    unsigned long onMs  = (state == STATE_WARNING) ? 750UL : 500UL;
    unsigned long offMs = (state == STATE_WARNING) ? 250UL : 500UL;
    unsigned long interval = blinkState ? onMs : offMs;
    if (millis() - lastBlink >= interval) {
      lastBlink  = millis();
      blinkState = !blinkState;
      String sType = (activeSession >= 0) ? sessions[activeSession].type : "";
      if (state == STATE_LAST_START) {
        displayBlink(blinkState, sType);
      } else {
        // WARNING: type line stays solid; only the countdown row blinks
        if (blinkState) displayCountdown(formatCountdown(remaining), sType);
        else            displayBlink(false, sType);
      }
    }
  }

  delay(100);
}