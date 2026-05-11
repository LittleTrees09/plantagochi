# HOW TO RUN — Plantagochi

---

## Before anything — set PROXY_URL in `plantagochi.html` (~line 492)

| Where are you opening the dashboard? | Set PROXY_URL to |
|---|---|
| Same laptop as server.py | `"http://127.0.0.1:5000"` |
| Phone via laptop hotspot | `"http://10.42.0.1:5000"` |
| Phone via external router | `"http://YOUR_LAPTOP_IP:5000"` → run `hostname -I` to find it |

---

## Run

```bash
# Terminal 1 — start CNN server
cd plantagochi/
source venv/bin/activate
python server.py

# Terminal 2 — start Live Server
# Right click plantagochi.html → Open with Live Server
```

---

## Open in browser

| Setup | URL |
|---|---|
| Same laptop | `http://127.0.0.1:5500/plantagochi/plantagochi.html` |
| Phone via hotspot | `http://10.42.0.1:5500/plantagochi/plantagochi.html` |
| Phone via router | `http://YOUR_LAPTOP_IP:5500/plantagochi/plantagochi.html` |

---

## Common Issues

**"Analysis failed"** → PROXY_URL is wrong. Fix it and refresh.

**Server won't start** → Run `python train.py` first.

**Phone can't connect** → Don't use your phone as the hotspot. Use laptop hotspot instead (`nmcli device wifi hotspot ifname wlan0 ssid "PLANTAGOCHI" password "12345678"`).

**Fake sensor values** → Normal without ESP32. `DEV.enabled = true` simulates sensors.