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
#include "provisioning.h"
#include "config.h"
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <Arduino.h>
#ifdef TUNNEL_TLS_VERIFY_CERT
  #include "certs.h"
#endif

// ── Module-level state ────────────────────────────────────────────────────────

#ifdef USE_TLS_TUNNEL
static NetworkClientSecure g_tunnel;
#else
static NetworkClient g_tunnel;
#endif
static NetworkClient g_backend;  // per-request TCP connection to local backend

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

// Per-domain backend routing table, populated from the CONFIG frame.
// g_backend_host/g_backend_port mirror the first domain and remain as the
// fallback target for domain_ids that didn't fit in the table.
struct DomainBackend {
  uint16_t domain_id;
  char     host[128];
  uint16_t port;
};
static DomainBackend g_domain_backends[MAX_TUNNEL_DOMAINS];
static uint8_t       g_domain_backend_count;

static char     g_backend_host[128];
static uint16_t g_backend_port;

// Look up the backend for a domain_id from the CONFIG frame. Returns false
// (leaving *out_host/*out_port untouched) if domain_id isn't in the table.
static bool resolveBackend(uint16_t domain_id, const char** out_host, uint16_t* out_port) {
  for (uint8_t i = 0; i < g_domain_backend_count; i++) {
    if (g_domain_backends[i].domain_id == domain_id) {
      *out_host = g_domain_backends[i].host;
      *out_port = g_domain_backends[i].port;
      return true;
    }
  }
  return false;
}

// Keepalive tracking — idle-based: a PING is sent only once nothing has been
// received for PING_INTERVAL_MS. Any received frame (not just PONG) proves the
// channel is alive and clears g_ping_pending, so actively flowing traffic
// (e.g. relaying a large response) naturally suppresses ping churn; pings only
// happen during genuine idle gaps.
static uint32_t g_last_rx_ms;    // last time any frame was received from the tunnel
static uint32_t g_ping_sent_ms;  // when the outstanding ping was sent (valid while g_ping_pending)
static bool     g_ping_pending;
static bool     g_tunnel_dead;   // set once a pending ping goes unanswered too long
static uint8_t  g_ping_seq;      // monotonic counter in PING opaque[0], matches rust-client

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
  opaque[0] = ++g_ping_seq;
  sendFrame(FRAME_PING, 0, 0, opaque, 8);
  g_ping_pending = true;
  g_ping_sent_ms = millis();
  log_d("PING sent (seq=%u)", g_ping_seq);
}

// Call whenever any frame is successfully read from the tunnel — proves the
// channel is alive right now, regardless of frame type.
static void noteFrameReceived() {
  g_last_rx_ms = millis();
  g_ping_pending = false;
}

// Idle-triggered keepalive: sends a PING only after PING_INTERVAL_MS with
// nothing received (see noteFrameReceived), and sets g_tunnel_dead if a
// pending PING goes unanswered for PONG_TIMEOUT_MS. Safe to call from
// anywhere, including mid-stream — it's a no-op while traffic is flowing.
static void checkKeepalive() {
  if (g_tunnel_dead) return;
  uint32_t now = millis();
  if (g_ping_pending) {
    // Single-stream: the PONG (or anything else) can be sitting fully-arrived
    // but unread behind other queued REQUEST frames ahead of it in the same
    // ordered TCP stream, while we're still busy handling an earlier one. Bytes
    // already waiting to be read prove the connection is alive even before we
    // get to them, so don't declare it dead out from under a legitimate backlog.
    if (g_tunnel.available() > 0) return;
    if (now - g_ping_sent_ms >= PONG_TIMEOUT_MS) {
      log_e("Keepalive timeout — nothing received since PING sent %lums ago", (unsigned long)(now - g_ping_sent_ms));
      g_tunnel_dead = true;
    }
  } else if (now - g_last_rx_ms >= PING_INTERVAL_MS) {
    sendPing();
  }
}

// Service the tunnel socket's keepalive while handleRequest() is busy relaying
// a slow backend response (single-stream: the outer loop's own keepalive check
// doesn't run again until the current request finishes). Answers/consumes any
// PING or PONG already queued from the server, but leaves any other queued
// frame type (e.g. another REQUEST from a parallel browser fetch) completely
// untouched for the outer loop to process once this request is done — call
// from idle waits in the backend-read helpers.
static void serviceTunnelKeepalive() {
  if (!g_tunnel.connected()) return;

  while (g_tunnel.available() > 0) {
    int next = g_tunnel.peek();
    if (next != FRAME_PING && next != FRAME_PONG) break;

    uint8_t hdr[FRAME_HEADER_SIZE];
    if (!tunnelRead(hdr, FRAME_HEADER_SIZE, 2000)) return;
    uint32_t flen = ((uint32_t)hdr[6] << 24) | ((uint32_t)hdr[7] << 16) |
                    ((uint32_t)hdr[8] <<  8) | hdr[9];
    noteFrameReceived();

    if (hdr[0] == FRAME_PING) {
      uint8_t opaque[8] = {};
      if (flen >= 8) { if (!tunnelRead(opaque, 8)) return; if (flen > 8) tunnelDrain(flen - 8); }
      else if (flen > 0) tunnelDrain(flen);
      sendFrame(FRAME_PONG, 0, 0, opaque, 8);
    } else {  // FRAME_PONG
      if (flen > 0) tunnelDrain(flen);
    }
  }

  checkKeepalive();
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
  NetworkClientSecure https;
  https.setInsecure();  // cert validation skipped; fine for bootstrap call
  https.setTimeout(15);

  if (!https.connect(getApiHost(), TUNNEL_API_PORT)) {
    log_e("API connect failed: %s:%d", getApiHost(), TUNNEL_API_PORT);
    return "";
  }

  https.printf("GET /api/client-tunnel/%s HTTP/1.1\r\n", getTunnelId());
  https.printf("Host: %s\r\n", getApiHost());
  https.printf("Authorization: Bearer %s\r\n", getApiKey());
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
  g_domain_backend_count = 0;

  for (uint16_t i = 0; i < count; i++) {
    if (pos + 3 > len) return false;
    uint16_t domain_id = ((uint16_t)payload[pos] << 8) | payload[pos + 1];
    pos += 3;  // domain_id (u16) + flags (u8)
    if (!readU16Str(payload, len, &pos, domain_str, sizeof(domain_str))) return false;
    if (!readU16Str(payload, len, &pos, local_host, sizeof(local_host))) return false;

    char host_only[128];
    uint16_t port;
    char* colon = strrchr(local_host, ':');
    if (colon) {
      size_t hlen = min((size_t)(colon - local_host), sizeof(host_only) - 1);
      memcpy(host_only, local_host, hlen);
      host_only[hlen] = '\0';
      port = (uint16_t)atoi(colon + 1);
    } else {
      strlcpy(host_only, local_host, sizeof(host_only));
      port = 80;
    }

    if (g_domain_backend_count < MAX_TUNNEL_DOMAINS) {
      DomainBackend& b = g_domain_backends[g_domain_backend_count++];
      b.domain_id = domain_id;
      strlcpy(b.host, host_only, sizeof(b.host));
      b.port = port;
    } else {
      log_w("Domain %s (id=%u) exceeds MAX_TUNNEL_DOMAINS (%d) — will fall back to domain 0's backend",
            domain_str, domain_id, MAX_TUNNEL_DOMAINS);
    }

    if (i == 0) {
      strlcpy(g_backend_host, host_only, sizeof(g_backend_host));
      g_backend_port = port;
    }
    log_i("Backend from CONFIG: domain_id=%u %s -> %s:%u", domain_id, domain_str, host_only, port);
  }
  return count > 0;
}

// ── Streaming backend response reader ─────────────────────────────────────────
// The response body is relayed to the tunnel as DATA frames while it's being
// read from the backend, never fully buffered — so response size isn't capped
// by MAX_RESPONSE_BODY (that buffer is used for the request body only).

// Read up to `max_len` bytes from g_backend into buf. Waits up to `timeout_ms`
// for the first byte, then drains whatever else is immediately available and
// returns promptly rather than blocking to fill the buffer (keeps DATA frames
// flowing at backend speed instead of batching). Returns 0 on idle timeout or
// backend close with nothing pending (both signal "no more data right now").
static uint32_t readBackendChunk(uint8_t* buf, uint32_t max_len, uint32_t timeout_ms) {
  uint32_t got = 0;
  uint32_t deadline = millis() + timeout_ms;
  while (got < max_len) {
    if (g_backend.available()) {
      int n = g_backend.read(buf + got, max_len - got);
      if (n > 0) { got += n; deadline = millis() + timeout_ms; continue; }
    }
    if (got > 0) break;
    if (!g_backend.connected()) break;
    if ((int32_t)(millis() - deadline) >= 0) break;
    serviceTunnelKeepalive();
    if (g_tunnel_dead) break;
    delay(2);
  }
  return got;
}

// Read one CRLF-terminated line (chunk-size line, or a chunk's trailing CRLF)
// from g_backend. Returns line length (CRLF stripped, 0 for a bare blank line),
// or -1 on timeout/backend close before a newline arrived.
static int readBackendLine(char* out, size_t out_max, uint32_t timeout_ms) {
  size_t n = 0;
  uint32_t deadline = millis() + timeout_ms;
  while (true) {
    if (g_backend.available()) {
      char c = (char)g_backend.read();
      if (c == '\n') {
        if (n > 0 && out[n - 1] == '\r') n--;
        out[n] = '\0';
        return (int)n;
      }
      if (n < out_max - 1) out[n++] = c;
      continue;
    }
    if (!g_backend.connected()) return -1;
    if ((int32_t)(millis() - deadline) >= 0) return -1;
    serviceTunnelKeepalive();
    if (g_tunnel_dead) return -1;
    delay(2);
  }
}

// ── REQUEST handler ───────────────────────────────────────────────────────────

static void handleRequest(uint32_t stream_id, uint8_t req_flags,
                          const uint8_t* payload, uint32_t payload_len) {
  uint32_t pos = 0;

  if (pos + 2 > payload_len) { sendReset(stream_id, RESET_INTERNAL_ERROR); return; }
  uint16_t domain_id = ((uint16_t)payload[pos] << 8) | payload[pos + 1];
  pos += 2;

  // Route to this domain's own backend; unknown/overflowed domain_ids fall
  // back to the first CONFIG domain (see resolveBackend / MAX_TUNNEL_DOMAINS).
  const char* backend_host = g_backend_host;
  uint16_t    backend_port = g_backend_port;
  resolveBackend(domain_id, &backend_host, &backend_port);

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
      noteFrameReceived();
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
      } else if (dtype == FRAME_PONG) {
        if (dlen > 0 && !tunnelDrain(dlen)) { sendReset(stream_id, RESET_INTERNAL_ERROR); return; }
      } else {
        if (!tunnelDrain(dlen)) { sendReset(stream_id, RESET_INTERNAL_ERROR); return; }
      }
    }
    http_req += "Content-Length: " + String(body_len) + "\r\n";
  }
  http_req += "\r\n";

  // Connect to local backend
  if (!g_backend.connect(backend_host, backend_port)) {
    log_e("Backend unreachable: %s:%u (domain_id=%u)", backend_host, backend_port, domain_id);
    sendReset(stream_id, RESET_BACKEND_UNREACHABLE); return;
  }
  g_backend.setTimeout(10);
  g_backend.setNoDelay(true);  // don't let Nagle batch our chunked reads/writes

  // Send request + optional body
  g_backend.print(http_req);
  if (body_len > 0) g_backend.write(g_body_buf, body_len);

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
    serviceTunnelKeepalive();
    if (g_tunnel_dead) break;
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

  // Note: resp_headers still carries the backend's original content-length (if any)
  // verbatim — we no longer rewrite it, since the body below is streamed rather
  // than buffered so the true length is never truncated. For chunked/unknown-length
  // responses no content-length header was captured above (transfer-encoding is
  // stripped, and a close-delimited response never had one); the proxy fills it in
  // server-side once all DATA frames are collected.

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

  uint8_t resp_flags = no_body ? 0 : FLAG_RESPONSE_HAS_BODY;
  sendFrame(FRAME_RESPONSE, stream_id, resp_flags, resp_payload, rp);

  // Stream the response body straight from the backend into DATA frames —
  // never fully buffered, so size isn't capped by MAX_RESPONSE_BODY.
  uint32_t streamed_total = 0;
  if (!no_body) {
    static uint8_t stream_buf[DATA_CHUNK_SIZE];
    bool end_stream_sent = false;

    if (content_length >= 0) {
      uint32_t remaining = (uint32_t)content_length;
      while (remaining > 0) {
        uint32_t want = min((uint32_t)DATA_CHUNK_SIZE, remaining);
        uint32_t got = readBackendChunk(stream_buf, want, 10000);
        if (got == 0) break;  // backend closed/stalled early — end the stream with what we have
        remaining -= got;
        streamed_total += got;
        bool last = (remaining == 0);
        sendFrame(FRAME_DATA, stream_id, last ? FLAG_END_STREAM : 0, stream_buf, got);
        if (last) end_stream_sent = true;
        serviceTunnelKeepalive();
      }
    } else if (is_chunked) {
      char line[16];
      while (true) {
        int n = readBackendLine(line, sizeof(line), 10000);
        if (n < 0) break;  // timeout/close mid-stream — end with what we have
        uint32_t chunk_size = (uint32_t)strtoul(line, nullptr, 16);
        if (chunk_size == 0) {
          char trail[4];
          readBackendLine(trail, sizeof(trail), 2000);  // trailing CRLF after "0"
          break;
        }
        uint32_t remaining = chunk_size;
        bool chunk_ok = true;
        while (remaining > 0) {
          uint32_t want = min((uint32_t)DATA_CHUNK_SIZE, remaining);
          uint32_t got = readBackendChunk(stream_buf, want, 10000);
          if (got == 0) { chunk_ok = false; break; }
          remaining -= got;
          streamed_total += got;
          sendFrame(FRAME_DATA, stream_id, 0, stream_buf, got);
          serviceTunnelKeepalive();
        }
        if (!chunk_ok) break;
        char crlf[4];
        readBackendLine(crlf, sizeof(crlf), 2000);  // trailing CRLF after chunk data
      }
    } else {
      // No Content-Length, not chunked — read until the backend closes (expected,
      // since our own outgoing request always sends "Connection: close").
      const uint32_t SAFETY_CAP = 8UL * 1024 * 1024;  // guards against a backend that never closes
      while (streamed_total < SAFETY_CAP) {
        uint32_t got = readBackendChunk(stream_buf, DATA_CHUNK_SIZE, 10000);
        if (got == 0) break;
        streamed_total += got;
        sendFrame(FRAME_DATA, stream_id, 0, stream_buf, got);
        serviceTunnelKeepalive();
      }
    }

    if (!end_stream_sent) sendFrame(FRAME_DATA, stream_id, FLAG_END_STREAM, nullptr, 0);
  }
  g_backend.stop();

  log_d("stream %lu: %s %s -> %u (%lu B streamed)", (unsigned long)stream_id,
        method, path, status_code, (unsigned long)streamed_total);
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
  g_tunnel.setNoDelay(true);  // don't let Nagle batch our chunked reads/writes
  log_i("Connected to proxy (%s)", mode);

  // 3. Send CONNECT frame
  // Payload: version(1) + caps(1) + u16-len + tunnel_id + u16-len + api_key
  {
    uint8_t buf[512];
    uint32_t p = 0;
    buf[p++] = PROTOCOL_VERSION;
    buf[p++] = 0x00;  // no HTTP/2 or TLS backend capability

    uint16_t tid_len = (uint16_t)strlen(getTunnelId());
    buf[p++] = (tid_len >> 8) & 0xFF; buf[p++] = tid_len & 0xFF;
    memcpy(buf + p, getTunnelId(), tid_len); p += tid_len;

    uint16_t key_len = (uint16_t)strlen(getApiKey());
    buf[p++] = (key_len >> 8) & 0xFF; buf[p++] = key_len & 0xFF;
    memcpy(buf + p, getApiKey(), key_len); p += key_len;

    if (!sendFrame(FRAME_CONNECT, 0, 0, buf, p)) {
      log_e("CONNECT send failed"); g_tunnel.stop(); return;
    }
  }
  log_i("Sent CONNECT (tunnel_id=%s)", getTunnelId());

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
  g_last_rx_ms = millis();
  g_ping_pending = false;
  g_tunnel_dead = false;
  g_ping_seq = 0;
  bool running = true;

  while (running && g_tunnel.connected()) {
    checkKeepalive();
    if (g_tunnel_dead) { log_e("Keepalive timeout — reconnecting"); break; }

    // Check for incoming frame
    if (g_tunnel.available() == 0) { delay(10); continue; }

    if (!tunnelRead(hdr, FRAME_HEADER_SIZE, 2000)) {
      log_e("Frame header read failed"); break;
    }
    noteFrameReceived();

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
