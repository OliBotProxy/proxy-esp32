#include <Arduino.h>
#include <WebServer.h>
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
  // Disable modem sleep. The Arduino default (WIFI_PS_MIN_MODEM) parks the radio
  // between DTIM beacons, which adds beacon-interval latency to every TCP
  // round-trip and throttles sustained transfers to a fraction of link speed
  // (measured here: ~180 kbit/s relaying a 1 MB response, vs multi-Mbit/s with
  // sleep off). This device is mains-powered and must relay traffic promptly,
  // so trade idle current for throughput.
  WiFi.setSleep(false);
  WiFi.begin(getWifiSsid(), getWifiPass());
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  log_i("WiFi connected, IP: %s (modem sleep disabled)", WiFi.localIP().toString().c_str());
}

static bool networkReady() { return WiFi.status() == WL_CONNECTED; }
#endif

static String getLocalIpString() {
#ifdef USE_ETHERNET
  return ETH.localIP().toString();
#else
  return WiFi.localIP().toString();
#endif
}

// ── Demo local web server ───────────────────────────────────────────────────
// Runs on its own FreeRTOS task (core 0) so it's fully independent of the
// tunnel session's blocking loop() on core 1. This is the "local backend"
// the tunnel forwards requests to — point a domain's localIp at this
// device's own IP:80 in the proxy-admin dashboard to expose it.

static WebServer s_demoServer(80);

static void handleDemoRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>proxy-esp32 demo</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "background:#0f1117;color:#e2e8f0;display:flex;align-items:center;"
      "justify-content:center;min-height:100vh;margin:0;padding:1rem}"
    ".card{background:#1a1f2e;border:1px solid #2d3748;border-radius:12px;"
      "padding:2rem 2.5rem;max-width:420px}"
    "h1{font-size:1.3rem;margin:0 0 .3rem}"
    ".sub{color:#718096;font-size:.85rem;margin-bottom:1.2rem}"
    "table{width:100%;font-size:.85rem;border-collapse:collapse}"
    "td{padding:.35rem 0;border-top:1px solid #2d3748}"
    "td:first-child{color:#a0aec0}"
    "td:last-child{text-align:right;font-family:ui-monospace,monospace}"
    "</style></head><body><div class='card'>"
    "<h1>&#9989; It works!</h1>"
    "<p class='sub'>Served locally by this ESP32 and reached through an oli.bot tunnel.</p>"
    "<table>"
    "<tr><td>Uptime</td><td>" + String(millis() / 1000) + " s</td></tr>"
    "<tr><td>Free heap</td><td>" + String(ESP.getFreeHeap() / 1024) + " KB</td></tr>"
    "<tr><td>Local address</td><td>" + getLocalIpString() + ":80</td></tr>"
    "<tr><td>Chip</td><td>" + String(ESP.getChipModel()) + "</td></tr>"
    "</table>"
    "</div></body></html>";
  s_demoServer.send(200, "text/html; charset=utf-8", html);
}

static void demoServerTask(void*) {
  s_demoServer.on("/", handleDemoRoot);
  s_demoServer.onNotFound(handleDemoRoot);
  s_demoServer.begin();
  log_i("Demo web server listening on %s:80", getLocalIpString().c_str());
  for (;;) {
    s_demoServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

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

  xTaskCreatePinnedToCore(demoServerTask, "demoServer", 4096, nullptr, 1, nullptr, 0);
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
