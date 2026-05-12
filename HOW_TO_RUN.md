# HOW TO RUN — Plantagochi
*Updated: May 2026*

---

## Setup Scenarios

### Scenario A — Laptop only, connected to ESP32 (recommended for demos)

Your laptop connects to the ESP32 hotspot, opens the dashboard from the ESP32, and runs `server.py` for photo analysis. Sensors and pump controls work fully.

```
Laptop browser → http://192.168.4.1        (ESP32 serves the dashboard)
Laptop browser → http://192.168.4.x:5000   (server.py handles photo analysis)
```

**Steps:**
1. Flash `main.c` to ESP32
2. Connect laptop to **PLANTAGOCHI** Wi-Fi
3. Activate venv and start the CNN server:
   ```bash
   source venv/bin/activate        # Windows: venv\Scripts\activate.bat
   python server.py
   ```
4. Open laptop browser → `http://192.168.4.1`

> ✅ Sensors, pump buttons, and photo analysis all work.
> `findServer()` automatically finds `server.py` when you tap Analyze — no IP configuration needed.

---

### Scenario B — Laptop only, using Live Server (development)

Use this when editing the HTML and want to see changes without reflashing. No real sensor data — DEV mode simulates values.

```
Laptop browser → http://127.0.0.1:5500   (Live Server serves the dashboard)
Laptop browser → http://127.0.0.1:5000   (server.py handles photo analysis)
```

**Steps:**
1. In `plantagochi.html`, set:
   ```js
   const DEV = { enabled: true, ... }   // simulates sensor values
   ```
2. Start the CNN server:
   ```bash
   source venv/bin/activate
   python server.py
   ```
3. Right-click `plantagochi.html` → **Open with Live Server**
4. Open: `http://127.0.0.1:5500/plantagochi/plantagochi.html`

> ⚠️ Sensor readings and pump buttons do NOT work — requests go to Live Server, not the ESP32.

---

### Scenario B.1 — Live Server + real ESP32 sensors (development with real data)

Same as Scenario B but with live sensor readings and working pump controls.

**One extra step** — in `plantagochi.html`, point `esp32_base_url` at the ESP32:
```js
const esp32_base_url = "http://192.168.4.1";
```
Connect your laptop to **PLANTAGOCHI** Wi-Fi, then run Live Server as normal.

> ✅ Sensors and pumps work. Photo analysis works via `server.py` on localhost.
> ⚠️ Remember to set `esp32_base_url` back to `""` before reflashing `main.c`.

---

### Scenario C — Laptop + Phone, both on ESP32 hotspot

Both devices connect to PLANTAGOCHI. The phone opens the dashboard from the ESP32. Photo analysis on the phone is routed to `server.py` running on the laptop.

```
Phone ──────┐
            ├── PLANTAGOCHI Wi-Fi (192.168.4.1) ── ESP32
Laptop ─────┘
Laptop also runs server.py, auto-discovered by the phone browser
```

**Steps:**
1. Flash `main.c` to ESP32
2. Connect laptop to **PLANTAGOCHI** Wi-Fi
3. Start the CNN server:
   ```bash
   source venv/bin/activate
   python server.py
   ```
4. Connect phone to **PLANTAGOCHI** Wi-Fi
5. Open on phone browser: `http://192.168.4.1`

> ✅ Sensors and pump controls work on both devices.
> ⚠️ Phone photo analysis is still being worked on.

---

## Common Issues

**"Could not find server.py"** → `server.py` is not running, or the phone can't reach the laptop. Verify the server is running and try opening `http://192.168.4.x:5000/health` directly in the phone browser to confirm.

**Dashboard loads slowly** → The HTML tries to load Google Fonts from the internet, which times out on the ESP32 network. Remove the two `<link>` font tags from the HTML and reflash.

**Sensors show 0 or fake values** → Either `DEV.enabled` is `true` (simulated mode) or `esp32_base_url` is pointing to the wrong place. Check both in `plantagochi.html`.

**Pump buttons do nothing** → Open the dashboard from `http://192.168.4.1`, not from Live Server. Or set `esp32_base_url = "http://192.168.4.1"` in Live Server mode.

**Server won't start** → Run `python train.py` first to generate `plantagochi_model.h5` and `class_indices.json`.

## Reminders

**SIMULATE_SENSORS**: When already using the motors and real sensor data, let SIMULATE_SENSORS = 0 to remove simulated data and let the web reflect gathered data from the sensors.

**const DEV**: When using the motors and real sensor data, under const DEV, equate `enabled` and `showPreviewBar` to `false` to remove the simulation controls. Doing this would pave way to real data unobstructed from the debugging controls from the DEV.