#include "provisioning.h"
#include "config.h"
#include <Preferences.h>
#include <WebServer.h>
#include <Arduino.h>

#ifndef USE_ETHERNET
  #include <WiFi.h>
  #include <DNSServer.h>
#else
  #include <ETH.h>
#endif

// ── NVS ──────────────────────────────────────────────────────────────────────

#define NVS_NS "proxy-esp32"

static char s_api_host[64]  = {};
static char s_tunnel_id[64] = {};
static char s_api_key[128]  = {};
#ifndef USE_ETHERNET
static char s_wifi_ssid[64] = {};
static char s_wifi_pass[64] = {};
#endif
static bool s_loaded = false;

const char* getApiHost()  { return s_api_host[0] ? s_api_host : "api-us.oli.bot"; }
const char* getTunnelId() { return s_tunnel_id; }
const char* getApiKey()   { return s_api_key;   }
#ifndef USE_ETHERNET
const char* getWifiSsid() { return s_wifi_ssid; }
const char* getWifiPass() { return s_wifi_pass; }
#endif

void loadParams() {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  strlcpy(s_api_host,  prefs.getString("api_host",  "").c_str(), sizeof(s_api_host));
  strlcpy(s_tunnel_id, prefs.getString("tunnel_id", "").c_str(), sizeof(s_tunnel_id));
  strlcpy(s_api_key,   prefs.getString("api_key",   "").c_str(), sizeof(s_api_key));
#ifndef USE_ETHERNET
  strlcpy(s_wifi_ssid, prefs.getString("wifi_ssid", "").c_str(), sizeof(s_wifi_ssid));
  strlcpy(s_wifi_pass, prefs.getString("wifi_pass", "").c_str(), sizeof(s_wifi_pass));
#endif
  prefs.end();

  if (s_api_host[0] == '\0') strlcpy(s_api_host, "api-us.oli.bot", sizeof(s_api_host));

  s_loaded = (strlen(s_tunnel_id) > 0) && (strlen(s_api_key) > 0)
#ifndef USE_ETHERNET
             && (strlen(s_wifi_ssid) > 0)
#endif
  ;
}

bool paramsLoaded() { return s_loaded; }

void clearParams() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
  s_loaded = false;
  log_i("NVS credentials cleared");
}

// ── Reset pin ─────────────────────────────────────────────────────────────────

void checkProvisioningReset() {
  if (PROVISION_RESET_PIN < 0) return;
  pinMode(PROVISION_RESET_PIN, INPUT_PULLUP);
  if (digitalRead(PROVISION_RESET_PIN) != LOW) return;
  log_w("Reset pin held — keep holding 3 s to clear credentials...");
  for (int i = 0; i < 30; i++) {
    delay(100);
    if (digitalRead(PROVISION_RESET_PIN) != LOW) return;
  }
  clearParams();
  log_w("Credentials cleared — restarting into provisioning mode");
  delay(300);
  ESP.restart();
}

// ── HTML ──────────────────────────────────────────────────────────────────────

static const char HTML_PAGE[] =
  "<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Tunnel Setup</title>"
  "<style>"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#0f1117;color:#e2e8f0;display:flex;align-items:center;"
    "justify-content:center;min-height:100vh;padding:1rem}"
  ".card{background:#1a1f2e;border:1px solid #2d3748;border-radius:12px;"
    "padding:2rem;width:100%;max-width:400px}"
  "h2{font-size:1.2rem;margin-bottom:.3rem}"
  ".sub{color:#718096;font-size:.82rem;margin-bottom:1.5rem}"
  "label{display:block;font-size:.78rem;font-weight:600;color:#a0aec0;margin-bottom:.3rem}"
  "input,select{display:block;width:100%;padding:.55rem .75rem;background:#2d3748;"
    "border:1px solid #4a5568;border-radius:7px;color:#e2e8f0;"
    "font-size:.88rem;margin-bottom:1rem}"
  "input:focus,select:focus{border-color:#4299e1;outline:none}"
  ".hint{font-size:.72rem;color:#718096;margin-top:-.7rem;margin-bottom:.85rem}"
  "hr{border:none;border-top:1px solid #2d3748;margin:1.1rem 0}"
  ".err{background:#742a2a;border:1px solid #9b2c2c;border-radius:7px;"
    "padding:.6rem .75rem;font-size:.85rem;margin-bottom:1rem;color:#feb2b2}"
  "button{width:100%;padding:.75rem;background:#3182ce;border:none;"
    "border-radius:7px;color:#fff;font-weight:600;font-size:.95rem;cursor:pointer;margin-top:.5rem}"
  "button:hover{background:#2b6cb0}"
  "</style></head><body><div class='card'>"
  "<h2>Tunnel Setup</h2>"
  "<p class='sub'>Credentials are saved to device flash. "
    "Hold the reset button 3&nbsp;s to clear.</p>"
  "<form method='POST' action='/'>"
  "{ERROR}"
  "{WIFI_FIELDS}"
  "<hr>"
  "<label>API Region</label>"
  "<select name='host'>"
    "<option value='api-us.oli.bot'{SEL_US}>United States</option>"
    "<option value='api-eu.oli.bot'{SEL_EU}>Europe</option>"
    "<option value='api-asia.oli.bot'{SEL_ASIA}>Asia</option>"
  "</select>"
  "<label>Tunnel ID</label>"
  "<input name='tid' value='{TID}' autocomplete='off' spellcheck='false' required>"
  "<label>API Key</label>"
  "<input name='key' type='password' value='{KEY}' autocomplete='new-password' required>"
  "<div class='hint'>Format: subscriptionId_salt &mdash; from the proxy-admin dashboard</div>"
  "<button type='submit'>Save &amp; Restart</button>"
  "</form></div></body></html>";

static const char WIFI_FIELDS[] =
  "<label>WiFi SSID</label>"
  "<input name='ssid' value='{SSID}' autocomplete='off' spellcheck='false' required>"
  "<label>WiFi Password</label>"
  "<input name='pass' type='password' value='{PASS}' autocomplete='new-password'>";

static const char HTML_DONE[] =
  "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Saved</title>"
  "<style>body{font-family:sans-serif;background:#0f1117;color:#e2e8f0;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh}"
  "h2{margin-bottom:.5rem}.sub{color:#718096;font-size:.85rem}"
  "</style></head><body><div style='text-align:center'>"
  "<h2>Saved. Restarting...</h2>"
  "<p class='sub'>The device will reconnect with the new credentials.</p>"
  "</div></body></html>";

static String buildPage(const String& error = "") {
  String html = HTML_PAGE;

#ifndef USE_ETHERNET
  String wf = WIFI_FIELDS;
  wf.replace("{SSID}", s_wifi_ssid);
  wf.replace("{PASS}", s_wifi_pass);
  html.replace("{WIFI_FIELDS}", wf);
#else
  html.replace("{WIFI_FIELDS}", "");
#endif

  html.replace("{ERROR}", error.isEmpty() ? "" :
    "<div class='err'>" + error + "</div>");

  String host = s_api_host;
  html.replace("{SEL_US}",   host == "api-us.oli.bot"   ? " selected" : "");
  html.replace("{SEL_EU}",   host == "api-eu.oli.bot"   ? " selected" : "");
  html.replace("{SEL_ASIA}", host == "api-asia.oli.bot"  ? " selected" : "");
  html.replace("{TID}", String(s_tunnel_id));
  html.replace("{KEY}", String(s_api_key));
  return html;
}

// ── WebServer handlers ────────────────────────────────────────────────────────

static WebServer s_server(80);
static bool      s_restart = false;

static void handleGet() {
  s_server.send(200, "text/html; charset=utf-8", buildPage());
}

static void handlePost() {
  String tid  = s_server.arg("tid");  tid.trim();
  String key  = s_server.arg("key");  key.trim();
  String host = s_server.arg("host"); host.trim();

#ifndef USE_ETHERNET
  String ssid = s_server.arg("ssid"); ssid.trim();
  String pass = s_server.arg("pass");
  if (ssid.isEmpty()) {
    s_server.send(200, "text/html; charset=utf-8",
                  buildPage("WiFi SSID is required."));
    return;
  }
#endif

  if (tid.isEmpty()) {
    s_server.send(200, "text/html; charset=utf-8",
                  buildPage("Tunnel ID is required."));
    return;
  }
  if (key.isEmpty() || key.indexOf('_') < 0) {
    s_server.send(200, "text/html; charset=utf-8",
                  buildPage("API Key is required and must be in subscriptionId_salt format."));
    return;
  }

  if (host.isEmpty()) host = "api-us.oli.bot";

  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putString("tunnel_id", tid);
  prefs.putString("api_key",   key);
  prefs.putString("api_host",  host);
#ifndef USE_ETHERNET
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
#endif
  prefs.end();

  log_i("Credentials saved to NVS");
  s_server.send(200, "text/html; charset=utf-8", HTML_DONE);
  s_restart = true;
}

static void handleRedirect() {
  s_server.sendHeader("Location", "/");
  s_server.send(302, "text/plain", "");
}

// ── runProvisioning ───────────────────────────────────────────────────────────

void runProvisioning() {
  s_server.on("/",    HTTP_GET,  handleGet);
  s_server.on("/",    HTTP_POST, handlePost);
  s_server.onNotFound(handleRedirect);

#ifndef USE_ETHERNET
  // ── WiFi: SoftAP + DNS captive portal ────────────────────────────────────
  char ap_ssid[32];
  snprintf(ap_ssid, sizeof(ap_ssid), "%s-%06lX",
           PROVISION_AP_PREFIX, (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid);
  delay(200);

  IPAddress ap_ip = WiFi.softAPIP();
  log_i("--- Provisioning mode ---");
  log_i("Connect to WiFi: %s", ap_ssid);
  log_i("Then open:       http://%s", ap_ip.toString().c_str());

  DNSServer dns;
  dns.start(53, "*", ap_ip);
  s_server.begin();

  while (!s_restart) {
    dns.processNextRequest();
    s_server.handleClient();
    delay(2);
  }

#else
  // ── Ethernet: serve on current IP ────────────────────────────────────────
  // Caller (main.cpp) must have already called connectNetwork().
  log_i("--- Provisioning mode ---");
  log_i("Open: http://%s", ETH.localIP().toString().c_str());

  s_server.begin();
  while (!s_restart) {
    s_server.handleClient();
    delay(2);
  }
#endif

  delay(800);  // let browser receive the done page before reset
  ESP.restart();
}
