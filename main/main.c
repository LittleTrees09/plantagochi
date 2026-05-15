/*
 * Plantagochi — ESP32-WROOM-32 Firmware (ESP-IDF 5.x)
 *
 * Starts a Wi-Fi Access Point and serves a browser dashboard on port 80.
 * Reads three sensors and drives three output buttons via HTTP.
 *
 * Endpoints:
 *   GET /       — serves the HTML dashboard
 *   GET /data   — returns { "water": %, "food": ppm, "ph": value }
 *   GET /action — triggers water / nutrient / pH pump
 *
 * Flash steps:
 *   1. Set WIFI_SSID, WIFI_PASSWORD, and LAPTOP_IP below.
 *   2. Paste plantagochi.html into HTML_PAGE (see instructions there).
 *   3. Set SIMULATE_SENSORS 0 when sensors are wired.
 *   4. idf.py set-target esp32 && idf.py build
 *      idf.py -p /dev/ttyUSB0 flash monitor
 *   5. Connect to Wi-Fi PLANTAGOCHI, open http://192.168.4.1
 *   6. On your laptop: source venv/bin/activate && python server.py
 *
 * ----------------------------------------------------------------
 * SENSORS (inputs)
 *   GPIO 34 — Water level sensor  (ADC1_CH6)
 *   GPIO 32 — pH sensor           (ADC1_CH4)
 *   GPIO 21 — I2C SDA → ADS1115  (TDS/Nutrients)
 *   GPIO 22 — I2C SCL → ADS1115  (TDS/Nutrients)
 *
 * BUTTON OUTPUTS
 *   GPIO 25 — Add Water     button → water pump relay
 *   GPIO 26 — Remove Water  button → drain pump relay
 *   GPIO 4  — Feed Plant    button → nutrient pump relay
 *   GPIO 27 — pH Down       button → pH down dosing pump/relay
 *   GPIO 33 — pH Up         button → pH up dosing pump/relay
 *   GPIO 14 — optional PWM output, unused by default
 * ----------------------------------------------------------------
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

/* ----------------------------------------------------------------
   STEP 1 — Configure these before building
   ---------------------------------------------------------------- */
#define WIFI_SSID       "PLANTAGOCHI"
#define WIFI_PASSWORD   "12345678"
#define LAPTOP_IP       "YOUR_LAPTOP_IP"   /* laptop IP on the AP network */

#define WIFI_AP_CHANNEL     1
#define WIFI_AP_MAX_CONN    4

/* 1 = fake sensor data for testing, 0 = real sensors */
#define SIMULATE_SENSORS 1

/* ----------------------------------------------------------------
   PUMP / MOTOR ON DURATIONS
   Controls how long each output stays active when its button is pressed.
   Change any value here — no need to touch the rest of the code.

   WATER_ON_DURATION_MS  — Add Water  button (GPIO 25 relay)
   FOOD_ON_DURATION_MS   — Feed Plant button (GPIO 4 relay)
   PH_DOWN_DURATION_MS     — Fix pH     button (GPIO 27 motor IN1)
   ---------------------------------------------------------------- */
#define WATER_ON_DURATION_MS   10000   /* Add Water — 10 s default */
#define DRAIN_ON_DURATION_MS   10000   /* Remove Water — 10 s default */
#define FOOD_ON_DURATION_MS    10000   /* Feed Plant — 10 s default */
#define PH_DOWN_DURATION_MS    10000   /* pH Down — 10 s default */
#define PH_UP_DURATION_MS      10000   /* pH Up — 10 s default */

/* ----------------------------------------------------------------
   TEST_GPIO27
   1 = run the GPIO 27 LED blink test instead of normal firmware
   0 = normal Plantagochi operation (leave this at 0 when done)

   Circuit: GPIO 27 ──── 330Ω ──── LED(+) ──── LED(–) ──── GND
   ---------------------------------------------------------------- */
#define TEST_GPIO27 0

/* ================================================================
   GPIO PINOUT — COMPLETE REFERENCE
   ================================================================
   INPUTS
   ──────────────────────────────────────────────────────────────
   GPIO 34   ADC1_CH6   Water level sensor          (analog in)
   GPIO 32   ADC1_CH4   pH sensor — PH-4502C        (analog in)
   GPIO 21   I2C SDA    ADS1115 → TDS / nutrients   (I2C data)
   GPIO 22   I2C SCL    ADS1115 → TDS / nutrients   (I2C clock)

   OUTPUTS
   ──────────────────────────────────────────────────────────────
   GPIO 25   PIN_PUMP_WATER      Add water pump      (relay)
   GPIO 26   PIN_PUMP_DRAIN      Remove water pump   (relay)
   GPIO  4   PIN_PUMP_NUTRIENT   Nutrient pump       (relay)
   GPIO 27   PIN_PH_DOWN         pH down pump        (relay)
   GPIO 33   PIN_PH_UP           pH up pump          (relay)
   GPIO 14   MOTOR_PIN_EN        L298N motor ENA     (PWM 30 kHz, optional)

   All output pins use GPIO_DRIVE_CAP_3 (max drive strength).
   All output durations are set via the *_ON_DURATION_MS defines above.
   ================================================================ */

/* ----------------------------------------------------------------
   PIN DEFINITIONS
   ---------------------------------------------------------------- */
#define PIN_WATER   ADC_CHANNEL_6   /* GPIO 34 */
#define PIN_PH      ADC_CHANNEL_4   /* GPIO 32 */

#define PIN_PUMP_WATER      GPIO_NUM_25   /* Add Water */
#define PIN_PUMP_DRAIN      GPIO_NUM_26   /* Remove Water */
#define PIN_PUMP_NUTRIENT   GPIO_NUM_4    /* Feed Plant */

#define PIN_PH_DOWN         GPIO_NUM_27   /* pH Down dosing pump/relay */
#define PIN_PH_UP           GPIO_NUM_33   /* pH Up dosing pump/relay */

/* Legacy motor names kept so the optional motor helper/test code still compiles. */
#define MOTOR_PIN_IN1       PIN_PH_DOWN
#define MOTOR_PIN_IN2       PIN_PH_UP

/* Optional PWM pin kept available if you later use a motor driver */
#define MOTOR_PIN_EN        GPIO_NUM_14

#define MOTOR_PWM_FREQ      30000
#define MOTOR_PWM_RES       LEDC_TIMER_8_BIT
#define MOTOR_PWM_TIMER     LEDC_TIMER_0
#define MOTOR_PWM_CHANNEL   LEDC_CHANNEL_0
#define MOTOR_PWM_DEFAULT   200   /* ~78% duty cycle */

/* ----------------------------------------------------------------
   I2C / ADS1115 (TDS sensor)
   ---------------------------------------------------------------- */
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_SCL_IO   22
#define I2C_MASTER_FREQ_HZ  100000
#define I2C_MASTER_PORT     I2C_NUM_0
#define I2C_TIMEOUT_MS      100

#define ADS1115_ADDR        0x48
#define ADS1115_REG_CONVERT 0x00
#define ADS1115_REG_CONFIG  0x01

/*
 * ADS1115 config: single-shot on AIN0, PGA=±4.096V, 128 SPS, comparator off.
 * High byte: OS=1, MUX=AIN0, PGA=±4.096V, single-shot
 * Low byte:  DR=128SPS, comparator disabled
 */
#define ADS_CFG_HI  0xC3
#define ADS_CFG_LO  0x83

#define ADS1115_LSB_VOLTS   0.000125f   /* 125 µV per LSB at PGA ±4.096V */

/* ----------------------------------------------------------------
   TDS ALGORITHM PARAMETERS
   ---------------------------------------------------------------- */
#define TDS_NUM_SAMPLES     32
#define TDS_WATER_TEMP_C    25.0f
#define TDS_K_CELL          1.00f   /* cell constant — adjust on calibration */
#define TDS_FACTOR          0.3f    /* EC → TDS conversion factor */
#define TDS_MAX_PPM         2000.0f

/* ================================================================
   HTML PAGE
   ─────────────────────────────────────────────────────────────────
   HOW TO PASTE YOUR HTML:
   1. Open plantagochi.html in VSCode
   2. In the <script> section set:
        const PROXY_URL      = "http://YOUR_LAPTOP_IP:5000";
        const esp32_base_url = "";
   3. Set DEV.enabled = false
   4. Select all → copy
   5. Delete the placeholder below and paste between the quotes
   ================================================================ */
static const char HTML_PAGE[] =
"<!--\n"
"  Plantagochi — ESP32 Hydroponics Monitor\n"
"  ─────────────────────────────────────────\n"
"  ESP32 endpoints:\n"
"    GET  /data                → { \"water\": 80, \"food\": 650, \"ph\": 6.1 }\n"
"    GET  /action?action=water → trigger water pump\n"
"    GET  /action?action=food  → trigger nutrient pump\n"
"    GET  /action?action=ph    → trigger pH adjustment\n"
"  ESP32 must send header: Access-Control-Allow-Origin: *\n"
"-->\n"
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"  <meta charset=\"UTF-8\"/>\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"/>\n"
"  <title>Plantagochi</title>\n"
"  <style>\n"
"    :root {\n"
"      --green:      #2d6a4f; --green-light:#40916c; --green-pale:#d8f3dc;\n"
"      --amber:      #e09f3e; --amber-pale: #fff3cd;\n"
"      --red:        #c1121f; --red-pale:   #fde8e9;\n"
"      --sky:        #5ea3e6; --sky-pale:   #dbeafe;\n"
"      --purple:     #9b72c8; --purple-pale:#ede9fb;\n"
"      --bg: #f0f7f4; --card: #ffffff; --text: #1c2b22;\n"
"      --text-muted: #5a7a68; --border: #c8e0d4;\n"
"    }\n"
"    * { box-sizing: border-box; margin: 0; padding: 0; }\n"
"    body {\n"
"      min-height: 100vh; font-family: 'Nunito', system-ui, sans-serif;\n"
"      color: var(--text); background-color: var(--bg);\n"
"      background-image:\n"
"        radial-gradient(ellipse 60% 40% at 10% 0%,  rgba(64,145,108,0.18) 0%, transparent 60%),\n"
"        radial-gradient(ellipse 50% 40% at 90% 5%,  rgba(94,163,230,0.14) 0%, transparent 55%),\n"
"        radial-gradient(ellipse 40% 30% at 50% 100%,rgba(155,114,200,0.10) 0%, transparent 60%);\n"
"      display: flex; justify-content: center; padding: 28px 16px 48px;\n"
"    }\n"
"    .shell { width: 100%; max-width: 820px; display: grid; gap: 16px; }\n"
"    .card  { background: var(--card); border: 1.5px solid var(--border); border-radius: 20px; box-shadow: 0 4px 20px rgba(45,106,79,0.08); }\n"
"\n"
"    /* HEADER */\n"
"    .header { padding: 20px 24px 18px; display: flex; align-items: center; justify-content: space-between; gap: 12px; position: relative; overflow: hidden; }\n"
"    .header-leaf { position: absolute; right: -20px; top: -20px; font-size: 120px; opacity: 0.07; pointer-events: none; line-height: 1; }\n"
"    .header-text h1 { font-family: 'Fredoka One', cursive; font-size: 36px; font-weight: 400; letter-spacing: 0.5px; color: var(--green); line-height: 1; }\n"
"    .header-text p  { font-size: 12px; font-weight: 700; letter-spacing: 1.4px; text-transform: uppercase; color: var(--text-muted); margin-top: 4px; }\n"
"    .header-right   { display: flex; align-items: center; gap: 8px; flex-shrink: 0; }\n"
"    .header-badge   { display: flex; align-items: center; gap: 6px; background: var(--green-pale); border: 1.5px solid #b7dfc6; border-radius: 999px; padding: 6px 14px 6px 10px; font-size: 13px; font-weight: 800; color: var(--green); }\n"
"    .pulse-dot      { width: 8px; height: 8px; border-radius: 50%; background: var(--green-light); animation: pulse 2s ease-in-out infinite; }\n"
"    @keyframes pulse { 0%,100%{opacity:1;transform:scale(1)}50%{opacity:.5;transform:scale(.7)} }\n"
"    .crop-selector-btn { display: flex; align-items: center; gap: 6px; background: var(--card); border: 1.5px solid var(--border); border-radius: 999px; padding: 6px 14px 6px 10px; font-size: 13px; font-weight: 800; color: var(--text-muted); cursor: pointer; transition: border-color 0.15s, color 0.15s; }\n"
"    .crop-selector-btn:hover { border-color: var(--green-light); color: var(--green); }\n"
"\n"
"    /* CROP MODAL */\n"
"    .modal-overlay { display: none; position: fixed; inset: 0; z-index: 200; background: rgba(28,43,34,0.45); backdrop-filter: blur(3px); align-items: center; justify-content: center; padding: 20px; }\n"
"    .modal-overlay.open { display: flex; }\n"
"    .modal-box { background: var(--card); border-radius: 24px; padding: 28px 24px; width: 100%; max-width: 580px; box-shadow: 0 20px 60px rgba(45,106,79,0.22); max-height: 90vh; overflow-y: auto; }\n"
"    .modal-title    { font-family: 'Fredoka One', cursive; font-size: 24px; color: var(--green); margin-bottom: 4px; }\n"
"    .modal-subtitle { font-size: 13px; font-weight: 700; color: var(--text-muted); margin-bottom: 20px; }\n"
"    .crop-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; }\n"
"    .crop-card { border: 1.5px solid var(--border); border-radius: 16px; padding: 16px; cursor: pointer; transition: border-color 0.15s, background 0.15s, transform 0.1s; }\n"
"    .crop-card:hover  { border-color: var(--green-light); background: var(--green-pale); transform: translateY(-2px); }\n"
"    .crop-card.active { border-color: var(--green); background: var(--green-pale); }\n"
"    .crop-card-header { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }\n"
"    .crop-emoji { font-size: 28px; line-height: 1; }\n"
"    .crop-name  { font-family: 'Fredoka One', cursive; font-size: 18px; color: var(--green); }\n"
"    .crop-days  { font-size: 11px; font-weight: 800; color: var(--text-muted); }\n"
"    .crop-ranges { display: flex; flex-direction: column; gap: 3px; margin-top: 4px; }\n"
"    .crop-range  { font-size: 11px; font-weight: 700; color: var(--text-muted); }\n"
"    .crop-range span { color: var(--green); }\n"
"    .crop-stages-preview { display: flex; gap: 4px; flex-wrap: wrap; margin-top: 8px; }\n"
"    .crop-stage-chip { font-size: 10px; font-weight: 800; padding: 2px 8px; border-radius: 999px; background: rgba(64,145,108,0.1); color: var(--green-light); }\n"
"    .modal-close { display: block; width: 100%; margin-top: 16px; padding: 12px; border: none; border-radius: 14px; background: var(--green); color: white; font-family: 'Nunito', sans-serif; font-size: 14px; font-weight: 800; cursor: pointer; }\n"
"    .modal-close:hover { opacity: 0.88; }\n"
"\n"
"    /* PLANT PANEL */\n"
"    .plant-panel { padding: 24px 24px 20px; display: flex; flex-direction: column; align-items: center; position: relative; overflow: hidden; }\n"
"    .scene-bg    { position: absolute; inset: 0; background: radial-gradient(ellipse 70% 60% at 50% 30%, rgba(64,145,108,0.10) 0%, transparent 70%); pointer-events: none; }\n"
"    .soil        { position: absolute; bottom: 0; left: 50%; transform: translateX(-50%); width: 200px; height: 48px; background: radial-gradient(ellipse 100% 80% at 50% 80%, #8B5E3C 0%, #A0714F 60%, transparent 100%); border-radius: 50%; opacity: 0.22; }\n"
"    .avatar-stage  { position: relative; z-index: 2; display: flex; flex-direction: column; align-items: center; width: 100%; }\n"
"    .avatar-canvas { width: 220px; height: 220px; position: relative; margin-bottom: 4px; }\n"
"    .mood-bubble   { position: absolute; top: 4px; right: 12px; width: 44px; height: 44px; border-radius: 50%; background: white; border: 2px solid var(--border); box-shadow: 0 4px 12px rgba(0,0,0,0.10); display: grid; place-items: center; font-size: 22px; z-index: 10; }\n"
"    .particle-layer { position: absolute; inset: 0; pointer-events: none; z-index: 8; }\n"
"    .particle { position: absolute; border-radius: 50%; opacity: 0; animation: particleFall 1.4s ease-in forwards; }\n"
"    @keyframes particleFall { 0%{opacity:1;transform:translateY(0) scale(1)}80%{opacity:.7}100%{opacity:0;transform:translateY(60px) scale(.4)} }\n"
"    .avatar-info   { display: flex; flex-direction: column; align-items: center; gap: 6px; margin-top: 4px; margin-bottom: 4px; }\n"
"    .plant-name-label { font-family: 'Fredoka One', cursive; font-size: 18px; color: var(--green); display: flex; align-items: center; gap: 6px; }\n"
"    .plant-status-pill { display: inline-flex; align-items: center; gap: 6px; padding: 6px 16px; border-radius: 999px; font-weight: 800; font-size: 13px; background: var(--green-pale); color: var(--green); border: 1.5px solid rgba(64,145,108,0.3); transition: all 0.4s; }\n"
"    .plant-status-pill.warning  { background: var(--amber-pale); color: #7a4e0a; border-color: rgba(224,159,62,0.4); }\n"
"    .plant-status-pill.critical { background: var(--red-pale);   color: #7a0c13; border-color: rgba(193,18,31,0.3); }\n"
"    .days-badge { font-size: 12px; font-weight: 800; color: var(--text-muted); }\n"
"    .days-badge span { color: var(--green-light); }\n"
"\n"
"    /* CARE SCORE */\n"
"    .growth-section  { width: 100%; max-width: 380px; margin-top: 12px; }\n"
"    .growth-header   { display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px; }\n"
"    .growth-label    { font-size: 11px; font-weight: 900; letter-spacing: 1.2px; text-transform: uppercase; color: var(--text-muted); }\n"
"    .growth-stage-name { font-size: 12px; font-weight: 800; color: var(--green-light); }\n"
"    .growth-track    { width: 100%; height: 14px; background: #e8f0ec; border-radius: 999px; overflow: hidden; }\n"
"    .growth-fill     { height: 100%; width: 0; border-radius: 999px; background: linear-gradient(90deg, var(--green), var(--green-light)); transition: width 1.2s cubic-bezier(0.34,1.1,0.64,1); }\n"
"    .growth-stages-row { display: flex; justify-content: space-between; margin-top: 6px; }\n"
"    .growth-stage-tick { display: flex; flex-direction: column; align-items: center; gap: 2px; font-size: 10px; font-weight: 700; color: var(--text-muted); opacity: 0.45; transition: opacity 0.4s, color 0.4s; }\n"
"    .growth-stage-tick.reached { opacity: 1; color: var(--green-light); }\n"
"    .growth-stage-tick .tick-emoji { font-size: 14px; }\n"
"    .reset-btn { background: none; border: 1.5px solid var(--border); border-radius: 999px; padding: 3px 10px; font-family: 'Nunito', sans-serif; font-size: 11px; font-weight: 800; color: var(--text-muted); cursor: pointer; transition: all 0.15s; }\n"
"    .reset-btn:hover { background: var(--red-pale); color: var(--red); border-color: rgba(193,18,31,0.3); }\n"
"\n"
"    /* HARVEST BUTTON */\n"
"    .harvest-btn { display: none; width: 100%; max-width: 380px; margin-top: 12px; padding: 13px 0; border: none; border-radius: 14px; background: linear-gradient(135deg, #f77f00, #e09f3e); color: white; font-family: 'Nunito', sans-serif; font-size: 14px; font-weight: 900; cursor: pointer; box-shadow: 0 6px 18px rgba(224,159,62,0.35); align-items: center; justify-content: center; gap: 8px; transition: transform 0.12s, box-shadow 0.12s; }\n"
"    .harvest-btn.visible { display: flex !important; }\n"
"    .harvest-btn:hover   { transform: translateY(-2px); box-shadow: 0 10px 24px rgba(224,159,62,0.4); }\n"
"\n"
"    /* STAGE PREVIEW BAR */\n"
"    .preview-bar { display: flex; gap: 6px; margin-top: 10px; width: 100%; max-width: 380px; }\n"
"    .preview-bar[hidden] { display: none !important; }\n"
"    .preview-btn { flex: 1; border: 1.5px solid var(--border); border-radius: 12px; padding: 7px 4px; background: var(--card); font-family: 'Nunito', sans-serif; font-size: 10px; font-weight: 800; color: var(--text-muted); cursor: pointer; display: flex; flex-direction: column; align-items: center; gap: 2px; transition: all 0.15s; }\n"
"    .preview-btn:hover  { background: var(--green-pale); border-color: rgba(64,145,108,0.4); color: var(--green); }\n"
"    .preview-btn.active { background: var(--green-pale); border-color: var(--green-light); color: var(--green); }\n"
"    .preview-btn .pb-emoji { font-size: 14px; line-height: 1; }\n"
"\n"
"    .confirm-btn { flex: 1; border: none; border-radius: 10px; padding: 8px 0; font-family: 'Nunito', sans-serif; font-size: 13px; font-weight: 800; cursor: pointer; }\n"
"    .confirm-yes { background: var(--red); color: #fff; }\n"
"    .confirm-no  { background: #e8f0ec; color: var(--text-muted); }\n"
"\n"
"    /* TOAST */\n"
"    .levelup-toast { position: fixed; top: 24px; left: 50%; transform: translateX(-50%) translateY(-80px); background: var(--green); color: white; padding: 12px 24px; border-radius: 999px; font-weight: 800; font-size: 15px; box-shadow: 0 8px 24px rgba(45,106,79,0.35); z-index: 300; display: flex; align-items: center; gap: 10px; transition: transform 0.4s cubic-bezier(0.34,1.4,0.64,1), opacity 0.4s; opacity: 0; pointer-events: none; }\n"
"    .levelup-toast.show { transform: translateX(-50%) translateY(0); opacity: 1; }\n"
"\n"
"    /* ALERT */\n"
"    .alert-banner { display: none; align-items: flex-start; gap: 10px; border-radius: 16px; padding: 14px 16px; font-weight: 700; font-size: 14px; line-height: 1.4; }\n"
"    .alert-banner.warning  { background: var(--amber-pale); border: 1.5px solid rgba(224,159,62,0.5); color: #6b3e08; }\n"
"    .alert-banner.critical { background: var(--red-pale);   border: 1.5px solid rgba(193,18,31,0.35); color: #6b080e; }\n"
"    .alert-icon  { font-size: 18px; flex-shrink: 0; }\n"
"    .alert-body  { flex: 1; }\n"
"    .alert-title { font-size: 13px; font-weight: 900; margin-bottom: 2px; }\n"
"    .alert-close { background: none; border: none; cursor: pointer; font-size: 20px; color: inherit; opacity: 0.6; }\n"
"    .alert-close:hover { opacity: 1; }\n"
"\n"
"    /* SENSORS */\n"
"    .sensors { display: grid; grid-template-columns: repeat(3, minmax(0,1fr)); gap: 12px; }\n"
"    .sensor-card { padding: 16px 16px 14px; border-radius: 20px; border: 1.5px solid var(--border); background: var(--card); box-shadow: 0 4px 16px rgba(45,106,79,0.07); display: flex; flex-direction: column; gap: 10px; }\n"
"    .sensor-header { display: flex; align-items: center; justify-content: space-between; gap: 6px; }\n"
"    .sensor-icon-name { display: flex; align-items: center; gap: 8px; }\n"
"    .sensor-icon { width: 34px; height: 34px; border-radius: 10px; display: grid; place-items: center; font-size: 18px; flex-shrink: 0; }\n"
"    .water-card .sensor-icon { background: var(--sky-pale); }\n"
"    .tds-card   .sensor-icon { background: var(--green-pale); }\n"
"    .ph-card    .sensor-icon { background: var(--purple-pale); }\n"
"    .sensor-name { font-weight: 800; font-size: 14px; }\n"
"    .sensor-unit { font-size: 11px; font-weight: 600; color: var(--text-muted); margin-top: 1px; }\n"
"    .status-badge { border-radius: 999px; font-size: 11px; font-weight: 800; padding: 4px 10px; flex-shrink: 0; }\n"
"    .status-normal   { background: var(--green-pale); color: var(--green); }\n"
"    .status-low      { background: var(--amber-pale); color: #7a4e0a; }\n"
"    .status-critical { background: var(--red-pale);   color: var(--red); }\n"
"    .sensor-reading  { font-family: 'Fredoka One', cursive; font-size: 28px; color: var(--text); line-height: 1; }\n"
"    .sensor-reading .unit-suffix { font-family: 'Nunito', sans-serif; font-size: 13px; font-weight: 700; color: var(--text-muted); margin-left: 2px; }\n"
"    .bar-track { width: 100%; height: 10px; background: #e8f0ec; border-radius: 999px; overflow: hidden; }\n"
"    .bar-fill  { height: 100%; width: 0; border-radius: 999px; background: var(--green); transition: width 0.8s cubic-bezier(0.34,1.2,0.64,1), background 0.5s; }\n"
"    .range-labels { display: flex; justify-content: space-between; font-size: 10px; font-weight: 700; color: var(--text-muted); opacity: 0.7; }\n"
"\n"
"    /* PUMP PANEL */\n"
"    .pump-panel { padding: 18px 18px 16px; }\n"
"    .pump-label { font-size: 11px; font-weight: 900; letter-spacing: 1.2px; text-transform: uppercase; color: var(--text-muted); margin-bottom: 12px; }\n"
"    .pump-buttons { display: grid; grid-template-columns: repeat(5, minmax(0,1fr)); gap: 10px; margin-bottom: 10px; }\n"
"    .pump-btn { border: none; border-radius: 14px; padding: 13px 10px; font-family: 'Nunito', sans-serif; font-size: 13px; font-weight: 800; cursor: pointer; display: flex; align-items: center; justify-content: center; gap: 7px; color: #fff; background: var(--green); box-shadow: 0 6px 16px rgba(45,106,79,0.28), inset 0 1px 0 rgba(255,255,255,0.2); transition: transform 0.12s, box-shadow 0.12s, background 0.3s; }\n"
"    .pump-btn:hover:not(:disabled)  { transform: translateY(-2px); }\n"
"    .pump-btn:active:not(:disabled) { transform: translateY(0); }\n"
"    .pump-btn:disabled { cursor: not-allowed; opacity: 0.85; }\n"
"    .pump-btn[data-state=\"loading\"] { background: var(--amber); }\n"
"    .pump-btn[data-state=\"done\"]    { background: var(--green-light); }\n"
"    .pump-btn-icon { font-size: 16px; }\n"
"    .pump-note { font-size: 12px; font-weight: 700; color: var(--text-muted); text-align: center; display: flex; align-items: center; justify-content: center; gap: 5px; }\n"
"    .pump-note::before { content: \"⚡\"; font-size: 12px; }\n"
"\n"
"    /* ── PHOTO ANALYSIS PANEL ─────────────────────────── */\n"
"    .photo-panel { padding: 20px 22px 22px; }\n"
"    .photo-panel-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 14px; }\n"
"    .photo-panel-title { font-size: 11px; font-weight: 900; letter-spacing: 1.2px; text-transform: uppercase; color: var(--text-muted); }\n"
"    .photo-panel-badge { font-size: 10px; font-weight: 800; padding: 3px 10px; border-radius: 999px; background: var(--purple-pale); color: var(--purple); border: 1px solid rgba(155,114,200,0.3); }\n"
"\n"
"    .drop-zone { border: 2px dashed var(--border); border-radius: 16px; padding: 28px 16px; display: flex; flex-direction: column; align-items: center; gap: 8px; cursor: pointer; transition: border-color 0.15s, background 0.15s; background: #f8fbf9; text-align: center; }\n"
"    .drop-zone:hover, .drop-zone.dragover { border-color: var(--green-light); background: var(--green-pale); }\n"
"    .drop-zone-icon { font-size: 32px; line-height: 1; margin-bottom: 2px; }\n"
"    .drop-zone-label { font-size: 14px; font-weight: 800; color: var(--text); }\n"
"    .drop-zone-sub { font-size: 12px; font-weight: 600; color: var(--text-muted); }\n"
"    .drop-zone input[type=file] { display: none; }\n"
"\n"
"    .photo-preview-wrap { position: relative; margin-top: 12px; border-radius: 14px; overflow: hidden; border: 1.5px solid var(--border); }\n"
"    .photo-preview-wrap img { width: 100%; max-height: 200px; object-fit: cover; display: block; }\n"
"    .photo-preview-remove { position: absolute; top: 8px; right: 8px; background: rgba(255,255,255,0.92); border: 1.5px solid var(--border); border-radius: 50%; width: 30px; height: 30px; cursor: pointer; display: flex; align-items: center; justify-content: center; font-size: 14px; color: var(--text-muted); font-weight: 800; transition: background 0.15s, color 0.15s; }\n"
"    .photo-preview-remove:hover { background: var(--red-pale); color: var(--red); }\n"
"\n"
"    .analyze-btn { width: 100%; margin-top: 12px; padding: 12px 0; border: none; border-radius: 14px; background: var(--green); color: white; font-family: 'Nunito', sans-serif; font-size: 14px; font-weight: 800; cursor: pointer; display: flex; align-items: center; justify-content: center; gap: 8px; transition: opacity 0.15s, transform 0.12s; }\n"
"    .analyze-btn:hover:not(:disabled) { opacity: 0.88; transform: translateY(-1px); }\n"
"    .analyze-btn:disabled { opacity: 0.45; cursor: not-allowed; transform: none; }\n"
"\n"
"    .analysis-spinner { width: 16px; height: 16px; border: 2px solid rgba(255,255,255,0.4); border-top-color: white; border-radius: 50%; animation: spin 0.7s linear infinite; flex-shrink: 0; }\n"
"    @keyframes spin { to { transform: rotate(360deg); } }\n"
"\n"
"    /* Analysis result */\n"
"    .analysis-result { margin-top: 16px; border: 1.5px solid var(--border); border-radius: 16px; overflow: hidden; }\n"
"    .analysis-result-header { padding: 14px 16px 12px; background: var(--green-pale); border-bottom: 1.5px solid rgba(64,145,108,0.2); display: flex; align-items: flex-start; gap: 12px; }\n"
"    .analysis-crop-icon { font-size: 28px; line-height: 1; flex-shrink: 0; }\n"
"    .analysis-result-meta { flex: 1; }\n"
"    .analysis-result-title { font-family: 'Fredoka One', cursive; font-size: 18px; color: var(--green); line-height: 1.1; margin-bottom: 3px; }\n"
"    .analysis-result-summary { font-size: 12px; font-weight: 700; color: var(--text-muted); line-height: 1.4; }\n"
"    .analysis-confidence { font-size: 10px; font-weight: 800; padding: 3px 9px; border-radius: 999px; flex-shrink: 0; }\n"
"    .conf-high   { background: var(--green-pale); color: var(--green); border: 1px solid rgba(64,145,108,0.3); }\n"
"    .conf-medium { background: var(--amber-pale); color: #7a4e0a; border: 1px solid rgba(224,159,62,0.4); }\n"
"    .conf-low    { background: var(--red-pale); color: var(--red); border: 1px solid rgba(193,18,31,0.3); }\n"
"\n"
"    .analysis-result-body { padding: 14px 16px; display: flex; flex-direction: column; gap: 12px; }\n"
"\n"
"    .analysis-stage-bar-labels { display: flex; justify-content: space-between; font-size: 11px; font-weight: 800; color: var(--text-muted); margin-bottom: 5px; }\n"
"    .analysis-stage-bar-track { height: 10px; background: #e8f0ec; border-radius: 999px; overflow: hidden; }\n"
"    .analysis-stage-bar-fill { height: 100%; border-radius: 999px; background: linear-gradient(90deg, var(--green), var(--green-light)); transition: width 0.8s cubic-bezier(0.34,1.2,0.64,1); }\n"
"\n"
"    .analysis-notes { display: flex; flex-direction: column; gap: 5px; }\n"
"    .analysis-note { display: flex; align-items: flex-start; gap: 8px; font-size: 12px; font-weight: 700; color: var(--text-muted); line-height: 1.4; }\n"
"    .analysis-note-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--green-light); margin-top: 4px; flex-shrink: 0; }\n"
"    .analysis-note-dot.warn { background: var(--amber); }\n"
"    .analysis-note-dot.crit { background: var(--red); }\n"
"\n"
"    .analysis-sync-row { display: flex; gap: 10px; padding-top: 12px; border-top: 1.5px solid var(--border); }\n"
"    .sync-apply-btn { flex: 1; padding: 10px 0; border: none; border-radius: 12px; background: var(--green); color: white; font-family: 'Nunito', sans-serif; font-size: 13px; font-weight: 800; cursor: pointer; transition: opacity 0.15s; }\n"
"    .sync-apply-btn:hover { opacity: 0.88; }\n"
"    .sync-dismiss-btn { padding: 10px 16px; border: 1.5px solid var(--border); border-radius: 12px; background: transparent; font-family: 'Nunito', sans-serif; font-size: 13px; font-weight: 800; color: var(--text-muted); cursor: pointer; transition: background 0.15s; }\n"
"    .sync-dismiss-btn:hover { background: #f0f4f2; }\n"
"    /* ── END PHOTO ANALYSIS PANEL ───────────────────── */\n"
"\n"
"    /* DEV PANEL */\n"
"    .dev-toggle { position: fixed; bottom: 20px; right: 20px; z-index: 50; background: #1c2b22; color: #74c69d; border: none; border-radius: 999px; padding: 8px 16px; font-family: 'Nunito', sans-serif; font-size: 12px; font-weight: 800; cursor: pointer; box-shadow: 0 4px 12px rgba(0,0,0,0.25); }\n"
"    .dev-panel { position: fixed; bottom: 60px; right: 20px; z-index: 49; width: 300px; background: #1c2b22; border-radius: 16px; padding: 16px; box-shadow: 0 8px 32px rgba(0,0,0,0.3); display: none; }\n"
"    .dev-panel.open { display: block; }\n"
"    .dev-panel h3 { color: #74c69d; font-size: 13px; font-weight: 900; margin-bottom: 12px; }\n"
"    .dev-row { margin-bottom: 10px; }\n"
"    .dev-row label { display: flex; justify-content: space-between; font-size: 11px; font-weight: 800; color: #a0c4b0; margin-bottom: 4px; }\n"
"    .dev-row input[type=range] { width: 100%; accent-color: #52b788; }\n"
"    .dev-divider { border: none; border-top: 1px solid rgba(255,255,255,0.08); margin: 12px 0; }\n"
"    .dev-stage-btns { display: flex; flex-wrap: wrap; gap: 5px; }\n"
"    .dev-stage-btn { background: rgba(255,255,255,0.07); border: 1px solid rgba(255,255,255,0.15); border-radius: 8px; padding: 5px 8px; font-family: 'Nunito', sans-serif; font-size: 11px; font-weight: 800; color: #d8f3dc; cursor: pointer; transition: background 0.15s; }\n"
"    .dev-stage-btn:hover  { background: rgba(82,183,136,0.25); }\n"
"    .dev-stage-btn.active { background: #2d6a4f; border-color: #52b788; color: #fff; }\n"
"\n"
"    /* ANIMATIONS */\n"
"    @keyframes critShake { 0%,90%,100%{transform:translateX(0)} 92%{transform:translateX(-4px) rotate(-1deg)} 94%{transform:translateX(4px) rotate(1deg)} 96%{transform:translateX(-2px)} 98%{transform:translateX(2px)} }\n"
"    .avatar-canvas.critical-shake { animation: critShake 3s ease-in-out infinite; }\n"
"    @keyframes plantLevelUp { 0%{filter:brightness(1)} 30%{filter:brightness(1.3) saturate(1.4)} 100%{filter:brightness(1)} }\n"
"    .leveling { animation: plantLevelUp 0.7s ease forwards; }\n"
"\n"
"    .footer { text-align: center; font-size: 11px; font-weight: 700; color: var(--text-muted); opacity: 0.6; padding-bottom: 4px; }\n"
"\n"
"    /* MOBILE RESPONSIVE */\n"
"    @media (max-width: 768px) {\n"
"      .shell { gap: 14px; }\n"
"      .header { padding: 18px 20px; gap: 8px; flex-wrap: wrap; }\n"
"      .header-text h1 { font-size: 28px; letter-spacing: 0px; }\n"
"      .header-right { gap: 6px; }\n"
"      .header-badge, .crop-selector-btn { font-size: 12px; padding: 5px 12px; }\n"
"      .header-leaf { font-size: 100px; }\n"
"      .modal-box { padding: 20px 18px; }\n"
"      .modal-title { font-size: 20px; }\n"
"      .sensors, .pump-buttons, .crop-grid { grid-template-columns: 1fr; }\n"
"      .sensor-card { padding: 14px; }\n"
"      .sensor-icon { width: 30px; height: 30px; font-size: 16px; }\n"
"      .sensor-reading { font-size: 26px; }\n"
"      .avatar-canvas { width: 180px; height: 180px; }\n"
"      .plant-panel { padding: 20px 18px 16px; }\n"
"      .growth-section { max-width: 100%; }\n"
"      .harvest-btn, .preview-bar { max-width: 100%; }\n"
"      .pump-panel { padding: 16px; }\n"
"      .pump-btn { padding: 12px 8px; font-size: 12px; }\n"
"      .photo-panel { padding: 16px 18px 18px; }\n"
"      .drop-zone { padding: 24px 12px; }\n"
"      .drop-zone-icon { font-size: 28px; }\n"
"      .alert-banner { font-size: 13px; padding: 12px 14px; }\n"
"      .analysis-result-title { font-size: 16px; }\n"
"    }\n"
"    \n"
"    /* SMALL PHONES */\n"
"    @media (max-width: 480px) {\n"
"      body { padding: 12px 10px 36px; }\n"
"      .shell { gap: 12px; }\n"
"      .header { padding: 14px 16px 12px; }\n"
"      .header-text { flex: 1; }\n"
"      .header-text h1 { font-size: 24px; letter-spacing: -0.5px; }\n"
"      .header-text p { font-size: 10px; }\n"
"      .header-right { flex-wrap: wrap; justify-content: flex-start; width: 100%; gap: 8px; }\n"
"      .header-badge, .crop-selector-btn { font-size: 11px; padding: 4px 10px; border-radius: 999px; }\n"
"      .header-leaf { display: none; }\n"
"      .card { border-radius: 16px; }\n"
"      .modal-overlay { padding: 16px; }\n"
"      .modal-box { padding: 18px 16px; border-radius: 20px; }\n"
"      .modal-title { font-size: 18px; margin-bottom: 8px; }\n"
"      .modal-subtitle { font-size: 12px; line-height: 1.5; }\n"
"      .crop-card { padding: 12px; border-radius: 14px; }\n"
"      .crop-emoji { font-size: 24px; }\n"
"      .crop-name { font-size: 16px; }\n"
"      .crop-days { font-size: 10px; }\n"
"      .modal-close { font-size: 12px; padding: 11px 0; margin-top: 12px; }\n"
"      .plant-panel { padding: 16px 14px 12px; }\n"
"      .avatar-canvas { width: 160px; height: 160px; margin-bottom: 8px; }\n"
"      .mood-bubble { width: 38px; height: 38px; font-size: 18px; right: 8px; top: 0px; }\n"
"      .plant-name-label { font-size: 16px; }\n"
"      .days-badge { font-size: 11px; }\n"
"      .plant-status-pill { font-size: 12px; padding: 5px 12px; }\n"
"      .growth-section { margin-top: 8px; }\n"
"      .growth-label { font-size: 10px; }\n"
"      .growth-stage-name { font-size: 11px; }\n"
"      .growth-track { height: 12px; }\n"
"      .reset-btn { font-size: 10px; padding: 2px 8px; }\n"
"      .harvest-btn { font-size: 13px; padding: 11px 0; margin-top: 10px; }\n"
"      .preview-bar { margin-top: 8px; gap: 5px; }\n"
"      .preview-btn { font-size: 9px; padding: 6px 3px; }\n"
"      .sensor-card { padding: 12px; gap: 8px; }\n"
"      .sensor-header { gap: 4px; }\n"
"      .sensor-icon { width: 28px; height: 28px; font-size: 14px; }\n"
"      .sensor-icon-name { gap: 6px; }\n"
"      .sensor-name { font-size: 13px; }\n"
"      .sensor-unit { font-size: 10px; }\n"
"      .status-badge { font-size: 10px; padding: 3px 8px; }\n"
"      .sensor-reading { font-size: 22px; }\n"
"      .sensor-reading .unit-suffix { font-size: 11px; }\n"
"      .bar-track { height: 8px; }\n"
"      .range-labels { font-size: 9px; }\n"
"      .pump-panel { padding: 14px; }\n"
"      .pump-label { font-size: 10px; margin-bottom: 10px; }\n"
"      .pump-buttons { gap: 8px; margin-bottom: 8px; }\n"
"      .pump-btn { padding: 11px 6px; font-size: 11px; border-radius: 12px; }\n"
"      .pump-btn-icon { font-size: 14px; }\n"
"      .pump-note { font-size: 11px; }\n"
"      .photo-panel { padding: 14px 14px 16px; }\n"
"      .photo-panel-header { margin-bottom: 12px; }\n"
"      .photo-panel-title { font-size: 10px; }\n"
"      .photo-panel-badge { font-size: 9px; }\n"
"      .drop-zone { padding: 20px 10px; gap: 6px; }\n"
"      .drop-zone-icon { font-size: 28px; margin-bottom: 0px; }\n"
"      .drop-zone-label { font-size: 13px; }\n"
"      .drop-zone-sub { font-size: 11px; }\n"
"      .photo-preview-wrap img { max-height: 160px; }\n"
"      .photo-preview-remove { width: 28px; height: 28px; font-size: 12px; top: 6px; right: 6px; }\n"
"      .analyze-btn { font-size: 13px; padding: 11px 0; margin-top: 10px; }\n"
"      .analysis-result { margin-top: 12px; border-radius: 14px; }\n"
"      .analysis-result-header { padding: 12px 12px 10px; gap: 10px; }\n"
"      .analysis-crop-icon { font-size: 24px; }\n"
"      .analysis-result-title { font-size: 16px; margin-bottom: 2px; }\n"
"      .analysis-result-summary { font-size: 11px; }\n"
"      .analysis-confidence { font-size: 9px; padding: 2px 7px; }\n"
"      .analysis-result-body { padding: 12px; gap: 10px; }\n"
"      .analysis-stage-bar-labels { font-size: 10px; margin-bottom: 4px; }\n"
"      .analysis-stage-bar-track { height: 8px; }\n"
"      .analysis-notes { gap: 4px; }\n"
"      .analysis-note { font-size: 11px; gap: 6px; }\n"
"      .analysis-sync-row { gap: 8px; padding-top: 10px; }\n"
"      .sync-apply-btn { font-size: 12px; padding: 9px 0; }\n"
"      .sync-dismiss-btn { font-size: 12px; padding: 9px 12px; }\n"
"      .alert-banner { font-size: 12px; padding: 10px 12px; gap: 8px; }\n"
"      .alert-title { font-size: 12px; }\n"
"      .alert-icon { font-size: 16px; }\n"
"      .dev-toggle { padding: 6px 12px; font-size: 11px; bottom: 16px; right: 16px; }\n"
"      .dev-panel { width: 280px; padding: 12px; right: 16px; bottom: 56px; }\n"
"      .dev-panel h3 { font-size: 12px; }\n"
"      .dev-row label { font-size: 10px; }\n"
"      .dev-stage-btn { font-size: 10px; padding: 4px 6px; }\n"
"      .footer { font-size: 10px; }\n"
"    }\n"
"    \n"
"    /* EXTRA SMALL PHONES */\n"
"    @media (max-width: 360px) {\n"
"      body { padding: 10px 8px 32px; }\n"
"      .shell { gap: 10px; }\n"
"      .header { padding: 12px 14px 10px; }\n"
"      .header-text h1 { font-size: 20px; }\n"
"      .header-text p { font-size: 9px; }\n"
"      .header-right { gap: 6px; }\n"
"      .header-badge, .crop-selector-btn { font-size: 10px; padding: 3px 8px; }\n"
"      .modal-box { padding: 16px 14px; }\n"
"      .modal-title { font-size: 16px; }\n"
"      .modal-subtitle { font-size: 11px; }\n"
"      .crop-card { padding: 10px; }\n"
"      .crop-emoji { font-size: 20px; }\n"
"      .crop-name { font-size: 14px; }\n"
"      .modal-close { font-size: 11px; }\n"
"      .avatar-canvas { width: 140px; height: 140px; }\n"
"      .mood-bubble { width: 34px; height: 34px; font-size: 16px; }\n"
"      .plant-name-label { font-size: 14px; }\n"
"      .sensor-reading { font-size: 20px; }\n"
"      .pump-btn { font-size: 10px; padding: 10px 4px; }\n"
"      .drop-zone-icon { font-size: 24px; }\n"
"      .drop-zone-label { font-size: 12px; }\n"
"    }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"\n"
"<div class=\"levelup-toast\" id=\"levelupToast\">\n"
"  <span id=\"levelupEmoji\">🥬</span>\n"
"  <span id=\"levelupText\">Stage reached!</span>\n"
"</div>\n"
"\n"
"<!-- CROP SELECTOR MODAL -->\n"
"<div class=\"modal-overlay\" id=\"cropModal\">\n"
"  <div class=\"modal-box\">\n"
"    <div class=\"modal-title\">Choose Your Crop</div>\n"
"    <div class=\"modal-subtitle\">Select what you're growing. The app adjusts sensor thresholds, growth timeline, and care messages to match your crop.</div>\n"
"    <div class=\"crop-grid\" id=\"cropGrid\"></div>\n"
"    <button class=\"modal-close\" id=\"modalClose\">Confirm Selection</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<div class=\"shell\">\n"
"\n"
"  <!-- HEADER -->\n"
"  <div class=\"card header\">\n"
"    <div class=\"header-text\">\n"
"      <h1>Plantagochi</h1>\n"
"      <p>Smart Hydroponics Monitor</p>\n"
"    </div>\n"
"    <div class=\"header-right\">\n"
"      <button class=\"crop-selector-btn\" id=\"openCropModal\">\n"
"        <span id=\"headerCropIcon\">🥬</span>\n"
"        <span id=\"headerCropName\">Lettuce</span>\n"
"      </button>\n"
"      <div class=\"header-badge\">\n"
"        <div class=\"pulse-dot\"></div>\n"
"        Live\n"
"      </div>\n"
"    </div>\n"
"    <div class=\"header-leaf\" aria-hidden=\"true\">🌿</div>\n"
"  </div>\n"
"\n"
"  <!-- PLANT PANEL -->\n"
"  <div class=\"card plant-panel\">\n"
"    <div class=\"scene-bg\"></div>\n"
"    <div class=\"soil\"></div>\n"
"    <div class=\"avatar-stage\">\n"
"      <div class=\"avatar-canvas\" id=\"avatarCanvas\">\n"
"        <div id=\"plantSvgContainer\" style=\"width:100%;height:100%;position:absolute;inset:0;\"></div>\n"
"        <div class=\"mood-bubble\" id=\"moodBubble\">😊</div>\n"
"        <div class=\"particle-layer\" id=\"particleLayer\"></div>\n"
"      </div>\n"
"\n"
"      <div class=\"avatar-info\">\n"
"        <div class=\"plant-name-label\"><span id=\"avatarCropEmoji\">🥬</span> Planty</div>\n"
"        <div class=\"plant-status-pill healthy\" id=\"plantStatusPill\">Your crop is thriving!</div>\n"
"        <div class=\"days-badge\">Day <span id=\"daysAlive\">0</span> of <span id=\"daysTotal\">35</span></div>\n"
"      </div>\n"
"\n"
"      <div class=\"growth-section\">\n"
"        <div class=\"growth-header\">\n"
"          <span class=\"growth-label\">Care Score</span>\n"
"          <div style=\"display:flex;align-items:center;gap:8px;\">\n"
"            <span class=\"growth-stage-name\" id=\"growthStageName\">Germination</span>\n"
"            <button class=\"reset-btn\" id=\"resetGrowthBtn\">↺ New Cycle</button>\n"
"          </div>\n"
"        </div>\n"
"        <div class=\"growth-track\"><div class=\"growth-fill\" id=\"growthFill\"></div></div>\n"
"        <div class=\"growth-stages-row\" id=\"growthStagesRow\"></div>\n"
"      </div>\n"
"\n"
"      <button class=\"harvest-btn\" id=\"harvestBtn\">🌾 Harvest Now — Start New Cycle</button>\n"
"\n"
"      <div class=\"preview-bar\" id=\"previewBar\" hidden></div>\n"
"\n"
"      <div id=\"resetConfirm\" style=\"display:none;margin-top:10px;background:var(--red-pale);border:1.5px solid rgba(193,18,31,0.25);border-radius:14px;padding:12px 14px;width:100%;max-width:380px;\">\n"
"        <p style=\"font-size:13px;font-weight:700;color:#7a0c13;margin-bottom:10px;\">🌱 Start a new crop cycle? This will reset all growth progress.</p>\n"
"        <div style=\"display:flex;gap:8px;\">\n"
"          <button class=\"confirm-btn confirm-yes\" id=\"confirmResetYes\">Yes, reset</button>\n"
"          <button class=\"confirm-btn confirm-no\"  id=\"confirmResetNo\">Cancel</button>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <!-- ALERT BANNER -->\n"
"  <div class=\"alert-banner\" id=\"alertBanner\" role=\"alert\">\n"
"    <span class=\"alert-icon\" id=\"alertIcon\">⚠️</span>\n"
"    <div class=\"alert-body\">\n"
"      <div class=\"alert-title\" id=\"alertTitle\">Attention needed</div>\n"
"      <div id=\"alertText\"></div>\n"
"    </div>\n"
"    <button class=\"alert-close\" id=\"alertClose\">×</button>\n"
"  </div>\n"
"\n"
"  <!-- SENSORS -->\n"
"  <div class=\"sensors\">\n"
"    <div class=\"sensor-card water-card\">\n"
"      <div class=\"sensor-header\">\n"
"        <div class=\"sensor-icon-name\">\n"
"          <div class=\"sensor-icon\">💧</div>\n"
"          <div><div class=\"sensor-name\">Water</div><div class=\"sensor-unit\">Reservoir level</div></div>\n"
"        </div>\n"
"        <span class=\"status-badge status-normal\" id=\"waterBadge\">Normal</span>\n"
"      </div>\n"
"      <div class=\"sensor-reading\" id=\"waterText\">0<span class=\"unit-suffix\">%</span></div>\n"
"      <div class=\"bar-track\"><div class=\"bar-fill\" id=\"waterBar\"></div></div>\n"
"      <div class=\"range-labels\"><span>Empty</span><span>Full</span></div>\n"
"    </div>\n"
"    <div class=\"sensor-card tds-card\">\n"
"      <div class=\"sensor-header\">\n"
"        <div class=\"sensor-icon-name\">\n"
"          <div class=\"sensor-icon\">🧪</div>\n"
"          <div><div class=\"sensor-name\">Nutrients</div><div class=\"sensor-unit\">TDS concentration</div></div>\n"
"        </div>\n"
"        <span class=\"status-badge status-normal\" id=\"tdsBadge\">Normal</span>\n"
"      </div>\n"
"      <div class=\"sensor-reading\" id=\"tdsText\">0<span class=\"unit-suffix\"> ppm</span></div>\n"
"      <div class=\"bar-track\"><div class=\"bar-fill\" id=\"tdsBar\"></div></div>\n"
"      <div class=\"range-labels\"><span>0</span><span id=\"tdsMax\">1500</span></div>\n"
"    </div>\n"
"    <div class=\"sensor-card ph-card\">\n"
"      <div class=\"sensor-header\">\n"
"        <div class=\"sensor-icon-name\">\n"
"          <div class=\"sensor-icon\">⚗️</div>\n"
"          <div><div class=\"sensor-name\">pH</div><div class=\"sensor-unit\">Acidity balance</div></div>\n"
"        </div>\n"
"        <span class=\"status-badge status-normal\" id=\"phBadge\">Normal</span>\n"
"      </div>\n"
"      <div class=\"sensor-reading\" id=\"phText\">0.0<span class=\"unit-suffix\"> pH</span></div>\n"
"      <div class=\"bar-track\"><div class=\"bar-fill\" id=\"phBar\"></div></div>\n"
"      <div class=\"range-labels\"><span>0</span><span>14</span></div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <!-- PHOTO ANALYSIS PANEL -->\n"
"  <div class=\"card photo-panel\">\n"
"    <div class=\"photo-panel-header\">\n"
"      <span class=\"photo-panel-title\">Plant Photo Analysis</span>\n"
"      <span class=\"photo-panel-badge\">AI-powered</span>\n"
"    </div>\n"
"\n"
"    <!-- Drop zone -->\n"
"    <div class=\"drop-zone\" id=\"dropZone\">\n"
"      <div class=\"drop-zone-icon\">📷</div>\n"
"      <span class=\"drop-zone-label\">Drop a plant photo or tap to upload</span>\n"
"      <span class=\"drop-zone-sub\">Top-down or side view · JPG, PNG, WEBP</span>\n"
"      <input type=\"file\" id=\"photoFileInput\" accept=\"image/jpeg,image/png,image/webp\">\n"
"    </div>\n"
"\n"
"    <!-- Preview -->\n"
"    <div class=\"photo-preview-wrap\" id=\"photoPreviewWrap\" style=\"display:none;\">\n"
"      <img id=\"photoPreviewImg\" src=\"\" alt=\"Plant preview\">\n"
"      <button class=\"photo-preview-remove\" id=\"photoRemoveBtn\" title=\"Remove photo\">✕</button>\n"
"    </div>\n"
"\n"
"    <!-- Analyze button -->\n"
"    <button class=\"analyze-btn\" id=\"analyzeBtn\" disabled>\n"
"      <span style=\"font-size:16px;\">🔬</span> Analyze Plant\n"
"    </button>\n"
"\n"
"    <!-- Result -->\n"
"    <div class=\"analysis-result\" id=\"analysisResult\" style=\"display:none;\">\n"
"      <div class=\"analysis-result-header\">\n"
"        <span class=\"analysis-crop-icon\" id=\"analysisCropIcon\">🥬</span>\n"
"        <div class=\"analysis-result-meta\">\n"
"          <div class=\"analysis-result-title\" id=\"analysisTitle\">—</div>\n"
"          <div class=\"analysis-result-summary\" id=\"analysisSummary\">—</div>\n"
"        </div>\n"
"        <span class=\"analysis-confidence\" id=\"analysisConfidence\">—</span>\n"
"      </div>\n"
"      <div class=\"analysis-result-body\">\n"
"        <div class=\"analysis-stage-bar-wrap\">\n"
"          <div class=\"analysis-stage-bar-labels\">\n"
"            <span>Growth progress detected</span>\n"
"            <span id=\"analysisPct\">—</span>\n"
"          </div>\n"
"          <div class=\"analysis-stage-bar-track\">\n"
"            <div class=\"analysis-stage-bar-fill\" id=\"analysisBarFill\" style=\"width:0%\"></div>\n"
"          </div>\n"
"        </div>\n"
"        <div class=\"analysis-notes\" id=\"analysisNotes\"></div>\n"
"        <div class=\"analysis-sync-row\">\n"
"          <button class=\"sync-apply-btn\" id=\"syncApplyBtn\">Apply to Growth Meter</button>\n"
"          <button class=\"sync-dismiss-btn\" id=\"syncDismissBtn\">Dismiss</button>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <!-- PUMP CONTROL -->\n"
"  <div class=\"card pump-panel\">\n"
"    <p class=\"pump-label\">Plant controls</p>\n"
"    <div class=\"pump-buttons\">\n"
"      <button class=\"pump-btn\" id=\"waterPumpBtn\" type=\"button\" data-state=\"idle\"><span class=\"pump-btn-icon\">💧</span> Add Water</button>\n"
"      <button class=\"pump-btn\" id=\"removeWaterBtn\" type=\"button\" data-state=\"idle\"><span class=\"pump-btn-icon\">🚰</span> Remove Water</button>\n"
"      <button class=\"pump-btn\" id=\"nutrientPumpBtn\" type=\"button\" data-state=\"idle\"><span class=\"pump-btn-icon\">🧪</span> Feed Plant</button>\n"
"      <button class=\"pump-btn\" id=\"phDownBtn\" type=\"button\" data-state=\"idle\"><span class=\"pump-btn-icon\">⬇️</span> pH Down</button>\n"
"      <button class=\"pump-btn\" id=\"phUpBtn\" type=\"button\" data-state=\"idle\"><span class=\"pump-btn-icon\">⬆️</span> pH Up</button>\n"
"    </div>\n"
"    <p class=\"pump-note\">Pump runs automatically when levels are critical</p>\n"
"  </div>\n"
"\n"
"  <div class=\"footer\">Plantagochi · ESP32 Hydroponics System</div>\n"
"</div>\n"
"\n"
"<!-- DEV PANEL -->\n"
"<button class=\"dev-toggle\" id=\"devToggle\">⚙ Dev</button>\n"
"<div class=\"dev-panel\" id=\"devPanel\">\n"
"  <h3>🔧 Dev Controls</h3>\n"
"  <div class=\"dev-row\">\n"
"    <label>Water Level <span id=\"devWaterVal\">0%</span></label>\n"
"    <input type=\"range\" id=\"devWater\" min=\"0\" max=\"100\" value=\"0\">\n"
"  </div>\n"
"  <div class=\"dev-row\">\n"
"    <label>Nutrients (TDS) <span id=\"devTdsVal\">0 ppm</span></label>\n"
"    <input type=\"range\" id=\"devTds\" min=\"0\" max=\"2000\" step=\"10\" value=\"0\">\n"
"  </div>\n"
"  <div class=\"dev-row\">\n"
"    <label>pH Level <span id=\"devPhVal\">0.0</span></label>\n"
"    <input type=\"range\" id=\"devPh\" min=\"0\" max=\"14\" step=\"0.1\" value=\"0\">\n"
"  </div>\n"
"  <hr class=\"dev-divider\">\n"
"  <div class=\"dev-row\">\n"
"    <label>Jump to Stage</label>\n"
"    <div class=\"dev-stage-btns\" id=\"devStageBtns\"></div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<script>\n"
"/* ══════════════════════════════════════════════\n"
"   CONFIGURATION — fill in before flashing to ESP32\n"
"   ══════════════════════════════════════════════\n"
"\n"
"   PROXY_URL:\n"
"     Your laptop's local IP running server.py (Flask + CNN).\n"
"     Find it: run  hostname -I  (Linux/Mac)  or  ipconfig  (Windows)\n"
"     Look for your Wi-Fi IPv4 address, e.g. 192.168.1.105\n"
"     Must be on the same Wi-Fi network as the ESP32.\n"
"\n"
"   esp32_base_url:\n"
"     Leave as \"\" (empty string) when the ESP32 serves this HTML.\n"
"     The browser will automatically send /data and /action\n"
"     requests to the same IP that served the page (the ESP32).\n"
"     Only set a full URL (e.g. \"http://192.168.1.200\") when\n"
"     testing via VSCode Live Server on your laptop.\n"
"\n"
"   DEV.enabled:\n"
"     Set to false before flashing — otherwise the dashboard\n"
"     shows fake sensor values instead of real ESP32 readings.\n"
"══════════════════════════════════════════════ */\n"
"let PROXY_URL = null;\n"
"\n"
"async function findServer() {\n"
"  const candidates = Array.from({length: 10}, (_, i) => `http://192.168.4.${i+2}:5000`);\n"
"  for (const url of candidates) {\n"
"    try {\n"
"      const res = await fetch(url + \"/health\", { signal: AbortSignal.timeout(1500) });\n"
"      if (res.ok) { PROXY_URL = url; return true; }\n"
"    } catch(e) {}\n"
"  }\n"
"  return false;\n"
"}\n"
"const esp32_base_url   = \"\";    // keep empty — ESP32 serves itself\n"
"const POLL_INTERVAL_MS = 30000; // poll sensors every 30 seconds\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   DEV OVERRIDE\n"
"══════════════════════════════════════════════ */\n"
"const DEV = {\n"
"  enabled:        false,\n"
"  showPreviewBar: false,\n"
"  waterLevel:     72,\n"
"  tdsValue:       650,\n"
"  phValue:        6.3,\n"
"  growthPoints:   0,\n"
"};\n"
"\n"
"const DEMO = { waterLevel: 0, tdsValue: 0, phValue: 0.0 };\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   CROP PROFILES\n"
"══════════════════════════════════════════════ */\n"
"const CROPS = [\n"
"  {\n"
"    id: \"lettuce\", name: \"Lettuce\", emoji: \"🥬\", daysToHarvest: 35,\n"
"    stages: [\n"
"      { name: \"Germination\",    emoji: \"🌱\", day: 0  },\n"
"      { name: \"Seedling\",       emoji: \"🌿\", day: 4  },\n"
"      { name: \"Vegetative\",     emoji: \"🥬\", day: 11 },\n"
"      { name: \"Head Formation\", emoji: \"🫛\", day: 21 },\n"
"      { name: \"Harvest Ready\",  emoji: \"🌾\", day: 31 },\n"
"    ],\n"
"    sensors: { ph: { min: 5.5, max: 6.5 }, tds: { min: 560, max: 840 }, water: { min: 30, max: 100 } },\n"
"    moods: {\n"
"      happy: \"Lettuce is thriving!\", thirsty: \"Leaves wilting — add water!\",\n"
"      hungry: \"Pale leaves — needs feeding!\", overfed: \"Too many nutrients — dilute!\",\n"
"      stressed: \"Nutrient lockout — fix pH!\", critical: \"Crop at risk — act now!\",\n"
"      multi: \"Multiple issues — check all sensors!\",\n"
"    }\n"
"  },\n"
"  {\n"
"    id: \"spinach\", name: \"Spinach\", emoji: \"🍃\", daysToHarvest: 45,\n"
"    stages: [\n"
"      { name: \"Germination\", emoji: \"🌱\", day: 0  },\n"
"      { name: \"Seedling\",    emoji: \"🌿\", day: 5  },\n"
"      { name: \"Leafing Out\", emoji: \"🍃\", day: 14 },\n"
"      { name: \"Full Growth\", emoji: \"🌿\", day: 28 },\n"
"      { name: \"Harvest Ready\", emoji: \"🌾\", day: 40 },\n"
"    ],\n"
"    sensors: { ph: { min: 6.0, max: 7.0 }, tds: { min: 1260, max: 1610 }, water: { min: 30, max: 100 } },\n"
"    moods: {\n"
"      happy: \"Spinach is growing strong!\", thirsty: \"Leaves drooping — add water!\",\n"
"      hungry: \"Yellowing leaves — add nutrients!\", overfed: \"Nutrient burn — reduce TDS!\",\n"
"      stressed: \"pH off — spinach is stressed!\", critical: \"Spinach in danger — act now!\",\n"
"      multi: \"Multiple issues detected!\",\n"
"    }\n"
"  },\n"
"  {\n"
"    id: \"basil\", name: \"Basil\", emoji: \"🌿\", daysToHarvest: 30,\n"
"    stages: [\n"
"      { name: \"Germination\", emoji: \"🌱\", day: 0  },\n"
"      { name: \"Seedling\",    emoji: \"🌿\", day: 4  },\n"
"      { name: \"Bushing Out\", emoji: \"🪴\", day: 10 },\n"
"      { name: \"Mature Herb\", emoji: \"🌿\", day: 20 },\n"
"      { name: \"Harvest Ready\", emoji: \"🌾\", day: 26 },\n"
"    ],\n"
"    sensors: { ph: { min: 5.5, max: 6.5 }, tds: { min: 700, max: 1120 }, water: { min: 30, max: 100 } },\n"
"    moods: {\n"
"      happy: \"Basil is fragrant and healthy!\", thirsty: \"Basil wilting — water it!\",\n"
"      hungry: \"Small pale leaves — needs food!\", overfed: \"Over-fertilized — ease up!\",\n"
"      stressed: \"pH stress — leaves may yellow!\", critical: \"Basil struggling — act now!\",\n"
"      multi: \"Multiple issues — check sensors!\",\n"
"    }\n"
"  },\n"
"  {\n"
"    id: \"kale\", name: \"Kale\", emoji: \"🥦\", daysToHarvest: 60,\n"
"    stages: [\n"
"      { name: \"Germination\", emoji: \"🌱\", day: 0  },\n"
"      { name: \"Seedling\",    emoji: \"🌿\", day: 6  },\n"
"      { name: \"Leafing Out\", emoji: \"🥦\", day: 18 },\n"
"      { name: \"Full Growth\", emoji: \"🥦\", day: 40 },\n"
"      { name: \"Harvest Ready\", emoji: \"🌾\", day: 55 },\n"
"    ],\n"
"    sensors: { ph: { min: 5.5, max: 6.5 }, tds: { min: 1120, max: 1680 }, water: { min: 30, max: 100 } },\n"
"    moods: {\n"
"      happy: \"Kale is growing vigorously!\", thirsty: \"Kale needs water!\",\n"
"      hungry: \"Light-coloured leaves — feed it!\", overfed: \"Too rich — dilute solution!\",\n"
"      stressed: \"pH off — kale is stressed!\", critical: \"Kale at risk — act now!\",\n"
"      multi: \"Multiple issues — check sensors!\",\n"
"    }\n"
"  },\n"
"  {\n"
"    id: \"pechay\", name: \"Pechay\", emoji: \"🥬\", daysToHarvest: 35,\n"
"    stages: [\n"
"      { name: \"Germination\",    emoji: \"🌱\", day: 0  },\n"
"      { name: \"Seedling\",       emoji: \"🌿\", day: 4  },\n"
"      { name: \"Vegetative\",     emoji: \"🥬\", day: 10 },\n"
"      { name: \"Head Formation\", emoji: \"🫛\", day: 22 },\n"
"      { name: \"Harvest Ready\",  emoji: \"🌾\", day: 30 },\n"
"    ],\n"
"    sensors: { ph: { min: 6.0, max: 7.0 }, tds: { min: 840, max: 1260 }, water: { min: 30, max: 100 } },\n"
"    moods: {\n"
"      happy: \"Pechay is looking great!\", thirsty: \"Pechay needs water!\",\n"
"      hungry: \"Pale leaves — add nutrients!\", overfed: \"Over-fed — reduce strength!\",\n"
"      stressed: \"pH imbalance — pechay stressed!\", critical: \"Pechay in danger — act now!\",\n"
"      multi: \"Multiple issues — check sensors!\",\n"
"    }\n"
"  },\n"
"];\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   STATE\n"
"══════════════════════════════════════════════ */\n"
"var currentCrop = loadCrop();\n"
"var growth      = loadGrowth();\n"
"var currentMood = \"happy\";\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   HELPERS\n"
"══════════════════════════════════════════════ */\n"
"function getStageIndex(pts) {\n"
"  var stages = currentCrop.stages;\n"
"  var pct    = pts / 1000;\n"
"  var dayEst = pct * currentCrop.daysToHarvest;\n"
"  for (var i = stages.length - 1; i >= 0; i--) {\n"
"    if (dayEst >= stages[i].day) return i;\n"
"  }\n"
"  return 0;\n"
"}\n"
"function shadeColor(hex, pct) {\n"
"  var n = parseInt(hex.replace('#',''), 16);\n"
"  return '#' + ((1<<24)|((Math.max(0,(n>>16)-pct))<<16)|((Math.max(0,((n>>8)&0xff)-pct))<<8)|(Math.max(0,(n&0xff)-pct))).toString(16).slice(1);\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   PERSISTENCE\n"
"══════════════════════════════════════════════ */\n"
"function loadCrop() {\n"
"  try {\n"
"    var id = localStorage.getItem(\"plantagochi_crop\");\n"
"    if (id) { var c = CROPS.find(function(c){ return c.id===id; }); if(c) return c; }\n"
"  } catch(e) {}\n"
"  return CROPS[0];\n"
"}\n"
"function saveCrop(crop) { try { localStorage.setItem(\"plantagochi_crop\", crop.id); } catch(e) {} }\n"
"\n"
"function loadGrowth() {\n"
"  if (DEV.enabled) {\n"
"    var pts = Math.max(0, Math.min(1000, DEV.growthPoints));\n"
"    return { points: pts, stage: 0, startDate: Date.now(), lastTick: Date.now() };\n"
"  }\n"
"  try {\n"
"    var saved = localStorage.getItem(\"plantagochi_growth\");\n"
"    if (saved) return JSON.parse(saved);\n"
"  } catch(e) {}\n"
"  return { points: 0, stage: 0, startDate: Date.now(), lastTick: Date.now() };\n"
"}\n"
"function saveGrowth(g) {\n"
"  if (DEV.enabled) return;\n"
"  try { localStorage.setItem(\"plantagochi_growth\", JSON.stringify(g)); } catch(e) {}\n"
"}\n"
"function resetGrowth() {\n"
"  growth = { points: 0, stage: 0, startDate: Date.now(), lastTick: Date.now() };\n"
"  saveGrowth(growth);\n"
"  document.getElementById(\"growthStagesRow\").innerHTML = \"\";\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   SENSOR STATUS\n"
"══════════════════════════════════════════════ */\n"
"function getWaterStatus(v) {\n"
"  var mn = currentCrop.sensors.water.min;\n"
"  return v < mn * 0.4 ? \"critical\" : v < mn ? \"low\" : \"normal\";\n"
"}\n"
"function getTdsStatus(v) {\n"
"  var mn = currentCrop.sensors.tds.min, mx = currentCrop.sensors.tds.max;\n"
"  if (v < mn*0.4 || v > mx*1.3) return \"critical\";\n"
"  if (v < mn     || v > mx)     return \"low\";\n"
"  return \"normal\";\n"
"}\n"
"function getPhStatus(v) {\n"
"  var mn = currentCrop.sensors.ph.min, mx = currentCrop.sensors.ph.max;\n"
"  if (v < mn-1.0 || v > mx+1.0) return \"critical\";\n"
"  if (v < mn     || v > mx)     return \"low\";\n"
"  return \"normal\";\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   MOOD\n"
"══════════════════════════════════════════════ */\n"
"function getMood(wSt, tSt, pSt, tdsValue) {\n"
"  var statuses    = [wSt, tSt, pSt];\n"
"  var hasCritical = statuses.includes(\"critical\");\n"
"  var badCount    = statuses.filter(function(s){ return s !== \"normal\"; }).length;\n"
"  if (hasCritical && badCount >= 2) return \"critical\";\n"
"  if (badCount >= 2) return \"multi\";\n"
"  if (wSt !== \"normal\") return \"thirsty\";\n"
"  if (tSt !== \"normal\") return tdsValue > currentCrop.sensors.tds.max ? \"overfed\" : \"hungry\";\n"
"  if (pSt !== \"normal\") return \"stressed\";\n"
"  return \"happy\";\n"
"}\n"
"var MOOD_EMOJI = { happy:\"😊\", thirsty:\"🥵\", hungry:\"😮‍💨\", overfed:\"🤢\", stressed:\"😟\", critical:\"😵\", multi:\"😰\" };\n"
"var MOOD_CLS   = { happy:\"healthy\", thirsty:\"warning\", hungry:\"warning\", overfed:\"warning\", stressed:\"warning\", critical:\"critical\", multi:\"critical\" };\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   TIME-BASED GROWTH TICK\n"
"══════════════════════════════════════════════ */\n"
"function growthTick(wSt, tSt, pSt) {\n"
"  if (DEV.enabled) {\n"
"    var pts      = Math.max(0, Math.min(1000, DEV.growthPoints));\n"
"    var oldStage = growth.stage;\n"
"    growth.points = pts;\n"
"    growth.stage  = getStageIndex(pts);\n"
"    return growth.stage > oldStage;\n"
"  }\n"
"  var now     = Date.now();\n"
"  var elapsed = (now - (growth.lastTick || now)) / 60000;\n"
"  growth.lastTick = now;\n"
"  var mins    = Math.min(elapsed, 10);\n"
"\n"
"  var baseRate  = 1000 / (currentCrop.daysToHarvest * 24 * 60);\n"
"  var statuses  = [wSt, tSt, pSt];\n"
"  var critical  = statuses.includes(\"critical\");\n"
"  var bad       = statuses.filter(function(s){ return s !== \"normal\"; }).length;\n"
"  var mult      = critical ? -0.5 : bad === 2 ? 0 : bad === 1 ? 0.4 : 1.0;\n"
"\n"
"  growth.points = Math.max(0, Math.min(1000, growth.points + baseRate * mult * mins));\n"
"  var newStage  = getStageIndex(growth.points);\n"
"  var leveled   = newStage > growth.stage;\n"
"  growth.stage  = Math.max(growth.stage, newStage);\n"
"  saveGrowth(growth);\n"
"  return leveled;\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   VEGETABLE SVG — ROSETTE STYLE\n"
"══════════════════════════════════════════════ */\n"
"function drawPlantSVG(stageIdx, mood) {\n"
"  var container = document.getElementById(\"plantSvgContainer\");\n"
"  var s = stageIdx;\n"
"  var moodColors = {\n"
"    happy:    { leaf:\"#52b788\", accent:\"#74c69d\", vein:\"#2d6a4f\", pot:\"#a0714f\" },\n"
"    thirsty:  { leaf:\"#c4924a\", accent:\"#d4a96a\", vein:\"#7a5c32\", pot:\"#8c5e30\" },\n"
"    hungry:   { leaf:\"#90c8a0\", accent:\"#b0ddb8\", vein:\"#4a7c59\", pot:\"#a0714f\" },\n"
"    overfed:  { leaf:\"#2a6640\", accent:\"#1e4d30\", vein:\"#163820\", pot:\"#a0714f\" },\n"
"    stressed: { leaf:\"#b8a040\", accent:\"#d4bc60\", vein:\"#7a6a20\", pot:\"#a0714f\" },\n"
"    critical: { leaf:\"#9a3030\", accent:\"#c04040\", vein:\"#6a2020\", pot:\"#8c4040\" },\n"
"    multi:    { leaf:\"#9a3030\", accent:\"#c04040\", vein:\"#6a2020\", pot:\"#8c4040\" },\n"
"  };\n"
"  var c     = moodColors[mood] || moodColors.happy;\n"
"  var droop = (mood===\"thirsty\"||mood===\"critical\"||mood===\"multi\") ? 1 : 0;\n"
"\n"
"  var CX = 100, CY = 140;\n"
"  var radii  = [20, 34, 50, 64, 78];\n"
"  var counts = [4,   6,  8, 10, 12];\n"
"  var lws    = [14, 18, 22, 26, 30];\n"
"  var lhs    = [8,  10, 13, 16, 18];\n"
"\n"
"  var svg = '<svg viewBox=\"0 0 200 200\" xmlns=\"http://www.w3.org/2000/svg\" width=\"100%\" height=\"100%\">';\n"
"  svg += '<ellipse cx=\"100\" cy=\"196\" rx=\"42\" ry=\"5\" fill=\"#5c3d1e\" opacity=\"0.15\"/>';\n"
"  svg += '<path d=\"M68 162 Q65 193 80 195 L120 195 Q135 193 132 162 Z\" fill=\"' + c.pot + '\"/>';\n"
"  svg += '<rect x=\"64\" y=\"152\" width=\"72\" height=\"13\" rx=\"6\" fill=\"' + shadeColor(c.pot,-10) + '\"/>';\n"
"  svg += '<ellipse cx=\"100\" cy=\"152\" rx=\"28\" ry=\"5\" fill=\"#5c3d1e\" opacity=\"0.55\"/>';\n"
"\n"
"  for (var si = 0; si <= s; si++) {\n"
"    var ri = radii[si], cnti = counts[si], lwi = lws[si], lhi = lhs[si];\n"
"    var opacity = 0.72 + si * 0.06;\n"
"    for (var i = 0; i < cnti; i++) {\n"
"      var angle  = (i / cnti) * 2 * Math.PI + (si * 0.3);\n"
"      var droopY = droop * ri * 0.18;\n"
"      var lx = CX + Math.cos(angle) * ri;\n"
"      var ly = CY + Math.sin(angle) * ri + droopY;\n"
"      var rot = angle * 180 / Math.PI + 90;\n"
"      svg += '<ellipse cx=\"' + lx.toFixed(1) + '\" cy=\"' + ly.toFixed(1) + '\"'\n"
"           + ' rx=\"' + lwi + '\" ry=\"' + lhi + '\"'\n"
"           + ' transform=\"rotate(' + rot.toFixed(1) + ' ' + lx.toFixed(1) + ' ' + ly.toFixed(1) + ')\"'\n"
"           + ' fill=\"' + c.leaf + '\" opacity=\"' + opacity + '\"/>';\n"
"      var vx = (CX * 0.4 + lx * 0.6), vy = (CY * 0.4 + ly * 0.6);\n"
"      svg += '<line x1=\"' + CX + '\" y1=\"' + CY + '\" x2=\"' + vx.toFixed(1) + '\" y2=\"' + vy.toFixed(1) + '\"'\n"
"           + ' stroke=\"' + c.vein + '\" stroke-width=\"0.7\" opacity=\"0.3\"/>';\n"
"    }\n"
"  }\n"
"\n"
"  var budR = 5 + s * 2;\n"
"  svg += '<circle cx=\"' + CX + '\" cy=\"' + CY + '\" r=\"' + (budR+3) + '\" fill=\"' + c.accent + '\" opacity=\"0.85\"/>';\n"
"  svg += '<circle cx=\"' + CX + '\" cy=\"' + CY + '\" r=\"' + budR + '\" fill=\"' + shadeColor(c.leaf,-20) + '\" opacity=\"0.9\"/>';\n"
"\n"
"  if (s >= 4 && mood === \"happy\") {\n"
"    [[68,60],[132,55],[55,82],[142,80],[100,44],[80,42],[122,48]].forEach(function(p,i){\n"
"      svg += '<circle cx=\"' + p[0] + '\" cy=\"' + p[1] + '\" r=\"2.2\" fill=\"#ffd166\" opacity=\"' + (0.35+i*0.09) + '\"/>';\n"
"    });\n"
"  }\n"
"  if (mood === \"thirsty\" && s >= 1) {\n"
"    svg += '<path d=\"M' + (CX-radii[s]*0.6).toFixed(0) + ' ' + (CY-10) + ' q-4 8 4 6\" stroke=\"' + c.accent + '\" stroke-width=\"1.5\" fill=\"none\" opacity=\"0.5\"/>';\n"
"  }\n"
"  svg += '</svg>';\n"
"  container.innerHTML = svg;\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   RENDER CARE SCORE UI\n"
"══════════════════════════════════════════════ */\n"
"function renderGrowthUI(leveled) {\n"
"  var pct    = (growth.points / 1000) * 100;\n"
"  var stages = currentCrop.stages;\n"
"  var stage  = stages[growth.stage];\n"
"\n"
"  document.getElementById(\"growthFill\").style.width = pct.toFixed(1) + \"%\";\n"
"  document.getElementById(\"growthStageName\").textContent = stage.name;\n"
"\n"
"  var daysAlive = Math.floor((Date.now() - (growth.startDate || Date.now())) / 86400000);\n"
"  document.getElementById(\"daysAlive\").textContent = daysAlive;\n"
"  document.getElementById(\"daysTotal\").textContent = currentCrop.daysToHarvest;\n"
"\n"
"  var row = document.getElementById(\"growthStagesRow\");\n"
"  row.innerHTML = \"\";\n"
"  stages.forEach(function(st, i) {\n"
"    var div = document.createElement(\"div\");\n"
"    div.className = \"growth-stage-tick\" + (i <= growth.stage ? \" reached\" : \"\");\n"
"    div.innerHTML = '<span class=\"tick-emoji\">' + st.emoji + '</span><span>' + st.name.split(\" \")[0] + '</span>';\n"
"    row.appendChild(div);\n"
"  });\n"
"\n"
"  var hBtn = document.getElementById(\"harvestBtn\");\n"
"  if (growth.stage >= stages.length - 1) hBtn.classList.add(\"visible\");\n"
"  else hBtn.classList.remove(\"visible\");\n"
"\n"
"  if (leveled) {\n"
"    document.getElementById(\"levelupEmoji\").textContent = stage.emoji;\n"
"    document.getElementById(\"levelupText\").textContent  = stage.name + \" stage reached!\";\n"
"    var toast = document.getElementById(\"levelupToast\");\n"
"    toast.classList.add(\"show\");\n"
"    setTimeout(function(){ toast.classList.remove(\"show\"); }, 3200);\n"
"  }\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   SENSOR RENDER HELPERS\n"
"══════════════════════════════════════════════ */\n"
"function badgeText(s)  { return s===\"normal\"?\"Normal\":s===\"low\"?\"Low\":\"Critical\"; }\n"
"function badgeClass(s) { return \"status-badge \"+(s===\"normal\"?\"status-normal\":s===\"low\"?\"status-low\":\"status-critical\"); }\n"
"function barColor(s)   { return s===\"normal\"?\"#40916c\":s===\"low\"?\"#e09f3e\":\"#c1121f\"; }\n"
"function clamp(v,a,b)  { return Math.min(Math.max(v,a),b); }\n"
"\n"
"function setSensor(cfg) {\n"
"  var badge = document.getElementById(cfg.badgeId);\n"
"  var bar   = document.getElementById(cfg.barId);\n"
"  var text  = document.getElementById(cfg.textId);\n"
"  badge.textContent    = badgeText(cfg.status);\n"
"  badge.className      = badgeClass(cfg.status);\n"
"  bar.style.width      = (clamp(cfg.value/cfg.max,0,1)*100).toFixed(1)+\"%\";\n"
"  bar.style.background = barColor(cfg.status);\n"
"  text.innerHTML = cfg.display + '<span class=\"unit-suffix\">' + cfg.unit + '</span>';\n"
"}\n"
"\n"
"function updateAlert(issues, hasCritical) {\n"
"  var banner = document.getElementById(\"alertBanner\");\n"
"  if (!issues.length) { banner.style.display = \"none\"; return; }\n"
"  banner.className = \"alert-banner \" + (hasCritical ? \"critical\" : \"warning\");\n"
"  document.getElementById(\"alertIcon\").textContent  = hasCritical ? \"🚨\" : \"⚠️\";\n"
"  document.getElementById(\"alertTitle\").textContent = hasCritical ? \"Critical alert\" : \"Attention needed\";\n"
"  document.getElementById(\"alertText\").textContent  = issues.join(\" · \") + \" require\" + (issues.length===1?\"s\":\"\") + \" your attention.\";\n"
"  banner.style.display = \"flex\";\n"
"}\n"
"\n"
"function spawnParticles(type) {\n"
"  var layer  = document.getElementById(\"particleLayer\");\n"
"  var colors = { water:\"#5ea3e6\", nutrient:\"#52b788\", ph:\"#9b72c8\" };\n"
"  for (var i = 0; i < 8; i++) {\n"
"    var p = document.createElement(\"div\");\n"
"    p.className = \"particle\";\n"
"    p.style.cssText = \"width:8px;height:8px;background:\"+(colors[type]||\"#52b788\")\n"
"      +\";left:\"+(30+Math.random()*140)+\"px;top:\"+(50+Math.random()*60)+\"px\"\n"
"      +\";animation-delay:\"+(Math.random()*0.4)+\"s\";\n"
"    layer.appendChild(p);\n"
"    setTimeout(function(){ p.remove(); }, 1800);\n"
"  }\n"
"}\n"
"\n"
"function runPump(btnId, label, icon, pt, action) {\n"
"  var btn = document.getElementById(btnId);\n"
"  if (btn.disabled) return;\n"
"  btn.disabled = true; btn.dataset.state = \"loading\";\n"
"  btn.innerHTML = '<span class=\"pump-btn-icon\">⏳</span> Running…';\n"
"  spawnParticles(pt);\n"
"  fetch(esp32_base_url + \"/action?action=\" + action)\n"
"    .then(function(r) { return r.json(); })\n"
"    .then(function(d) {\n"
"      var duration = d.duration || 3000;\n"
"      setTimeout(function() {\n"
"        btn.dataset.state = \"done\";\n"
"        btn.innerHTML = '<span class=\"pump-btn-icon\">✅</span> Done!';\n"
"        setTimeout(function() {\n"
"          btn.dataset.state = \"idle\";\n"
"          btn.innerHTML = '<span class=\"pump-btn-icon\">' + icon + '</span> ' + label;\n"
"          btn.disabled = false;\n"
"        }, 900);\n"
"      }, duration);\n"
"    })\n"
"    .catch(function() {\n"
"      btn.dataset.state = \"idle\";\n"
"      btn.innerHTML = '<span class=\"pump-btn-icon\">' + icon + '</span> ' + label;\n"
"      btn.disabled = false;\n"
"    });\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   MAIN RENDER\n"
"══════════════════════════════════════════════ */\n"
"function renderDashboard(waterLevel, tdsValue, phValue) {\n"
"  var wSt = getWaterStatus(waterLevel);\n"
"  var tSt = getTdsStatus(tdsValue);\n"
"  var pSt = getPhStatus(phValue);\n"
"\n"
"  var tdsMax = Math.round(currentCrop.sensors.tds.max * 1.5);\n"
"  document.getElementById(\"tdsMax\").textContent = tdsMax;\n"
"\n"
"  setSensor({ badgeId:\"waterBadge\", barId:\"waterBar\", textId:\"waterText\", value:waterLevel, max:100,    status:wSt, display:waterLevel,        unit:\"%\" });\n"
"  setSensor({ badgeId:\"tdsBadge\",   barId:\"tdsBar\",   textId:\"tdsText\",   value:tdsValue,   max:tdsMax, status:tSt, display:tdsValue,           unit:\" ppm\" });\n"
"  setSensor({ badgeId:\"phBadge\",    barId:\"phBar\",     textId:\"phText\",    value:phValue,    max:14,     status:pSt, display:phValue.toFixed(1), unit:\" pH\" });\n"
"\n"
"  var mood    = getMood(wSt, tSt, pSt, tdsValue);\n"
"  currentMood = mood;\n"
"  document.getElementById(\"moodBubble\").textContent = MOOD_EMOJI[mood];\n"
"  var pill = document.getElementById(\"plantStatusPill\");\n"
"  pill.textContent = currentCrop.moods[mood];\n"
"  pill.className   = \"plant-status-pill \" + MOOD_CLS[mood];\n"
"\n"
"  var canvas = document.getElementById(\"avatarCanvas\");\n"
"  if (mood===\"critical\"||mood===\"multi\") canvas.classList.add(\"critical-shake\");\n"
"  else canvas.classList.remove(\"critical-shake\");\n"
"\n"
"  var leveled = growthTick(wSt, tSt, pSt);\n"
"  drawPlantSVG(growth.stage, mood);\n"
"  renderGrowthUI(leveled);\n"
"\n"
"  var statuses    = [wSt, tSt, pSt];\n"
"  var hasCritical = statuses.includes(\"critical\");\n"
"  var badCount    = statuses.filter(function(s){ return s!==\"normal\"; }).length;\n"
"  var issues = [];\n"
"  if (wSt!==\"normal\") issues.push(\"Water level\");\n"
"  if (tSt!==\"normal\") issues.push(\"Nutrients\");\n"
"  if (pSt!==\"normal\") issues.push(\"pH balance\");\n"
"  updateAlert(issues, hasCritical || badCount >= 2);\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   CROP MODAL\n"
"══════════════════════════════════════════════ */\n"
"function buildCropModal() {\n"
"  var grid = document.getElementById(\"cropGrid\");\n"
"  grid.innerHTML = \"\";\n"
"  CROPS.forEach(function(crop) {\n"
"    var card = document.createElement(\"div\");\n"
"    card.className = \"crop-card\" + (crop.id===currentCrop.id ? \" active\" : \"\");\n"
"    card.dataset.cropId = crop.id;\n"
"    card.innerHTML =\n"
"      '<div class=\"crop-card-header\"><span class=\"crop-emoji\">' + crop.emoji + '</span>'\n"
"      + '<div><div class=\"crop-name\">' + crop.name + '</div>'\n"
"      + '<div class=\"crop-days\">~' + crop.daysToHarvest + ' days to harvest</div></div></div>'\n"
"      + '<div class=\"crop-ranges\">'\n"
"      + '<div class=\"crop-range\">💧 Water <span>&gt;' + crop.sensors.water.min + '%</span></div>'\n"
"      + '<div class=\"crop-range\">🧪 TDS <span>' + crop.sensors.tds.min + '–' + crop.sensors.tds.max + ' ppm</span></div>'\n"
"      + '<div class=\"crop-range\">⚗️ pH <span>' + crop.sensors.ph.min + '–' + crop.sensors.ph.max + '</span></div>'\n"
"      + '</div>'\n"
"      + '<div class=\"crop-stages-preview\">'\n"
"      + crop.stages.map(function(st){ return '<span class=\"crop-stage-chip\">'+st.emoji+' '+st.name+'</span>'; }).join(\"\")\n"
"      + '</div>';\n"
"    card.addEventListener(\"click\", function() {\n"
"      document.querySelectorAll(\".crop-card\").forEach(function(c){ c.classList.remove(\"active\"); });\n"
"      card.classList.add(\"active\");\n"
"    });\n"
"    grid.appendChild(card);\n"
"  });\n"
"}\n"
"\n"
"function applySelectedCrop() {\n"
"  var active = document.querySelector(\".crop-card.active\");\n"
"  if (!active) return;\n"
"  var crop = CROPS.find(function(c){ return c.id===active.dataset.cropId; });\n"
"  if (!crop || crop.id===currentCrop.id) return;\n"
"  currentCrop = crop;\n"
"  saveCrop(crop);\n"
"  resetGrowth();\n"
"  updateCropUI();\n"
"  fetchSensorData();\n"
"}\n"
"\n"
"function updateCropUI() {\n"
"  document.getElementById(\"headerCropIcon\").textContent  = currentCrop.emoji;\n"
"  document.getElementById(\"headerCropName\").textContent  = currentCrop.name;\n"
"  document.getElementById(\"avatarCropEmoji\").textContent = currentCrop.emoji;\n"
"  buildPreviewBar();\n"
"  buildDevStageBtns();\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   PREVIEW BAR + DEV STAGE BUTTONS\n"
"══════════════════════════════════════════════ */\n"
"function buildPreviewBar() {\n"
"  var bar = document.getElementById(\"previewBar\");\n"
"  bar.innerHTML = \"\";\n"
"  currentCrop.stages.forEach(function(st, i) {\n"
"    var pts = Math.round((i / (currentCrop.stages.length-1)) * 1000);\n"
"    var btn = document.createElement(\"button\");\n"
"    btn.className = \"preview-btn\" + (i===0?\" active\":\"\");\n"
"    btn.dataset.pts = pts;\n"
"    btn.innerHTML = '<span class=\"pb-emoji\">' + st.emoji + '</span>' + st.name.split(\" \")[0];\n"
"    btn.addEventListener(\"click\", function() {\n"
"      document.querySelectorAll(\".preview-btn\").forEach(function(b){ b.classList.remove(\"active\"); });\n"
"      btn.classList.add(\"active\");\n"
"      growth.points = pts; growth.stage = getStageIndex(pts);\n"
"      drawPlantSVG(growth.stage, currentMood);\n"
"      renderGrowthUI(false);\n"
"    });\n"
"    bar.appendChild(btn);\n"
"  });\n"
"}\n"
"\n"
"function buildDevStageBtns() {\n"
"  var container = document.getElementById(\"devStageBtns\");\n"
"  container.innerHTML = \"\";\n"
"  currentCrop.stages.forEach(function(st, i) {\n"
"    var pts = Math.round((i / (currentCrop.stages.length-1)) * 1000);\n"
"    var btn = document.createElement(\"button\");\n"
"    btn.className = \"dev-stage-btn\" + (i===0?\" active\":\"\");\n"
"    btn.textContent = st.emoji + \" \" + st.name.split(\" \")[0];\n"
"    btn.dataset.pts = pts;\n"
"    btn.addEventListener(\"click\", function() {\n"
"      document.querySelectorAll(\".dev-stage-btn\").forEach(function(b){ b.classList.remove(\"active\"); });\n"
"      btn.classList.add(\"active\");\n"
"      growth.points = pts; growth.stage = getStageIndex(pts); growth.lastTick = Date.now();\n"
"      saveGrowth(growth);\n"
"      renderDashboard(\n"
"        parseFloat(document.getElementById(\"devWater\").value),\n"
"        parseFloat(document.getElementById(\"devTds\").value),\n"
"        parseFloat(document.getElementById(\"devPh\").value)\n"
"      );\n"
"    });\n"
"    container.appendChild(btn);\n"
"  });\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   ESP32 FETCH\n"
"══════════════════════════════════════════════ */\n"
"function fetchSensorData() {\n"
"  if (DEV.enabled) { renderDashboard(DEV.waterLevel, DEV.tdsValue, DEV.phValue); return; }\n"
"  fetch(esp32_base_url + \"/data\")\n"
"    .then(function(r){ return r.json(); })\n"
"    .then(function(d){ renderDashboard(Number(d.water)||0, Number(d.food)||0, Number(d.ph)||0); })\n"
"    .catch(function(){ renderDashboard(DEMO.waterLevel, DEMO.tdsValue, DEMO.phValue); });\n"
"}\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   PHOTO ANALYSIS\n"
"   Uses the Anthropic API to classify plant\n"
"   growth stage from an uploaded image, then\n"
"   applies the result to the growth meter.\n"
"══════════════════════════════════════════════ */\n"
"(function() {\n"
"  var photoFileInput   = document.getElementById(\"photoFileInput\");\n"
"  var dropZone         = document.getElementById(\"dropZone\");\n"
"  var photoPreviewWrap = document.getElementById(\"photoPreviewWrap\");\n"
"  var photoPreviewImg  = document.getElementById(\"photoPreviewImg\");\n"
"  var photoRemoveBtn   = document.getElementById(\"photoRemoveBtn\");\n"
"  var analyzeBtn       = document.getElementById(\"analyzeBtn\");\n"
"  var analysisResult   = document.getElementById(\"analysisResult\");\n"
"  var analysisCropIcon = document.getElementById(\"analysisCropIcon\");\n"
"  var analysisTitle    = document.getElementById(\"analysisTitle\");\n"
"  var analysisSummary  = document.getElementById(\"analysisSummary\");\n"
"  var analysisConf     = document.getElementById(\"analysisConfidence\");\n"
"  var analysisBarFill  = document.getElementById(\"analysisBarFill\");\n"
"  var analysisPct      = document.getElementById(\"analysisPct\");\n"
"  var analysisNotes    = document.getElementById(\"analysisNotes\");\n"
"  var syncApplyBtn     = document.getElementById(\"syncApplyBtn\");\n"
"  var syncDismissBtn   = document.getElementById(\"syncDismissBtn\");\n"
"\n"
"  var imageBase64     = null;\n"
"  var imageMimeType   = \"image/jpeg\";\n"
"  var pendingPoints   = null;\n"
"\n"
"  /* Drop zone interactions */\n"
"  dropZone.addEventListener(\"click\", function() { photoFileInput.click(); });\n"
"  dropZone.addEventListener(\"dragover\", function(e) {\n"
"    e.preventDefault();\n"
"    dropZone.classList.add(\"dragover\");\n"
"  });\n"
"  dropZone.addEventListener(\"dragleave\", function() { dropZone.classList.remove(\"dragover\"); });\n"
"  dropZone.addEventListener(\"drop\", function(e) {\n"
"    e.preventDefault();\n"
"    dropZone.classList.remove(\"dragover\");\n"
"    var file = e.dataTransfer.files[0];\n"
"    if (file) loadPhotoFile(file);\n"
"  });\n"
"  photoFileInput.addEventListener(\"change\", function() {\n"
"    if (photoFileInput.files[0]) loadPhotoFile(photoFileInput.files[0]);\n"
"  });\n"
"\n"
"  function loadPhotoFile(file) {\n"
"    imageMimeType = file.type || \"image/jpeg\";\n"
"    var reader = new FileReader();\n"
"    reader.onload = function(e) {\n"
"      var dataUrl = e.target.result;\n"
"      imageBase64 = dataUrl.split(\",\")[1];\n"
"      photoPreviewImg.src = dataUrl;\n"
"      photoPreviewWrap.style.display = \"block\";\n"
"      analyzeBtn.disabled = false;\n"
"      analysisResult.style.display = \"none\";\n"
"      pendingPoints = null;\n"
"    };\n"
"    reader.readAsDataURL(file);\n"
"  }\n"
"\n"
"  photoRemoveBtn.addEventListener(\"click\", function() {\n"
"    imageBase64 = null;\n"
"    pendingPoints = null;\n"
"    photoPreviewImg.src = \"\";\n"
"    photoPreviewWrap.style.display = \"none\";\n"
"    analyzeBtn.disabled = true;\n"
"    analyzeBtn.innerHTML = '<span style=\"font-size:16px;\">🔬</span> Analyze Plant';\n"
"    analysisResult.style.display = \"none\";\n"
"    photoFileInput.value = \"\";\n"
"  });\n"
"\n"
"  analyzeBtn.addEventListener(\"click\", async function() {\n"
"    if (!imageBase64) return;\n"
"    analyzeBtn.disabled = true;\n"
"    analysisResult.style.display = \"none\";\n"
"\n"
"    if (!PROXY_URL) {\n"
"      analyzeBtn.innerHTML = '<span class=\"analysis-spinner\"></span> Finding server…';\n"
"      const found = await findServer();\n"
"      if (!found) {\n"
"        analyzeBtn.disabled = false;\n"
"        analyzeBtn.innerHTML = '<span style=\"font-size:16px;\">🔬</span> Analyze Plant';\n"
"        alert(\"Could not find server.py on the network. Make sure it is running on your laptop.\");\n"
"        return;\n"
"      }\n"
"    }\n"
"\n"
"    analyzeBtn.innerHTML = '<span class=\"analysis-spinner\"></span> Analyzing…';\n"
"    analysisResult.style.display = \"none\";\n"
"\n"
"    /* Send image + crop context to the local Flask proxy (server.py).\n"
"       The proxy holds the API key and forwards the request to Anthropic,\n"
"       avoiding the CORS block browsers enforce on direct API calls. */\n"
"    var stageNames = currentCrop.stages.map(function(s){ return s.name; }).join(\", \");\n"
"\n"
"    fetch(PROXY_URL + \"/analyze\", {\n"
"      method: \"POST\",\n"
"      headers: { \"Content-Type\": \"application/json\" },\n"
"      body: JSON.stringify({\n"
"        image_base64: imageBase64,\n"
"        mime_type:    imageMimeType,\n"
"        crop_name:    currentCrop.name,\n"
"        stage_names:  stageNames\n"
"      })\n"
"    })\n"
"    .then(function(res) { return res.json(); })\n"
"    .then(function(data) {\n"
"      if (!data.ok) throw new Error(data.error || \"Proxy error\");\n"
"      var result = data.result;\n"
"\n"
"      pendingPoints = Math.max(0, Math.min(1000, Math.round(result.growthPoints)));\n"
"\n"
"      /* Populate result card */\n"
"      analysisCropIcon.textContent = currentCrop.emoji;\n"
"      analysisTitle.textContent    = result.crop + \" — \" + result.stage;\n"
"      analysisSummary.textContent  = result.healthSummary;\n"
"\n"
"      var confCls = result.confidence === \"High\" ? \"conf-high\"\n"
"                  : result.confidence === \"Medium\" ? \"conf-medium\" : \"conf-low\";\n"
"      analysisConf.textContent = result.confidence + \" confidence\";\n"
"      analysisConf.className   = \"analysis-confidence \" + confCls;\n"
"\n"
"      var pct = Math.round((pendingPoints / 1000) * 100);\n"
"      analysisBarFill.style.width = pct + \"%\";\n"
"      analysisPct.textContent     = pct + \"%\";\n"
"\n"
"      /* Notes + warnings */\n"
"      analysisNotes.innerHTML = \"\";\n"
"      (result.notes || []).forEach(function(n) {\n"
"        var li = document.createElement(\"div\");\n"
"        li.className = \"analysis-note\";\n"
"        li.innerHTML = '<span class=\"analysis-note-dot\"></span>' + n;\n"
"        analysisNotes.appendChild(li);\n"
"      });\n"
"      (result.warnings || []).forEach(function(w) {\n"
"        var li = document.createElement(\"div\");\n"
"        li.className = \"analysis-note\";\n"
"        li.innerHTML = '<span class=\"analysis-note-dot warn\"></span>' + w;\n"
"        analysisNotes.appendChild(li);\n"
"      });\n"
"\n"
"      analysisResult.style.display = \"block\";\n"
"    })\n"
"    .catch(function(err) {\n"
"      console.error(\"Analysis error:\", err);\n"
"      alert(\"Analysis failed. Please check your connection or API key configuration.\");\n"
"    })\n"
"    .finally(function() {\n"
"      analyzeBtn.disabled = false;\n"
"      analyzeBtn.innerHTML = '<span style=\"font-size:16px;\">🔬</span> Analyze Plant';\n"
"    });\n"
"  });\n"
"\n"
"  /* Apply detected stage to growth meter */\n"
"  syncApplyBtn.addEventListener(\"click\", function() {\n"
"    if (pendingPoints === null) return;\n"
"    var detectedStage = getStageIndex(pendingPoints);\n"
"    growth.points   = pendingPoints;\n"
"    growth.stage    = Math.max(growth.stage, detectedStage);\n"
"    growth.lastTick = Date.now();\n"
"    saveGrowth(growth);\n"
"    drawPlantSVG(growth.stage, currentMood);\n"
"    renderGrowthUI(false);\n"
"\n"
"    /* Visual feedback */\n"
"    syncApplyBtn.textContent = \"✅ Applied!\";\n"
"    syncApplyBtn.style.background = \"#40916c\";\n"
"    setTimeout(function() {\n"
"      syncApplyBtn.textContent = \"Apply to Growth Meter\";\n"
"      syncApplyBtn.style.background = \"\";\n"
"    }, 2000);\n"
"\n"
"    /* Show level-up toast if stage was bumped */\n"
"    var stage = currentCrop.stages[growth.stage];\n"
"    document.getElementById(\"levelupEmoji\").textContent = stage.emoji;\n"
"    document.getElementById(\"levelupText\").textContent  = \"Photo synced: \" + stage.name;\n"
"    var toast = document.getElementById(\"levelupToast\");\n"
"    toast.classList.add(\"show\");\n"
"    setTimeout(function(){ toast.classList.remove(\"show\"); }, 3000);\n"
"  });\n"
"\n"
"  syncDismissBtn.addEventListener(\"click\", function() {\n"
"    analysisResult.style.display = \"none\";\n"
"    pendingPoints = null;\n"
"  });\n"
"})();\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   EVENT LISTENERS\n"
"══════════════════════════════════════════════ */\n"
"document.getElementById(\"alertClose\").addEventListener(\"click\", function() {\n"
"  document.getElementById(\"alertBanner\").style.display = \"none\";\n"
"});\n"
"document.getElementById(\"openCropModal\").addEventListener(\"click\", function() {\n"
"  buildCropModal();\n"
"  document.getElementById(\"cropModal\").classList.add(\"open\");\n"
"});\n"
"document.getElementById(\"modalClose\").addEventListener(\"click\", function() {\n"
"  applySelectedCrop();\n"
"  document.getElementById(\"cropModal\").classList.remove(\"open\");\n"
"});\n"
"document.getElementById(\"cropModal\").addEventListener(\"click\", function(e) {\n"
"  if (e.target === this) { applySelectedCrop(); this.classList.remove(\"open\"); }\n"
"});\n"
"document.getElementById(\"resetGrowthBtn\").addEventListener(\"click\", function() {\n"
"  document.getElementById(\"resetConfirm\").style.display = \"block\";\n"
"});\n"
"document.getElementById(\"confirmResetNo\").addEventListener(\"click\", function() {\n"
"  document.getElementById(\"resetConfirm\").style.display = \"none\";\n"
"});\n"
"document.getElementById(\"confirmResetYes\").addEventListener(\"click\", function() {\n"
"  resetGrowth();\n"
"  document.getElementById(\"resetConfirm\").style.display = \"none\";\n"
"  fetchSensorData();\n"
"});\n"
"document.getElementById(\"harvestBtn\").addEventListener(\"click\", function() {\n"
"  resetGrowth();\n"
"  document.getElementById(\"harvestBtn\").classList.remove(\"visible\");\n"
"  fetchSensorData();\n"
"});\n"
"document.getElementById(\"waterPumpBtn\").addEventListener(\"click\",       function(){ runPump(\"waterPumpBtn\",    \"Add Water\",    \"💧\", \"water\",    \"water\"); });\n"
"document.getElementById(\"removeWaterBtn\").addEventListener(\"click\",  function(){ runPump(\"removeWaterBtn\", \"Remove Water\", \"🚰\", \"water\",    \"drain\"); });\n"
"document.getElementById(\"nutrientPumpBtn\").addEventListener(\"click\", function(){ runPump(\"nutrientPumpBtn\", \"Feed Plant\",   \"🧪\", \"nutrient\", \"food\"); });\n"
"document.getElementById(\"phDownBtn\").addEventListener(\"click\",       function(){ runPump(\"phDownBtn\",      \"pH Down\",      \"⬇️\", \"ph\",       \"phdown\"); });\n"
"document.getElementById(\"phUpBtn\").addEventListener(\"click\",         function(){ runPump(\"phUpBtn\",        \"pH Up\",        \"⬆️\", \"ph\",       \"phup\"); });\n"
"\n"
"(function() {\n"
"  var devOpen = false;\n"
"  document.getElementById(\"devToggle\").addEventListener(\"click\", function() {\n"
"    devOpen = !devOpen;\n"
"    document.getElementById(\"devPanel\").className = \"dev-panel\" + (devOpen?\" open\":\"\");\n"
"  });\n"
"  function devRender() {\n"
"    growth.lastTick = Date.now();\n"
"    renderDashboard(\n"
"      parseFloat(document.getElementById(\"devWater\").value),\n"
"      parseFloat(document.getElementById(\"devTds\").value),\n"
"      parseFloat(document.getElementById(\"devPh\").value)\n"
"    );\n"
"  }\n"
"  document.getElementById(\"devWater\").addEventListener(\"input\", function() {\n"
"    document.getElementById(\"devWaterVal\").textContent = this.value + \"%\"; devRender();\n"
"  });\n"
"  document.getElementById(\"devTds\").addEventListener(\"input\", function() {\n"
"    document.getElementById(\"devTdsVal\").textContent = this.value + \" ppm\"; devRender();\n"
"  });\n"
"  document.getElementById(\"devPh\").addEventListener(\"input\", function() {\n"
"    document.getElementById(\"devPhVal\").textContent = parseFloat(this.value).toFixed(1); devRender();\n"
"  });\n"
"})();\n"
"\n"
"/* ══════════════════════════════════════════════\n"
"   INIT\n"
"══════════════════════════════════════════════ */\n"
"updateCropUI();\n"
"if (DEV.showPreviewBar) document.getElementById(\"previewBar\").removeAttribute(\"hidden\");\n"
"else document.getElementById(\"previewBar\").setAttribute(\"hidden\",\"\");\n"
"\n"
"if (DEV.enabled) renderDashboard(DEV.waterLevel, DEV.tdsValue, DEV.phValue);\n"
"else             renderDashboard(DEMO.waterLevel, DEMO.tdsValue, DEMO.phValue);\n"
"\n"
"fetchSensorData();\n"
"setInterval(fetchSensorData, POLL_INTERVAL_MS);\n"
"</script>\n"
"</body>\n"
"</html>\n";

static const char *TAG = "PLANTAGOCHI";

/* Pump jobs are queued and processed by a worker task so HTTP handlers
   never block waiting for a relay to finish. */
typedef struct {
    gpio_num_t pin;
    int32_t duration_ms;
    char action[12];
} pump_job_t;

static QueueHandle_t pump_queue = NULL;

static void trigger_pump(gpio_num_t pin, int duration_ms);
static void motor_run_sequence(void);

static void pump_worker(void *arg)
{
    pump_job_t job;
    for (;;) {
        if (xQueueReceive(pump_queue, &job, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Pump worker: action=%s pin=%d duration=%dms",
                     job.action, job.pin, job.duration_ms);

            trigger_pump(job.pin, job.duration_ms);

            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ----------------------------------------------------------------
   WI-FI ACCESS POINT
   ---------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Device connected    MAC: %02x:%02x:%02x:%02x:%02x:%02x  AID: %d",
                 e->mac[0], e->mac[1], e->mac[2],
                 e->mac[3], e->mac[4], e->mac[5], e->aid);

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Device disconnected MAC: %02x:%02x:%02x:%02x:%02x:%02x  AID: %d",
                 e->mac[0], e->mac[1], e->mac[2],
                 e->mac[3], e->mac[4], e->mac[5], e->aid);
    }
}

/* Starts the ESP32 as a Wi-Fi AP. Reachable at http://192.168.4.1 */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t ap_config = {
        .ap = {
            .ssid           = WIFI_SSID,
            .ssid_len       = strlen(WIFI_SSID),
            .password       = WIFI_PASSWORD,
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (strlen(WIFI_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started — SSID: \"%s\"", WIFI_SSID);
    ESP_LOGI(TAG, "Connect your device to that network,");
    ESP_LOGI(TAG, "then open http://192.168.4.1 in your browser.");
}

/* ----------------------------------------------------------------
   ADC — water level (GPIO 34) and pH (GPIO 32)
   TDS uses ADS1115 via I2C, not the internal ADC.
   ---------------------------------------------------------------- */
static adc_oneshot_unit_handle_t adc_handle;

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    /* 12-bit, 11 dB attenuation → ~0–3.1 V input range */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, PIN_WATER, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, PIN_PH,    &chan_cfg));

    ESP_LOGI(TAG, "ADC1 configured — channels: water(GPIO34), pH(GPIO32)");
}

/* ----------------------------------------------------------------
   I2C MASTER — SDA GPIO 21, SCL GPIO 22
   ---------------------------------------------------------------- */
static void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_MASTER_SDA_IO,
        .scl_io_num       = I2C_MASTER_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_PORT, conf.mode, 0, 0, 0));
    ESP_LOGI(TAG, "I2C master ready — SDA:GPIO%d SCL:GPIO%d @ %d Hz",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ);
}

/* ----------------------------------------------------------------
   ADS1115 — single-shot read on channel A0
   Returns raw signed 16-bit value, or 0 on I2C error.
   ---------------------------------------------------------------- */
static int16_t ads1115_read_a0(void)
{
    uint8_t write_buf[3] = {
        ADS1115_REG_CONFIG,
        ADS_CFG_HI,
        ADS_CFG_LO,
    };
    esp_err_t err = i2c_master_write_to_device(
        I2C_MASTER_PORT, ADS1115_ADDR,
        write_buf, sizeof(write_buf),
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 config write failed: %s", esp_err_to_name(err));
        return 0;
    }

    vTaskDelay(pdMS_TO_TICKS(10)); /* wait for conversion at 128 SPS */

    uint8_t reg_ptr = ADS1115_REG_CONVERT;
    err = i2c_master_write_to_device(
        I2C_MASTER_PORT, ADS1115_ADDR,
        &reg_ptr, 1,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 reg pointer write failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint8_t data[2] = {0};
    err = i2c_master_read_from_device(
        I2C_MASTER_PORT, ADS1115_ADDR,
        data, sizeof(data),
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 read failed: %s", esp_err_to_name(err));
        return 0;
    }

    return (int16_t)((data[0] << 8) | data[1]);
}

/* ----------------------------------------------------------------
   SENSOR READING FUNCTIONS
   ---------------------------------------------------------------- */

/* Returns water level as a percentage (0–100%).
   Averages 10 ADC samples; maps raw 0–4095 to 0–100%.
   Simulated: ~72% ±3%. */
static float read_water_level(void)
{
#if SIMULATE_SENSORS
    return 72.0f + ((rand() % 600 - 300) / 100.0f);
#else
    long acc = 0;
    for (int i = 0; i < 10; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc_handle, PIN_WATER, &raw) != ESP_OK) {
            ESP_LOGW(TAG, "Water ADC read failed on sample %d", i);
            raw = 0;
        }
        acc += raw;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int avg = (int)(acc / 10);
    float pct = (avg / 4095.0f) * 100.0f;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
#endif
}

/* Returns TDS in ppm (0–2000).
   Averages 32 ADS1115 samples, then applies:
     EC (mS/cm) = 133.42·V³ − 255.86·V² + 857.39·V
     EC25 = EC / (1 + 0.02·(T − 25))   [temperature compensated]
     TDS = EC25 × K_CELL × TDS_FACTOR × 1000
   Simulated: ~700 ppm ±50. */
static float read_tds(void)
{
#if SIMULATE_SENSORS
    return 700.0f + ((rand() % 1000 - 500) / 10.0f);
#else
    long acc = 0;
    for (uint16_t i = 0; i < TDS_NUM_SAMPLES; i++) {
        int16_t raw = ads1115_read_a0();
        acc += raw;
        vTaskDelay(pdMS_TO_TICKS(10)); /* wait ≥8 ms for 128 SPS conversion */
    }
    float avg_raw = (float)acc / TDS_NUM_SAMPLES;

    float v = avg_raw * ADS1115_LSB_VOLTS;
    if (v < 0.0f) v = 0.0f;
    if (v > 3.3f) v = 3.3f;

    float ec = (133.42f * v * v * v)
             - (255.86f * v * v)
             + (857.39f * v);
    ec *= TDS_K_CELL;

    float ec25 = ec / (1.0f + 0.02f * (TDS_WATER_TEMP_C - 25.0f));
    float tds  = ec25 * TDS_FACTOR * 1000.0f;

    if (tds < 0.0f)        tds = 0.0f;
    if (tds > TDS_MAX_PPM) tds = TDS_MAX_PPM;

    return tds;
#endif
}

/* Returns pH value (0–14).
   Collects 10 ADC samples, bubble-sorts, averages the 6 middle values,
   then converts: pH = (−5.70 × volt) + 22.84
   Simulated: ~6.1 ±0.3. */
static float read_ph(void)
{
#if SIMULATE_SENSORS
    return 6.1f + ((rand() % 60 - 30) / 100.0f);
#else
    int buf[10];
    for (int i = 0; i < 10; i++) {
        buf[i] = 0;
        if (adc_oneshot_read(adc_handle, PIN_PH, &buf[i]) != ESP_OK) {
            ESP_LOGW(TAG, "pH ADC read failed on sample %d", i);
            buf[i] = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    /* Bubble sort ascending */
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 10; j++) {
            if (buf[i] > buf[j]) {
                int tmp = buf[i];
                buf[i]  = buf[j];
                buf[j]  = tmp;
            }
        }
    }

    /* Average middle 6 samples, discard 2 highest and 2 lowest */
    long sum = 0;
    for (int i = 2; i < 8; i++) sum += buf[i];
    float avg_adc = (float)sum / 6.0f;

    float volt = avg_adc * (3.3f / 4095.0f);
    float ph   = (-5.70f * volt) + 22.84f;

    if (ph < 0.0f)  ph = 0.0f;
    if (ph > 14.0f) ph = 14.0f;

    return ph;
#endif
}

/* ----------------------------------------------------------------
   PUMP CONTROL
   ---------------------------------------------------------------- */

/* Pulses a relay pin HIGH for duration_ms, then LOW. */
static void trigger_pump(gpio_num_t pin, int duration_ms)
{
    ESP_LOGI(TAG, "Pump ON → GPIO %d for %d ms", pin, duration_ms);
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(pin, 0);
    ESP_LOGI(TAG, "Pump OFF → GPIO %d", pin);
}

/* ----------------------------------------------------------------
   DC MOTOR CONTROL HELPERS (L298N on GPIO 27/33/14)
   NOTE: These are retained for future use if a real L298N is wired.
   Currently all pH actions use trigger_pump() via the queue, not
   these helpers. To use them, dispatch motor_run_sequence() from
   pump_worker instead of trigger_pump() for the phdown/phup actions.
   ---------------------------------------------------------------- */

static void motor_set_speed(uint32_t duty)
{
    if (duty > 255) duty = 255;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_PWM_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_PWM_CHANNEL);
    ESP_LOGI(TAG, "Motor speed duty=%"PRIu32, duty);
}

/* IN1=LOW, IN2=HIGH → forward */
static void motor_forward(uint32_t duty)
{
    gpio_set_level(MOTOR_PIN_IN1, 0);
    gpio_set_level(MOTOR_PIN_IN2, 1);
    motor_set_speed(duty);
    ESP_LOGI(TAG, "Motor FORWARD duty=%"PRIu32, duty);
}

/* IN1=HIGH, IN2=LOW → backward */
static void motor_backward(uint32_t duty)
{
    gpio_set_level(MOTOR_PIN_IN1, 1);
    gpio_set_level(MOTOR_PIN_IN2, 0);
    motor_set_speed(duty);
    ESP_LOGI(TAG, "Motor BACKWARD duty=%"PRIu32, duty);
}

/* IN1=LOW, IN2=LOW → coast stop */
static void motor_stop(void)
{
    gpio_set_level(MOTOR_PIN_IN1, 0);
    gpio_set_level(MOTOR_PIN_IN2, 0);
    motor_set_speed(0);
    ESP_LOGI(TAG, "Motor STOPPED");
}

/*
 * motor_run_sequence()
 * Called when Fix pH button is pressed.
 *
 * Currently: holds GPIO 27 HIGH for PH_DOWN_DURATION_MS then LOW.
 * Simple and controllable — good for LED testing.
 *
 * When the real L298N motor is connected, replace the body with:
 *   motor_forward(255);  vTaskDelay(pdMS_TO_TICKS(2000));
 *   motor_stop();        vTaskDelay(pdMS_TO_TICKS(1000));
 *   motor_backward(255); vTaskDelay(pdMS_TO_TICKS(2000));
 *   motor_stop();
 */
static void motor_run_sequence(void)
{
    ESP_LOGI(TAG, "pH: GPIO 27 HIGH for %d ms", PH_DOWN_DURATION_MS);
    gpio_set_level(MOTOR_PIN_IN1, 1);           /* GPIO 27 → 3.3V, LED ON  */
    vTaskDelay(pdMS_TO_TICKS(PH_DOWN_DURATION_MS));
    gpio_set_level(MOTOR_PIN_IN1, 0);           /* GPIO 27 → 0V,   LED OFF */
    ESP_LOGI(TAG, "pH: GPIO 27 LOW");
}

/* Configures LEDC PWM on GPIO 14 (ENA) only.
   GPIO 27 (IN1/PIN_PH_DOWN) and GPIO 33 (IN2/PIN_PH_UP) are configured
   as outputs by gpio_init_pumps() — no need to repeat that here. */
static void motor_init(void)
{
    /* Drive-strength boost is done in gpio_init_pumps; skip gpio_config here. */

    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = MOTOR_PWM_RES,
        .timer_num       = MOTOR_PWM_TIMER,
        .freq_hz         = MOTOR_PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t ch_conf = {
        .gpio_num   = MOTOR_PIN_EN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = MOTOR_PWM_CHANNEL,
        .timer_sel  = MOTOR_PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));

    ESP_LOGI(TAG, "Motor init OK — IN1:GPIO%d IN2:GPIO%d EN:GPIO%d (PWM %d Hz)",
             MOTOR_PIN_IN1, MOTOR_PIN_IN2, MOTOR_PIN_EN, MOTOR_PWM_FREQ);
}

/* ----------------------------------------------------------------
   HTTP HELPERS
   ---------------------------------------------------------------- */

/* Adds CORS headers so the browser can POST to the Flask server
   (different origin) while the page is served from the ESP32. */
static esp_err_t add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

/* ----------------------------------------------------------------
   HTTP ROUTE HANDLERS
   ---------------------------------------------------------------- */

/* GET / — serves the HTML dashboard */
static esp_err_t handler_root(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, sizeof(HTML_PAGE) - 1);
    return ESP_OK;
}

/* GET /data — returns sensor readings as JSON */
static esp_err_t handler_data(httpd_req_t *req)
{
    float water = read_water_level();
    float food  = read_tds();
    float ph    = read_ph();

    char json[128];
    snprintf(json, sizeof(json),
             "{\"water\":%.1f,\"food\":%.1f,\"ph\":%.2f}",
             water, food, ph);

    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));

    ESP_LOGI(TAG, "/data → water:%.1f%% food:%.1fppm ph:%.2f",
             water, food, ph);
    return ESP_OK;
}

/* GET /action?action=water|drain|food|phdown|phup — enqueues an output job.
   Returns {"ok":true,"duration":<ms>} so the HTML button stays in
   "Running..." for exactly as long as the hardware actually runs. */
static esp_err_t handler_action(httpd_req_t *req)
{
    char param[32]  = {0};
    char action[16] = {0};
    int32_t duration_to_report = 0;

    if (httpd_req_get_url_query_str(req, param, sizeof(param)) == ESP_OK) {
        if (httpd_query_key_value(param, "action", action, sizeof(action)) == ESP_OK) {
            pump_job_t job;
            if (strcmp(action, "water") == 0) {
                job.pin = PIN_PUMP_WATER; job.duration_ms = WATER_ON_DURATION_MS;
                snprintf(job.action, sizeof(job.action), "%s", "water");
                duration_to_report = WATER_ON_DURATION_MS;
            } else if (strcmp(action, "drain") == 0) {
                job.pin = PIN_PUMP_DRAIN; job.duration_ms = DRAIN_ON_DURATION_MS;
                snprintf(job.action, sizeof(job.action), "%s", "drain");
                duration_to_report = DRAIN_ON_DURATION_MS;
            } else if (strcmp(action, "food") == 0) {
                job.pin = PIN_PUMP_NUTRIENT; job.duration_ms = FOOD_ON_DURATION_MS;
                snprintf(job.action, sizeof(job.action), "%s", "food");
                duration_to_report = FOOD_ON_DURATION_MS;
            } else if (strcmp(action, "phdown") == 0) {
                job.pin = PIN_PH_DOWN; job.duration_ms = PH_DOWN_DURATION_MS;
                snprintf(job.action, sizeof(job.action), "%s", "phdown");
                duration_to_report = PH_DOWN_DURATION_MS;
            } else if (strcmp(action, "phup") == 0) {
                job.pin = PIN_PH_UP; job.duration_ms = PH_UP_DURATION_MS;
                snprintf(job.action, sizeof(job.action), "%s", "phup");
                duration_to_report = PH_UP_DURATION_MS;
            } else {
                ESP_LOGW(TAG, "Unknown action: %s", action);
                add_cors_headers(req);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"ok\":false,\"error\":\"unknown_action\"}", strlen("{\"ok\":false,\"error\":\"unknown_action\"}"));
                return ESP_OK;
            }

            if (pump_queue == NULL || xQueueSend(pump_queue, &job, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Pump queue full \xe2\x80\x94 action=%s", job.action);
                add_cors_headers(req);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"ok\":false,\"error\":\"queue_full\"}", strlen("{\"ok\":false,\"error\":\"queue_full\"}"));
                return ESP_OK;
            }
            ESP_LOGI(TAG, "Enqueued action=%s duration=%" PRId32 "ms", job.action, duration_to_report);
        } else {
            /* Query string present but no "action" key */
            ESP_LOGW(TAG, "/action called with no 'action' parameter");
            add_cors_headers(req);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing_action\"}", strlen("{\"ok\":false,\"error\":\"missing_action\"}"));
            return ESP_OK;
        }
    } else {
        /* No query string at all */
        ESP_LOGW(TAG, "/action called with no query string");
        add_cors_headers(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing_action\"}", strlen("{\"ok\":false,\"error\":\"missing_action\"}"));
        return ESP_OK;
    }

    /* Send duration back so the HTML button syncs its Running... timer */
    char resp_json[64];
    snprintf(resp_json, sizeof(resp_json), "{\"ok\":true,\"duration\":%" PRId32 "}", duration_to_report);
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_json, strlen(resp_json));
    return ESP_OK;
}

/* OPTIONS /analyze — handles CORS preflight for image POST to Flask */
static esp_err_t handler_options(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ----------------------------------------------------------------
   HTTP SERVER STARTUP
   ---------------------------------------------------------------- */
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    config.max_uri_handlers = 8;
    config.stack_size      = 16384;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t uri_root = { .uri = "/",       .method = HTTP_GET,     .handler = handler_root    };
    httpd_uri_t uri_data = { .uri = "/data",    .method = HTTP_GET,     .handler = handler_data    };
    httpd_uri_t uri_act  = { .uri = "/action",  .method = HTTP_GET,     .handler = handler_action  };
    httpd_uri_t uri_opts = { .uri = "/analyze", .method = HTTP_OPTIONS, .handler = handler_options };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_data);
    httpd_register_uri_handler(server, &uri_act);
    httpd_register_uri_handler(server, &uri_opts);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return server;
}

/* ----------------------------------------------------------------
   GPIO SETUP — PUMP RELAYS
   ---------------------------------------------------------------- */
static void gpio_init_pumps(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_PUMP_WATER) |
                        (1ULL << PIN_PUMP_DRAIN) |
                        (1ULL << PIN_PUMP_NUTRIENT) |
                        (1ULL << PIN_PH_DOWN) |
                        (1ULL << PIN_PH_UP),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Max drive strength — ensures GPIO 25 and 26 reach full 3.3V output */
    gpio_set_drive_capability(PIN_PUMP_WATER,    GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_PUMP_DRAIN,    GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_PUMP_NUTRIENT, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_PH_DOWN,       GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_PH_UP,         GPIO_DRIVE_CAP_3);

    gpio_set_level(PIN_PUMP_WATER,    0);
    gpio_set_level(PIN_PUMP_DRAIN,    0);
    gpio_set_level(PIN_PUMP_NUTRIENT, 0);
    gpio_set_level(PIN_PH_DOWN,       0);
    gpio_set_level(PIN_PH_UP,         0);

    ESP_LOGI(TAG, "Output GPIOs configured — water:25 drain:26 nutrient:4 pH-down:27 pH-up:33, all OFF");
}

/* ================================================================
   TEST — GPIO 27 OUTPUT VERIFICATION
   ════════════════════════════════════════════════════════════════
   Enable by setting  #define TEST_GPIO27 1  near the top.
   Set back to 0 before deploying — this replaces app_main entirely
   and the normal firmware will NOT run while the test is active.

   What to expect (repeating forever):
     Phase 1 — SLOW BLINK x5  : 500 ms ON / 500 ms OFF
     Phase 2 — HOLD HIGH 3 s  : LED stays solid ON
     Phase 3 — HOLD LOW  2 s  : LED stays OFF
     Phase 4 — FAST BLINK x10 : 100 ms ON / 100 ms OFF

   If the LED does not light during Phase 2 (HOLD HIGH):
     - Flip the LED — long leg to resistor, short leg to GND
     - Confirm the 330Ohm is between GPIO 27 and the LED (+)
     - Check you actually flashed with TEST_GPIO27 1
   ================================================================ */
#if TEST_GPIO27

static void _test_pin_set(int level)
{
    gpio_set_level(MOTOR_PIN_IN1, level);
    ESP_LOGI(TAG, "[TEST] GPIO %d -> %s  (%s)",
             MOTOR_PIN_IN1,
             level ? "HIGH (3.3V)" : "LOW  (0V)",
             level ? "LED ON" : "LED OFF");
}

static void _test_blink(int count, int on_ms, int off_ms)
{
    ESP_LOGI(TAG, "[TEST] BLINK x%d  (%d ms ON / %d ms OFF)", count, on_ms, off_ms);
    for (int i = 0; i < count; i++) {
        _test_pin_set(1);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        _test_pin_set(0);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

static void _test_hold(int level, int duration_ms)
{
    ESP_LOGI(TAG, "[TEST] HOLD %s for %d ms", level ? "HIGH" : "LOW", duration_ms);
    _test_pin_set(level);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " GPIO 27 OUTPUT TEST                    ");
    ESP_LOGI(TAG, " Normal firmware is disabled.           ");
    ESP_LOGI(TAG, " Circuit: GPIO27 -> 330Ohm -> LED -> GND");
    ESP_LOGI(TAG, "========================================");

    /* Configure GPIO 27 (MOTOR_PIN_IN1) as output, start LOW */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << MOTOR_PIN_IN1),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(MOTOR_PIN_IN1, 0);
    ESP_LOGI(TAG, "[TEST] GPIO %d configured as output, starting LOW", MOTOR_PIN_IN1);

    for (;;) {
        ESP_LOGI(TAG, "[TEST] === Cycle start ===");
        _test_blink(5, 500, 500);   /* Phase 1: slow blink  — confirms toggle    */
        _test_hold(1, 3000);        /* Phase 2: hold HIGH   — confirms sustained */
        _test_hold(0, 2000);        /* Phase 3: hold LOW    — confirms clean GND */
        _test_blink(10, 100, 100);  /* Phase 4: fast blink  — confirms switching */
        ESP_LOGI(TAG, "[TEST] === Cycle done, restarting in 1 s ===");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ================================================================
   END TEST BLOCK — set TEST_GPIO27 0 to restore normal firmware
   ================================================================ */
#else  /* normal Plantagochi firmware */

/* ----------------------------------------------------------------
   APP MAIN
   ---------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, " Plantagochi ESP32-WROOM-32 v2.0   ");
    ESP_LOGI(TAG, " Framework : ESP-IDF               ");
    ESP_LOGI(TAG, " SIMULATE  : %s",
             SIMULATE_SENSORS ? "ON (fake data)" : "OFF (real sensors)");
    ESP_LOGI(TAG, "===================================");

    /* NVS — required by Wi-Fi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_init_pumps();
    motor_init();

    pump_queue = xQueueCreate(8, sizeof(pump_job_t));
    if (pump_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create pump queue");
    } else {
        BaseType_t ok = xTaskCreate(pump_worker, "pump_worker", 2048, NULL, 5, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to start pump worker task");
        } else {
            ESP_LOGI(TAG, "Pump worker started (queue len=8)");
        }
    }

    adc_init();
    i2c_master_init();

    ESP_LOGI(TAG, "Starting Wi-Fi AP: %s", WIFI_SSID);
    wifi_init();
    start_webserver();

    ESP_LOGI(TAG, "System ready.");
    ESP_LOGI(TAG, "Connect to Wi-Fi \"%s\", then open http://192.168.4.1", WIFI_SSID);
    ESP_LOGI(TAG, "For Flask image analysis: connect laptop to same AP,");
    ESP_LOGI(TAG, "update LAPTOP_IP to its 192.168.4.x address, rebuild.");
}

#endif /* TEST_GPIO27 */