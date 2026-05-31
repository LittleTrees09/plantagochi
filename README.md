# Plantagochi — ESP32 Hydroponics Monitor

## 1. Function of the Website

Plantagochi is a web-based hydroponics monitoring dashboard served directly from an ESP32 microcontroller. It provides:

- **Real-time sensor monitoring**: Water level, nutrient concentration (TDS), and pH levels
- **Automated pump control**: Trigger water, drain, nutrient, and pH pumps on-demand or automatically when levels are critical
- **Plant growth tracking**: Visual growth stages and harvest readiness indicator
- **AI-powered plant photo analysis**: Upload photos for CNN-based crop health assessment and growth predictions
- **Local Wi-Fi access**: The ESP32 creates its own access point — no internet required

Simply connect to the **PLANTAGOCHI** Wi-Fi network and open `http://192.168.4.1` in any browser.

---

## 2. How to Enable/Disable DEV TOOLS

**File**: `main/main.c` (embedded HTML/JavaScript)

**Location**: Line ~855 in the `DEV` object

```javascript
const DEV = {
  enabled:        false,      // ← TOGGLE HERE
  showPreviewBar: false,
  waterLevel:     72,
  tdsValue:       650,
  phValue:        6.3,
  growthPoints:   0,
};
```

**To Enable DEV TOOLS:**
- Change `enabled: false` to `enabled: true`
- A floating "⚙ Dev" button will appear in the bottom-right corner of the dashboard
- Clicking it opens a panel with sliders to manually override sensor values

**To Disable DEV TOOLS:**
- Change `enabled: true` to `enabled: false`
- The dev button and panel will be hidden

---

## 3. How to Enable/Disable SIMULATED VALUES

**File**: `main/main.c` (C preprocessor)

**Location**: Line ~142 (firmware-level simulation)

```c
#define SIMULATE_SENSORS 1
```

**To Enable SIMULATED SENSORS** (fake data for testing):
- Keep `SIMULATE_SENSORS 1`
- Water, TDS, and pH values are generated with realistic noise instead of reading real sensors
- Useful for testing without hardware wired up

**To Disable SIMULATED SENSORS** (read real sensors):
- Change to `SIMULATE_SENSORS 0`
- The firmware will read actual ADC values from GPIO 34, GPIO 32, and the I2C ADS1115 chip
- Sensors must be physically connected to the ESP32

---

## 4. How to Flash and Configure

**Prerequisites:**
- ESP-IDF 5.x installed (`idf.py` command available)
- ESP32-WROOM-32 board connected via USB

**Setup Steps:**

1. **Configure Wi-Fi** (`main/main.c`, lines ~62–63):
   ```c
   #define WIFI_SSID       "PLANTAGOCHI"
   #define WIFI_PASSWORD   "12345678"
   ```

2. **Flash the firmware:**
   ```bash
   idf.py set-target esp32
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```
   Replace `/dev/ttyUSB0` with your actual port (`/dev/ttyACM0` on some Linux boards, `/dev/cu.usbserial-XXXX` on Mac).

3. **Connect and access:**
   - On your phone/laptop, join Wi-Fi: **PLANTAGOCHI**
   - Open browser: `http://192.168.4.1/`

---

## 5. Flask AI Server Setup

The ESP32 dashboard can send plant photos to a Flask server running on your laptop for CNN-based analysis. The browser **auto-discovers** the server by scanning `192.168.4.2` through `192.168.4.11` — you do not need to hardcode any IP address.

> Note: The `LAPTOP_IP` define in `main/main.c` is a placeholder left for reference. It is not used by the running firmware — auto-discovery handles finding the server automatically.

### Steps

1. **Connect your laptop** to the **PLANTAGOCHI** Wi-Fi network created by the ESP32.

2. **Start the Flask server** on your laptop:
   ```bash
   cd plantagochi/
   python server.py
   ```
   Flask will print something like:
   ```
   * Running on http://192.168.4.4:5000
   ```

3. **Allow port 5000 through your laptop's firewall** (required so the phone/browser can reach Flask):
   ```bash
   sudo ufw allow 5000
   ```
   This is safe — only devices on the local PLANTAGOCHI hotspot can reach it.

4. **Verify the connection** — open this in your phone's browser (use the IP Flask reported):
   ```
   http://192.168.4.4:5000/health
   ```
   You should see JSON: `{"ok": true, "status": "Plantagochi CNN server running", ...}`

5. **Open the dashboard** on your phone:
   ```
   http://192.168.4.1/
   ```
   The "Could not find server" error will be gone and photo analysis will work.

### Why the laptop IP changes

Your laptop gets a DHCP-assigned IP each time it reconnects to the hotspot (e.g., `.2`, `.4`). Since the browser auto-scans the full `.2`–`.11` range, this does not matter. If you want a stable IP, assign a static IP to the wireless interface on your laptop.

---

## 6. Pump Configuration

The firmware controls **five pumps** via relay outputs. Each pump is triggered by a dashboard button and runs for a configurable duration before switching off automatically.

### Pump Pin Assignments

| Button (Dashboard) | GPIO | Define | Purpose |
|--------------------|------|--------|---------|
| Add Water | GPIO 25 | `PIN_PUMP_WATER` | Fills the reservoir |
| Remove Water | GPIO 26 | `PIN_PUMP_DRAIN` | Drains the reservoir |
| Feed Plant | GPIO 4 | `PIN_PUMP_NUTRIENT` | Doses liquid nutrients |
| pH Down | GPIO 27 | `PIN_PH_DOWN` | Doses pH-lowering solution |
| pH Up | GPIO 33 | `PIN_PH_UP` | Doses pH-raising solution |

### Pump On-Durations

Each pump runs for a fixed duration (in milliseconds) then switches off. Change these in `main/main.c` (~line 85):

```c
#define WATER_ON_DURATION_MS   10000   /* Add Water     — 10 s */
#define DRAIN_ON_DURATION_MS   10000   /* Remove Water  — 10 s */
#define FOOD_ON_DURATION_MS    10000   /* Feed Plant    — 10 s */
#define PH_DOWN_DURATION_MS    10000   /* pH Down       — 10 s */
#define PH_UP_DURATION_MS      10000   /* pH Up         — 10 s */
```

Increase a value to run a pump longer per button press, decrease it to dose less.

### Wiring Relays to the ESP32

All pump output pins use **active-HIGH logic** (GPIO goes HIGH to energize the relay, LOW to release):

```
ESP32 GPIO ──→ Relay IN pin
ESP32 GND  ──→ Relay GND
ESP32 3.3V ──→ Relay VCC  (use 5V rail if your relay module requires it)

Relay COM  ──→ Pump positive (or mains live — use appropriate relay rating)
Relay NO   ──→ Power supply positive
```

All output pins are configured with **GPIO_DRIVE_CAP_3** (maximum drive strength) to reliably trigger relay optocouplers.

### Optional PWM Motor Output

**GPIO 14** (`MOTOR_PIN_EN`) is reserved for an L298N motor driver ENA/ENB pin. It outputs a 30 kHz PWM signal at ~78% duty cycle by default. It is unused in the standard build but available if you later add a DC motor:

```c
#define MOTOR_PIN_EN        GPIO_NUM_14
#define MOTOR_PWM_FREQ      30000
#define MOTOR_PWM_DEFAULT   200   /* 0–255 */
```

## 7. Autocorrect

Autocorrect is a background task that runs on the ESP32 and automatically fires the correct pump when a sensor value goes out of its healthy range — no button press needed.

### How it works

Every `AUTO_CHECK_INTERVAL_MS` milliseconds the ESP32 reads the latest sensor values and checks them against the thresholds. If a value is out of range, it queues a short pump pulse (`AUTO_DOSE_MS`). After dosing, a per-parameter cooldown prevents the same pump from firing again too soon, giving the solution time to mix before the next check.

**Priority order (only one correction fires per cycle):**
1. pH — highest priority
2. Nutrients (TDS)
3. Water level

### Enabling autocorrect in code

**File**: `main/main.c` — line ~1981

```c
static volatile int s_autocorrect_enabled = 1;  /* 1 = on, 0 = off */
```

Set to `1` to enable on boot. Set to `0` to keep it off until triggered via HTTP.

### Enabling/disabling at runtime (no reflash needed)

While connected to the **PLANTAGOCHI** Wi-Fi, open in a browser:

| URL | Effect |
|-----|--------|
| `http://192.168.4.1/autocorrect?enabled=1` | Turn autocorrect ON |
| `http://192.168.4.1/autocorrect?enabled=0` | Turn autocorrect OFF |
| `http://192.168.4.1/autocorrect` | Check current status |

Returns JSON: `{"autocorrect": true}` or `{"autocorrect": false}`.

### Configurable defines

All autocorrect settings are in `main/main.c` (~line 95). Change any value here without touching the rest of the code:

```c
/* Target ranges — correction fires when value goes outside these */
#define AUTO_PH_MIN              5.5f    /* dose pH Up  when pH drops below this */
#define AUTO_PH_MAX              6.5f    /* dose pH Down when pH rises above this */
#define AUTO_TDS_MIN             400.0f  /* dose nutrients when TDS drops below this (ppm) */
#define AUTO_TDS_MAX            1200.0f  /* add water to dilute when TDS exceeds this (ppm) */
#define AUTO_WATER_MIN           30.0f   /* add water when level drops below this (%) */
#define AUTO_WATER_MAX           90.0f   /* drain when level rises above this (%) */

/* Timing */
#define AUTO_DOSE_MS             2000    /* how long each pump pulse runs (ms) */
#define AUTO_CHECK_INTERVAL_MS  10000    /* how often sensors are evaluated (ms) */
#define AUTO_PH_COOLDOWN_MS     60000    /* min gap between pH corrections — 1 min */
#define AUTO_TDS_COOLDOWN_MS    60000    /* min gap between nutrient corrections — 1 min */
#define AUTO_WATER_COOLDOWN_MS  60000    /* min gap between water level corrections — 1 min */
```

### Correction cycle (example — TDS too low)

```
T+0s   ESP32 boots, autocorrect task starts
T+10s  Check: TDS=200 < 400 → nutrient pump ON for 2s
T+12s  Pump stops → 2-minute cooldown starts
T+72s  Cooldown ends
T+82s  Check again: TDS still low → nutrient pump fires again
       ...repeats until TDS rises above AUTO_TDS_MIN
```

### Serial monitor output

Every cycle prints one of these:
```
[AUTO] All OK — pH:6.10 TDS:650ppm Water:72.0%
[AUTO] TDS=200 < 400 → Nutrient 2000 ms
[AUTO] pH=7.20 > 6.5 → pH Down 2000 ms
[AUTO] Water=20.0% < 30% → Add water 2000 ms
```

---

## 8. Full Pin Reference

| GPIO | Role | Direction | Notes |
|------|------|-----------|-------|
| 34 | Water level sensor | Analog in | ADC1_CH6 |
| 32 | pH sensor (PH-4502C) | Analog in | ADC1_CH4 |
| 21 | I2C SDA | Bidirectional | ADS1115 TDS sensor at 0x48 |
| 22 | I2C SCL | Output | ADS1115 TDS sensor at 0x48 |
| 25 | Add Water pump relay | Output | `PIN_PUMP_WATER` |
| 26 | Remove Water pump relay | Output | `PIN_PUMP_DRAIN` |
| 4 | Nutrient pump relay | Output | `PIN_PUMP_NUTRIENT` |
| 27 | pH Down pump relay | Output | `PIN_PH_DOWN` / `MOTOR_PIN_IN1` |
| 33 | pH Up pump relay | Output | `PIN_PH_UP` / `MOTOR_PIN_IN2` |
| 14 | Optional PWM (L298N ENA) | PWM out | 30 kHz, unused by default |
