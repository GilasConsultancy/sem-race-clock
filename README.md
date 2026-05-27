# SEM Race Clock — Time to Last Start

An ESP32-based race clock for **Shell Eco-marathon** team operations. It counts down to the last-start cut-off of each on-track session and drives a P10 LED display panel visible from the pit area. A web management interface (served from the device itself) lets you configure sessions and settings from any phone or laptop on the same network.

A standalone `webonly.html` page replicates the full display on any laptop or TV — useful for a second screen at the pit.

---

## Hardware

| Component | Details | Status |
|---|---|---|
| ESP32 | DFRobot FireBeetle 2 ESP32-E | ✅ In use |
| Status LEDs | Two 5 mm LEDs — red (GPIO25) + green (GPIO26) | ✅ Code ready |
| DS3231 RTC | I²C real-time clock, SDA=GPIO21 / SCL=GPIO22 | ⏳ ~2 days |
| P10 LED panels | HUB12, 32×16 px, ×2 → 64×16 total | ⏳ ~3 weeks |
| Push buttons | Up / Down / Confirm / Reset | ⏳ pending |

---

## Software dependencies

Install these through the Arduino IDE Library Manager or board manager:

| Library | Purpose |
|---|---|
| **ESP32 Arduino core** | Board support (Espressif) |
| **ezTime** | NTP sync, IANA timezone handling |
| **ArduinoJson** | Session persistence (JSON) |
| **LittleFS** (built-in) | Flash filesystem for web files |
| **Preferences** (built-in) | NVS storage for sessions, timezone, and settings |
| **ArduinoOTA** (built-in) | Firmware upload over WiFi from Arduino IDE |
| **ElegantOTA** (by Ayush Sharma) | Browser-based firmware + filesystem update at `/update` |
| **HTTPUpdate** (built-in) | Self-OTA from GitHub Releases |
| **RTClib** (Adafruit) | DS3231 real-time clock — enable with `#define HAS_RTC` |
| **DMD2** (Freetronics) | P10 / HUB12 display driver — enable with `#define HAS_P10` |

---

## File structure

```
SEM_Time2LastStart/
├── SEM_Time2LastStart.ino   # Firmware — all device logic
├── partitions.csv           # Custom partition table (1536 KB OTA slots, 960 KB LittleFS)
├── version.json             # Current firmware version (managed by CI)
└── data/                    # LittleFS filesystem image
    ├── index.html           # Web management UI (self-contained)
    ├── style.css            # Shell-branded stylesheet
    └── webonly.html         # Standalone full-screen display (laptop / TV)
```

---

## Building and flashing

### 1. Board setup

In Arduino IDE, select **DFRobot FireBeetle 2 ESP32-E** (or generic ESP32 Dev Module).

The project includes `partitions.csv`, which overrides the default partition table and gives each OTA slot 1,536 KB (vs 1,280 KB default) while keeping 960 KB for LittleFS. Arduino IDE picks this file up automatically — no manual `Tools → Partition Scheme` selection is required.

### 2. WiFi credentials

WiFi credentials are **not compiled into the firmware**. On first boot (or if no credentials are stored), the device starts an access point:

- SSID: **`RaceClock`** (or `RaceClock2`, `RaceClock3`, … for additional devices)
- Password: **`raceclock1`**

Connect to that network, open **`http://192.168.4.1`**, and enter your WiFi details in the **WiFi** card. The device saves them to NVS (a separate flash partition), restarts, and joins your network. Up to 10 networks can be stored; they are tried top-to-bottom on each boot. Credentials survive all future firmware and filesystem OTA updates.

To switch networks (e.g. home → competition venue), just update the WiFi card in the web UI.

### 3. Flash the firmware

Upload `SEM_Time2LastStart.ino` via **Sketch → Upload**.

### 4. Upload the web files

Install the **Arduino LittleFS Upload** plugin, then use **Tools → ESP32 LittleFS Data Upload** to push the `data/` folder to flash.

If the uploader reports "No port specified", restart the IDE — it reads the port from a build cache that can go stale.

### 5. After first USB flash — use OTA for subsequent updates

Once the firmware is running on the device and it has joined WiFi, you can update without a USB cable:

**Firmware via Arduino IDE (ArduinoOTA)**

The device appears as a network port in **Tools → Port** (look for `raceclock` under "Network ports"). Select it and upload normally. When prompted for a password, enter **`raceclock`**.

**Firmware or filesystem via browser (ElegantOTA)**

Open **`http://raceclock.local/update`** and upload:
- a firmware `.bin` (produced by **Sketch → Export Compiled Binary**), or
- a filesystem `.bin` (produced by the LittleFS Data Upload tool — use the `.bin` file it drops in the sketch folder rather than flashing it directly).

Credentials: username **`admin`**, password **`raceclock`**.

**Firmware via GitHub Releases (from the web UI)**

Open the **Firmware** card in the management UI (`http://raceclock.local`):

1. Click **Check for updates** — the device fetches `version.json` from this repository's `main` branch and compares it against the installed version.
2. If a newer version is available, click **Install update** — the device downloads `firmware.bin` from the matching GitHub Release and reboots.

The page polls every 3 seconds and shows a confirmation once the device is back online.

> **Session data is stored in NVS**, a separate flash partition. It survives both firmware and filesystem OTA updates, as well as manual LittleFS uploads from the IDE.

### 6. Enable hardware features

Near the top of `SEM_Time2LastStart.ino`, two feature flags are commented out by default so the sketch compiles without the optional libraries:

```cpp
// #define HAS_RTC   // uncomment when DS3231 is wired up + RTClib installed
// #define HAS_P10   // uncomment when P10 panels are wired up + DMD2 installed
```

The status LEDs (GPIO25 / GPIO26) are always compiled in — no flag needed.

---

## First boot

On power-up the device:

1. Both LEDs flash briefly (self-test).
2. If DS3231 is present and has a valid time, the clock starts immediately — no network required.
3. Connects to the configured WiFi network (STA mode).
4. If WiFi fails, starts a fallback access point: **SSID `RaceClock`, password `raceclock1`**. Connect your phone/laptop to that network and open `http://192.168.4.1`. Sessions can still be configured, but the clock will not show correct time until a network with internet access is available.
5. Syncs time via NTP (ezTime, up to 10 s wait). On success the DS3231 is updated from NTP.
6. Scans for other race clocks via mDNS, negotiates a unique device number, and registers as `raceclock.local` (or `raceclock2.local`, etc.) on port 80.

Open **`http://raceclock.local`** or the device's IP address to access the management UI.

### Status LED meaning

| Green solid | WiFi connected + NTP synced |
|---|---|
| Green slow blink | WiFi connected, NTP not yet locked |
| Red solid | No WiFi (STA disconnected or AP mode) |

---

## Clock states

The display cycles through these states automatically:

| State | Condition | Display |
|---|---|---|
| **NO SESSION** | No sessions configured | `NO SESSION` |
| **TRACK CLOSED** | Before first session of the day | `TRACK CLOSED` (static) |
| **COUNTDOWN** | Session open, > warning threshold to last start | `HH:MM:SS` counting down |
| **WARNING** | Within warning window of last start (default 5 min) | `HH:MM:SS` blinking (750 ms on / 250 ms off) |
| **LAST START** | Past last start, session still running | `00:00:00` blinking (1 s) |
| **TRACK CLOSED** | All sessions done for the day | `TRACK CLOSED` blinking (1 s) |

---

## Web UI

The management interface is a single-page app served directly by the ESP32.

### Sessions card

- Lists all sessions for the day with their status (upcoming / active / past).
- **Add session** opens a form with four fields: vehicle type, start time, last-start time, end time.
- Times must satisfy `start < last start < end`. The firmware also rejects overlapping sessions.
- **Import from official schedule** fetches the current event's session list from `results.sem-app.com` directly from the device.
- Sessions are sorted by start time and persisted to NVS.

### Settings card

| Setting | Description |
|---|---|
| **Event location** | IANA timezone used for the clock display and session status. Preset list covers common SEM venues; "Other" accepts any IANA name. |
| **Warning threshold** | Minutes before last start at which the countdown begins blinking (1–30, clamped server-side). |

### Clock card

Shows the current local event time, synced to the device's NTP clock. A small indicator below the clock shows whether NTP is synchronised.

### Firmware card

Shows the installed firmware version. Use **Check for updates** and **Install update** to upgrade over-the-air from GitHub Releases.

---

## webonly.html — standalone display

`data/webonly.html` is a self-contained full-screen countdown page designed to run on a laptop with a TV connected. It does not talk to the ESP32 — it fetches the session schedule directly from `results.sem-app.com` (CORS is allowed), manages its own session list in `localStorage`, and mirrors the full state machine of the firmware.

Open it in any browser by double-clicking the file (or serving it locally). Use the ⚙ button (or press `S`) to open the settings overlay.

---

## Multi-device setup

Two (or more) devices can run side by side at the same event — e.g. one in each pit area — without any manual configuration.

On boot, each device scans the local network for other race clocks via mDNS and automatically claims a unique device number:

| Device number | Hostname | AP fallback SSID |
|---|---|---|
| 1 (first / only) | `raceclock.local` | `RaceClock` |
| 2 | `raceclock2.local` | `RaceClock2` |
| 3 | `raceclock3.local` | `RaceClock3` |
| … | … | … |

The number is stored in NVS and reused on subsequent boots. If a conflict is detected (another device already claimed the same number), the lower-priority device picks the next free number and saves it.

In the web UI, the **WiFi** card shows a list of other race clocks discovered on the network. A **Sync sessions** button copies all sessions from a peer device, which is the easiest way to keep both clocks in step without entering the schedule twice.

---

## API reference

All endpoints return JSON.

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/time` | `{ epoch, synced }` — UTC epoch + NTP status |
| `GET` | `/api/display` | `{ state, line1, line2, blinkMs }` — current P10 content |
| `GET` | `/api/settings` | `{ tz, warnMinutes, maxSessions, ntpSynced, apMode, hostname, deviceNum }` |
| `POST` | `/api/settings` | `{ tz, warnMinutes }` — save settings |
| `GET` | `/api/sessions` | Array of session objects |
| `POST` | `/api/sessions` | `{ index, session }` — add (index -1) or edit |
| `POST` | `/api/sessions/delete` | `{ index }` |
| `POST` | `/api/sessions/clear` | Remove all sessions |
| `GET` | `/api/override` | `{ text }` — current override message |
| `POST` | `/api/override` | `{ text }` — set override (empty string clears) |
| `GET` | `/api/sem/days` | `{ eventName, days[] }` — available days from SEM API |
| `GET` | `/api/sem/sessions?date=YYYY-MM-DD` | Array of parsed sessions for that day |
| `GET` | `/api/peers` | Array of `{ hostname, ip }` for other race clocks on the network |
| `GET` | `/api/wifi` | `{ connected, ip, apMode, networks[] }` — WiFi status + saved networks |
| `POST` | `/api/wifi/add` | `{ ssid, password }` — add or update a saved network |
| `POST` | `/api/wifi/remove` | `{ index }` — remove a saved network |
| `POST` | `/api/wifi/move` | `{ index, direction: "up"\|"down" }` — reorder saved networks |
| `POST` | `/api/wifi/restart` | Restart to reconnect using saved network list |
| `GET` | `/api/version` | `{ version }` — installed firmware version |
| `GET` | `/api/checkupdate` | `{ current, latest, updateAvailable }` — compare vs GitHub |
| `POST` | `/api/doupdate` | Trigger OTA from GitHub Releases (device reboots on success) |

---

## Releasing a new firmware version

Releases are fully automated by a GitHub Actions workflow (`.github/workflows/release.yml`).
Pushing a version tag compiles the firmware in CI, names the binary `firmware.bin`, and
publishes a GitHub Release — no manual binary export or renaming required.

### Release steps

The git tag is the **only** thing you need to change. The workflow patches
`version.json` and `FIRMWARE_VERSION` in the sketch automatically, then commits
those changes back to `main` so the device OTA check can find the new version.

```bash
git tag v0.4.2
git push origin v0.4.2
```

That's it. GitHub Actions compiles the firmware, packages the web files into a
LittleFS image, commits the version bump to `main`, and publishes a Release with
`firmware.bin` and `littlefs.bin` attached.

---

## P10 display layout

The 64×16 px panel uses two custom bitmap fonts rendered pixel-by-pixel via `dmd.writePixel()`:

- **Large digit font** — 6 wide × 9 tall, used for the countdown time
- **Small label font** — 3 wide × 5 tall, used for the session type label

System messages (TRACK CLOSED, NO SESSION, CONNECTING…) use `Arial14` centred at y=1. Long messages scroll automatically at 40 px/s (2 LED px per 50 ms).

Countdown and blink states use a two-row layout:

```
┌────────────────────────────────────────────────────────────────┐  ← row 0
│                      Prototype                                 │  ← rows 0–4  (3×5 label, centred)
│                                                                │  ← rows 5–6  (blank separator)
│                       01:23:45                                 │  ← rows 7–15 (6×9 digits)
└────────────────────────────────────────────────────────────────┘  ← row 15
```

Horizontal digit layout (total 64 px): 8 px margin | D0 D1 | 4 px colon | D2 D3 | 4 px colon | D4 D5 | 9 px margin.

---

## Pending features (hardware not yet arrived)

- **Physical buttons** — Up / Down / Confirm / Reset for on-device control without the web UI.
