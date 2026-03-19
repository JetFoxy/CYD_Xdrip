/*  ESP32-2432S028 (Cheap Yellow Display) glucose monitor — CYDDrip
    Based on M5Stack Nightscout monitor by Johan Degraeve.
    Adapted for ESP32-2432S028 (CYD) with ILI9341 2.8" display.

    Receives BG readings from CYDDrip Android app via BLE.
    Protocol: CYDDrip BLE protocol (opcodes 0x09/0x0A/0x20/0x21).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Original code: Martin Lukasek <martin@lukasek.cz>
    CYD port + WatchDrip protocol: adapted from M5StickCPlus2 version
*/

// ESP32-2432S028: backlight on GPIO 21, active HIGH
#define TFT_BL_PIN      21
#define BOOT_BTN_PIN     0   // BOOT button — cycle brightness (fallback)
#define BTN_BRIGHTNESS  27   // P3 pin 1 — cycle brightness
#define BTN_ALARM_RESET 22   // P3 pin 2 — silence alarm
#define SPEAKER_PIN     26   // CN1 speaker via on-board amplifier

#include <TFT_eSPI.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include "time.h"
#include "Free_Fonts.h"
#include "CYDconfig.h"
#include "WebConfig.h"
#include <cstring>
#include <string>

// ESP32-2432S028: SD card on VSPI (pins 18/19/23), CS = GPIO 5
#define SD_CS_PIN 5

// WiFi + Nightscout
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// BLE
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

// ---- Display ----
TFT_eSPI tft = TFT_eSPI();

const bool debugLogging = true;

Preferences preferences;
tConfig cfg;

#ifndef min
  #define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

// Display colours — hardware double-inversion: XOR 0xFFFF before writing
static const bool  displayColorsInverted = true;
static uint16_t    textColor             = TFT_WHITE;
static uint16_t    backGroundColor       = TFT_BLACK;

// BG range colours (RGB565)
static const uint16_t COLOR_BG_HIGH = 0xFD14;  // salmon/pink
static const uint16_t COLOR_BG_LOW  = 0x8DDF;  // light blue

// BG thresholds — defaults, overridden from CYD.INI after config load
static float BG_LOW       = 3.9f;
static float BG_WARN_LOW  = 4.5f;
static float BG_WARN_HIGH = 9.0f;
static float BG_HIGH      = 10.0f;

// ---- BLE constants ----
// Device name stays "M5Stack" for WatchDrip compatibility
static const char* BLE_DEVICE_NAME       = "M5Stack";
static const char* BLE_SERVICE_UUID      = "AF6E5F78-706A-43FB-B1F4-C27D7D5C762F";
static const char* BLE_CHAR_UUID         = "6D810E9F-0983-4030-BDA7-C7C9A6A19C1C";
static const int   BLE_TX_MAX_BYTES      = 20;   // max notification payload
static const int   BLE_RX_MAX_BYTES      = 128;  // max write payload (0x21 = 3+35×2=73 bytes)

// ---- WiFi state ----
static bool          wifiEnabled   = false;
static bool          ntpSynced     = false;
static unsigned long lastNsFetchMs = 0;
static const unsigned long NS_FETCH_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 min

// ---- State ----
static const int historySize = 36;  // 36 × 5 min = 3 hours
float readingHistory[historySize]  = {0};

unsigned long timeStampLatestBgReadingInSecondsUTC = 0;
unsigned long msCount                              = 0;

// BLE time sync (set from 0x20 packet)
static unsigned long bleTimestampSec    = 0;  // local time at last BLE update
static unsigned long bleTimestampMillis = 0;  // millis() at that moment
static long          utcOffsetSec       = 0;  // UTC offset in seconds

struct NSinfo {
  time_t   sensTime        = 0;
  char     sensDir[32]     = {};
  float    sensSgvMgDl     = 0;
  float    sensSgv         = 0;
  int      arrowAngle      = 180;  // 180 = hidden
  float    iob             = 0;    // predict IoB (units), 0 = not available
  float    lastBolusU      = 0;    // last bolus (units), 0 = not available
  uint8_t  lastBolusAgeMins = 0;   // minutes since last bolus, 0 = unknown
  float    pumpIob         = 0;    // pump bolus IoB (units), 0 = not available
  float    pumpReservoir   = 0;    // pump reservoir (units), 0 = not available
  uint8_t  pumpBattery     = 255;  // pump battery %, 255 = no pump data
  uint8_t  cob             = 0;    // Carbs on Board (grams), 0 = not available
} ns;

BLEServer         *pServer             = nullptr;
BLECharacteristic *pRxTxCharacteristic = nullptr;
bool               bleAuthenticated    = false;
byte               rxBuf[BLE_RX_MAX_BYTES];

// ---- Forward declarations ----
void     setupBLE();
void     updateGlycemia();
void     setNsArrowAngle();
uint16_t toPanelColor(uint16_t color);
uint16_t bgValueColor(float mmol);
void     getDeltaStr(char *buf, int bufSize);
void     pushReadingToHistory(float mmolValue);
void     drawMiniGraph(TFT_eSPI &surface, int16_t x, int16_t y, int16_t w, int16_t h);
void     drawArrow(TFT_eSPI &surface, int x, int y, int asize, int aangle, int pwidth, int plength, uint16_t color);

// ---- Colour helpers ----

uint16_t toPanelColor(uint16_t color) {
  return displayColorsInverted ? uint16_t(color ^ 0xFFFF) : color;
}

uint16_t bgValueColor(float mmol) {
  if (mmol <= 0)           return toPanelColor(textColor);
  if (mmol < BG_LOW)       return toPanelColor(COLOR_BG_LOW);
  if (mmol < BG_WARN_LOW)  return toPanelColor(TFT_YELLOW);
  if (mmol > BG_HIGH)      return toPanelColor(COLOR_BG_HIGH);
  if (mmol > BG_WARN_HIGH) return toPanelColor(TFT_YELLOW);
  return toPanelColor(TFT_GREEN);
}

// ---- Backlight ----

static const uint8_t BL_LEVELS[]  = {40, 130, 220};  // dim / mid / bright
static const int     BL_NUM_LEVELS = 3;
static int           blLevelIdx    = 2;  // start at bright

void initBacklight() {
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL_PIN, 0);
  ledcWrite(0, BL_LEVELS[blLevelIdx]);
  pinMode(BOOT_BTN_PIN,     INPUT_PULLUP);
  pinMode(BTN_BRIGHTNESS,   INPUT_PULLUP);
  pinMode(BTN_ALARM_RESET,  INPUT_PULLUP);
}

void cycleBrightness() {
  blLevelIdx = (blLevelIdx + 1) % BL_NUM_LEVELS;
  ledcWrite(0, BL_LEVELS[blLevelIdx]);
}

void handleBrightnessButton() {
  static bool lastBoot = HIGH;
  static bool lastP3   = HIGH;
  bool b = digitalRead(BOOT_BTN_PIN);
  bool p = digitalRead(BTN_BRIGHTNESS);
  if ((lastBoot == HIGH && b == LOW) || (lastP3 == HIGH && p == LOW))
    cycleBrightness();
  lastBoot = b;
  lastP3   = p;
}

// ---- Alarm ----

enum AlarmLevel { ALARM_NONE, ALARM_WARN, ALARM_URGENT };
static AlarmLevel alarmActive  = ALARM_NONE;
static bool       alarmSilenced = false;

// LEDC channel 1 for speaker (channel 0 used by backlight)
void beep(uint32_t freqHz, uint32_t durationMs) {
  ledcSetup(1, freqHz, 8);
  ledcAttachPin(SPEAKER_PIN, 1);
  ledcWrite(1, 128);
  delay(durationMs);
  ledcWrite(1, 0);
  ledcDetachPin(SPEAKER_PIN);
}

void playAlarm(AlarmLevel level) {
  if (level == ALARM_URGENT) {
    // Three short high beeps
    for (int i = 0; i < 3; i++) { beep(1800, 150); delay(100); }
  } else if (level == ALARM_WARN) {
    // One medium beep
    beep(900, 300);
  }
}

void handleAlarmButton() {
  static bool lastState = HIGH;
  bool state = digitalRead(BTN_ALARM_RESET);
  if (lastState == HIGH && state == LOW) {
    alarmSilenced = true;
    alarmActive   = ALARM_NONE;
  }
  lastState = state;
}

void checkAlarms() {
  if (alarmSilenced) return;
  if (ns.sensSgvMgDl <= 0) return;

  static unsigned long lastAlarmMs = 0;
  // Re-alarm every 5 min
  if (millis() - lastAlarmMs < 5UL * 60UL * 1000UL) return;

  AlarmLevel level = ALARM_NONE;
  if (ns.sensSgv < BG_LOW || ns.sensSgv > BG_HIGH)       level = ALARM_URGENT;
  else if (ns.sensSgv < BG_WARN_LOW || ns.sensSgv > BG_WARN_HIGH) level = ALARM_WARN;

  if (level != ALARM_NONE) {
    alarmActive = level;
    playAlarm(level);
    lastAlarmMs = millis();
  } else {
    alarmActive   = ALARM_NONE;
    alarmSilenced = false;  // reset silence when BG returns to range
  }
}

// ---- BG history ----

void saveHistoryToNVS() {
  preferences.begin("bghistory", false);
  preferences.putBytes("hist36", readingHistory, sizeof(readingHistory));
  preferences.end();
}

void loadHistoryFromNVS() {
  preferences.begin("bghistory", true);
  size_t n = preferences.getBytes("hist36", readingHistory, sizeof(readingHistory));
  preferences.end();
  if (n == sizeof(readingHistory))
    Serial.println(F("BG history restored from NVS"));
}

void pushReadingToHistory(float mmolValue) {
  if (mmolValue <= 0) return;
  for (int i = historySize - 1; i > 0; --i)
    readingHistory[i] = readingHistory[i - 1];
  readingHistory[0] = mmolValue;
  saveHistoryToNVS();
}

// ---- Time helpers ----
// Time is synced from the 0x20 BLE packet; no NTP needed.

unsigned long getLocalTimeInSeconds() {
  // NTP (WiFi) takes priority — most accurate
  if (ntpSynced) {
    struct tm t;
    if (getLocalTime(&t, 100)) return (unsigned long)mktime(&t);
  }
  // Fall back to BLE time sync
  if (bleTimestampSec > 0)
    return bleTimestampSec + (millis() - bleTimestampMillis) / 1000UL;
  return 0;
}

unsigned long getUTCTimeInSeconds() {
  unsigned long local = getLocalTimeInSeconds();
  return (local > 0) ? (unsigned long)((long)local - utcOffsetSec) : 0;
}

// ---- Delta string ----

void getDeltaStr(char *buf, int bufSize) {
  if (readingHistory[0] <= 0 || readingHistory[1] <= 0) { buf[0] = '\0'; return; }
  float d = readingHistory[0] - readingHistory[1];
  if (cfg.show_mgdl == 1) {
    int di = (int)(d * 18.0f + (d >= 0 ? 0.5f : -0.5f));
    snprintf(buf, bufSize, di >= 0 ? "+%d" : "%d", di);
  } else {
    snprintf(buf, bufSize, d >= 0.0f ? "+%.1f" : "%.1f", d);
  }
}

// ---- Graph ----

void drawMiniGraph(TFT_eSPI &surface, int16_t x, int16_t y, int16_t w, int16_t h) {
  const float mmolMin   = 2.5f;
  const float mmolMax   = 15.0f;
  const float mmolRange = mmolMax - mmolMin;

  const uint16_t frameColor  = toPanelColor(TFT_DARKGREY);
  const uint16_t greenColor  = toPanelColor(TFT_GREEN);
  const uint16_t yellowColor = toPanelColor(TFT_YELLOW);
  const uint16_t redColor    = toPanelColor(COLOR_BG_HIGH);
  const uint16_t blueColor   = toPanelColor(COLOR_BG_LOW);

  surface.drawRoundRect(x - 2, y - 2, w + 4, h + 4, 4, frameColor);

  int16_t yLow  = y + h - (int16_t)((BG_LOW  - mmolMin) / mmolRange * h);
  int16_t yHigh = y + h - (int16_t)((BG_HIGH - mmolMin) / mmolRange * h);
  surface.drawFastHLine(x, yLow,  w, toPanelColor(0x001F));  // dim blue
  surface.drawFastHLine(x, yHigh, w, toPanelColor(0xA000));  // dim red

  int16_t spacing = (w - 10) / (historySize - 1);
  int16_t dotR    = (spacing >= 8) ? 2 : 1;
  int16_t prevPx  = -1, prevPy = -1;

  for (int i = 0; i < historySize; i++) {
    float glk = readingHistory[historySize - 1 - i];  // oldest→left, newest→right
    if (glk <= 0) { prevPx = -1; continue; }
    glk = constrain(glk, mmolMin, mmolMax);

    uint16_t pc = greenColor;
    if      (glk < BG_LOW)       pc = blueColor;
    else if (glk < BG_WARN_LOW)  pc = yellowColor;
    else if (glk > BG_HIGH)      pc = redColor;
    else if (glk > BG_WARN_HIGH) pc = yellowColor;

    int16_t px = x + 5 + i * spacing;
    int16_t py = y + h - (int16_t)((glk - mmolMin) / mmolRange * h);

    if (prevPx >= 0) surface.drawLine(prevPx, prevPy, px, py, frameColor);
    surface.fillCircle(px, py, dotR, pc);
    prevPx = px; prevPy = py;
  }
}

// ---- Arrow ----

void setNsArrowAngle() {
  ns.arrowAngle = 180;
  if      (strcmp(ns.sensDir, "DoubleDown")    == 0) ns.arrowAngle =  90;
  else if (strcmp(ns.sensDir, "SingleDown")    == 0) ns.arrowAngle =  75;
  else if (strcmp(ns.sensDir, "FortyFiveDown") == 0) ns.arrowAngle =  45;
  else if (strcmp(ns.sensDir, "Flat")          == 0) ns.arrowAngle =   0;
  else if (strcmp(ns.sensDir, "FortyFiveUp")   == 0) ns.arrowAngle = -45;
  else if (strcmp(ns.sensDir, "SingleUp")      == 0) ns.arrowAngle = -75;
  else if (strcmp(ns.sensDir, "DoubleUp")      == 0) ns.arrowAngle = -90;
}

void drawArrow(TFT_eSPI &surface, int x, int y, int asize, int aangle, int pwidth, int plength, uint16_t color) {
  float dx  = (asize - 10) * cos(aangle - 90) * PI / 180 + x;
  float dy  = (asize - 10) * sin(aangle - 90) * PI / 180 + y;
  float angle = aangle * PI / 180 - 135;
  float ca = cos(angle), sa = sin(angle);
  float x1 = 0, y1 = plength;
  float x2 =  pwidth / 2.0f, y2 = pwidth / 2.0f;
  float x3 = -pwidth / 2.0f, y3 = pwidth / 2.0f;
  float xx1 = x1*ca - y1*sa + dx,  yy1 = y1*ca + x1*sa + dy;
  float xx2 = x2*ca - y2*sa + dx,  yy2 = y2*ca + x2*sa + dy;
  float xx3 = x3*ca - y3*sa + dx,  yy3 = y3*ca + x3*sa + dy;
  surface.fillTriangle(xx1, yy1, xx3, yy3, xx2, yy2, color);
  // Draw thick shaft (7px wide)
  for (int d = -3; d <= 3; d++) {
    surface.drawLine(x + d, y,     xx1 + d, yy1,     color);
    surface.drawLine(x,     y + d, xx1,     yy1 + d, color);
  }
}

// ---- Display update ----

void updateGlycemia() {
  const int16_t W = tft.width();   // 320
  const int16_t H = tft.height();  // 240

  // Build BG value string
  char sgvStr[16];
  strlcpy(sgvStr, "---", sizeof(sgvStr));
  unsigned long utcNow = getUTCTimeInSeconds();
  if (utcNow > 0 && ns.sensSgvMgDl > 0 &&
      utcNow <= timeStampLatestBgReadingInSecondsUTC + 5*60 + 10) {
    if (cfg.show_mgdl == 1)
      snprintf(sgvStr, sizeof(sgvStr), ns.sensSgvMgDl < 100 ? "%2.0f" : "%3.0f", ns.sensSgvMgDl);
    else
      snprintf(sgvStr, sizeof(sgvStr), ns.sensSgv < 10 ? "%3.1f" : "%4.1f", ns.sensSgv);
  }

  char deltaStr[12] = {};
  bool hasData = (strcmp(sgvStr, "---") != 0);
  if (hasData) getDeltaStr(deltaStr, sizeof(deltaStr));

  unsigned long minSince = 0;
  if (utcNow > 0 && timeStampLatestBgReadingInSecondsUTC > 0 &&
      utcNow >= timeStampLatestBgReadingInSecondsUTC)
    minSince = (utcNow - timeStampLatestBgReadingInSecondsUTC) / 60;

  if (debugLogging)
    Serial.printf("updateGlycemia: %s delta=%s min=%lu\n", sgvStr, deltaStr, minSince);

  tft.fillScreen(toPanelColor(backGroundColor));

  const uint16_t bgCol  = hasData ? bgValueColor(ns.sensSgv) : toPanelColor(textColor);
  const uint16_t txtCol = toPanelColor(textColor);
  const uint16_t bgFill = toPanelColor(backGroundColor);

  // Row 0 — current time top-left (font2 × size1, ~14px tall)
  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(txtCol, bgFill);
  tft.setTextDatum(TL_DATUM);
  unsigned long localNow = getLocalTimeInSeconds();
  if (localNow > 0) {
    char clockStr[6];
    snprintf(clockStr, sizeof(clockStr), "%02d:%02d",
             (int)((localNow % 86400UL) / 3600),
             (int)((localNow % 3600UL)  / 60));
    tft.drawString(clockStr, 4, 6, 2);
  }

  // Row 0 — large BG value (font4 × size3 = 78px, y=6..84)
  tft.setTextFont(4);
  tft.setTextSize(3);
  tft.setTextColor(bgCol, bgFill);
  tft.drawCentreString(sgvStr, 160, 6, 4);

  // Row 0 — trend arrow (base at x=262, y=48; flat tip at ~x=300)
  if (hasData && ns.arrowAngle != 180)
    drawArrow(tft, 262, 48, 30, ns.arrowAngle + 85, 30, 40, bgCol);

  // Row 1 — delta | units | time-ago (y=98)
  tft.setTextFont(2);
  tft.setTextSize(1);
  if (deltaStr[0]) {
    tft.setTextColor(bgCol, bgFill);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(deltaStr, 8, 98, 2);
  }
  tft.setTextColor(txtCol, bgFill);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(cfg.show_mgdl == 1 ? "mg/dL" : "mmol/L", W / 2, 98, 2);
  if (hasData && minSince > 0) {
    char timeStr[12];
    snprintf(timeStr, sizeof(timeStr), "%lu min", minSince);
    uint16_t tc = (minSince >= 10) ? toPanelColor(TFT_YELLOW) : txtCol;
    tft.setTextColor(tc, bgFill);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(timeStr, W - 4, 98, 2);
  }

  // Row 2 — IoB + CoB (left) + last bolus (right) (y=116, cyan)
  const bool hasIob   = hasData && ns.iob > 0.05f;
  const bool hasCob   = hasData && ns.cob > 0;
  const bool hasBolus = hasData && ns.lastBolusU > 0.0f && ns.lastBolusAgeMins > 0;
  const bool hasRow2  = hasIob || hasCob || hasBolus;
  if (hasRow2) {
    tft.setTextColor(toPanelColor(TFT_CYAN), bgFill);
    // Build left string: IoB and/or CoB
    char leftStr[24] = {}, bolStr[16] = {};
    if (hasIob && hasCob)
      snprintf(leftStr, sizeof(leftStr), "IoB:%.1fU CoB:%dg", ns.iob, ns.cob);
    else if (hasIob)
      snprintf(leftStr, sizeof(leftStr), "IoB:%.1fU", ns.iob);
    else if (hasCob)
      snprintf(leftStr, sizeof(leftStr), "CoB:%dg", ns.cob);
    if (hasBolus) {
      if (ns.lastBolusAgeMins < 60)
        snprintf(bolStr, sizeof(bolStr), "%.1fU %dm", ns.lastBolusU, ns.lastBolusAgeMins);
      else
        snprintf(bolStr, sizeof(bolStr), "%.1fU %dh", ns.lastBolusU, ns.lastBolusAgeMins / 60);
    }
    if (leftStr[0] && bolStr[0]) {
      tft.setTextDatum(ML_DATUM); tft.drawString(leftStr, 8,     116, 2);
      tft.setTextDatum(MR_DATUM); tft.drawString(bolStr,  W - 4, 116, 2);
    } else {
      tft.setTextDatum(MC_DATUM);
      tft.drawString(leftStr[0] ? leftStr : bolStr, W / 2, 116, 2);
    }
  }

  // Row 3 — pump data (y=134, magenta) — only if pump connected
  const bool hasPump = hasData && (ns.pumpReservoir > 0.1f || ns.pumpBattery < 255);
  if (hasPump) {
    tft.setTextColor(toPanelColor(TFT_MAGENTA), bgFill);
    char resStr[16] = {}, batStr[16] = {};
    if (ns.pumpReservoir > 0.1f)
      snprintf(resStr, sizeof(resStr), "Res:%.0fu", ns.pumpReservoir);
    if (ns.pumpBattery < 255)
      snprintf(batStr, sizeof(batStr), "Bat:%d%%", ns.pumpBattery);
    if (resStr[0]) { tft.setTextDatum(ML_DATUM); tft.drawString(resStr, 8,     134, 2); }
    if (batStr[0]) { tft.setTextDatum(MR_DATUM); tft.drawString(batStr, W - 4, 134, 2); }
  }

  // Graph
  const int16_t gx = 12;
  const int16_t gy = hasPump ? 152 : (hasRow2 ? 132 : 122);
  const int16_t gw = W - 24;
  const int16_t gh = H - gy - 8;
  tft.drawFastHLine(0, gy - 6, W, toPanelColor(TFT_DARKGREY));
  drawMiniGraph(tft, gx, gy, gw, gh);
}

// ---- WiFi + Nightscout ----

void setupWifi() {
  // Always start configuration AP so the web interface is reachable at 192.168.4.1
  WiFi.mode(cfg.wifi_ssid[0] ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP("CYDrip-Setup");
  Serial.println(F("Config AP started: CYDrip-Setup / 192.168.4.1"));

  if (cfg.wifi_ssid[0] == '\0') return;

  Serial.printf("Connecting to WiFi: %s\n", cfg.wifi_ssid);
  tft.println(F("Connecting WiFi..."));
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_password);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000UL)
    delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi STA failed, AP-only mode"));
    return;
  }
  wifiEnabled = true;
  Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  // NTP — use configured server, fallback to pool.ntp.org
  const char *ntpSrv = cfg.ntp_server[0] ? cfg.ntp_server : "pool.ntp.org";
  configTime(utcOffsetSec, 0, ntpSrv, "time.google.com");
  struct tm t2;
  if (getLocalTime(&t2, 5000)) {
    ntpSynced = true;
    Serial.println(F("NTP synced"));
  }
}

void fetchNightscout() {
  if (!wifiEnabled || cfg.ns_url[0] == '\0') return;
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }

  // Fetch last 36 entries (3 hours of 5-min readings)
  String url = String(cfg.ns_url) + "/api/v1/entries.json?count=36&fields=sgv,date,direction";
  if (cfg.ns_token[0]) { url += "&token="; url += cfg.ns_token; }

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Nightscout HTTP %d\n", code);
    http.end();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
    Serial.printf("Nightscout JSON error: %s\n", err.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();

  // Latest reading
  JsonObject latest = arr[0];
  float sgvMgDl = latest["sgv"] | 0.0f;
  uint64_t dateMs = latest["date"] | (uint64_t)0;
  const char *dir = latest["direction"] | "NONE";

  if (sgvMgDl <= 0) return;

  ns.sensSgvMgDl = sgvMgDl;
  ns.sensSgv     = sgvMgDl / 18.0f;
  timeStampLatestBgReadingInSecondsUTC = (unsigned long)(dateMs / 1000ULL);
  strlcpy(ns.sensDir, dir, sizeof(ns.sensDir));
  setNsArrowAngle();

  // Populate full history (API returns newest first — same order as readingHistory)
  int count = (int)arr.size();
  if (count > historySize) count = historySize;
  for (int i = 0; i < count; i++) {
    float v = arr[i]["sgv"] | 0.0f;
    readingHistory[i] = (v > 0) ? v / 18.0f : 0.0f;
  }
  saveHistoryToNVS();

  Serial.printf("Nightscout: %.1f mmol/L %s\n", ns.sensSgv, ns.sensDir);
  updateGlycemia();
  lastNsFetchMs = millis();
}

// ---- BLE send helpers ----

// Send a single opcode with no payload (3-byte packet: opcode, 0x01, 0x01)
static void bleNotify(uint8_t opcode) {
  uint8_t pkt[3] = {opcode, 0x01, 0x01};
  pRxTxCharacteristic->setValue(pkt, 3);
  pRxTxCharacteristic->notify();
}

// Send opcode + text payload (split into BLE_TX_MAX_BYTES chunks if needed)
static void bleNotifyText(uint8_t opcode, const char *text) {
  int len = text ? (int)strlen(text) : 0;
  if (len == 0) { bleNotify(opcode); return; }
  int total = (len + BLE_TX_MAX_BYTES - 4) / (BLE_TX_MAX_BYTES - 3);
  if (total == 0) total = 1;
  int sent = 0, pktNum = 1;
  while (sent < len) {
    int chunk = min(len - sent, BLE_TX_MAX_BYTES - 3);
    uint8_t pkt[BLE_TX_MAX_BYTES];
    pkt[0] = opcode; pkt[1] = pktNum; pkt[2] = total;
    memcpy(pkt + 3, text + sent, chunk);
    pRxTxCharacteristic->setValue(pkt, 3 + chunk);
    pRxTxCharacteristic->notify();
    sent += chunk; pktNum++;
  }
}

// ---- BLE receive ----

class BLECharacteristicCallBack : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    std::string rx = pChar->getValue();
    if (rx.empty()) return;

    int rxLen = (int)rx.length();
    if (rxLen > BLE_RX_MAX_BYTES) rxLen = BLE_RX_MAX_BYTES;
    memset(rxBuf, 0, BLE_RX_MAX_BYTES);
    memcpy(rxBuf, rx.c_str(), rxLen);

    if (debugLogging) {
      char hex[BLE_RX_MAX_BYTES * 2 + 1] = {};
      for (int i = 0; i < rxLen; i++)
        snprintf(hex + i*2, 3, "%02X", rxBuf[i]);
      Serial.printf("BLE RX: %s\n", hex);
    }

    switch (rxBuf[0]) {

      case 0x09: {  // Auth init
        Serial.println(F("Auth init"));
        if (cfg.blepassword[0] == '\0') {
          // No password configured — generate random, send as 0x0E (new password)
          static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
          for (int i = 0; i < 10; i++)
            cfg.blepassword[i] = alpha[random(0, 36)];
          cfg.blepassword[10] = '\0';
          bleNotifyText(0x0E, cfg.blepassword);
        } else {
          // Password stored — send bypass (authenticated, no check)
          bleNotify(0x0F);
        }
        bleAuthenticated = true;
      } break;

      case 0x0A: {  // Auth pass — verify
        Serial.println(F("Auth pass"));
        int passLen = (int)strlen(cfg.blepassword);
        bool ok = (rxLen == passLen + 1);
        if (ok) {
          for (int i = 0; i < passLen; i++)
            if (cfg.blepassword[i] != (char)rxBuf[i + 1]) { ok = false; break; }
        }
        if (ok) { bleNotify(0x0B); bleAuthenticated = true; }
        else    { bleNotify(0x0C); bleAuthenticated = false; }
      } break;

      case 0x21: {  // BG history pre-fill
        uint8_t count = rxBuf[1];
        if (count > historySize) count = historySize;
        if (!bleAuthenticated || rxLen < 3 + count * 2) break;
        for (uint8_t i = 0; i < count; i++) {
          uint16_t mg = (uint16_t(rxBuf[3 + i*2]) << 8) | rxBuf[4 + i*2];
          readingHistory[i] = mg / 18.0f;
        }
        Serial.printf("History preloaded: %u points\n", count);
      } break;

      case 0x20: {  // WatchDrip CYD update
        Serial.println(F("WatchDrip update (0x20)"));
        if (!bleAuthenticated || rxLen < 13) break;

        uint16_t bgMgdl = (uint16_t(rxBuf[3])  << 8) | rxBuf[4];
        uint32_t ts     = (uint32_t(rxBuf[7])  << 24) | (uint32_t(rxBuf[8])  << 16)
                        | (uint32_t(rxBuf[9])  <<  8) |  uint32_t(rxBuf[10]);
        uint8_t  dir    = rxBuf[11];
        uint8_t  flags  = rxBuf[12];
        bool     isMgdl = (flags & 0x01) != 0;
        bool     isStale = (flags & 0x02) != 0;

        // Sync local clock from UTC offset (bytes 13-14, int16 minutes)
        if (rxLen >= 15) {
          int16_t offsetMin = (int16_t(rxBuf[13]) << 8) | rxBuf[14];
          utcOffsetSec = (long)offsetMin * 60L;
        }
        bleTimestampSec    = ts + utcOffsetSec;
        bleTimestampMillis = millis();

        cfg.show_mgdl   = isMgdl ? 1 : 0;
        ns.sensSgvMgDl  = float(bgMgdl);
        ns.sensSgv      = bgMgdl / 18.0f;
        ns.sensTime     = time_t(ts);
        timeStampLatestBgReadingInSecondsUTC = ts;

        static const char* dirNames[] = {
          "NONE","DoubleUp","SingleUp","FortyFiveUp",
          "Flat","FortyFiveDown","SingleDown","DoubleDown"
        };
        strlcpy(ns.sensDir, dir < 8 ? dirNames[dir] : "NONE", sizeof(ns.sensDir));
        setNsArrowAngle();

        // IoB (bytes 15-16)
        ns.iob = (rxLen >= 17)
          ? ((uint16_t(rxBuf[15]) << 8) | rxBuf[16]) / 100.0f
          : 0.0f;

        // Last bolus (bytes 17-19)
        if (rxLen >= 20) {
          ns.lastBolusU       = ((uint16_t(rxBuf[17]) << 8) | rxBuf[18]) / 100.0f;
          ns.lastBolusAgeMins = rxBuf[19];
        } else {
          ns.lastBolusU = 0; ns.lastBolusAgeMins = 0;
        }

        // Pump data (bytes 20-24)
        if (rxLen >= 25) {
          ns.pumpIob       = ((uint16_t(rxBuf[20]) << 8) | rxBuf[21]) / 100.0f;
          ns.pumpReservoir = ((uint16_t(rxBuf[22]) << 8) | rxBuf[23]) / 10.0f;
          ns.pumpBattery   = rxBuf[24];
        } else {
          ns.pumpIob = 0; ns.pumpReservoir = 0; ns.pumpBattery = 255;
        }

        // CoB (byte 25)
        ns.cob = (rxLen >= 26) ? rxBuf[25] : 0;

        if (!isStale) pushReadingToHistory(ns.sensSgv);
        updateGlycemia();
        msCount = millis();
      } break;
    }
  }
};

class BLEServerCallBack : public BLEServerCallbacks {
  void onConnect(BLEServer *pSrv) {
    Serial.println(F("BLE connect"));
    // Continue advertising so a second client can also connect
    pSrv->getAdvertising()->start();
  }
  void onDisconnect(BLEServer *pSrv) {
    Serial.println(F("BLE disconnect"));
    bleAuthenticated = false;
    pSrv->getAdvertising()->start();
  }
};

// ---- BLE init ----

void setupBLE() {
  Serial.println(F("Starting BLE"));
  BLEDevice::init(BLE_DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BLEServerCallBack());
  BLEService *svc = pServer->createService(BLE_SERVICE_UUID);
  pRxTxCharacteristic = svc->createCharacteristic(
    BLE_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  pRxTxCharacteristic->addDescriptor(new BLE2902());
  pRxTxCharacteristic->setCallbacks(new BLECharacteristicCallBack());
  svc->start();
  pServer->getAdvertising()->start();
}

// ---- Setup ----

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.invertDisplay(false);
  tft.setRotation(1);  // landscape
  tft.setTextColor(toPanelColor(textColor), toPanelColor(backGroundColor));
  tft.fillScreen(toPanelColor(backGroundColor));
  tft.setCursor(0, 0);
  tft.setTextSize(1);

  initBacklight();
  yield();

  loadHistoryFromNVS();

  // Load config: NVS first (web-configured), then SD card overrides if present
  loadCYDConfigFromNVS(cfg);
  if (SD.begin(SD_CS_PIN)) {
    Serial.println(F("SD card OK"));
    readCYDConfig("/CYD.INI", &cfg);
  } else {
    Serial.println(F("SD card not found"));
  }
  // Apply config values
  utcOffsetSec = (long)cfg.utc_offset_min * 60L;
  BG_LOW       = cfg.bg_low;
  BG_WARN_LOW  = cfg.bg_warn_low;
  BG_WARN_HIGH = cfg.bg_warn_high;
  BG_HIGH      = cfg.bg_high;
  blLevelIdx   = cfg.brightness;

  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  setupWifi();
  webConfigBegin(cfg, tft, displayColorsInverted);
  tft.println(F("Config: 192.168.4.1"));
  tft.println(F("Waiting for CYDDrip..."));
  setupBLE();
}

// ---- Main loop ----

void loop() {
  webConfigHandle();
  handleBrightnessButton();
  handleAlarmButton();
  checkAlarms();
  if (wifiEnabled && millis() - lastNsFetchMs >= NS_FETCH_INTERVAL_MS)
    fetchNightscout();
  delay(20);
  unsigned long utc = getUTCTimeInSeconds();
  unsigned long elapsed = millis() - msCount;
  // Refresh every 30 s when we have a clock (to keep time display current),
  // every 5 s while waiting for first BLE data, every 2 min otherwise.
  bool shouldUpdate =
      (elapsed > 30000UL  && bleTimestampSec > 0) ||
      (elapsed > 5000UL   && utc == 0) ||
      (elapsed > 120000UL) ||
      (elapsed > 15000UL  && utc > 0 &&
       utc - timeStampLatestBgReadingInSecondsUTC > 120UL);
  if (shouldUpdate) {
    updateGlycemia();
    msCount = millis();
  }
}
