# COOPSENSE V1

An ESP32-based smart tray inventory tracker. A single load cell reads the weight of whatever's sitting on the tray, and the firmware figures out how many items are left, whether it's time to restock, and lets you check status over WiFi. No manual "fill it up to 100%" calibration step required.

- This is a Arduino IDE based project, not PlatformIO IDE. You may use the default board upload settings.

## Why did I make this

For school cooperatives, it is known that logging items in-stock by hand is very tedious, yeah? I decided to make a tool that would eliminate tedious counting by hand, and use automated systems instead. You can monitor the item stock wirelessly since the ESP32 broadcasts its data under an IP. You can access it via https://textlocked.github.io/COOPSENSE-SMKBBB/

## What it does

- Reads tray weight continuously via an HX711 load cell amplifier
- Learns the weight of a single item through a guided re-weigh flow (hold a button, place one item, wait)
- Tracks multiple item "profiles" (up to 6) so swapping what's on the tray doesn't require re-learning from scratch every time
- Auto-estimates tray capacity by watching restock events over time (asymmetric leaky-max: jumps up fast when a bigger restock is observed, decays slowly if it turns out to be an outlier)
- Shows status on a 16x2 I2C LCD — either item quantity or total weight, with an RSTK / LOW / OK indicator
- RGB LED gives an at-a-glance status color (red = restock, yellow = low, green = good)
- Non-blocking buzzer alerts on state changes, with a mute button (and mute state, tare offset, and item weight all persist across reboots via `Preferences`)
- Small built-in web server exposes live status as JSON and lets you set a nickname for the tray remotely

## Hardware / pin map

| Function | Component | ESP32 Pin |
|---|---|---|
| Load cell data | HX711 `DT` | GPIO 18 |
| Load cell clock | HX711 `SCK` | GPIO 19 |
| LCD | I2C `SDA` | GPIO 33 |
| LCD | I2C `SCL` | GPIO 32 |
| Status LED | Red | GPIO 26 |
| Status LED | Green | GPIO 27 |
| Status LED | Blue (unused channel) | GPIO 25 |
| Silence indicator LED | — | GPIO 13 |
| Buzzer | — | GPIO 4 |
| Tare button | — | GPIO 17 (`INPUT_PULLUP`) |
| Silence button | — | GPIO 16 (`INPUT_PULLUP`) |
| Re-weigh button | — | GPIO 35 (input-only pin — needs an **external pull-down resistor**; idles LOW, reads HIGH when pressed) |

**LCD address:** `0x27` (16x2, I2C backpack)

**RGB LED:** wired common-cathode. `setColor()` drives R/G high together for yellow since there's no dedicated yellow channel.

## Libraries used

- `HX711` — load cell amplifier interface
- `Wire` — I2C bus
- `LiquidCrystal_I2C` — LCD driver
- `Preferences` — persistent storage (tare offset, mute state, item weight, nickname) on ESP32's NVS
- `WiFi` / `WebServer` — onboard status API

## Setup

1. Wire everything per the pin map above. Double-check the re-weigh button's pull-down resistor — GPIO 35 has no internal pull option.
2. Fill in your network credentials near the top of the sketch:
   ```cpp
   const char* WIFI_SSID = "WIFI_SSID_HERE";
   const char* WIFI_PASSWORD = "WIFI_PASSWORD_HERE";
   ```
3. Flash the sketch. On first boot with no saved tare offset, it'll tare automatically with an empty tray — make sure the tray is empty when you power it on for the first time.
4. Set `PASSIVE_BUZZER` to `true` or `false` depending on which buzzer you're using (defaults to `false`, i.e. active buzzer).
5. Adjust `calibration_factor` if your load cell reads noticeably off from a known reference weight.

## Using it

- **Short-press the re-weigh button:** toggles the LCD between quantity view and total weight view (only works once an item weight has been learned).
- **Hold the re-weigh button 5 seconds:** starts the re-weigh flow — tray tares itself, place a single item within 30 seconds, hold it steady for 1 second, then it measures for 5 seconds and averages the result.
- **Short-press the tare button:** re-tares the tray to zero at whatever's currently on it.
- **Hold the tare button 3 seconds:** hard-resets the tare offset to 0 (escape hatch if something's gone wrong).
- **Short-press the silence button:** mutes/unmutes the buzzer.
- **Hold the silence button 3 seconds:** shows the tray's current IP address and WiFi SSID on the LCD for 3 seconds.

## Restock logic

Controlled by the compile-time `THRESHOLD_MODE` constant:

- `MODE_QTY_ONLY` *(current default)* — always thresholds on estimated item quantity, using the auto-learned capacity estimate and hysteresis bands (`PCT_RESTOCK_ENTER/CLEAR`, `PCT_LOW_ENTER/CLEAR`) so the status doesn't flicker right at a boundary.
- `MODE_WEIGHT_ONLY` — always thresholds on raw total weight (`gramThresholdRstk` / `gramThresholdLow`).
- `MODE_FOLLOW_DISPLAY` — matches whatever the LCD is currently showing (quantity vs. total weight).

## Web API

Once connected to WiFi, the tray exposes:

- `GET /status` — returns JSON with nickname, MAC address, total weight, learned unit weight, estimated quantity, stock status, mute state, and uptime.
- `POST /nickname` — set a display nickname for the tray (raw text body, 1–32 characters).

## Notes

- Item profiles and their learned capacity estimates currently live only in RAM — they reset on reboot. The per-item unit weight *is* persisted for the currently active item, but the full profile table (`profiles[]`) is not yet saved to `Preferences`.
- The `LED_B` pin is wired but never driven independently — only used as part of the yellow (R+G) mix.
