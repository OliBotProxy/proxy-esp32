// Tunnel client for ESP32 — protocol v2
//
// Build variants (set via platformio.ini build_flags):
//   default          — plain TCP to proxy (port 8779), WiFi
//   -DUSE_TLS_TUNNEL — TLS to proxy (port 8778), setInsecure by default
//                      add -DTUNNEL_TLS_VERIFY_CERT to validate ISRG Root X1
//   -DUSE_ETHERNET   — Ethernet instead of WiFi (main.cpp handles init)
//
// Single-stream: one request handled at a time.
// 4 KB max REQUEST payload; larger requests answered with RESET.

#include "tunnel_client.h"
#include "tunnel_protocol.h"
#include "config.h"
#include <WiFiClientSecure.h>
#include <Arduino.h>
#ifdef TUNNEL_TLS_VERIFY_CERT
  #include "certs.h"
#endif

// ── Module-level state ────────────────────────────────────────────────────────

// g_tunnel is WiFiClientSecure for both TLS and plain builds:
//   - TLS build: handshake happens, cert optionally verified
//   - Plain build: connect() without startHandshake → plain TCP via the same class
// Using a single type avoids duplicating all I/O helpers.
#ifdef USE_TLS_TUNNEL
static WiFiClientSecure g_tunnel;
#else
static WiFiClient g_tunnel;
#endif
static WiFiClient g_backend;  // per-request TCP connection to local backend

// Called once before the first session. Sets TLS parameters when USE_TLS_TUNNEL.
static void initTunnelClient() {
#ifdef USE_TLS_TUNNEL
  #ifdef TUNNEL_TLS_VERIFY_CERT
    g_tunnel.setCACert(ISRG_ROOT_X1_CERT);
    log_i("TLS tunnel: cert validation enabled (ISRG Root X1)");
  #else
    g_tunnel.setInsecure();
    log_w("TLS tunnel: cert validation disabled (setInsecure). "
          "Define TUNNEL_TLS_VERIFY_CERT in config.h for production.");
  #endif
#endif
}

// Backend address populated from the first CONFIG domain mapping
static char     g_backend_host[128];
static uint16_t g_backend_port;

// PING keepalive tracking
static uint32_t g_last_ping_ms;
static bool     g_ping_pending;

// Shared buffers (single-stream: REQUEST and body are sequential, not concurrent)
static uint8_t g_frame_buf[MAX_REQUEST_PAYLOAD];  // incoming REQUEST payload
static uint8_t g_body_buf[MAX_RESPONSE_BODY];     // request body in / response body out
static uint8_t g_drain_buf[256];                  // for draining unknown/oversized payloads

// ── Low-level tunnel I/O ──────────────────────────────────────────────────────

static bool tunnelWrite(const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    if (!g_tunnel.connected()) return false;
    int n = g_tunnel.write(data + sent, len - sent);
    if (n <= 0) return false;
    sent += n;
  }
  return true;
}

// Read exactly `len` bytes from tunnel socket; timeout_ms per byte-group wait.
static bool tunnelRead(uint8_t* buf, size_t len, uint32_t timeout_ms = 5000) {
  size_t got = 0;
  uint32_t deadline = millis() + timeout_ms;
  while (got < len) {
    if (!g_tunnel.connected()) return false;
    int avail = g_tunnel.available();
    if (avail > 0) {
      int n = g_tunnel.read(buf + got, min((size_t)avail, len - got));
      if (n > 0) got += n;
    } else {
      if ((int32_t)(millis() - deadline) >= 0) return false;
      delay(2);
    }
  }
  return true;
}

// Drain `len` bytes from tunnel socket, discarding them.
static bool tunnelDrain(uint32_t len) {
  while (len > 0) {
    uint32_t chunk = min(len, (uint32_t)sizeof(g_drain_buf));
    if (!tunnelRead(g_drain_buf, chunk)) return false;
    len -= chunk;
  }
  return true;
}

// ── Frame send helpers ────────────────────────────────────────────────────────

static bool sendFrame(uint8_t type, uint32_t stream_id, uint8_t flags,
                      const uint8_t* payload, uint32_t payload_len) {
  uint8_t hdr[FRAME_HEADER_SIZE];
  hdr[0] = type;
  hdr[1] = (stream_id >> 24) & 0xFF;
  hdr[2] = (stream_id >> 16) & 0xFF;
  hdr[3] = (stream_id >>  8) & 0xFF;
  hdr[4] =  stream_id        & 0xFF;
  hdr[5] = flags;
  hdr[6] = (payload_len >> 24) & 0xFF;
  hdr[7] = (payload_len >> 16) & 0xFF;
  hdr[8] = (payload_len >>  8) & 0xFF;
  hdr[9] =  payload_len        & 0xFF;
  if (!tunnelWrite(hdr, FRAME_HEADER_SIZE)) return false;
  if (payload_len > 0 && !tunnelWrite(payload, payload_len)) return false;
  return true;
}

static void sendReset(uint32_t stream_id, uint16_t error_code) {
  uint8_t p[2] = { (uint8_t)(error_code >> 8), (uint8_t)(error_code & 0xFF) };
  sendFrame(FRAME_RESET, stream_id, 0, p, 2);
}

static void sendPing() {
  uint8_t opaque[8] = {};
  sendFrame(FRAME_PING, 0, 0, opaque, 8);
  g_ping_pending = true;
  g_last_ping_ms = millis();
  log_d("PING sent");
}

// ── API: fetch proxy address ──────────────────────────────────────────────────

static bool extractJsonString(const String& json, const char* field, String& out) {
  String key = String('"') + field + '"';
  int idx = json.indexOf(key);
  if (idx < 0) return false;
  int colon = json.indexOf(':', idx + key.length());
  if (colon < 0) return false;
  int q1 = json.indexOf('"', colon + 1);
  if (q1 < 0) return false;
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  out = json.substring(q1 + 1, q2);
  return true;
}

// HTTPS GET /api/client-tunnel/{TUNNEL_ID} → proxyAddress field
static String fetchProxyAddress() {
  WiFiClientSecure https;
  https.setInsecure();  // cert validation skipped; fine for bootstrap call
  https.setTimeout(15);

  if (!https.connect(TUNNEL_API_HOST, TUNNEL_API_PORT)) {
    log_e("API connect failed: %s:%d", TUNNEL_API_HOST, TUNNEL_API_PORT);
    return "";
  }

  https.printf("GET /api/client-tunnel/%s HTTP/1.1\r\n", TUNNEL_ID);
  https.printf("Host: %s\r\n", TUNNEL_API_HOST);
  https.printf("Authorization: Bearer %s\r\n", TUNNEL_API_KEY);
  https.print("Connection: close\r\n\r\n");

  // Read full response (headers + body)
  String response;
  uint32_t deadline = millis() + 12000;
  while ((https.connected() || https.available()) && millis() < deadline) {
    while (https.available()) response += (char)https.read();
    delay(5);
  }
  https.stop();

  int body_start = response.indexOf("\r\n\r\n");
  if (body_start < 0) { log_e("API response malformed"); return ""; }

  // Verify 200 OK on the status line
  int first_lf = response.indexOf('\r');
  if (response.substring(0, first_lf > 0 ? first_lf : 12).indexOf("200") < 0) {
    log_e("API error: %s", response.substring(0, 80).c_str());
    return "";
  }

  String body = response.substring(body_start + 4);
  String addr;
  if (!extractJsonString(body, "proxyAddress", addr) || addr.isEmpty()) {
    log_e("No proxyAddress in: %s", body.c_str());
    return "";
  }
  return addr;
}

// ── CONFIG frame parser ───────────────────────────────────────────────────────

// Read a u16-length-prefixed string from payload at *pos into out (null-terminated).
static bool readU16Str(const uint8_t* p, uint32_t plen, uint32_t* pos,
                       char* out, size_t out_max) {
  if (*pos + 2 > plen) return false;
  uint16_t slen = ((uint16_t)p[*pos] << 8) | p[*pos + 1]; *pos += 2;
  if (*pos + slen > plen || slen >= out_max) return false;
  memcpy(out, p + *pos, slen);
  out[slen] = '\0';
  *pos += slen;
  return true;
}

// Read a u8-length-prefixed string.
static bool readU8Str(const uint8_t* p, uint32_t plen, uint32_t* pos,
                      char* out, size_t out_max) {
  if (*pos >= plen) return false;
  uint8_t slen = p[(*pos)++];
  if (*pos + slen > plen || slen >= out_max) return false;
  memcpy(out, p + *pos, slen);
  out[slen] = '\0';
  *pos += slen;
  return true;
}

static bool parseConfig(const uint8_t* payload, uint32_t len) {
  if (len < 2) return false;
  uint16_t count = ((uint16_t)payload[0] << 8) | payload[1];
  uint32_t pos = 2;
  char domain_str[128], local_host[128];

  for (uint16_t i = 0; i < count; i++) {
    if (pos + 3 > len) return false;
    // domain_id (u16), flags (u8) — we only need local_host for the first entry
    pos += 3;
    if (!readU16Str(payload, len, &pos, domain_str, sizeof(domain_str))) return false;
    if (!readU16Str(payload, len, &pos, local_host, sizeof(local_host))) return false;

    if (i == 0) {
      char* colon = strrchr(local_host, ':');
      if (colon) {
        *colon = '\0';
        g_backend_port = (uint16_t)atoi(colon + 1);
      } else {
        g_backend_port = 80;
      }
      strlcpy(g_backend_host, local_host, sizeof(g_backend_host));
      log_i("Backend from CONFIG: %s:%u", g_backend_host, g_backend_port);
    }
  }
  return count > 0;
}

// ── Chunked transfer decoding ─────────────────────────────────────────────────

static uint32_t dechunk(uint8_t* data, uint32_t len) {
  uint32_t out = 0, pos = 0;
  while (pos < len) {
    uint32_t line_end = pos;
    while (line_end + 1 < len && !(data[line_end] == '\r' && data[line_end + 1] == '\n'))
      line_end++;
    if (line_end + 1 >= len) break;
    char sz[12] = {};
    uint32_t sz_len = min((uint32_t)(line_end - pos), (uint32_t)(sizeof(sz) - 1));
    memcpy(sz, data + pos, sz_len);
    uint32_t chunk_size = (uint32_t)strtoul(sz, nullptr, 16);
    pos = line_end + 2;
    if (chunk_size == 0) break;
    if (pos + chunk_size > len) { chunk_size = len - pos; }
    memmove(data + out, data + pos, chunk_size);
    out += chunk_size;
    pos += chunk_size + 2;  // skip chunk data + trailing \r\n
  }
  return out;
}

// ── REQUEST handler ───────────────────────────────────────────────────────────

static void handleRequest(uint32_t stream_id, uint8_t req_flags,
                          const uint8_t* payload, uint32_t payload_len) {
  uint32_t pos = 0;

  // domain_id — ignored (single backend on ESP32)
  if (pos + 2 > payload_len) { sendReset(stream_id, RESET_INTERNAL_ERROR); return; }
  pos += 2;

  // method (u8-str), path (u16-str)
  char method[16], path[512];
  if (!readU8Str(payload, payload_len, &pos, method, sizeof(method)) ||
      !readU16Str(payload, payload_len, &pos, path, sizeof(path))) {
    sendReset(stream_id, RESET_INTERNAL_ERROR); return;
  }

  // headers: u16 count, then (u8-str name, u16-str value) pairs
  if (pos + 2 > payload_len) { sendReset(stream_id, RESET_INTERNAL_ERROR); return; }
  uint16_t hdr_count = ((uint16_t)payload[pos] << 8) | payload[pos + 1]; pos += 2;

  // Build outgoing HTTP/1.1 request line
  String http_req = String(method) + ' ' + path + " HTTP/1.1\r\n";
  char hname[64], hval[512];

  for (uint16_t i = 0; i < hdr_count; i++) {
    if (!readU8Str(payload, payload_len, &pos, hname, sizeof(hname)) ||
        !readU16Str(payload, payload_len, &pos, hval, sizeof(hval))) {
      sendReset(stream_id, RESET_INTERNAL_ERROR); return;
    }
    String ln = hname; ln.toLowerCase();
    if (ln == "connection" || ln == "keep-alive") continue;
    http_req += String(hname) + ": " + hval + "\r\n";
  }
  http_req += "Connection: close\r\n";

  // Collect request body from DATA frames if HAS_BODY
  uint32_t body_len = 0;
  if (req_flags & FLAG_HAS_BODY) {
    uint8_t dhdr[FRAME_HEADER_SIZE];
    bool done = false;
    while (!done) {
      if (!tunnelRead(dhdr, FRAME_HEADER_SIZE, 8000)) {
        sendReset(stream_id, RESET_INTERNAL_ERROR); return;
      }
      uint8_t  dtype  = dhdr[0];
      uint8_t  dflags = dhdr[5];
      uint32_t dlen   = ((uint32_t)dhdr[6] << 24) | ((uint32_t)dhdr[7] << 16) |
                        ((uint32_t)dhdr[8] <<  8) | dhdr[9];

      if (dtype == FRAME_DATA) {
        uint32_t to_read = min(dlen, (uint32_t)MAX_RESPONSE_BODY - body_len);
        if (to_read > 0) {
          if (!tunnelRead(g_body_buf + body_len, to_read)) {
            sendReset(stream_id, RESET_INTERNAL_ERROR); return;
          }
          body_len += to_read;
        }
        if (dlen > to_read) tunnelDrain(dlen - to_read);  // body overflow
        if (dflags & FLAG_END_STREAM) done = true;
      } else if (dtype == FRAME_PING) {
        uint8_t opaque[8] = {};
        if (dlen >= 8) tunnelRead(opaque, 8);
        else if (dlen > 0) tunnelDrain(dlen);
        sendFrame(FRAME_PONG, 0, 0, opaque, 8);
      } else {
        if (!tunnelDrain(dlen)) { sendReset(stream_id, RESET_INTERNAL_ERROR); return; }
      }
    }
    http_req += "Content-Length: " + String(body_len) + "\r\n";
  }
  http_req += "\r\n";

  // Connect to local backend
  if (!g_backend.connect(g_backend_host, g_backend_port)) {
    log_e("Backend unreachable: %s:%u", g_backend_host, g_backend_port);
    sendReset(stream_id, RESET_BACKEND_UNREACHABLE); return;
  }
  g_backend.setTimeout(10);

  // Send request + optional body
  g_backend.print(http_req);
  if (body_len > 0) g_backend.write(g_body_buf, body_len);
  g_backend.flush();

  // Read response headers until \r\n\r\n
  String resp_raw;
  uint32_t deadline = millis() + 10000;
  bool headers_done = false;
  while (millis() < deadline) {
    while (g_backend.available()) {
      resp_raw += (char)g_backend.read();
      if (resp_raw.endsWith("\r\n\r\n")) { headers_done = true; break; }
    }
    if (headers_done) break;
    if (!g_backend.connected() && !g_backend.available()) break;
    delay(2);
  }
  if (!headers_done) {
    g_backend.stop();
    sendReset(stream_id, RESET_BACKEND_TIMEOUT); return;
  }

  // Parse status code
  int sp1 = resp_raw.indexOf(' ');
  int sp2 = resp_raw.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) {
    g_backend.stop(); sendReset(stream_id, RESET_INTERNAL_ERROR); return;
  }
  uint16_t status_code = (uint16_t)resp_raw.substring(sp1 + 1, sp2).toInt();

  // Parse response header lines into name/value arrays
  struct RHdr { String name, value; };
  RHdr resp_headers[32];
  uint16_t resp_hdr_count = 0;
  int32_t content_length = -1;
  bool is_chunked = false;
  bool no_body = (status_code >= 100 && status_code < 200) ||
                  status_code == 204 || status_code == 304 ||
                  String(method).equalsIgnoreCase("HEAD");

  int line_start = resp_raw.indexOf('\n') + 1;  // skip status line
  while (line_start > 0 && line_start < (int)resp_raw.length() && resp_hdr_count < 32) {
    int line_end = resp_raw.indexOf('\r', line_start);
    if (line_end <= line_start) break;
    String line = resp_raw.substring(line_start, line_end);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String n = line.substring(0, colon); n.trim();
      String v = line.substring(colon + 1); v.trim();
      String nl = n; nl.toLowerCase();
      if (nl == "content-length") content_length = v.toInt();
      if (nl == "transfer-encoding" && v.indexOf("chunked") >= 0) is_chunked = true;
      if (nl == "transfer-encoding") { line_start = line_end + 2; continue; }  // dechunk ourselves
      resp_headers[resp_hdr_count++] = { n, v };
    }
    line_start = line_end + 2;
  }

  // Collect response body
  uint32_t resp_body_len = 0;
  if (!no_body) {
    uint32_t body_deadline = millis() + 10000;
    if (content_length >= 0) {
      uint32_t cl = min((uint32_t)content_length, (uint32_t)MAX_RESPONSE_BODY);
      while (resp_body_len < cl && millis() < body_deadline) {
        while (g_backend.available() && resp_body_len < cl)
          g_body_buf[resp_body_len++] = (uint8_t)g_backend.read();
        if (resp_body_len < cl) delay(2);
      }
    } else if (is_chunked) {
      while (millis() < body_deadline && resp_body_len < MAX_RESPONSE_BODY) {
        while (g_backend.available() && resp_body_len < MAX_RESPONSE_BODY)
          g_body_buf[resp_body_len++] = (uint8_t)g_backend.read();
        // Look for chunked terminator
        if (resp_body_len >= 5) {
          for (uint32_t k = resp_body_len < 10 ? 0 : resp_body_len - 10; k < resp_body_len - 4; k++) {
            if (memcmp(g_body_buf + k, "0\r\n\r\n", 5) == 0) goto body_done;
          }
        }
        if (!g_backend.connected() && !g_backend.available()) break;
        delay(2);
      }
      body_done:
      resp_body_len = dechunk(g_body_buf, resp_body_len);
    } else {
      while (millis() < body_deadline && resp_body_len < MAX_RESPONSE_BODY) {
        while (g_backend.available() && resp_body_len < MAX_RESPONSE_BODY)
          g_body_buf[resp_body_len++] = (uint8_t)g_backend.read();
        if (!g_backend.connected() && !g_backend.available()) break;
        delay(2);
      }
    }
  }
  g_backend.stop();

  // Ensure Content-Length header reflects actual body
  bool has_cl = false;
  for (uint16_t i = 0; i < resp_hdr_count; i++) {
    String nl = resp_headers[i].name; nl.toLowerCase();
    if (nl == "content-length") { resp_headers[i].value = String(resp_body_len); has_cl = true; break; }
  }
  if (!has_cl && resp_hdr_count < 32)
    resp_headers[resp_hdr_count++] = { "content-length", String(resp_body_len) };

  // Build RESPONSE frame payload: u16 status + u16 count + [u8 nlen + name + u16 vlen + value]*
  static uint8_t resp_payload[4096];
  uint32_t rp = 0;
  resp_payload[rp++] = (status_code >> 8) & 0xFF;
  resp_payload[rp++] = status_code & 0xFF;
  resp_payload[rp++] = (resp_hdr_count >> 8) & 0xFF;
  resp_payload[rp++] = resp_hdr_count & 0xFF;
  for (uint16_t i = 0; i < resp_hdr_count && rp < sizeof(resp_payload) - 600; i++) {
    const char* n = resp_headers[i].name.c_str();
    const char* v = resp_headers[i].value.c_str();
    uint8_t  nlen = (uint8_t)min((int)strlen(n), 255);
    uint16_t vlen = (uint16_t)min((int)strlen(v), 1023);
    resp_payload[rp++] = nlen;
    memcpy(resp_payload + rp, n, nlen); rp += nlen;
    resp_payload[rp++] = (vlen >> 8) & 0xFF;
    resp_payload[rp++] = vlen & 0xFF;
    memcpy(resp_payload + rp, v, vlen); rp += vlen;
  }

  uint8_t resp_flags = (resp_body_len > 0) ? FLAG_RESPONSE_HAS_BODY : 0;
  sendFrame(FRAME_RESPONSE, stream_id, resp_flags, resp_payload, rp);

  // Send DATA frames (DATA_CHUNK_SIZE bytes each, END_STREAM on last)
  if (resp_body_len > 0) {
    static uint8_t data_payload[DATA_CHUNK_SIZE];
    uint32_t sent = 0;
    while (sent < resp_body_len) {
      uint32_t chunk = min((uint32_t)DATA_CHUNK_SIZE, resp_body_len - sent);
      bool last = (sent + chunk == resp_body_len);
      memcpy(data_payload, g_body_buf + sent, chunk);
      sendFrame(FRAME_DATA, stream_id, last ? FLAG_END_STREAM : 0, data_payload, chunk);
      sent += chunk;
    }
  }

  log_d("stream %lu: %s %s -> %u (%lu B)", (unsigned long)stream_id,
        method, path, status_code, (unsigned long)resp_body_len);
}

// ── Session ───────────────────────────────────────────────────────────────────

void runTunnelSession() {
  initTunnelClient();

  // 1. Fetch proxy address from API
  String proxy_addr = fetchProxyAddress();
  if (proxy_addr.isEmpty()) { log_e("Failed to fetch proxy address"); return; }

#ifdef USE_TLS_TUNNEL
  const uint16_t default_port = TUNNEL_PROXY_TLS_PORT_DEFAULT;
  const char* mode = "TLS";
#else
  const uint16_t default_port = TUNNEL_PROXY_PORT_DEFAULT;
  const char* mode = "plain TCP";
#endif

  int colon = proxy_addr.lastIndexOf(':');
  String proxy_host = (colon >= 0) ? proxy_addr.substring(0, colon) : proxy_addr;
  uint16_t proxy_port = (colon >= 0) ? (uint16_t)proxy_addr.substring(colon + 1).toInt()
                                      : default_port;
  log_i("Proxy (%s): %s:%u", mode, proxy_host.c_str(), proxy_port);

  // 2. Connect to proxy
  if (!g_tunnel.connect(proxy_host.c_str(), proxy_port)) {
    log_e("%s connect to proxy failed", mode); return;
  }
  log_i("Connected to proxy (%s)", mode);

  // 3. Send CONNECT frame
  // Payload: version(1) + caps(1) + u16-len + tunnel_id + u16-len + api_key
  {
    uint8_t buf[512];
    uint32_t p = 0;
    buf[p++] = PROTOCOL_VERSION;
    buf[p++] = 0x00;  // no HTTP/2 or TLS backend capability

    uint16_t tid_len = (uint16_t)strlen(TUNNEL_ID);
    buf[p++] = (tid_len >> 8) & 0xFF; buf[p++] = tid_len & 0xFF;
    memcpy(buf + p, TUNNEL_ID, tid_len); p += tid_len;

    uint16_t key_len = (uint16_t)strlen(TUNNEL_API_KEY);
    buf[p++] = (key_len >> 8) & 0xFF; buf[p++] = key_len & 0xFF;
    memcpy(buf + p, TUNNEL_API_KEY, key_len); p += key_len;

    if (!sendFrame(FRAME_CONNECT, 0, 0, buf, p)) {
      log_e("CONNECT send failed"); g_tunnel.stop(); return;
    }
  }
  log_i("Sent CONNECT (tunnel_id=%s)", TUNNEL_ID);

  // 4. Read CONFIG frame
  uint8_t hdr[FRAME_HEADER_SIZE];
  if (!tunnelRead(hdr, FRAME_HEADER_SIZE, 15000)) {
    log_e("CONFIG frame timeout"); g_tunnel.stop(); return;
  }
  if (hdr[0] != FRAME_CONFIG) {
    log_e("Expected CONFIG, got 0x%02x", hdr[0]); g_tunnel.stop(); return;
  }
  uint32_t cfg_len = ((uint32_t)hdr[6] << 24) | ((uint32_t)hdr[7] << 16) |
                     ((uint32_t)hdr[8] <<  8) | hdr[9];
  if (cfg_len > sizeof(g_frame_buf)) {
    log_e("CONFIG payload too large: %lu", (unsigned long)cfg_len); g_tunnel.stop(); return;
  }
  if (!tunnelRead(g_frame_buf, cfg_len, 5000)) {
    log_e("CONFIG payload read failed"); g_tunnel.stop(); return;
  }
  if (!parseConfig(g_frame_buf, cfg_len)) {
    log_e("CONFIG parse failed"); g_tunnel.stop(); return;
  }
  log_i("Session established. Backend: %s:%u", g_backend_host, g_backend_port);

  // 5. Main request loop
  g_last_ping_ms = millis();
  g_ping_pending = false;
  bool running = true;

  while (running && g_tunnel.connected()) {
    uint32_t now = millis();

    // Keepalive
    if (!g_ping_pending && (now - g_last_ping_ms >= PING_INTERVAL_MS)) {
      sendPing();
    }
    if (g_ping_pending && (now - g_last_ping_ms >= PONG_TIMEOUT_MS)) {
      log_e("PONG timeout — reconnecting");
      break;
    }

    // Check for incoming frame
    if (g_tunnel.available() == 0) { delay(10); continue; }

    if (!tunnelRead(hdr, FRAME_HEADER_SIZE, 2000)) {
      log_e("Frame header read failed"); break;
    }

    uint8_t  ftype     = hdr[0];
    uint32_t stream_id = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
                         ((uint32_t)hdr[3] <<  8) | hdr[4];
    uint8_t  flags     = hdr[5];
    uint32_t flen      = ((uint32_t)hdr[6] << 24) | ((uint32_t)hdr[7] << 16) |
                         ((uint32_t)hdr[8] <<  8) | hdr[9];

    switch (ftype) {
      case FRAME_REQUEST:
        if (flen > MAX_REQUEST_PAYLOAD) {
          log_w("REQUEST too large (%lu B) on stream %lu — RESET", (unsigned long)flen, (unsigned long)stream_id);
          if (!tunnelDrain(flen)) { running = false; break; }
          sendReset(stream_id, RESET_INTERNAL_ERROR);
        } else {
          if (flen > 0 && !tunnelRead(g_frame_buf, flen, 5000)) { running = false; break; }
          handleRequest(stream_id, flags, g_frame_buf, flen);
        }
        break;

      case FRAME_DATA:
        // Unexpected DATA outside handleRequest (e.g. for a previous stream) — drain
        if (!tunnelDrain(flen)) running = false;
        break;

      case FRAME_PING: {
        uint8_t opaque[8] = {};
        if (flen >= 8) { if (!tunnelRead(opaque, 8)) { running = false; break; } if (flen > 8) tunnelDrain(flen - 8); }
        else if (flen > 0) tunnelDrain(flen);
        sendFrame(FRAME_PONG, 0, 0, opaque, 8);
        log_d("PONG sent");
        break;
      }

      case FRAME_PONG:
        if (!tunnelDrain(flen)) { running = false; break; }
        g_ping_pending = false;
        g_last_ping_ms = millis();  // reset interval from PONG receipt
        log_d("PONG received");
        break;

      case FRAME_GOAWAY:
        log_i("GOAWAY received");
        tunnelDrain(flen);
        running = false;
        break;

      case FRAME_RESET:
        if (!tunnelDrain(flen)) { running = false; break; }
        log_d("RESET stream %lu", (unsigned long)stream_id);
        break;

      default:
        log_w("Unknown frame 0x%02x, draining %lu B", ftype, (unsigned long)flen);
        if (!tunnelDrain(flen)) running = false;
        break;
    }
  }

  g_tunnel.stop();
  log_i("Session ended");
}
