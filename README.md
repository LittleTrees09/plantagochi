# Plantagochi — ESP32 Hydroponics Monitor

## 1. Function of the Website

Plantagochi is a web-based hydroponics monitoring dashboard served directly from an ESP32 microcontroller. It provides:

- **Real-time sensor monitoring**: Water level, nutrient concentration (TDS), and pH levels
- **Automated pump control**: Trigger water, nutrient, and pH adjustment pumps on-demand or automatically when levels are critical
- **Plant growth tracking**: Visual growth stages and harvest readiness indicator
- **AI-powered plant photo analysis**: Upload photos for CNN-based crop health assessment and growth predictions
- **Local Wi-Fi access**: The ESP32 creates its own access point—no internet required

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

**Location**: Line 142 (firmware-level simulation)

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

1. **Configure Wi-Fi** (in `main/main.c` header comments, lines 28–30):
   ```c
   #define WIFI_SSID       "PLANTAGOCHI"
   #define WIFI_PASSWORD   "12345678"
   ```

2. **Set Flask server IP** (in embedded HTML, line ~849):
   ```javascript
   const PROXY_URL = "http://YOUR_LAPTOP_IP:5000";
   ```
   Replace with your laptop's local IP (find via `hostname -I` on Linux/Mac or `ipconfig` on Windows)

3. **Flash the firmware:**
   ```bash
   idf.py set-target esp32
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```
   (Replace `/dev/ttyUSB0` with your actual port: `/dev/ttyACM0` on some Linux boards, `/dev/cu.usbserial-XXXX` on Mac)

4. **Connect and access:**
   - On your phone/laptop, join Wi-Fi: **PLANTAGOCHI**
   - Open browser: `http://192.168.4.1`

**Pin Assignments (ESP32-WROOM-32):**
- **GPIO 34** → Water level sensor (ADC1_CH6)
- **GPIO 32** → pH sensor (ADC1_CH4)
- **GPIO 21/22** → I2C SDA/SCL (ADS1115 TDS sensor at address 0x48)
- **GPIO 25/26/27** → Water / Nutrient / pH pump relays
