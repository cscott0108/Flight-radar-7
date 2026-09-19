# \# ESP32 Flight Radar - 7 Inch Basic

# 

# A real-time Flight Radar built using the \*\*Elecrow 7-inch CrowPanel Basic HMI Display\*\*. The project downloads live aircraft data from the \*\*OpenSky Network API\*\* and displays nearby aircraft on an animated radar screen. 

# 

# This project is a heavily modified fork of the original flight radar by Tech Talkies.

# 

# \---

# 

# \## Key Features \& Major Updates

# 

# \### 🌐 WebUI \& Display Management

# \* \*\*Brightness Control:\*\* Full LCD backlight brightness adjustment accessible directly through the WebUI.

# \* \*\*Day \& Night Logic:\*\* Automatic day/night scheduling with configurable brightness levels based on local time.

# \* \*\*UTC Timezone Offset:\*\* Dynamic local time configuration specified via UTC minutes (e.g., PDT = `-420` mins, PST = `-480` mins).

# \* \*\*Airfield Map Markers:\*\* Dedicated webpage setup to define custom map markers (red dots) for local airports and airfields on the radar view.

# 

# \### 🎨 Aircraft Classification \& Visuals

# \* \*\*Runway-Style Compass:\*\* Clean outer compass ring displaying compact 2-digit aviation headings (`36`, `09`, `18`, `27`).

# \* \*\*Replaced "Category" with "Craft Type":\*\* Replaced OpenSky's unreliable/empty `category` field with custom classification logic to explicitly identify and display Commercial, Emergency, Police, Military, or Private traffic.

# \* \*\*Shape \& Color Hierarchy:\*\*

# &#x20; \* \*\*Private Aircraft:\*\* Small \*\*Hollow/Outline White Triangle\*\*

# &#x20; \* \*\*Non-Private / Special Aircraft:\*\* Large \*\*Solid Triangle\*\* with color coding:

# &#x20;   \* 🟠 \*\*Commercial:\*\* Orange

# &#x20;   \* 🟢 \*\*Military:\*\* Green

# &#x20;   \* 🔵 \*\*Police:\*\* Blue

# &#x20;   \* 🔴 \*\*Emergency Services:\*\* Red

# 

# \### 🏷️ Custom Callsigns \& Registration Overrides

# \* \*\*Default ICAO Pre-configurations:\*\* Built-in default lists for common US commercial airline codes, military prefixes, police units, and emergency services.

# \* \*\*Custom Commercial Prefixes:\*\* Manage custom airline prefixes via the WebUI (e.g., `AAL` for American, `JAL` for Japan Airlines) with support for up to \*\*50 custom codes\*\*.

# \* \*\*Custom Registration Overrides:\*\* Support for up to \*\*100 custom registration tail numbers\*\* with user-selectable classifications (Commercial, Military, Emergency, Police).

# 

# \### ⚙️ Telemetry, Filtering \& Stability

# \* \*\*Ground Traffic Filter:\*\* Filter out grounded aircraft (altitude 0m, speed below 25 km/h, or on-ground flags).

# \* \*\*API Rate Limit Guard (429 Logic):\*\* Automatically detects HTTP 429 quota exhaustion, displays an on-screen rate-limit warning banner, and enters a \*\*1-hour backoff pause\*\* before retrying.

# \* \*\*Display Refresh \& Serial Debugger:\*\* Fixed screen refresh timing issues and added toggleable `Serial` debug logging for real-time payload inspection.

# 

# \---

# 

# \## Hardware \& Software Stack

# 

# \* \*\*Hardware:\*\* Elecrow CrowPanel Basic 7" ESP32-S3 HMI Display (V1.2)

# \* \*\*Framework:\*\* ESP-IDF / FreeRTOS

# \* \*\*Graphics:\*\* LVGL 8 / TFT\_eSPI / SquareLine Studio

# \* \*\*API:\*\* OpenSky Network REST API

# 

# \---

# 

# \## Configuration \& Usage

# 

# 1\. \*\*WiFi \& WebUI Setup:\*\* Connect to the captive portal AP on boot to set up local WiFi. Once connected, access the WebUI via the device's IP address.

# 2\. \*\*OpenSky Credentials:\*\* Upload your OpenSky credentials through the WebUI to authorize API polling.

# 3\. \*\*Radar \& Classification Setup:\*\* Use the WebUI to set your center latitude/longitude coordinates, timezone offsets, day/night schedules, and custom callsign/registration override rules.

# 

# \---

# 

# \## Roadmap

# 

# \* \[ ] Touch support implementation for CrowPanel Basic

# \* \[ ] Manual helicopter override via WebUI (rendering hollow circles for private, solid circles for non-private)

# \* \[ ] Aircraft trails and path history

# \* \[ ] Distance rings and adjustable radar range

# \* \[ ] ADS-B receiver hardware integration

# 

# \---

# 

# \## License

# 

# MIT License

