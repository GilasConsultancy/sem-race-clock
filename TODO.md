# TODO — SEM Race Clock

Items are grouped by milestone. Check off items as they are completed.

---

## Now — DS3231 RTC (wired and enabled)

- [x] Install **RTClib** (by Adafruit) via Arduino IDE Library Manager
- [x] Wire DS3231 module: SDA → D21, SCL → D22, VCC → 3V3, GND → GND
- [x] Ensure CR2032 battery is installed in the DS3231 module
- [x] Uncomment `#define HAS_RTC` in firmware
- [x] Flash firmware
- [ ] Verify on Serial Monitor:
  - `RTC loaded: <timestamp>` on boot (if battery is installed and time was set)
  - `RTC time invalid (year …)` on fresh module — expected on first boot
  - `RTC updated from NTP` after NTP sync
- [ ] Test power-cut recovery: disconnect WiFi, restart → time should come from RTC
- [ ] Test NTP override: with WiFi and RTC both present, NTP time should win

---

## Next — P10 HUB12 LED Panels (firmware ready, hardware pending)

- [x] Install **DMD2** (by Freetronics) via Arduino IDE Library Manager
- [x] Resolve DMD2 API differences (constructor, clearScreen, setPixel, drawString)
- [x] Uncomment `#define HAS_P10` in firmware
- [x] Flash firmware (compiles clean; degrades gracefully without panels connected)
- [ ] Wire both panels (2 × 32×16, daisy-chained):

  | Signal | ESP32 pin | HUB12 pin |
  |---|---|---|
  | DATA (MOSI) | D23 | 2 |
  | CLK (SCK) | D18 | 1 |
  | LATCH (SS) | D5 | 3 |
  | OE (active low) | D4 | 5 |
  | Row select A | D16 | 7 |
  | Row select B | D17 | 9 |
  | GND | GND | 4, 6, 8, 10 |
  | +5V | VIN | 14, 16 |

  Power: USB 5V 2A+ charger → ESP32 USB-C. VIN carries the raw 5V to the panels.
  Daisy-chain: OUTPUT of panel 1 → INPUT of panel 2 via included ribbon cable.

- [ ] Verify startup scroll "CONNECTING..." is visible
- [ ] Verify two-row countdown layout: type label on top, time on bottom
- [ ] Verify blink timing (WARNING: 750 ms on / 250 ms off; LAST START: 1 s)
- [ ] Verify auto-scroll kicks in for long messages (IP address, AP credentials)
- [ ] **"URBAN CONCEPT" overflow** — 13 chars × 6 px = 78 px > 64 px panel width.
  The trim loop shaves trailing characters until it fits; confirm what the
  truncated string looks like and decide if abbreviation is needed.

### Font — revisit with hardware in hand

- [ ] **Visual hierarchy** — both rows render at the same font size. A smaller
  type label would de-emphasise the session name. Defer until hardware present.
- [ ] **Smaller font for label row** — a 3×5 font fits "URBAN CONCEPT" (52 px)
  without trimming and would also fix the overflow issue.
  `github.com/filmote/Font3x5` exists but needs conversion to DMD2 format:
  1. Verify bit ordering in `Font3x5.cpp` (bit 0 = top or bottom?)
  2. Add DMD2 header `[width, height, firstChar, lastChar]`
  3. Adjust bit order per byte if needed
  4. Drop in as `fonts/Font3x5_DMD2.h`, use for label row only
  **Do not attempt blind — test on hardware.**

---

## Before the event — Competition prep

- [ ] Add competition venue WiFi via the web UI **WiFi** card (no reflash needed)
- [ ] Confirm `raceclock.local` resolves on the venue network (mDNS may not work
  on some managed networks — fall back to IP address if needed)
- [ ] Pre-load the full day's session schedule via **Import from official schedule**
  or by pushing from one clock to the other
- [ ] Confirm DS3231 battery is charged and time is correct after NTP sync
- [ ] If running two clocks: verify peer discovery and session push works on venue network

---

## Future / design-pending

- [ ] **SEM import overlap rejection** — when a Prototype and Urban Concept session
  share the same time slot the second POST is rejected by the overlap validator.
  Fix: relax the rule for sessions of different types, or import both atomically.
- [ ] **`displayNeedsRefresh` flag** — `loop()` resets clock state after an override
  is cleared by casting `-1` to `ClockState`. Replace with a `bool` flag to make
  intent explicit and avoid UB.
- [ ] **Multi-day scheduling** — sessions have no date field (time-of-day only).
  For multi-day events, either add a date field or accept that sessions are
  re-entered each morning.

---

## Completed

- [x] Core countdown logic — session states, `computeState`, blink timing
- [x] Seconds-level state transitions (was off by up to 59 s at boundaries)
- [x] Session persistence in NVS (Preferences) — survives LittleFS uploads and OTA
- [x] Overlap detection and time-order validation on session save
- [x] Session sorting by start time
- [x] Web management UI — Shell-branded, responsive, two-column desktop layout
- [x] NTP sync via ezTime with IANA timezone support
- [x] UTC fix — `/api/time` now returns true UTC epoch; JS applies timezone
- [x] Event-timezone session status in web UI (was using browser timezone)
- [x] `applyTimezone` made non-blocking
- [x] WiFi reconnect triggers NTP re-sync
- [x] `warnMinutes` clamped server-side (1–30)
- [x] NTP sync status indicator in web UI
- [x] WiFi AP fallback (SSID `RaceClock` / `raceclock1`) instead of restart
- [x] WiFi credential management via web UI — NVS-stored, add/remove/reorder, up to 10 networks
- [x] Multi-device peer discovery — mDNS scan, auto device number negotiation, no NVS persistence
- [x] Session push to peers — `POST /api/peers/push`; firmware does server-to-server HTTP
- [x] Auto-refresh sessions in web UI when pushed by peer (8 s background polling)
- [x] WiFi card redesign — CONNECTION / PEERS / NETWORKS / ADD NETWORK subheaders
- [x] Toast notifications — replaced all `alert()` with non-blocking slide-up toasts
- [x] Non-blocking Shell font load — `media="print"` trick; page renders immediately in AP mode
- [x] Mock peer tool — `tools/mock_raceclock.py` with push-in/push-out test modes
- [x] Status LED logic (GPIO25/26) — compiled in but LEDs dropped from BOM
- [x] DS3231 RTC — wired, library installed, `#define HAS_RTC` enabled
- [x] P10 HUB12 display code — DMD2 API fixed, `#define HAS_P10` enabled, hardware pending
- [x] P10 live display preview widget in web UI (`/api/display` + rendered panel canvas)
- [x] ArduinoOTA — firmware upload over WiFi from Arduino IDE (password: `raceclock`)
- [x] ElegantOTA — browser-based firmware + filesystem upload at `/update` (admin / raceclock)
- [x] GitHub Actions CI — automated firmware + LittleFS build and release on version tag
- [x] WARNING blink asymmetric (750 ms on / 250 ms off); type line stays solid
- [x] `handleGetOverride` uses `serializeJson` — safe for quotes and backslashes
- [x] SEM import checks per-session POST results and reports exact ok/rejected counts
- [x] SEM day picker auto-selects today (device time in event timezone via `en-CA` locale)
- [x] SEM import panel auto-closes 2 s after fully-successful import
- [x] Redundant `typeShort` alias removed from `handleGetDisplay`
- [x] Tabindex on all interactive elements
