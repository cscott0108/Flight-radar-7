<div align="center">

# ✈️ ESP32 Flight Radar — 7" Basic

**A real-time ADS-B flight radar built for the Elecrow 7" CrowPanel Basic HMI display.**

Live aircraft data from the **OpenSky Network API**, rendered on an animated, sweep-style radar screen — fully configured through an on-device WebUI, no reflashing required.

*A heavily modified fork of the original Flight Radar project by Tech Talkies.*

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](#license)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.1-informational)
![Graphics](https://img.shields.io/badge/graphics-LVGL%208.4-orange)

</div>

---

## ✨ Key Features

### 🌐 WebUI & Display Management

- **Brightness control** — full LCD backlight adjustment from the WebUI, including a manual slider.
- **Day/night scheduling** — separate poll-interval and brightness behavior for day vs. night, on a configurable local time window (UTC offset in minutes, e.g. PDT = `-420`, PST = `-480`).
- **Idle dimming** — automatically dims the backlight after a period with zero aircraft in range, restoring instantly when traffic reappears.
- **Low-traffic poll slowdown** — stretches the OpenSky polling interval when few or no aircraft are nearby, to conserve API quota.
- **Airfield map markers** — define custom airport/airfield reference dots on the radar, each with **its own configurable color** (not just a fixed red).

### 🎨 Aircraft Classification & Visuals

- **Runway-style compass** — clean outer ring with compact 2-digit aviation headings (`36`, `09`, `18`, `27`).
- **"Craft Type" classification** — replaces OpenSky's unreliable/empty `category` field with a fully custom, user-editable classification system covering **ten** categories:

  | Type | Color | Type | Color |
  |---|---|---|---|
  | Personal *(default/unknown)* | ⚪ White | Military | 🫒 Olive |
  | Private | ⬜ Grey | Police | 🔵 Blue |
  | Business | 🩵 Powder Blue | Emergency Services | 🔴 Red |
  | Commercial | 🟠 Orange | Interesting *(personal watchlist)* | 🩷 Pink |
  | Cargo | 🟣 Purple | Important *(VIP / high-priority)* | 🟡 Yellow fill, 🔴 red border |

- **Manual Aircraft Type (Fixed-Wing / Helicopter)** — independent of classification color. Any registered aircraft can be manually marked as a helicopter, rendering it as a **solid circle with a ring** in its classification's color, instead of the usual triangle. This is deliberately manual: OpenSky has no reliable way to tell fixed-wing and rotary-wing aircraft apart, so it's never guessed automatically.
- **Marker hierarchy:**
  - **Personal** aircraft (the default/fallback): small **hollow outline triangle**.
  - **Important:** solid filled triangle with a **red outline** — yellow fill, red border, for maximum visibility.
  - **Every other classification:** solid filled triangle, colored per the table above.
  - **Any classification, when manually marked as a Helicopter:** solid circle (≈18px) with a ring in that classification's color — e.g. a Police helicopter is a blue-ringed circle, an Emergency helicopter is a red one. An Important helicopter's ring is red (matching its border), while every other classification's ring is a fixed gray.

### 🏷️ Custom Callsigns, Registrations & Operators

- **Built-in defaults** — a maintained table of common airline ICAO codes, military/police/emergency callsign prefixes, shipped out of the box.
- **VIP / special-mission prefix rules** — seeded defaults (`SAM`, `SPAR`, `EXEC`, `PAT` → Important) for watching VIP and government-transport call signs, fully editable/removable like any other rule. These are ordinary prefix rules, not a hard-coded list — a specific registration you configure separately (e.g. `SAM123`) always takes precedence over the broader pattern.
- **Notes** — an optional, free-text field on any registration entry (e.g. `HL8299 → Interesting → "South Korea, LG Electronics"`), persisted alongside its classification, editable from the WebUI and shown as a tooltip on the Current Aircraft table.
- **Fully editable via WebUI** — add, edit, delete, or restore-to-default any operator code or aircraft registration rule directly from the browser, no hard caps on how many you keep (backed by plain CSV files on the device's flash, not a fixed-size table).
- **Bulk backup/restore** — export and re-import your custom classification rules as CSV.
- **One-click Lookup + Add/Edit** — the Current Aircraft table lets you look up any aircraft currently in range and configure its classification/type in a couple of clicks, prefilled with what's already known about it.

### ⚙️ Telemetry, Filtering & Stability

- **Ground traffic filter** — hides grounded aircraft (on-ground flag, or near-zero altitude *and* speed as a fallback for feeders that omit the flag).
- **API rate-limit guard** — detects HTTP 429 quota exhaustion, shows an on-screen warning banner, and backs off for an hour before retrying automatically.
- **Toggleable serial debug logging** — inspect raw OpenSky payloads over Serial without reflashing.

---

## 🛠️ Hardware & Software Stack

| | |
|---|---|
| **Hardware** | Elecrow CrowPanel Basic 7" — ESP32-S3, 800×480 RGB TFT |
| **Framework** | ESP-IDF v6.1 / FreeRTOS |
| **Graphics** | LVGL 8.4, UI laid out in SquareLine Studio, driven via the ESP32-S3's native RGB LCD peripheral (`esp_lcd`) |
| **Data source** | OpenSky Network REST API |

> **Note on touch:** this board has a GT911 capacitive touch controller, but it does not respond over I2C on this particular unit — a known, community-documented hardware-level issue with this SKU rather than a bug in this firmware. All configuration is done through the WebUI instead.

---

## 🚀 Configuration & Usage

1. **Wi-Fi & WebUI setup** — on first boot, connect to the device's fallback access point and configure your home Wi-Fi. Once connected, access the WebUI at the device's IP address.
2. **OpenSky credentials** — upload your OpenSky API client credentials through the WebUI to authorize polling.
3. **Radar & classification setup** — set your center latitude/longitude and range, timezone/UTC offset, day/night and idle-dim schedules, and your custom aircraft/operator classification rules.

---

## 🗺️ Roadmap

- [ ] Aircraft trails and path history
- [ ] Adjustable/independent distance-ring spacing
- [ ] ADS-B receiver hardware integration (local reception, not just OpenSky)
- [ ] Automatic DST switching for the day/night UTC offset (currently a manual twice-a-year setting)
- [ ] Revisit touchscreen support if a working fix for this board's GT911 issue ever surfaces

---

## 📄 License

MIT License
