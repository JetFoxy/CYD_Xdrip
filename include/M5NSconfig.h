#ifndef _M5NSCONFIG_H
#define _M5NSCONFIG_H

#include "IniFile.h"

struct tConfig {
  int   show_mgdl         = 0;    // 0 = mmol/L, 1 = mg/dL (overridden by BLE packet)
  char  blepassword[64]   = {0};  // BLE auth password; if empty, device auto-generates one
  int   utc_offset_min    = 0;    // UTC offset in minutes (e.g. 180 for UTC+3), fallback if BLE doesn't send tz
  float bg_low            = 3.9f; // BG threshold: low
  float bg_warn_low       = 4.5f; // BG threshold: warning low
  float bg_warn_high      = 9.0f; // BG threshold: warning high
  float bg_high           = 10.0f;// BG threshold: high
  int   brightness        = 2;    // initial brightness level: 0=dim, 1=mid, 2=bright
  char  wifi_ssid[64]     = {0};  // WiFi SSID (leave empty to disable WiFi)
  char  wifi_password[64] = {0};  // WiFi password
  char  ns_url[128]       = {0};  // Nightscout URL, e.g. https://mysite.herokuapp.com
  char  ns_token[64]      = {0};  // Nightscout access token (optional)
};

void readConfiguration(char *iniFilename, tConfig *cfg);

#endif
