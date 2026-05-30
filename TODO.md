# TODO — SEM Race Clock

Items are grouped by milestone. Check off items as they are completed.

---

## Now — DS3231 RTC (wired and enabled)

- [x] Install **RTClib** (by Adafruit) + **Adafruit BusIO** via Arduino IDE Library Manager
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

- [x] Vendor DMD32 (ESP32-native fork of DMD) in `src/DMD32/` — compiles clean
- [x] Uncomment `#define HAS_P10` — ready but commented until panels arrive
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

- [ ] Uncomment `#define HAS_P10` and flash
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
- [ ] **Smaller font for label row** — a 3×5 font would fit "URBAN CONCEPT" (52 px)
  without trimming and fix the overflow issue. Needs conversion to DMD32 format
  and on-hardware testing. **Do not attempt blind.**

---

## Before the event — Competition prep

- [ ] Add competition venue WiFi via the web UI **WiFi** card (no reflash needed)
- [ ] Confirm `raceclock.local` resolves on the venue network (mDNS may not work
  on some managed networks — fall back to IP address if needed)
- [ ] Pre-load the full day's session schedule via **Import from official schedule**
  or by pushing from one clock to the other
- [ ] Confirm DS3231 battery is charged and time is correct after NTP sync
- [ ] If running two clocks: power on second device near first — it finds the
  `RaceClock` AP automatically and appears in the **UNCONFIGURED** section;
  click **Adapt** to provision it with one tap

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
- [x] WiFi card redesign — CONNECTION / UNCONFIGURED / PEERS / NETWORKS / ADD NETWORK
- [x] Toast notifications — replaced all `alert()` with non-blocking slide-up toasts
- [x] Non-blocking Shell font load — `media="print"` trick; page renders immediately in AP mode
- [x] Mock peer tool — `tools/mock_raceclock.py` with push-in/push-out test modes
- [x] Status LED logic (GPIO25/26) — compiled in but LEDs dropped from BOM
- [x] DS3231 RTC — wired, library installed, `#define HAS_RTC` enabled
- [x] P10 HUB12 display code — custom pixel fonts, layout, blink logic all written
- [x] DMD32 ESP32-native P10 driver — vendored in `src/DMD32/`, pins pre-configured, compiles clean
- [x] P10 live display preview widget in web UI — local state machine, no REST polling
- [x] ArduinoOTA — firmware upload over WiFi from Arduino IDE
- [x] ElegantOTA — browser-based firmware + filesystem upload at `/update` (admin / raceclock)
- [x] GitHub Actions CI — automated firmware + LittleFS build and release on version tag
- [x] WARNING blink asymmetric (750 ms on / 250 ms off); type line stays solid
- [x] `handleGetOverride` uses `serializeJson` — safe for quotes and backslashes
- [x] SEM import checks per-session POST results and reports exact ok/rejected counts
- [x] SEM day picker auto-selects today (device time in event timezone via `en-CA` locale)
- [x] SEM import panel auto-closes 2 s after fully-successful import
- [x] SEM schedule merged into single `GET /api/sem/schedule` — one external fetch, day selection client-side
- [x] SSE real-time push notifications on port 81 — events: sessions_changed, override_changed,
  peers_changed, unpaired_changed, wifi_reconnected, ntp_synced, update_start, update_failed
- [x] Zero-config device provisioning — always-on AP (WIFI_AP_STA), unconfigured devices
  appear in web UI, Adapt button pushes full config in one tap
- [x] Captive portal — DNSServer redirects all DNS to AP IP; browser opens automatically
- [x] Background peer scan in loop() — SSE notifies browser on peer list changes
- [x] Initial page loads parallelised with Promise.all
- [x] Session poll reduced from 8 s to 30 s (SSE handles instant updates)
- [x] Peers Refresh button removed — peer list kept current by SSE
- [x] v0.5.0 released
