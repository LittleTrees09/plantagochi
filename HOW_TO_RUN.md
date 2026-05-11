# HOW TO RUN — Plantagochi

---

## Step 1 — Check Your IP

Every time you switch networks, your IP changes. Always check first:

```bash
hostname -I
```

Note the number — you'll need it in the next step.

---

## Step 2 — Update PROXY_URL

Open `plantagochi/plantagochi.html` and find this line (~line 492):

```javascript
const PROXY_URL = "http://YOUR_LAPTOP_IP:5000";
```

Replace `YOUR_LAPTOP_IP` with the number from Step 1.
Example: `"http://10.13.4.129:5000"`

Save the file.

---

## Step 3 — Start the CNN Server

```bash
cd plantagochi/
source venv/bin/activate
python server.py
```

Wait until you see:
```
Model loaded: plantagochi_model.h5
Listening on: http://0.0.0.0:5000
```

Leave this terminal open — do not close it.

---

## Step 4 — Open the Dashboard

In VSCode:
```
Right click plantagochi.html → Open with Live Server
```

Then open in your browser:
```
http://YOUR_LAPTOP_IP:5500/plantagochi/plantagochi.html
```

---

## Step 5 — Test Photo Analysis

1. Scroll to **Plant Photo Analysis** section
2. Upload a lettuce photo
3. Click **Analyze Plant**
4. Wait 2–3 seconds for the result
5. Click **Apply to Growth Meter** to sync the avatar

---

## Done.

Both terminals must stay running while using the dashboard:

```
Terminal 1 → python server.py     (CNN inference)
Terminal 2 → Live Server          (serves the HTML)
```

---

## Common Issues

**"Analysis failed" error**
→ `PROXY_URL` is wrong. Recheck Step 1 and 2.

**Server won't start**
→ Model not trained yet. Run `python train.py` first.

**Page won't load on phone**
→ Phone must be on the same Wi-Fi as laptop. Hotspot self-connections don't work — use a router.

**Sensor readings show 0 or fake values**
→ Normal without ESP32. Set `DEV.enabled = true` in the HTML to use simulated values.