#include <WiFi.h>
#include <WebServer.h>
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

// ─── Hardware configuration ──────────────────────────────────
// Uncomment a line when that hardware is physically connected.
// Each flag pulls in the required library and replaces the
// Serial-only display stubs with real hardware calls.
//
// #define HAS_RTC   // DS3231 real-time clock  (needs: RTClib by Adafruit)
//                   //   SDA → GPIO21,  SCL → GPIO22  (hardware I²C)
//
// #define HAS_P10   // P10 HUB12 LED panels    (needs: DMD2 by Freetronics)
//                   //   DATA → GPIO23 (MOSI)  CLK  → GPIO18 (SCK)
//                   //   LATCH→ GPIO5  (SS)    OE   → GPIO4
//                   //   A    → GPIO16         B    → GPIO17
//                   //   2 panels wide × 1 tall = 64 × 16 px total

// Status LEDs — two separate 5 mm LEDs (always compiled in)
#define LED_RED    25
#define LED_GREEN  26

// P10 control pins (only used when HAS_P10 is defined)
#define P10_PANELS_WIDE  2
#define P10_PANELS_TALL  1
#define P10_PIN_OE       4   // Output-enable, active low
#define P10_PIN_A        16  // Row-select A
#define P10_PIN_B        17  // Row-select B
#define P10_WIDTH       (P10_PANELS_WIDE * 32)  // 64 px — always available

#ifdef HAS_RTC
#include <Wire.h>
#include <RTClib.h>
#endif

#ifdef HAS_P10
#include <DMD2.h>
#include <fonts/SystemFont5x7.h>
#endif
// ─────────────────────────────────────────────────────────────

// --- Version ---
#define FIRMWARE_VERSION "0.4.2"
#define GITHUB_OWNER     "gilasconsultancy"
#define GITHUB_REPO      "sem-race-clock"

// --- Configuration ---
#include "credentials.h"   // gitignored — copy credentials.h.example to get started
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* hostname = "raceclock";

#define MAX_SESSIONS 20

// --- Session structure ---
struct Session {
  String type;        // "Prototype" or "Urban Concept"
  int startH, startM;
  int lastStartH, lastStartM;
  int endH, endM;
};

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
bool         updateRequested   = false; // set by /api/doupdate;   acted on in loop()
bool         fsUpdateRequested = false; // set by /api/doupdatefs; acted on in loop()

#ifdef HAS_RTC
RTC_DS3231   rtc;
bool         rtcAvailable  = false;
bool         rtcSynced     = false;   // true once NTP has written the RTC
#endif

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
SPIDMD dmd(P10_PANELS_WIDE, P10_PANELS_TALL, P10_PIN_OE, P10_PIN_A, P10_PIN_B);

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
  dmd.selectFont(SystemFont5x7);
  dmd.drawString(p10ScrollX, 4, p10ScrollText.c_str(),
                 p10ScrollText.length(), GRAPHICS_NORMAL);
  int tw = dmd.stringWidth(p10ScrollText.c_str());
  p10ScrollX -= 2;
  if (p10ScrollX < -tw) p10ScrollX = P10_WIDTH;   // loop
}
#endif  // HAS_P10

void displayMessage(const String& msg) {
#ifdef HAS_P10
  p10Scrolling = false;
  dmd.selectFont(SystemFont5x7);
  int tw = dmd.stringWidth(msg.c_str());
  if (tw > P10_WIDTH) {          // too wide — scroll instead
    p10StartScroll(msg);
    return;
  }
  dmd.clearScreen(true);
  dmd.drawString((P10_WIDTH - tw) / 2, 4,
                 msg.c_str(), msg.length(), GRAPHICS_NORMAL);
#else
  Serial.println("[DISPLAY] " + msg);
#endif
}

void displayCountdown(const String& timeStr, const String& sessionType) {
#ifdef HAS_P10
  p10Scrolling = false;
  dmd.clearScreen(true);
  dmd.selectFont(SystemFont5x7);
  // Line 1 (y=1): session type, full name uppercased.
  // Trim trailing chars if wider than display (unlikely for "PROTOTYPE", possible
  // for "URBAN CONCEPT" — exact fit depends on font metrics; confirm with hardware).
  String typeLine = sessionType;
  //typeLine.toUpperCase();
  int tw1 = dmd.stringWidth(typeLine.c_str());
  while (tw1 > P10_WIDTH && typeLine.length() > 0) {
    typeLine.remove(typeLine.length() - 1);
    tw1 = dmd.stringWidth(typeLine.c_str());
  }
  dmd.drawString((P10_WIDTH - tw1) / 2, 1,
                 typeLine.c_str(), typeLine.length(), GRAPHICS_NORMAL);
  // Line 2 (y=9): countdown time, centred
  int tw2 = dmd.stringWidth(timeStr.c_str());
  dmd.drawString((P10_WIDTH - tw2) / 2, 9,
                 timeStr.c_str(), timeStr.length(), GRAPHICS_NORMAL);
#else
  Serial.println("[DISPLAY] " + sessionType + " | " + timeStr);
#endif
}

void displayBlink(bool showTime, const String& sessionType) {
#ifdef HAS_P10
  p10Scrolling = false;
  dmd.clearScreen(true);
  dmd.selectFont(SystemFont5x7);
  String typeLine = sessionType;
  //typeLine.toUpperCase();
  int tw1 = dmd.stringWidth(typeLine.c_str());
  while (tw1 > P10_WIDTH && typeLine.length() > 0) {
    typeLine.remove(typeLine.length() - 1);
    tw1 = dmd.stringWidth(typeLine.c_str());
  }
  dmd.drawString((P10_WIDTH - tw1) / 2, 1,
                 typeLine.c_str(), typeLine.length(), GRAPHICS_NORMAL);
  if (showTime) {
    const char* zeros = "00:00:00";
    int tw2 = dmd.stringWidth(zeros);
    dmd.drawString((P10_WIDTH - tw2) / 2, 9, zeros, 8, GRAPHICS_NORMAL);
  }
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
  time_t now = localTZ.now();
  struct tm* ti = localtime(&now);
  int nowSecs = ti->tm_hour * 3600 + ti->tm_min * 60 + ti->tm_sec;

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
  time_t now = localTZ.now();
  struct tm* ti = localtime(&now);
  int nowSecs       = ti->tm_hour * 3600 + ti->tm_min * 60 + ti->tm_sec;
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
    if (name != "/index.html" && name != "/style.css") {
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
  http.setTimeout(15000);
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

// GET /api/sem/days — returns event name + list of days that have track sessions.
void handleSEMDays() {
  if (apMode || WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\":\"No internet — cannot reach SEM API\"}");
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
    int count = 0;
    for (JsonObject item : day["schedule"].as<JsonArray>())
      if (isSEMTrackSession(item["activity"] | "", item["notes"] | "")) count++;
    if (count > 0) {
      JsonObject d = days.add<JsonObject>();
      d["date"]  = day["date"];
      d["day"]   = day["day"];
      d["count"] = count;
    }
  }
  String result; serializeJson(out, result);
  server.send(200, "application/json", result);
}

// GET /api/sem/sessions?date=YYYY-MM-DD — returns parsed track sessions for one day.
void handleSEMSessions() {
  if (apMode || WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\":\"No internet — cannot reach SEM API\"}");
    return;
  }
  String dateFilter = server.arg("date");
  if (dateFilter.isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"date parameter required\"}");
    return;
  }
  String eventName;
  JsonDocument schDoc;
  if (!fetchSEMSchedule(eventName, schDoc)) {
    server.send(503, "application/json", "{\"error\":\"Could not fetch SEM schedule\"}");
    return;
  }
  JsonDocument out;
  JsonArray sessions = out.to<JsonArray>();
  for (JsonObject day : schDoc["data"].as<JsonArray>()) {
    if (day["date"].as<String>() != dateFilter) continue;
    for (JsonObject item : day["schedule"].as<JsonArray>()) {
      String activity = item["activity"] | "";
      String notes    = item["notes"]    | "";
      if (!isSEMTrackSession(activity, notes)) continue;
      String type = (activity.indexOf("Prototype") >= 0) ? "Prototype" : "Urban Concept";
      // Extract "HH:MM" after "Last Start: " (12 chars)
      int lsIdx    = notes.indexOf("Last Start:") + 12;
      String lastStart = notes.substring(lsIdx, lsIdx + 5);
      lastStart.trim();
      JsonObject s  = sessions.add<JsonObject>();
      s["type"]      = type;
      s["start"]     = item["timeStart"];
      s["lastStart"] = lastStart;
      s["end"]       = item["timeEnd"];
    }
    break; // found the requested day
  }
  String result; serializeJson(out, result);
  server.send(200, "application/json", result);
}

void handleGetTime() {
  time_t now = UTC.now();   // must be UTC — JS applies the timezone offset itself
  String out = "{\"epoch\":" + String((long)now) +
               ",\"synced\":" + (timeStatus() == timeSet ? "true" : "false") + "}";
  server.send(200, "application/json", out);
}

// --- API: GET /api/settings ---
void handleGetSettings() {
  JsonDocument doc;
  doc["tz"]          = currentTZName;
  doc["warnMinutes"] = warnMinutes;
  doc["maxSessions"] = MAX_SESSIONS;
  doc["ntpSynced"]   = (timeStatus() == timeSet);
  doc["apMode"]      = apMode;
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
  int newWarn  = doc["warnMinutes"] | 5;
  if (newWarn < 1)  newWarn = 1;
  if (newWarn > 30) newWarn = 30;

  displayMessage("SYNC...");
  if (!applyTimezone(newTZ)) {
    server.send(400, "application/json",
      "{\"ok\":false,\"error\":\"Unknown timezone\"}");
    return;
  }
  warnMinutes = newWarn;
  prefs.putInt("warn", warnMinutes);
  server.send(200, "application/json", "{\"ok\":true}");
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
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- API: POST /api/sessions/delete ---
void handleDeleteSession() {
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= sessionCount) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  for (int i = idx; i < sessionCount - 1; i++) sessions[i] = sessions[i+1];
  sessionCount--;
  saveSessions();
  updateActiveSession();
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- API: POST /api/sessions/clear ---
void handleClearSessions() {
  sessionCount = 0;
  activeSession = -1;
  saveSessions();
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

#ifdef HAS_P10
  displayMessage("OTA...");
#endif

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.update(client, url);

  switch (ret) {
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] Success — rebooting");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] No update available");
      break;
    case HTTP_UPDATE_FAILED:
      Serial.println("[OTA] Failed: " + httpUpdate.getLastErrorString());
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

#ifdef HAS_P10
  displayMessage("FS OTA...");
#endif

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.updateSpiffs(client, url);

  switch (ret) {
    case HTTP_UPDATE_OK:
      Serial.println("[FSOTA] Success — rebooting");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[FSOTA] No update available");
      break;
    case HTTP_UPDATE_FAILED:
      Serial.println("[FSOTA] Failed: " + httpUpdate.getLastErrorString());
      break;
  }
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
  currentTZName = prefs.getString("tz", "Europe/Warsaw");
  warnMinutes   = prefs.getInt("warn", 5);

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
  dmd.begin();
  dmd.clearScreen(true);
#endif

  displayScroll("CONNECTING...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    apMode = true;
    Serial.println("\nWiFi failed — starting AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("RaceClock", "raceclock1");
    displayScroll("AP:RaceClock pw:raceclock1");
    Serial.println("AP IP: " + WiFi.softAPIP().toString());
  } else {
    Serial.println("\nConnected: " + WiFi.localIP().toString());
    if (MDNS.begin(hostname))
      Serial.println("mDNS: http://" + String(hostname) + ".local");
    displayMessage("SYNC...");
    applyTimezone(currentTZName);
    waitForSync(10);
    if (timeStatus() != timeSet) displayMessage("NO TIME");
    else displayMessage(WiFi.localIP().toString());
    Serial.println("Ready — http://" + String(hostname) + ".local");
  }

  updateActiveSession();

  // --- ArduinoOTA (firmware upload over WiFi from Arduino IDE) ---
  // Works in both STA and AP mode. Password prevents accidental flashing.
  ArduinoOTA.setHostname(hostname);
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
  server.on("/style.css",          HTTP_GET,  []() { serveFile("/style.css",  "text/css");  });
  server.on("/api/settings",       HTTP_GET,  handleGetSettings);
  server.on("/api/settings",       HTTP_POST, handlePostSettings);
  server.on("/api/sessions",       HTTP_GET,  handleGetSessions);
  server.on("/api/sessions",       HTTP_POST, handlePostSession);
  server.on("/api/sessions/delete",HTTP_POST, handleDeleteSession);
  server.on("/api/sessions/clear", HTTP_POST, handleClearSessions);
  server.on("/api/time",           HTTP_GET,  handleGetTime);
  server.on("/api/display",        HTTP_GET,  handleGetDisplay);
  server.on("/api/sem/days",       HTTP_GET,  handleSEMDays);
  server.on("/api/sem/sessions",   HTTP_GET,  handleSEMSessions);
  server.on("/api/override",       HTTP_GET,  handleGetOverride);
  server.on("/api/override",       HTTP_POST, handlePostOverride);
  server.on("/api/version",        HTTP_GET,  handleGetVersion);
  server.on("/api/checkupdate",    HTTP_GET,  handleCheckUpdate);
  server.on("/api/doupdate",       HTTP_POST, handleDoUpdate);
  server.on("/api/doupdatefs",     HTTP_POST, handleDoUpdateFS);
  server.on("/upload",             HTTP_POST,
    []() { server.send(200, "application/json", "{\"ok\":true}"); },
    handleFileUpload
  );
  server.onNotFound([]() { server.send(404, "text/plain", "Not found."); });

  // --- ElegantOTA (browser-based firmware + filesystem upload at /update) ---
  ElegantOTA.begin(&server, "admin", "raceclock");
  server.begin();
}

// --- Loop ---
void loop() {
  events();
  server.handleClient();
  ArduinoOTA.handle();
  ElegantOTA.loop();

  if (updateRequested)   performFirmwareUpdate();
  if (fsUpdateRequested) performFilesystemUpdate();

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
        Serial.println("Wi-Fi lost — reconnecting...");
      }
      WiFi.reconnect();
    } else if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.println("Wi-Fi restored — triggering NTP sync");
      updateNTP();
#ifdef HAS_RTC
      rtcSynced = false;   // update RTC once NTP re-syncs
#endif
    }
  }

  // Write RTC once each time NTP achieves a fresh lock
#ifdef HAS_RTC
  if (rtcAvailable && !rtcSynced && timeStatus() == timeSet) {
    rtcSynced = true;
    rtc.adjust(DateTime((uint32_t)UTC.now()));
    Serial.println("RTC updated from NTP");
  }
#endif

  // Re-evaluate active session periodically
  static unsigned long lastSessionCheck = 0;
  if (millis() - lastSessionCheck > 30000) {
    lastSessionCheck = millis();
    updateActiveSession();
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
      if (dmd.stringWidth(overrideText.c_str()) > P10_WIDTH)
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