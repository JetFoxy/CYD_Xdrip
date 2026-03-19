/*  WebConfig.cpp — captive-portal style web configuration for CYDrip
    Always starts AP "CYDrip-Setup" (no password) for AP_TIMEOUT_MS (default 3 min).
    If STA credentials are present, WiFi runs in AP_STA mode simultaneously.
    After the timeout the AP is shut down to free RF resources for BLE:
      - STA connected  → switch to WIFI_STA, web server keeps running on local IP.
      - no STA         → web server stopped, WiFi off, pure BLE mode.
    Saving via the web form writes to NVS and reboots the device.
*/

#include "WebConfig.h"
#include <WebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Arduino.h>

static const char    NVS_NS[]        = "cydcfg";
static const uint32_t AP_TIMEOUT_MS  = 3UL * 60UL * 1000UL;  // 3 min, then AP shuts down

// ---- module state ----
static WebServer      s_srv(80);
static tConfig       *s_cfg        = nullptr;
static TFT_eSPI      *s_tft        = nullptr;
static bool           s_invertClr  = false;
static bool           s_saved      = false;
static bool           s_apActive   = false;
static unsigned long  s_apStartMs  = 0;

// Colour helper — mirrors the XOR inversion used in main.cpp
static inline uint16_t pc(uint16_t c) {
  return s_invertClr ? (uint16_t)(c ^ 0xFFFF) : c;
}

// ---- NVS ----

bool loadCYDConfigFromNVS(tConfig &cfg) {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;
  bool found = p.isKey("wifi_ssid");
  if (found) {
    String v;
    v = p.getString("wifi_ssid",  ""); strlcpy(cfg.wifi_ssid,     v.c_str(), sizeof(cfg.wifi_ssid));
    v = p.getString("wifi_pass",  ""); strlcpy(cfg.wifi_password,  v.c_str(), sizeof(cfg.wifi_password));
    v = p.getString("ns_url",     ""); strlcpy(cfg.ns_url,         v.c_str(), sizeof(cfg.ns_url));
    v = p.getString("ns_token",   ""); strlcpy(cfg.ns_token,       v.c_str(), sizeof(cfg.ns_token));
    v = p.getString("ntp_server", ""); strlcpy(cfg.ntp_server,     v.c_str(), sizeof(cfg.ntp_server));
    Serial.println(F("Config loaded from NVS"));
  }
  p.end();
  return found;
}

void saveCYDConfigToNVS(const tConfig &cfg) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("wifi_ssid",  cfg.wifi_ssid);
  p.putString("wifi_pass",  cfg.wifi_password);
  p.putString("ns_url",     cfg.ns_url);
  p.putString("ns_token",   cfg.ns_token);
  p.putString("ntp_server", cfg.ntp_server);
  p.end();
  Serial.println(F("Config saved to NVS"));
}

// ---- HTML helpers ----

// Escape special chars for use inside an HTML attribute value (double-quoted).
static String htmlEsc(const char *s) {
  String r;
  r.reserve(strlen(s) + 8);
  for (; *s; ++s) {
    switch (*s) {
      case '&': r += F("&amp;");  break;
      case '"': r += F("&quot;"); break;
      case '<': r += F("&lt;");   break;
      case '>': r += F("&gt;");   break;
      default:  r += *s;
    }
  }
  return r;
}

// Main config page — pre-fills current values (except WiFi password for security).
static String buildMainPage() {
  const char *ntp = s_cfg->ntp_server[0] ? s_cfg->ntp_server : "pool.ntp.org";

  String p;
  p.reserve(3200);
  p = F("<!DOCTYPE html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>CYDrip Setup</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}"
        "h1{color:#4caf50;margin:0 0 2px;font-size:22px}"
        ".sub{color:#555;font-size:12px;margin:0 0 18px}"
        ".card{background:#1e1e1e;border-radius:8px;padding:14px 16px;margin-bottom:12px}"
        "h2{color:#4caf50;font-size:13px;margin:0 0 10px;text-transform:uppercase;letter-spacing:.5px}"
        "label{display:block;font-size:12px;color:#888;margin:8px 0 3px}"
        "label:first-of-type{margin-top:0}"
        "input{width:100%;background:#2a2a2a;border:1px solid #444;"
        "border-radius:4px;color:#eee;padding:8px 10px;font-size:15px}"
        "input:focus{outline:none;border-color:#4caf50}"
        "button{width:100%;background:#4caf50;color:#000;border:none;"
        "border-radius:6px;padding:14px;font-size:16px;font-weight:bold;"
        "cursor:pointer;margin-top:4px}"
        "button:active{background:#388e3c}"
        "</style></head><body>"
        "<h1>CYDrip</h1>"
        "<p class=\"sub\">Device configuration</p>"
        "<form method=\"POST\" action=\"/save\">"

        "<div class=\"card\"><h2>WiFi</h2>"
        "<label>Network (SSID)</label>"
        "<input name=\"ssid\" value=\"");
  p += htmlEsc(s_cfg->wifi_ssid);
  p += F("\" autocomplete=\"off\" spellcheck=\"false\">"
         "<label>Password</label>"
         "<input name=\"pass\" type=\"password\" autocomplete=\"new-password\""
         " placeholder=\"leave empty to keep current\">"
         "</div>"

         "<div class=\"card\"><h2>Nightscout</h2>"
         "<label>URL</label>"
         "<input name=\"nsurl\" value=\"");
  p += htmlEsc(s_cfg->ns_url);
  p += F("\" placeholder=\"https://mysite.ns.io\" autocomplete=\"off\" spellcheck=\"false\">"
         "<label>Token (optional)</label>"
         "<input name=\"nstoken\" value=\"");
  p += htmlEsc(s_cfg->ns_token);
  p += F("\" placeholder=\"leave empty if not needed\">"
         "</div>"

         "<div class=\"card\"><h2>Time / NTP</h2>"
         "<label>NTP Server</label>"
         "<input name=\"ntp\" value=\"");
  p += htmlEsc(ntp);
  p += F("\">"
         "</div>"

         "<button type=\"submit\">Save and Reboot</button>"
         "</form></body></html>");
  return p;
}

static const char HTML_SAVED[] =
  "<!DOCTYPE html><html><head>"
  "<meta charset=\"utf-8\">"
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>Saved</title>"
  "<style>body{font-family:sans-serif;background:#111;color:#eee;"
  "text-align:center;padding:60px 16px}"
  "h1{color:#4caf50}p{color:#888;font-size:14px}</style>"
  "</head><body>"
  "<h1>Saved!</h1>"
  "<p>Settings saved. Device is rebooting...</p>"
  "</body></html>";

// ---- HTTP handlers ----

static void handleRoot() {
  s_srv.send(200, F("text/html"), buildMainPage());
}

static void handleSave() {
  strlcpy(s_cfg->wifi_ssid, s_srv.arg("ssid").c_str(), sizeof(s_cfg->wifi_ssid));

  // Only overwrite password if the user typed something
  String pass = s_srv.arg("pass");
  if (pass.length() > 0)
    strlcpy(s_cfg->wifi_password, pass.c_str(), sizeof(s_cfg->wifi_password));

  strlcpy(s_cfg->ns_url,     s_srv.arg("nsurl").c_str(),   sizeof(s_cfg->ns_url));
  strlcpy(s_cfg->ns_token,   s_srv.arg("nstoken").c_str(), sizeof(s_cfg->ns_token));

  String ntp = s_srv.arg("ntp");
  if (ntp.length() > 0)
    strlcpy(s_cfg->ntp_server, ntp.c_str(), sizeof(s_cfg->ntp_server));

  saveCYDConfigToNVS(*s_cfg);
  s_srv.send(200, F("text/html"), HTML_SAVED);
  s_saved = true;
}

static void handleNotFound() {
  s_srv.sendHeader(F("Location"), F("/"));
  s_srv.send(302);
}

// ---- Public API ----

void webConfigBegin(tConfig &cfg, TFT_eSPI &tft, bool invertColors) {
  s_cfg       = &cfg;
  s_tft       = &tft;
  s_invertClr = invertColors;
  s_saved     = false;

  s_srv.on("/",     HTTP_GET,  handleRoot);
  s_srv.on("/save", HTTP_POST, handleSave);
  s_srv.onNotFound(handleNotFound);
  s_srv.begin();

  s_apActive  = true;
  s_apStartMs = millis();

  Serial.printf("Web config: http://192.168.4.1  (AP: CYDrip-Setup, closes in %lus)\n",
                AP_TIMEOUT_MS / 1000UL);
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("Also at: http://%s\n", WiFi.localIP().toString().c_str());
}

void webConfigHandle() {
  if (!s_cfg) return;

  // Shut down AP after timeout to give BLE full use of the RF
  if (s_apActive && millis() - s_apStartMs >= AP_TIMEOUT_MS) {
    s_apActive = false;
    WiFi.softAPdisconnect(true);
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.mode(WIFI_STA);
      Serial.printf("Config AP closed — web server still on http://%s\n",
                    WiFi.localIP().toString().c_str());
    } else {
      s_srv.stop();
      WiFi.mode(WIFI_OFF);
      s_cfg = nullptr;  // stop polling
      Serial.println(F("Config AP closed — WiFi off, BLE-only mode"));
      return;
    }
  }

  s_srv.handleClient();

  if (s_saved) {
    s_saved = false;  // prevent re-entry
    if (s_tft) {
      s_tft->fillScreen(pc(TFT_BLACK));
      s_tft->setTextFont(4);
      s_tft->setTextSize(1);
      s_tft->setTextColor(pc(TFT_GREEN), pc(TFT_BLACK));
      s_tft->drawCentreString("Config saved", 160, 80, 4);
      s_tft->setTextFont(2);
      s_tft->setTextColor(pc(TFT_WHITE), pc(TFT_BLACK));
      s_tft->drawCentreString("Rebooting...", 160, 130, 2);
    }
    delay(1000);
    ESP.restart();
  }
}
