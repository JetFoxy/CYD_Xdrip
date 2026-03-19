#include "M5NSconfig.h"

void readConfiguration(char *iniFilename, tConfig *cfg) {
  const size_t bufferLen = 80;
  char buffer[bufferLen];

  IniFile ini(iniFilename);

  if (!ini.open()) {
    Serial.printf("Config file %s not found, using defaults\n", iniFilename);
    return;
  }

  if (!ini.validate(buffer, bufferLen)) {
    Serial.println("Config file invalid, using defaults");
    return;
  }

  if (ini.getValue("config", "show_mgdl", buffer, bufferLen)) {
    cfg->show_mgdl = atoi(buffer);
    Serial.printf("show_mgdl = %d\n", cfg->show_mgdl);
  }

  if (ini.getValue("config", "blepassword", buffer, bufferLen)) {
    strlcpy(cfg->blepassword, buffer, sizeof(cfg->blepassword));
    Serial.println("blepassword loaded from config");
  }

  if (ini.getValue("config", "utc_offset_min", buffer, bufferLen)) {
    cfg->utc_offset_min = atoi(buffer);
    Serial.printf("utc_offset_min = %d\n", cfg->utc_offset_min);
  }

  if (ini.getValue("config", "bg_low", buffer, bufferLen)) {
    cfg->bg_low = atof(buffer);
    Serial.printf("bg_low = %.1f\n", cfg->bg_low);
  }
  if (ini.getValue("config", "bg_warn_low", buffer, bufferLen)) {
    cfg->bg_warn_low = atof(buffer);
    Serial.printf("bg_warn_low = %.1f\n", cfg->bg_warn_low);
  }
  if (ini.getValue("config", "bg_warn_high", buffer, bufferLen)) {
    cfg->bg_warn_high = atof(buffer);
    Serial.printf("bg_warn_high = %.1f\n", cfg->bg_warn_high);
  }
  if (ini.getValue("config", "bg_high", buffer, bufferLen)) {
    cfg->bg_high = atof(buffer);
    Serial.printf("bg_high = %.1f\n", cfg->bg_high);
  }

  if (ini.getValue("config", "brightness", buffer, bufferLen)) {
    int v = atoi(buffer);
    if (v >= 0 && v <= 2) cfg->brightness = v;
    Serial.printf("brightness = %d\n", cfg->brightness);
  }

  if (ini.getValue("wifi", "ssid", buffer, bufferLen)) {
    strlcpy(cfg->wifi_ssid, buffer, sizeof(cfg->wifi_ssid));
    Serial.println("wifi_ssid loaded");
  }
  if (ini.getValue("wifi", "password", buffer, bufferLen)) {
    strlcpy(cfg->wifi_password, buffer, sizeof(cfg->wifi_password));
    Serial.println("wifi_password loaded");
  }
  if (ini.getValue("nightscout", "url", buffer, bufferLen)) {
    strlcpy(cfg->ns_url, buffer, sizeof(cfg->ns_url));
    Serial.printf("ns_url = %s\n", cfg->ns_url);
  }
  if (ini.getValue("nightscout", "token", buffer, bufferLen)) {
    strlcpy(cfg->ns_token, buffer, sizeof(cfg->ns_token));
    Serial.println("ns_token loaded");
  }
}
