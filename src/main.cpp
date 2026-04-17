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
#define BOOT_BTN_PIN     0   // BOOT button — short press brightness, long press orientation
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
#include <WiFiClientSecure.h>
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
static const char* BLE_DEVICE_NAME       = "CYDDrip";
static const char* BLE_SERVICE_UUID      = "AF6E5F78-706A-43FB-B1F4-C27D7D5C762F";
static const char* BLE_CHAR_UUID         = "6D810E9F-0983-4030-BDA7-C7C9A6A19C1C";
static const int   BLE_TX_MAX_BYTES      = 20;   // max notification payload
static const int   BLE_RX_MAX_BYTES      = 128;  // max write payload (0x21 = 3+35×2=73 bytes)

// ---- WiFi state ----
static bool          wifiEnabled        = false;
static bool          ntpSynced          = false;
static unsigned long lastNsFetchMs      = 0;
static unsigned long lastWifiReconnectMs = 0;
static int           wifiDisconnects    = 0;   // total disconnect events
static const unsigned long NS_FETCH_INTERVAL_MS    = 5UL * 60UL * 1000UL;  // 5 min
static const unsigned long WIFI_RECONNECT_INTERVAL = 30UL * 1000UL;        // 30 sec

// ---- State ----
static const int          historySize          = 180;         // 3 h at 1-min resolution
static const unsigned long BG_HISTORY_WINDOW_SEC = 3UL * 3600UL; // graph time window
static const unsigned long BG_PREFILL_INTERVAL_SEC = 5UL * 60UL; // assumed CGM interval for 0x21
static const unsigned long BG_FRESHNESS_SEC      = 7UL * 60UL;  // staleness threshold for display
static const unsigned long BTN_LONG_PRESS_MS     = 800UL;       // long-press threshold
static const float         GRAPH_MMOL_MIN        = 2.5f;
static const float         GRAPH_MMOL_MAX        = 15.0f;

struct BgReading {
  float         mmol   = 0.0f;
  unsigned long utcSec = 0;      // UTC timestamp; 0 = unknown
};
static BgReading readingHistory[historySize];

unsigned long timeStampLatestBgReadingInSecondsUTC = 0;
unsigned long msCount                              = 0;

// BLE time sync (set from 0x20 packet)
static unsigned long bleTimestampSec    = 0;  // local time at last BLE update
static unsigned long bleTimestampMillis = 0;  // millis() at that moment
static long          utcOffsetSec       = 0;  // UTC offset in seconds

// Pending BLE update — set from BLE task, consumed in main loop to avoid concurrent TFT access
static volatile bool bleDataReady = false;

// Display orientation
static bool isPortrait = false;  // false = landscape (default)

void saveRotationToNVS(bool portrait) {
  preferences.begin("cydui", false);
  preferences.putBool("portrait", portrait);
  preferences.end();
}

bool loadRotationFromNVS() {
  preferences.begin("cydui", true);
  bool v = preferences.getBool("portrait", false);
  preferences.end();
  return v;
}

void applyRotation(bool portrait) {
  isPortrait = portrait;
  tft.setRotation(portrait ? 2 : 1);
}

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
void     pushReadingToHistory(float mmolValue, unsigned long utcSec);
void     drawMiniGraph(TFT_eSPI &surface, int16_t x, int16_t y, int16_t w, int16_t h, unsigned long now);
void     drawArrow(TFT_eSPI &surface, int x, int y, int asize, int aangle, int pwidth, int plength, uint16_t color);

// ---- Colour helpers ----

uint16_t toPanelColor(uint16_t color) {
  return displayColorsInverted ? uint16_t(color ^ 0xFFFF) : color;
}

static inline float mgdlToMmol(float mg) { return mg / 18.0f; }

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

static void toggleOrientation() {
  isPortrait = !isPortrait;
  saveRotationToNVS(isPortrait);
  applyRotation(isPortrait);
  updateGlycemia();
  msCount = millis();
}

void handleBrightnessButton() {
  static const int pins[]        = {BTN_BRIGHTNESS, BOOT_BTN_PIN};
  static bool          pressing[2]   = {};
  static unsigned long pressStart[2] = {};
  for (int i = 0; i < 2; i++) {
    bool p = (digitalRead(pins[i]) == LOW);
    if (!pressing[i] && p) { pressing[i] = true; pressStart[i] = millis(); }
    else if (pressing[i] && !p) {
      pressing[i] = false;
      if (millis() - pressStart[i] >= BTN_LONG_PRESS_MS) toggleOrientation();
      else                                                cycleBrightness();
    }
  }
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
  preferences.putBytes("hist", readingHistory, sizeof(readingHistory));
  preferences.putULong("lastTs", timeStampLatestBgReadingInSecondsUTC);
  preferences.end();
}

void loadHistoryFromNVS() {
  preferences.begin("bghistory", true);
  size_t n = preferences.getBytes("hist", readingHistory, sizeof(readingHistory));
  if (n == sizeof(readingHistory)) {
    timeStampLatestBgReadingInSecondsUTC = preferences.getULong("lastTs", 0);
    Serial.println(F("BG history restored from NVS"));
  }
  preferences.end();
}

void pushReadingToHistory(float mmolValue, unsigned long utcSec) {
  if (mmolValue <= 0) return;
  // Skip duplicate timestamp
  if (utcSec > 0 && readingHistory[0].utcSec == utcSec) return;
  for (int i = historySize - 1; i > 0; --i)
    readingHistory[i] = readingHistory[i - 1];
  readingHistory[0].mmol   = mmolValue;
  readingHistory[0].utcSec = utcSec;
  // NVS write is deferred — main loop flushes every 5 min
}

// ---- Time helpers ----
// Time is synced from the 0x20 BLE packet; no NTP needed.

unsigned long getLocalTimeInSeconds() {
  // NTP (WiFi) takes priority — most accurate
  if (ntpSynced) {
    struct tm t;
    if (getLocalTime(&t, 100)) return (unsigned long)mktime(&t) + utcOffsetSec;
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
  if (readingHistory[0].mmol <= 0 || readingHistory[1].mmol <= 0) { buf[0] = '\0'; return; }
  float d = readingHistory[0].mmol - readingHistory[1].mmol;
  if (cfg.show_mgdl == 1) {
    int di = (int)(d * 18.0f + (d >= 0 ? 0.5f : -0.5f));
    snprintf(buf, bufSize, di >= 0 ? "+%d" : "%d", di);
  } else {
    snprintf(buf, bufSize, d >= 0.0f ? "+%.1f" : "%.1f", d);
  }
}

// ---- Graph ----

void drawMiniGraph(TFT_eSPI &surface, int16_t x, int16_t y, int16_t w, int16_t h, unsigned long now) {
  const float mmolRange = GRAPH_MMOL_MAX - GRAPH_MMOL_MIN;

  const uint16_t frameColor  = toPanelColor(TFT_DARKGREY);
  const uint16_t greenColor  = toPanelColor(TFT_GREEN);
  const uint16_t yellowColor = toPanelColor(TFT_YELLOW);
  const uint16_t redColor    = toPanelColor(COLOR_BG_HIGH);
  const uint16_t blueColor   = toPanelColor(COLOR_BG_LOW);

  surface.drawRoundRect(x - 2, y - 2, w + 4, h + 4, 4, frameColor);

  int16_t yLow  = y + h - (int16_t)((BG_LOW  - GRAPH_MMOL_MIN) / mmolRange * h);
  int16_t yHigh = y + h - (int16_t)((BG_HIGH - GRAPH_MMOL_MIN) / mmolRange * h);
  surface.drawFastHLine(x, yLow,  w, toPanelColor(0x001F));  // dim blue
  surface.drawFastHLine(x, yHigh, w, toPanelColor(0xA000));  // dim red

  // Time-based positioning: right = now, left = now - BG_HISTORY_WINDOW_SEC
  // Fallback (no time sync): evenly space by array index
  const bool useTime = (now > 0);

  int16_t prevPx = -1, prevPy = -1;

  for (int i = historySize - 1; i >= 0; i--) {  // oldest → newest
    float glk = readingHistory[i].mmol;
    if (glk <= 0) { prevPx = -1; continue; }
    glk = constrain(glk, GRAPH_MMOL_MIN, GRAPH_MMOL_MAX);

    int16_t px;
    if (useTime) {
      if (readingHistory[i].utcSec == 0) { prevPx = -1; continue; }
      long age = (long)now - (long)readingHistory[i].utcSec;
      if (age < 0 || age > (long)BG_HISTORY_WINDOW_SEC) { prevPx = -1; continue; }
      px = x + 5 + (int16_t)((1.0f - (float)age / BG_HISTORY_WINDOW_SEC) * (w - 10));
    } else {
      // No time sync: spread by array index (oldest at left, newest at right)
      px = x + 5 + (int16_t)((float)(historySize - 1 - i) / (historySize - 1) * (w - 10));
    }

    uint16_t pc = greenColor;
    if      (glk < BG_LOW)       pc = blueColor;
    else if (glk < BG_WARN_LOW)  pc = yellowColor;
    else if (glk > BG_HIGH)      pc = redColor;
    else if (glk > BG_WARN_HIGH) pc = yellowColor;
    int16_t py = y + h - (int16_t)((glk - GRAPH_MMOL_MIN) / mmolRange * h);

    if (prevPx >= 0) surface.drawLine(prevPx, prevPy, px, py, frameColor);
    int16_t dotR = (prevPx >= 0 && px - prevPx >= 8) ? 2 : 1;
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
      utcNow <= timeStampLatestBgReadingInSecondsUTC + BG_FRESHNESS_SEC) {
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

  tft.setFreeFont(nullptr);
  tft.setTextFont(2);
  tft.setTextSize(1);

  // Build shared strings used in both layouts
  const bool hasIob   = hasData && ns.iob > 0.05f;
  const bool hasCob   = hasData && ns.cob > 0;
  const bool hasBolus = hasData && ns.lastBolusU > 0.0f && ns.lastBolusAgeMins > 0;
  const bool hasRow2  = hasIob || hasCob || hasBolus;
  const bool hasPump  = hasData && (ns.pumpReservoir > 0.1f || ns.pumpBattery < 255);

  char leftStr[24] = {}, bolStr[16] = {}, resStr[16] = {}, batStr[16] = {};
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
  if (hasPump) {
    if (ns.pumpReservoir > 0.1f) snprintf(resStr, sizeof(resStr), "Res:%.0fu", ns.pumpReservoir);
    if (ns.pumpBattery < 255)    snprintf(batStr, sizeof(batStr), "Bat:%d%%",  ns.pumpBattery);
  }

  char clockStr[6] = {};
  unsigned long localNow = getLocalTimeInSeconds();
  if (localNow > 0)
    snprintf(clockStr, sizeof(clockStr), "%02d:%02d",
             (int)((localNow % 86400UL) / 3600),
             (int)((localNow % 3600UL)  / 60));

  char timeStr[12] = {};
  uint16_t timeCol = txtCol;
  if (hasData && minSince > 0) {
    snprintf(timeStr, sizeof(timeStr), "%lu min", minSince);
    timeCol = (minSince >= 10) ? toPanelColor(TFT_YELLOW) : txtCol;
  }

  // Layout parameters (portrait vs landscape)
  const int16_t margin = isPortrait ? 4   : 8;
  const int16_t row1Y  = isPortrait ? 108 : 98;
  const int16_t row2Y  = isPortrait ? 124 : 116;
  const int16_t row3Y  = isPortrait ? 140 : 134;
  const int16_t gyPump = isPortrait ? 164 : 152;
  const int16_t gyR2   = isPortrait ? 150 : 132;
  const int16_t gyNone = isPortrait ? 132 : 122;

  // Row 0 — clock (both modes)
  tft.setTextColor(txtCol, bgFill);
  tft.setTextDatum(TL_DATUM);
  if (clockStr[0]) tft.drawString(clockStr, 4, 6, 2);

  if (isPortrait) {
    // Portrait Row 0: delta TR, BG centered at y=24, arrow right
    if (deltaStr[0]) {
      tft.setTextColor(bgCol, bgFill);
      tft.setTextDatum(TR_DATUM);
      tft.drawString(deltaStr, W - 4, 6, 2);
    }
    tft.setTextFont(4); tft.setTextSize(3);
    tft.setTextColor(bgCol, bgFill);
    tft.drawCentreString(sgvStr, W / 2, 24, 4);
    if (hasData && ns.arrowAngle != 180)
      drawArrow(tft, W - 28, 60, 22, ns.arrowAngle + 85, 22, 32, bgCol);
    // Row 1: units ML
    tft.setTextFont(2); tft.setTextSize(1);
    tft.setTextColor(txtCol, bgFill);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(cfg.show_mgdl == 1 ? "mg/dL" : "mmol/L", margin, row1Y, 2);
  } else {
    // Landscape Row 0: BG centered at 160, arrow right
    tft.setTextFont(4); tft.setTextSize(3);
    tft.setTextColor(bgCol, bgFill);
    tft.drawCentreString(sgvStr, 160, 6, 4);
    if (hasData && ns.arrowAngle != 180)
      drawArrow(tft, 262, 48, 30, ns.arrowAngle + 85, 30, 40, bgCol);
    // Row 1: delta ML, units MC
    tft.setTextFont(2); tft.setTextSize(1);
    if (deltaStr[0]) {
      tft.setTextColor(bgCol, bgFill);
      tft.setTextDatum(ML_DATUM);
      tft.drawString(deltaStr, margin, row1Y, 2);
    }
    tft.setTextColor(txtCol, bgFill);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(cfg.show_mgdl == 1 ? "mg/dL" : "mmol/L", W / 2, row1Y, 2);
  }

  // Row 1 — time-ago MR (shared)
  if (timeStr[0]) {
    tft.setTextColor(timeCol, bgFill);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(timeStr, W - 4, row1Y, 2);
  }

  // Row 2 — IoB/CoB/bolus (shared)
  if (hasRow2) {
    tft.setTextColor(toPanelColor(TFT_CYAN), bgFill);
    if (leftStr[0] && bolStr[0]) {
      tft.setTextDatum(ML_DATUM); tft.drawString(leftStr, margin, row2Y, 2);
      tft.setTextDatum(MR_DATUM); tft.drawString(bolStr,  W - 4,  row2Y, 2);
    } else {
      tft.setTextDatum(MC_DATUM);
      tft.drawString(leftStr[0] ? leftStr : bolStr, W / 2, row2Y, 2);
    }
  }

  // Row 3 — pump (shared)
  if (hasPump) {
    tft.setTextColor(toPanelColor(TFT_MAGENTA), bgFill);
    if (resStr[0]) { tft.setTextDatum(ML_DATUM); tft.drawString(resStr, margin, row3Y, 2); }
    if (batStr[0]) { tft.setTextDatum(MR_DATUM); tft.drawString(batStr, W - 4,  row3Y, 2); }
  }

  // Graph (shared)
  const int16_t gx = 12;
  const int16_t gy = hasPump ? gyPump : (hasRow2 ? gyR2 : gyNone);
  const int16_t gw = W - 24;
  const int16_t gh = H - gy - 8;
  tft.drawFastHLine(0, gy - 6, W, toPanelColor(TFT_DARKGREY));
  drawMiniGraph(tft, gx, gy, gw, gh, utcNow);
}

// ---- WiFi + Nightscout ----

void setupWifi() {
  // WiFi event callbacks for disconnect/reconnect logging
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    wifiDisconnects++;
    Serial.printf("[WiFi] Disconnected #%d, reason: %d\n",
                  wifiDisconnects, info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.printf("[WiFi] Reconnected — IP: %s, RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    lastNsFetchMs = 0;  // trigger immediate Nightscout fetch
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

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

// Temporarily deinit BLE to free ~89 KB for the SSL handshake.
// Called only when heap is too fragmented for HTTPS with BLE running.
static void blePause() {
  Serial.printf("[BLE] Pausing for HTTPS. heap: %u  largest: %u\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  // Stop advertising first so clients disconnect cleanly
  if (pServer) pServer->getAdvertising()->stop();
  delay(100);
  BLEDevice::deinit(true);
  pServer             = nullptr;
  pRxTxCharacteristic = nullptr;
  bleAuthenticated    = false;
  delay(200);  // let Bluedroid stack fully unwind before any heap use
  Serial.printf("[BLE] Paused.  heap: %u  largest: %u\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void bleResume() {
  Serial.println(F("[BLE] Resuming after fetch..."));
  delay(100);
  setupBLE();
  Serial.printf("[BLE] Resumed. heap: %u\n", ESP.getFreeHeap());
}

void fetchNightscout() {
  if (!wifiEnabled || cfg.ns_url[0] == '\0') return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WiFi] fetchNightscout: not connected, skipping"));
    return;
  }

  // Update immediately — prevents busy-loop retries when all error paths return early
  lastNsFetchMs = millis();

  uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  bool isHttps = (strncmp(cfg.ns_url, "https://", 8) == 0);
  Serial.printf("[NS] Fetching... RSSI: %d dBm  heap: %u  largest: %u\n",
                WiFi.RSSI(), ESP.getFreeHeap(), largestBlock);

  // SSL handshake needs ~32 KB contiguous. Pause BLE (~89 KB) if heap is tight.
  bool pausedBle = (isHttps && pServer != nullptr && largestBlock < 40000);
  if (pausedBle) blePause();

  // Fetch last 36 entries (3 hours of 5-min readings)
  String base = cfg.ns_url;
  if (base.endsWith("/")) base.remove(base.length() - 1);
  String url = base + "/api/v1/entries.json?count=36&fields=sgv,date,direction";
  if (cfg.ns_token[0]) { url += "&token="; url += cfg.ns_token; }
  Serial.printf("[NS] host: %s\n", base.c_str());

  HTTPClient http;
  WiFiClientSecure secureClient;
  if (isHttps) {
    secureClient.setInsecure();  // no CA bundle on ESP32 — skip cert validation
    http.begin(secureClient, url);
  } else {
    http.begin(url);
  }
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[NS] HTTP %d\n", code);
    http.end();
    if (pausedBle) bleResume();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
    Serial.printf("[NS] JSON error: %s\n", err.c_str());
    if (pausedBle) bleResume();
    return;
  }

  if (pausedBle) bleResume();

  JsonArray arr = doc.as<JsonArray>();

  // Latest reading
  JsonObject latest = arr[0];
  float sgvMgDl = latest["sgv"] | 0.0f;
  uint64_t dateMs = latest["date"] | (uint64_t)0;
  const char *dir = latest["direction"] | "NONE";

  if (sgvMgDl <= 0) return;

  ns.sensSgvMgDl = sgvMgDl;
  ns.sensSgv     = mgdlToMmol(sgvMgDl);
  timeStampLatestBgReadingInSecondsUTC = (unsigned long)(dateMs / 1000ULL);
  strlcpy(ns.sensDir, dir, sizeof(ns.sensDir));
  setNsArrowAngle();

  // Populate full history (API returns newest first — same order as readingHistory)
  int count = (int)arr.size();
  if (count > historySize) count = historySize;
  for (int i = 0; i < count; i++) {
    float    v   = arr[i]["sgv"] | 0.0f;
    uint64_t dMs = arr[i]["date"] | (uint64_t)0;
    readingHistory[i].mmol   = (v > 0) ? mgdlToMmol(v) : 0.0f;
    readingHistory[i].utcSec = (unsigned long)(dMs / 1000ULL);
  }
  saveHistoryToNVS();

  Serial.printf("[NS] OK: %.1f mmol/L %s (disconnects total: %d)\n",
                ns.sensSgv, ns.sensDir, wifiDisconnects);
  updateGlycemia();
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

      case 0x21: {  // BG history pre-fill (newest first, no timestamps in packet)
        uint8_t count = rxBuf[1];
        if (count > historySize) count = historySize;
        if (!bleAuthenticated || rxLen < 3 + count * 2) break;
        // Estimate timestamps: assume 5-min intervals from last known reading
        unsigned long baseTs = timeStampLatestBgReadingInSecondsUTC;
        for (uint8_t i = 0; i < count; i++) {
          uint16_t mg = (uint16_t(rxBuf[3 + i*2]) << 8) | rxBuf[4 + i*2];
          readingHistory[i].mmol   = (mg > 0) ? mgdlToMmol(mg) : 0.0f;
          readingHistory[i].utcSec = (baseTs > 0) ? baseTs - (unsigned long)i * BG_PREFILL_INTERVAL_SEC : 0;
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

        // Sync local clock from UTC offset (bytes 13-14, int16 minutes).
        // Only apply BLE-provided offset if not set in config file — config takes priority.
        if (rxLen >= 15 && cfg.utc_offset_min == 0) {
          int16_t offsetMin = (int16_t(rxBuf[13]) << 8) | rxBuf[14];
          utcOffsetSec = (long)offsetMin * 60L;
        }
        bleTimestampSec    = ts + utcOffsetSec;
        bleTimestampMillis = millis();

        cfg.show_mgdl   = isMgdl ? 1 : 0;
        ns.sensSgvMgDl  = float(bgMgdl);
        ns.sensSgv      = mgdlToMmol(bgMgdl);
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

        if (!isStale) {
          pushReadingToHistory(ns.sensSgv, ts);
          // Retroactively assign timestamps to pre-fill readings (0x21) that arrived
          // before this 0x20 packet and therefore have utcSec == 0.
          for (int i = 1; i < historySize; i++) {
            if (readingHistory[i].mmol > 0 && readingHistory[i].utcSec == 0)
              readingHistory[i].utcSec = ts - (unsigned long)i * BG_PREFILL_INTERVAL_SEC;
          }
        }
        bleDataReady = true;  // signal main loop to redraw (avoids concurrent TFT access)
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
  tft.setRotation(1);  // landscape default until config loaded
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

  // Apply rotation: NVS-saved toggle state, overridden by CYD.INI if explicitly set
  isPortrait = loadRotationFromNVS();
  if (cfg.rotation == 0) isPortrait = false;
  else if (cfg.rotation == 1) isPortrait = true;
  applyRotation(isPortrait);

  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  setupWifi();
  // First NS fetch BEFORE BLE init: heap is unfragmented, SSL allocation succeeds
  Serial.printf("Pre-BLE heap: free=%u  largest=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  fetchNightscout();
  Serial.printf("Post-fetch heap: free=%u  largest=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  webConfigBegin(cfg, tft, displayColorsInverted);
  tft.println(F("Config: 192.168.4.1"));
  tft.println(F("Waiting for CYDDrip..."));
  setupBLE();
  Serial.printf("Post-BLE heap: free=%u  largest=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// ---- Main loop ----

void loop() {
  webConfigHandle();
  handleBrightnessButton();
  handleAlarmButton();
  checkAlarms();
  // WiFi reconnect watchdog: retry every 30 s if connection lost
  if (wifiEnabled && WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiReconnectMs >= WIFI_RECONNECT_INTERVAL) {
      lastWifiReconnectMs = millis();
      Serial.printf("[WiFi] Not connected, reconnecting... (disconnects: %d)\n", wifiDisconnects);
      WiFi.reconnect();
    }
  }

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
  if (bleDataReady || shouldUpdate) {
    bleDataReady = false;
    updateGlycemia();
    msCount = millis();
  }

  // Flush history to NVS at most once per 5 min (avoids flash wear on every BLE reading)
  static unsigned long lastHistorySaveMs = 0;
  if (millis() - lastHistorySaveMs >= NS_FETCH_INTERVAL_MS) {
    saveHistoryToNVS();
    lastHistorySaveMs = millis();
  }
}
