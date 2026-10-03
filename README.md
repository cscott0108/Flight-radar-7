<div align="center">

# ✈️ ESP32 Flight Radar — 7" Basic

**A real-time ADS-B flight radar built for the Elecrow 7" CrowPanel Basic HMI display.**

Live aircraft data from **OpenSky Network** or **adsb.lol**, rendered on an animated, sweep-style radar screen — fully configured through an on-device WebUI, no reflashing required.

*A heavily modified fork of the original Flight Radar project by Tech Talkies.*

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](#license)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.1-informational)
![Graphics](https://img.shields.io/badge/graphics-LVGL%208.4-orange)

</div>

---

## ✨ Key Features

### 🌐 Aircraft Data & Providers

- **Two supported data providers, switchable from the WebUI:**
  - **OpenSky Network** (default) — the original data source, ~4000 requests/day, requires free OpenSky client credentials.
  - **adsb.lol** — a community-run ADSBExchange-compatible API. No account or credentials needed.
- **Automatic Aircraft Type detection (adsb.lol only)** — adsb.lol's ADS-B category field gives a best-effort hint (helicopter, glider/UAV/lighter-than-air, ground vehicle), but it's only ever a *hint*: any aircraft you've manually classified in the registry keeps your manual choice, always. OpenSky's own category data is too unreliable to use for this and is never consulted.
- **Provider operator (adsb.lol, best effort)** — if adsb.lol includes an owner/operator name for an aircraft it is shown (labelled as coming from the provider) and kept in the Seen Aircraft history. This field is not part of adsb.lol's documented output, so it may be empty. OpenSky never provides one.
- **Robust to bad responses** — a malformed, truncated or unexpected response, or coordinates outside the valid range, never crashes the radar or wipes the aircraft already on screen; failed polls keep the last good picture.
- **Provider diagnostics mode** — Off / Normal / Verbose / Raw, toggled from the WebUI, no reflashing. Verbose shows exactly why each aircraft got the Aircraft Type it did (provider hint vs. your registry override); Raw adds a size-capped, credential-redacted preview of the provider's response for troubleshooting.
- **Runtime capacity report** (`/diag`) — a lightweight, read-only page showing live memory health (internal, DMA-capable internal and PSRAM free / largest block / minimum-ever, plus failed memory allocations since boot), filesystem usage, current vs. maximum-observed aircraft count, and how full your custom-rule/operator/airport/Seen-Aircraft storage is. No reflashing, no serial connection needed. See *Memory Stability & Diagnostics* below.
- Switching providers is instant and doesn't require reconfiguring your radar location, range, or classification rules — they're shared across both.

### 🕑 Time Zone & Daylight Saving

- Pick your **time zone** by name (about 90 IANA zones such as `America/Los_Angeles`, `Europe/London`, `Australia/Sydney`, `Asia/Kolkata`), or use a **custom fixed UTC offset** for anywhere not listed.
- **Automatic daylight saving** switches PST/PDT (and the EU, Australian and New Zealand equivalents) on the correct dates by itself. It's a toggle, so regions that don't observe DST — or anyone who prefers standard time all year — can turn it off.
- Times are stored in UTC and converted only for display, so changing your zone never corrupts history. If the clock hasn't synchronized yet, the UI says "Not synchronized" instead of showing a made-up time.
- The day/night schedules use the same zone and DST setting. Existing installs keep working exactly as before: an older UTC-offset setting is carried over as a custom fixed offset.

### 📒 Current Aircraft & Seen Aircraft History

- **Current Aircraft** (`/current`) explains what you're looking at: call sign, ICAO24, craft type, Aircraft Type with its icon, operator (provider-supplied and configured operators are shown separately), your registry rule and note, and a **Decided by** column that says whether the classification and the Aircraft Type came from your registry, a built-in rule, the provider, or the default.
- **Seen Aircraft** (`/seen`) is a persistent log of every aircraft the radar has picked up, one row per ICAO24 and independent of which provider reported it: call sign, types, operator, first/last seen (in your time zone), seen count, and whether you've already configured it. Search, filter (all / configured / not configured), sort, and jump straight into the existing Add/Edit dialog.
- **CSV export** for backup or spreadsheets (times in ISO-8601 UTC). There is no import, on purpose.
- **Flash-friendly by design:** sightings are kept in RAM and saved incrementally — only the aircraft that actually changed since the last save, not the whole history — at most every 10 minutes, and before a deliberate reboot, capped at **3,000 aircraft** with the least-recently-seen dropped first, and never written if flash space is tight. *Seen count* counts visits — an aircraft returning after 30 minutes or more counts again — rather than every poll.

### 🌐 WebUI & Display Management

- **Organized pages with one consistent look** — **Setup** (`/`), **Current Aircraft** (`/current`), **Registered Aircraft** (`/registered`: registry rules plus the Add/Edit dialog), **Operators** (`/operators`), **Seen** (`/seen`) and **History** (`/history`) share a navigation bar of plain links (works without JavaScript) and one stylesheet. The old `/rules` address redirects to Registered Aircraft, including `?edit=` links. From **Seen**, *Add / Edit* opens that aircraft's registration directly. `/diag` and `/airports` are reachable by URL/link from Setup.

- **History page** (`/history`) — the persistent record on the TF card, kept distinct from the other views: *Current/Radar* is what is overhead now, *Seen* is the recent/hot list, *History* is what the card has stored, and *Diag* is system/storage diagnostics. A read-only table of ICAO24, call sign, aircraft type / craft class, operator, first seen, last seen and seen count, 50 rows per page, browsing the on-card group files in fingerprint order (unassigned first, registry-defined last) with a *group* link per row to list one group, plus an exact ICAO24 lookup. A record that the index no longer considers current (the aircraft moved to a different operator/registry group) is shown, not hidden, with a **Superseded** badge; **Not indexed** and **Unverified** badges cover a missing index entry and an unreadable index. The page only reads the card: it never writes, deletes or repairs anything, never loads a group file into RAM (at most 8 records at a time) and releases the storage lock before sending each part of the page, so it cannot stall history writes. The operator is not stored in the record; it is derived from the group's fingerprint and the current operator list, so a group whose operator has since been removed shows as an unknown/removed operator. Linked from **Seen** and **/diag**.
- **Optional features (Setup → Features & appearance)** — switch **Seen logging**, **Current Aircraft**, **Registered Aircraft matching** and **Registered Operators matching** on or off; they persist across reboots. Turning one off never deletes data and never affects tracking or the radar display: aircraft simply use the default classification where a registry/operator lookup is off, and Seen stops adding new records (existing history stays visible).
- **Dark mode** — optional, persistent, WebUI only (the radar screen is unchanged). Off by default.
- **Firmware build identifier** — put a one-line build name in `src/VERSION` (for example `07.04.00-WEBUI1`); it is compiled in at build time and shown as **Firmware Build** at the top of `/diag`, so you can tell which firmware is really running. A missing or empty file stops the build.
- **Brightness control** — full LCD backlight adjustment from the WebUI, including a manual slider.
- **Day/night scheduling** — separate poll-interval and brightness behavior for day vs. night, on a configurable local time window that follows your configured time zone and daylight saving setting.
- **Idle dimming** — automatically dims the backlight after a period with zero aircraft in range, restoring instantly when traffic reappears.
- **Low-traffic poll slowdown** — stretches the polling interval when few or no aircraft are nearby, to conserve API quota.
- **Airfield map markers** — define custom airport/airfield reference dots on the radar (up to 100), each with **its own configurable color** (not just a fixed red), and a choice of **Dot** or **Directional** marker mode. A fresh install starts with about 50 real Northern California airports (plus a few major West Coast/Southwest hubs) already on the map as a starting point — edit, remove, or add your own from `/airports` at any time; an existing install's list is never touched by this. Directional mode draws a simplified runway-axis marker (a short line through the airport, capped at each end, with a small center dot) along the primary runway's physical axis — enter a runway designator like `09`, `27L` or `18C` and it figures out the orientation, correctly treating reciprocal ends (09/27, 18/36, ...) as the same physical strip. If the runway designator is left blank or doesn't parse, the airport just shows as a plain dot, same as before — it never guesses. The `/airports` preview page shows exactly what the radar will draw.
- **Off-screen aircraft indicators** — a known aircraft that's flown beyond your configured radar range still gets a small marker on the outer ring, positioned by its real geographic direction from your radar (never by which way it's pointed), in its usual classification color. Moves and rescales correctly with whatever range you have set, from the smallest to the largest supported value.

### 🎨 Aircraft Classification & Visuals

- **Runway-style compass** — clean outer ring with compact 2-digit aviation headings (`36`, `09`, `18`, `27`).
- **"Craft Type" classification** — a fully custom, user-editable classification system covering **ten** categories, independent of whichever data provider is active:

  | Type | Color | Type | Color |
  |---|---|---|---|
  | Personal *(default/unknown)* | ⚪ White | Military | 🫒 Olive |
  | Private | ⬜ Grey | Police | 🔵 Blue |
  | Business | 🩵 Powder Blue | Emergency Services | 🔴 Red |
  | Commercial | 🟠 Orange | Interesting *(personal watchlist)* | 🩷 Pink |
  | Cargo | 🟣 Purple | Important *(VIP / high-priority)* | 🟡 Yellow fill, 🔴 red border |

- **Aircraft Type (Fixed-Wing / Helicopter / Other)** — independent of classification color. Any aircraft can be marked as a **Helicopter** (solid circle) or **Other** (diamond, for uncommon aircraft — airships, autogyros, and the like) instead of the usual triangle, either automatically (adsb.lol hint) or manually via a registry rule. A manual registry rule always wins over the automatic hint, even if that rule just confirms Fixed-Wing.
- **Directional indicators** — Helicopter and Other markers both show heading: a short line through the helicopter's ring, and a small gray/black forward tip on the Other diamond. Both use the radar's usual north-up convention and simply don't draw if heading is unavailable.
- **Marker hierarchy:**
  - **Personal** aircraft (the default/fallback): small **hollow outline triangle**.
  - **Important:** solid filled triangle with a **red outline** — yellow fill, red border, for maximum visibility.
  - **Every other classification:** solid filled triangle, colored per the table above.
  - **Any classification, when marked as a Helicopter:** solid circle (≈18px) with a ring in that classification's color, plus a heading line in the same color — e.g. a Police helicopter is a blue-ringed circle, an Emergency helicopter is a red one. An Important helicopter's ring (and heading line) is red (matching its border), while every other classification's ring is a fixed gray.
  - **Any classification, when marked as Other:** a classification-colored **diamond** with a fixed gray/black forward tip showing heading — e.g. a pink diamond for Interesting, an olive diamond for Military. Important's diamond gets its usual red outline in addition to the yellow fill; the tip stays gray/black regardless of classification.

### 🏷️ Custom Callsigns, Registrations & Operators

- **Built-in defaults** — a maintained table of common airline ICAO codes, military/police/emergency callsign prefixes, shipped out of the box.
- **VIP / special-mission prefix rules** — seeded defaults (`SAM`, `SPAR`, `EXEC`, `PAT` → Important) for watching VIP and government-transport call signs, fully editable/removable like any other rule. These are ordinary prefix rules, not a hard-coded list — a specific registration you configure separately (e.g. `SAM123`) always takes precedence over the broader pattern.
- **Notes** — an optional, free-text field on any registration entry (e.g. `HL8299 → Interesting → "South Korea, LG Electronics"`), persisted alongside its classification, editable from the WebUI and shown as a tooltip on the Current Aircraft table.
- **Fully editable via WebUI** — add, edit, delete, or restore-to-default any operator code or aircraft registration rule directly from the browser, no hard caps on how many you keep (backed by plain CSV files on the device's flash, not a fixed-size table).
- **Bulk backup/restore** — export and re-import your custom classification rules as CSV.
- **One-click Lookup + Add/Edit** — the Current Aircraft table lets you look up any aircraft currently in range and configure its classification/type in a couple of clicks, prefilled with what's already known about it.

### ⚙️ Telemetry, Filtering & Stability

- **Ground traffic filter** — hides grounded aircraft (on-ground flag, or near-zero altitude *and* speed as a fallback for feeders that omit the flag), applied identically regardless of which provider is active.
- **API rate-limit guard** — detects rate-limit responses from the active provider, shows an on-screen warning banner naming that provider, and backs off automatically before retrying (an hour for OpenSky's documented quota, a shorter conservative pause for adsb.lol, which publishes no fixed quota).
- **Toggleable serial debug logging** — inspect raw OpenSky fields, or the new multi-level provider diagnostics (Normal/Verbose/Raw, covering both providers), over Serial without reflashing.

### 🧠 Memory Stability & Diagnostics

- **Why it matters:** the ESP32-S3 has megabytes of PSRAM, but Wi-Fi and TLS need a specific, scarce kind of memory — *internal, DMA-capable* RAM. An earlier version failed intermittently on OpenSky HTTPS requests because Wi-Fi could not get a ~1.6 KB internal DMA buffer, even though plenty of total memory was free.
- **What was changed (implemented):** CPU-only working buffers no longer take that scarce memory. The WebUI's upload, main-page and `/seen` page buffers now live in PSRAM; the OpenSky HTTP client's own buffers and its authorization header are deliberately placed in PSRAM; one-time crypto/network initialization is done at a controlled point during boot; and LVGL's 64 KB graphics memory pool is provided from PSRAM. Internal DMA-capable RAM is intentionally kept free for Wi-Fi, TLS and hardware (display, SD card).
- **Tested:** after the WebUI-buffer and HTTP-client changes, the device has been observed completing repeated OpenSky HTTPS requests (HTTP 200) with zero failed allocations, around 72 KB of DMA-capable memory free and a ~31.7 KB largest DMA-capable block. The LVGL pool move is built and verified at link time (+64 KB internal DMA-capable heap) but still awaits a long on-device soak.
- **TF diagnostics and device controls on `/diag`:** a stage-by-stage view of the TF/microSD pipeline (mount, filesystem, history folder, index, last error with codes), a non-destructive **TF self-test** (writes, reads back and deletes one scratch file; never touches history data), **Unmount TF / Reinitialize TF** (real teardown and re-init of the card driver, showing which step succeeds or fails), and **Reboot Device** (confirmation dialog; a normal restart that keeps all settings, Seen data, registered aircraft/operators/rules and the card contents).
- **Diagnostics:** the normal `/diag` page shows DMA-specific memory health and a failed-allocation counter (with the size, type and task of the last failure) — no special mode needed. Optional **Advanced Diagnostics** and **Expert Debug** modes (toggled on `/diag`, applied after a reboot, off by default) add detailed heap tracing for troubleshooting; normal operation never needs them.

### 🗄️ Data Storage (HOT / WARM / COLD)

| Tier | Where | What |
|---|---|---|
| **HOT** | RAM / PSRAM | Current aircraft, the Seen Aircraft table, the History Manager's per-aircraft working copies (with change tracking), and the Universal Value table (a compact, stable ID for each operator, in PSRAM) |
| **WARM** | Internal flash (SPIFFS) | Seen Aircraft history, saved **incrementally** (only changed aircraft) at most every 10 minutes; settings in NVS; your rule/operator CSV files |
| **COLD** | TF / microSD card | *(active; still being verified)* Long-term per-aircraft history, grouped into files by operator with an on-card index and per-record checksums |

**TF/microSD status: working, still being verified.** The long-term history code (History Manager, Universal Value table, on-card index and history files) is switched on; the card is mounted at boot and history is written to it. On the development unit the earlier "nothing is written" symptom was caused by a **defective, read-only 16 GB card**, not by the firmware; with a working 64 GB FAT32 card the history folder and index are created, records are written (41 created in the first run, 0 errors) and the index reloads correctly after a reboot. `/diag` now shows each TF stage with its error codes and has a non-destructive TF self-test, so a bad card shows up immediately ("card mounts but cannot be written"). Still to be confirmed over a longer run: lookups finding existing history after a reboot, the 10-minute flush cycle and multi-day behaviour. Several planned pieces are not built yet: moving existing Seen history onto the card, a ~12-hour "hot" Seen cache, running card work in its own background task, and file compaction.

The card is optional either way: without one, or if it fails, the radar, WebUI and Seen history run normally from RAM and internal flash.

---

## 🛠️ Hardware & Software Stack

| | |
|---|---|
| **Hardware** | Elecrow CrowPanel Basic 7" — ESP32-S3, 800×480 RGB TFT |
| **Framework** | ESP-IDF v6.1 / FreeRTOS |
| **Graphics** | LVGL 8.4, UI laid out in SquareLine Studio, driven via the ESP32-S3's native RGB LCD peripheral (`esp_lcd`) |
| **Data sources** | OpenSky Network REST API (default), adsb.lol REST API |

> **Note on touch:** this board has a GT911 capacitive touch controller, but it does not respond over I2C on this particular unit — a known, community-documented hardware-level issue with this SKU rather than a bug in this firmware. All configuration is done through the WebUI instead.

---

## 🚀 Configuration & Usage

1. **Wi-Fi & WebUI setup** — on first boot, connect to the device's fallback access point and configure your home Wi-Fi. Once connected, access the WebUI at the device's IP address.
2. **Choose a data provider** — from the Radar Settings page, pick **OpenSky Network** (upload your free OpenSky client credentials through the WebUI) or **adsb.lol** (no credentials needed — just select it and go).
3. **Radar & classification setup** — set your center latitude/longitude and range, time zone and daylight saving, day/night and idle-dim schedules, and your custom aircraft/operator classification rules. These apply the same way no matter which provider you've picked.
4. **Watch and learn** — the **Current Aircraft** table shows what's overhead and why it's classified the way it is; **Seen Aircraft** (`/seen`) builds a history you can search, export, and turn into registry/operator rules.

---

## 🗺️ Roadmap

## 🗺️ Roadmap

- [ ] Verify TF/microSD long-term history over a longer run: lookup hits after reboot, flush cycles, multi-day soak, index behavior, and storage integrity. Writes and reboot persistence have already been confirmed on a working card.
- [ ] History CSV export.
- [ ] Revisit touchscreen support if a working fix for this board's GT911 issue can be validated on the current board revision.

---

## 🧪 Building & Testing

- **Firmware version:** edit `src/VERSION` before each build you intend to flash (single line; letters, digits, `.`, `-`, `_`, `+`). CMake re-runs automatically when it changes.
- **Build:** ESP-IDF v6.1, target ESP32-S3, C (gnu23), e.g. VS Code with the ESP-IDF extension, then `idf.py build`. After pulling this version do a **clean rebuild** (widely-included headers changed).
- **Host tests:** an earlier off-device host-test suite (`host_tests/`) is **not included in this repository** at present, so there is currently no automated test run beyond the firmware build itself.
- **Diagnostics build option:** `menuconfig` → *Flight Radar diagnostics* → `FR_ALLOC_TRACE` compiles in an allocation tracer used only for deep memory troubleshooting (default off in Kconfig).
- **Developer notes:** `PROJECT_STATE_COMPACT.md` is the compact technical project-state summary, including the memory design rules that should not be undone.

---

## ⚠️ Current Status & Limitations

- **Verification status:**
  - *Implemented and built:* everything described above builds successfully with ESP-IDF v6.1 (`idf.py build`).
  - *Tested on hardware:* OpenSky login and repeated aircraft HTTPS requests, the WebUI, `/diag` memory reporting, incremental Seen Aircraft saving (with measured timings), TF card mounting, history writes and index reload after reboot (64 GB card).
  - *Not yet confirmed:* long-term (cold) history soak and lookup-after-reboot (writes and index reload work on a working card — see *Data Storage*), a long soak of the LVGL-pool-in-PSRAM change, and adsb.lol's live response.
- **Memory:** the fix above is based on measurement and has behaved well in testing, but it is not a guarantee that memory fragmentation can never recur — `/diag` exists so that can be checked at any time.
- **adsb.lol's live response could not be checked** from the development environment; the parser follows its published field list, and the optional operator field is best effort.
- **Touchscreen:** still non-functional on this board (see above); everything is configured through the WebUI.
- **Time zones:** the built-in list is limited (about 90 zones) and intentionally leaves out places whose daylight-saving rules are irregular or recently changed; use the custom fixed offset there. DST rules are those in force at build time.
- **Seen Aircraft:** up to 10 minutes of sightings can be lost if power is cut; "configured" is judged by call sign (an aircraft with no call sign can't match a registry rule); history is capped at 3,000 aircraft, a safety-margin limit rather than a guaranteed number of days. Incremental saving cut the periodic save from ~5 seconds to under a second on real hardware; a brief (~1 second, measured) radar flicker can still happen during a save, mostly filesystem seek time — a known, minor rough edge.
- adsb.lol's ground-vehicle categories (ADS-B category `C0`–`C7`) are shown with the same diamond marker as other "Other" aircraft, since there's no dedicated ground-vehicle shape; a registry rule can override this per-aircraft if it's noisy at your location.
- adsb.lol publishes no fixed rate limit, so this project applies its own conservative polling floor and backoff rather than a documented provider limit.

---

## 📄 License

MIT License
