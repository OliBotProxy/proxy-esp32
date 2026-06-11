#include <Arduino.h>
#include "config.h"
#include "provisioning.h"
#include "tunnel_client.h"

#ifdef USE_ETHERNET
// ── Ethernet (ESP32-P4 and similar) ──────────────────────────────────────────
#include <ETH.h>

static bool s_eth_ready = false;

static void onEthEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      log_i("ETH started");
      ETH.setHostname("proxy-esp32");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      log_i("ETH link up");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      log_i("ETH ready, IP: %s", ETH.localIP().toString().c_str());
      s_eth_ready = true;
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      log_w("ETH link lost");
      s_eth_ready = false;
      break;
    default:
      break;
  }
}

static void connectNetwork() {
  log_i("Starting Ethernet (PHY type %d, addr %d, MDC %d, MDIO %d)",
        ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO);
  Network.onEvent(onEthEvent);
#ifdef BOARD_WAVESHARE_ESP32P4_ETH
  ETH.begin(ETH_PHY_IP101, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO,
            ETH_PHY_POWER, ETH_CLK_MODE);
#else
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO,
            ETH_PHY_POWER, ETH_CLK_MODE);
#endif
  uint32_t deadline = millis() + 15000;
  while (!s_eth_ready && millis() < deadline) {
    delay(200);
    Serial.print('.');
  }
  Serial.println();
  if (!s_eth_ready) log_e("Ethernet did not come up within 15 s");
}

static bool networkReady() { return s_eth_ready; }

#else
// ── WiFi (ESP32 DevKit and similar) ──────────────────────────────────────────
#include <WiFi.h>

static void connectNetwork() {
  log_i("Connecting to WiFi: %s", getWifiSsid());
  WiFi.mode(WIFI_STA);
  WiFi.begin(getWifiSsid(), getWifiPass());
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  log_i("WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
}

static bool networkReady() { return WiFi.status() == WL_CONNECTED; }
#endif

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  log_i("proxy-esp32 starting");

  loadParams();
  checkProvisioningReset();

  if (!paramsLoaded()) {
#ifdef USE_ETHERNET
    connectNetwork();   // ETH needs to be up before we can serve the setup page
#endif
    runProvisioning();  // never returns; reboots after form submit
  }

  connectNetwork();
}

void loop() {
  if (!networkReady()) {
    log_w("Network lost, reconnecting...");
    connectNetwork();
    return;
  }

  runTunnelSession();

  log_i("Reconnecting in %d ms...", RECONNECT_DELAY_MS);
  delay(RECONNECT_DELAY_MS);
}
