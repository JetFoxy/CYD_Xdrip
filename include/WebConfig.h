#ifndef _WEBCONFIG_H
#define _WEBCONFIG_H

#include "CYDconfig.h"
#include <TFT_eSPI.h>

// Load WiFi/Nightscout/NTP settings from NVS.
// Returns true if any values were found (i.e. not a first boot).
bool loadCYDConfigFromNVS(tConfig &cfg);

// Save WiFi/Nightscout/NTP settings to NVS.
void saveCYDConfigToNVS(const tConfig &cfg);

// Start the config AP ("CYDrip-Setup") and the web server.
// Call once in setup(), after WiFi is initialised.
// tft + invertColors are used to show a "Rebooting..." screen on save.
void webConfigBegin(tConfig &cfg, TFT_eSPI &tft, bool invertColors);

// Poll the web server — call every loop iteration.
// Reboots the device if the user just saved new settings.
void webConfigHandle();

#endif
