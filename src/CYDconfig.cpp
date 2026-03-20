#include "CYDconfig.h"

void readCYDConfig(const char *iniFilename, tConfig *cfg) {
  const size_t bufLen = 128;
  char buf[bufLen];

  IniFile ini(iniFilename);
  if (!ini.open()) {
    Serial.printf("Config file %s not found, using defaults\n", iniFilename);
    return;
  }
  if (!ini.validate(buf, bufLen)) {
    Serial.println("Config file invalid, using defaults");
    return;
  }

  // [config]
  if (ini.getValue("config", "show_mgdl", buf, bufLen))
    cfg->show_mgdl = atoi(buf);

  if (ini.getValue("config", "blepassword", buf, bufLen))
    strlcpy(cfg->blepassword, buf, sizeof(cfg->blepassword));

  if (ini.getValue("config", "utc_offset_min", buf, bufLen))
    cfg->utc_offset_min = atoi(buf);

  if (ini.getValue("config", "bg_low", buf, bufLen))
    cfg->bg_low = atof(buf);

  if (ini.getValue("config", "bg_warn_low", buf, bufLen))
    cfg->bg_warn_low = atof(buf);

  if (ini.getValue("config", "bg_warn_high", buf, bufLen))
    cfg->bg_warn_high = atof(buf);

  if (ini.getValue("config", "bg_high", buf, bufLen))
    cfg->bg_high = atof(buf);

  if (ini.getValue("config", "brightness", buf, bufLen)) {
    int v = atoi(buf);
    if (v >= 0 && v <= 2) cfg->brightness = v;
  }

  if (ini.getValue("config", "rotation", buf, bufLen)) {
    int v = atoi(buf);
    if (v == 0 || v == 1) cfg->rotation = v;
  }

  if (ini.getValue("config", "ntp_server", buf, bufLen))
    strlcpy(cfg->ntp_server, buf, sizeof(cfg->ntp_server));

  // [wifi]
  if (ini.getValue("wifi", "ssid", buf, bufLen))
    strlcpy(cfg->wifi_ssid, buf, sizeof(cfg->wifi_ssid));

  if (ini.getValue("wifi", "password", buf, bufLen))
    strlcpy(cfg->wifi_password, buf, sizeof(cfg->wifi_password));

  // [nightscout]
  if (ini.getValue("nightscout", "url", buf, bufLen))
    strlcpy(cfg->ns_url, buf, sizeof(cfg->ns_url));

  if (ini.getValue("nightscout", "token", buf, bufLen))
    strlcpy(cfg->ns_token, buf, sizeof(cfg->ns_token));

  Serial.println(F("CYD.INI loaded"));
}
