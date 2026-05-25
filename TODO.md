# TODO — SEM Race Clock

Items are grouped by the hardware milestone that unlocks them.
Check off items as they are completed.

---

## Now — Status LEDs (parts on hand)

- [ ] Wire red LED: anode → GPIO25, cathode → GND via 330 Ω resistor
- [ ] Wire green LED: anode → GPIO26, cathode → GND via 330 Ω resistor
- [ ] Power on — verify both LEDs flash briefly on startup (self-test)
- [ ] Verify LED states during normal operation:
  - Green solid = WiFi + NTP OK
  - Green slow blink = WiFi OK, NTP not yet locked
  - Red solid = no WiFi or AP mode

---

## ~2 days — DS3231 RTC

- [ ] Install **RTClib** (by Adafruit) via Arduino IDE Library Manager
- [ ] Wire DS3231 module: SDA → GPIO21, SCL → GPIO22, VCC → 3.3 V, GND → GND
- [ ] Ensure CR2032 battery is installed in the DS3231 module
- [ ] Uncomment `#define HAS_RTC` near the top of `SEM_Time2LastStart.ino`
- [ ] Flash firmware
- [ ] Verify on Serial Monitor:
  - `RTC loaded: <timestamp>` on boot (if battery was installed and time was set)
  - `RTC time invalid (year …)` on fresh module — expected first boot
  - `RTC updated from NTP` after NTP sync
- [ ] Test power-cut recovery: disconnect WiFi, restart → time should come from RTC
- [ ] Test NTP override: with WiFi and RTC both present, NTP time should win

---

## ~3 weeks — P10 HUB12 LED Panels

- [ ] Install **DMD2** (by Freetronics) via Arduino IDE Library Manager
- [ ] Wire both panels (2 × 32×16, daisy-chained):

  | HUB12 pin | ESP32 GPIO |
  |---|---|
  | DATA (D) | GPIO23 (MOSI) |
  | CLK | GPIO18 (SCK) |
  | SCLK / LATCH | GPIO5 (SS) |
  | nOE | GPIO4 |
  | A | GPIO16 |
  | B | GPIO17 |
  | GND | GND |
  | VCC | 5 V (external supply recommended) |

- [ ] Uncomment `#define HAS_P10` near the top of `SEM_Time2LastStart.ino`
- [ ] Flash firmware
- [ ] Verify startup scroll "CONNECTING..." is visible
- [ ] Verify two-row countdown layout: type on top line, time on bottom
- [ ] Verify blink timing on hardware (code done — see Completed)
- [ ] Verify auto-scroll kicks in for long messages (IP address, AP credentials)
- [ ] **Possible DMD2 API adjustment**: if the sketch fails to compile, the
  `drawString(x, y, str, len, GRAPHICS_NORMAL)` signature may differ in the
  installed version. Check DMD2 examples and adjust the call signature in the
  four display functions if needed.
- [ ] **"URBAN CONCEPT" overflow** — 13 chars × 6 px = 78 px > 64 px panel width.
  The trim loop in `displayCountdown` and `displayBlink` shaves trailing characters
  until it fits; confirm what the truncated string looks like on hardware and decide
  if raw truncation is acceptable or a smarter abbreviation is needed.

### Font — revisit with hardware in hand

- [x] `SystemFont5x7` confirmed as the only built-in DMD2 font that fits the
  two-row layout. Panel is 16 px tall; type row at y=1 (7 px) + time row at
  y=9 (7 px) fills all 16 px. At 10 mm LED pitch characters are 50×70 mm —
  readable at pit-lane distances.
- [ ] **Visual hierarchy** — both rows currently render at the same font size.
  A smaller type label would de-emphasise the session name and make the
  countdown more prominent. Defer until hardware is present so the result
  can be judged visually.
- [ ] **Smaller font research** — DMD2 ships no font smaller than 5×7.
  `github.com/filmote/Font3x5` exists but is Arduboy-specific (own renderer
  class, writes to Arduboy framebuffer). Its data IS column-based (3 bytes /
  char), the same structure DMD2 uses, so conversion is feasible:
    1. Verify bit ordering in `Font3x5.cpp` (bit 0 = top or bottom?)
    2. Add DMD2 header `[width=3, height=5, firstChar, lastChar]`
    3. Adjust bit order per byte if needed
    4. Drop in as `fonts/Font3x5_DMD2.h`, use with `dmd.selectFont(Font3x5_DMD2)`
       for the type row only; keep `SystemFont5x7` for the time row
  A 3×5 font at 4 px/char fits "URBAN CONCEPT" (52 px) without trimming and
  would also fix the overflow issue. **Do not attempt blind — test on hardware.**

---

## Before the event — Competition prep

- [ ] Change WiFi credentials: comment out `Gilas` network, uncomment `TRANS`
  network in `SEM_Time2LastStart.ino`
- [ ] Re-flash firmware with competition credentials
- [ ] Confirm `raceclock.local` resolves on the venue network (mDNS may not work
  on some managed networks — fall back to IP address if needed)
- [ ] Pre-load the full day's session schedule from pit WiFi before track opens
- [ ] Confirm DS3231 battery is charged and time is correct

---

## Future / design-pending

- [ ] **SEM import overlap rejection** — when both a Prototype and Urban Concept
  session share the same time slot (as they do at most SEM events), the second
  `POST /api/sessions` is silently rejected by the overlap validator. Fix options:
  (a) relax the overlap rule for sessions of different types; or (b) import both
  types in a single request that the server applies atomically. Until fixed, only
  the first type in the response will be imported and the user sees "X rejected"
  in the status bar.
- [ ] **`displayNeedsRefresh` flag** — `loop()` currently resets the clock state
  after an override is cleared by casting `-1` to `ClockState`. Replace with a
  `bool displayNeedsRefresh` flag to make the intent explicit and avoid UB.
- [ ] **Physical buttons** (Up / Down / Confirm / Reset) — GPIO pins TBD.
  Requires designing the on-device UI flow before implementation.
- [ ] **WiFi credential management via web UI** — currently credentials are
  hardcoded and require a reflash to change. A settings page for SSID/password
  stored in NVS would remove this dependency.
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
- [x] Status LED logic (GPIO25/26) — wiring pending
- [x] DS3231 RTC integration code — hardware + library pending
- [x] P10 HUB12 display code via DMD2 — hardware + library pending
- [x] P10 live display preview widget in web UI (`/api/display` + rendered panel)
- [x] ArduinoOTA — firmware upload over WiFi from Arduino IDE (password: `raceclock`)
- [x] ElegantOTA — browser-based firmware + filesystem upload at `/update` (admin / raceclock)
- [x] WARNING blink asymmetric (750 ms on / 250 ms off); type line stays solid
- [x] `handleGetOverride` uses `serializeJson` — safe for quotes and backslashes in override text
- [x] SEM import checks per-session POST results and reports exact ok/rejected counts
- [x] SEM day picker auto-selects today (device time in event timezone via `en-CA` locale)
- [x] SEM import panel auto-closes 2 s after a fully-successful import
- [x] Redundant `typeShort` alias removed from `handleGetDisplay`; uses `sType` directly
- [x] Tabindex on all interactive elements: sessions (1–5), settings (6–9), P10/override (10–12), web files (13–14), session form (20–25), confirm dialog (30–31)
