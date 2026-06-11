#pragma once
#include <Arduino.h>

// Read credentials from NVS into internal state. Call once at boot.
void loadParams();

// Returns true if all required credentials are present in NVS.
bool paramsLoaded();

// Erase all credentials from NVS.
void clearParams();

// Hold PROVISION_RESET_PIN LOW for 3 s at boot to clear credentials and restart.
void checkProvisioningReset();

// Start the provisioning web server. Never returns — reboots after form submit.
//   WiFi build : starts SoftAP + DNS captive portal on 192.168.4.1
//   ETH build  : serves on the current Ethernet IP (call connectNetwork first)
void runProvisioning();

// Credential getters — valid after loadParams() succeeds.
const char* getApiHost();     // e.g. "api-us.oli.bot"
const char* getTunnelId();
const char* getApiKey();      // "<subscriptionId>_<salt>"
#ifndef USE_ETHERNET
const char* getWifiSsid();
const char* getWifiPass();
#endif
