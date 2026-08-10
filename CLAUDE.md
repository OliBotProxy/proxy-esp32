# proxy-esp32 — Developer Context

## What this project is

ESP32 PlatformIO reference implementation of the `tunnel-client` for the rust-rpxy tunnel proxy system. Connects to the tunnel proxy over plain TCP (no TLS), exposes a local HTTP backend (typically the ESP32's own web server) through the proxy.

Part of the rust-rpxy workspace: proxy server lives in `../rust-rpxy`, backend admin in `../proxy-admin`.

## Build variants

| Environment | Board | Network | Tunnel | Port |
|-------------|-------|---------|--------|------|
| `esp32dev` | ESP32 DevKit | WiFi | Plain TCP | 8779 |
| `esp32-s3` | Generic ESP32-S3 (e.g. SparkleIoT XH-S3E) | WiFi | Plain TCP | 8779 |
| `waveshare-esp32p4-eth` | Waveshare ESP32-P4-ETH | Ethernet (IP101 PHY) | TLS (mbedTLS) | 8778 |

```bash
# WiFi + plain TCP (default)
pio run -e esp32dev --target upload

# Generic ESP32-S3 module (native USB-CDC serial)
pio run -e esp32-s3 --target upload

# Waveshare ESP32-P4-ETH + TLS
pio run -e waveshare-esp32p4-eth --target upload

pio device monitor   # 115200 baud
```

## Quick start

1. Flash with the appropriate environment above. No credentials are baked into the build —
   `include/config.h` only holds protocol/timing constants, never secrets.
2. On first boot (no stored credentials), the device starts its own captive-portal web
   server (`runProvisioning()` in `provisioning.cpp`) — connect to it and submit WiFi
   (or leave blank on Ethernet builds), tunnel ID, and API key via the form. These are
   stored in NVS flash, never in source, so nothing secret ever needs to be committed.
3. The device reboots, connects to the network, fetches the proxy address from the API,
   then maintains a persistent tunnel connection.
4. A built-in demo web server (`main.cpp`, runs on its own FreeRTOS task on core 0) serves
   a status page on port 80 of the device's local IP — point a domain's `localIp` at
   `<device-ip>:80` in the proxy-admin dashboard to expose it through a tunnel domain.
   Hold `PROVISION_RESET_PIN` (GPIO0/BOOT button) for 3 s to clear stored credentials and
   re-enter provisioning mode.

## Key files

```
include/
  config.h            — Protocol/timing constants only (ports, timeouts, buffer sizes) — no secrets
  tunnel_protocol.h   — Frame type / flag / reset-code constants
  certs.h             — ISRG Root X1 PEM cert (used when TUNNEL_TLS_VERIFY_CERT is defined)

src/
  main.cpp            — setup(): network init (WiFi or ETH) + demo web server task; loop(): reconnect wrapper
  provisioning.cpp     — NVS credential storage + captive-portal setup form (first-boot / reset)
  tunnel_client.h     — Public API: runTunnelSession()
  tunnel_client.cpp   — Full tunnel session logic (plain TCP or TLS via USE_TLS_TUNNEL)
```

## Configuration (`include/config.h`)

| Macro | Default | Description |
|-------|---------|-------------|
| `WIFI_SSID` / `WIFI_PASSWORD` | — | WiFi credentials |
| `TUNNEL_API_HOST` | `api-us.oli.bot` | Regional API host (`api-eu`, `api-asia`) |
| `TUNNEL_API_PORT` | `443` | HTTPS port for bootstrap API call |
| `TUNNEL_ID` | — | Tunnel ID from proxy-admin dashboard |
| `TUNNEL_API_KEY` | — | `<subscriptionId>_<salt>` from dashboard |
| `TUNNEL_PROXY_PORT_DEFAULT` | `8779` | Plain-TCP port on the proxy server |
| `RECONNECT_DELAY_MS` | `3000` | Delay between session reconnects |
| `PING_INTERVAL_MS` | `30000` | How often to send PING frames |
| `PONG_TIMEOUT_MS` | `10000` | Reconnect if no PONG within this window |
| `MAX_REQUEST_PAYLOAD` | `4096` | Max REQUEST frame payload; larger → RESET |
| `MAX_RESPONSE_BODY` | `32768` | Max REQUEST *body* buffered (browser→backend). Response bodies are streamed, not size-capped |
| `DATA_CHUNK_SIZE` | `2048` | DATA frame chunk size, and streaming read chunk size |

## Session flow

```
setup()
  └─ connectWiFi()

loop()
  └─ runTunnelSession()
       ├─ fetchProxyAddress()   HTTPS GET api-us.oli.bot/api/client-tunnel/{id}
       │                        Authorization: Bearer {api_key}
       │                        → proxyAddress field (e.g. "92.5.152.130:8779")
       ├─ g_tunnel.connect(host, port)   plain TCP (8779) or TLS (8778)
       ├─ sendFrame(CONNECT)             version=0x02, caps=0x00
       ├─ readFrame(CONFIG)              → g_backend_host / g_backend_port
       └─ main loop
            ├─ REQUEST → handleRequest() → g_backend TCP → RESPONSE + DATA frames
            ├─ PING    → PONG
            ├─ PONG    → reset g_ping_pending
            ├─ GOAWAY  → break (reconnect)
            └─ every 30 s: send PING; reconnect if no PONG in 10 s
```

## Protocol v2 subset (ESP32 constraints)

Full protocol spec is in `../rust-rpxy/rpxy-lib/src/tunnel/protocol.rs`.

**Frame header:** 10 bytes — `type(1) | stream_id(4) | flags(1) | length(4)` big-endian.

**ESP32 limitations vs full client:**
- **Plain TCP by default** — `esp32dev` env uses port 8779; `esp32p4-eth` uses TLS on 8778
- **TLS without cert verification by default** — define `TUNNEL_TLS_VERIFY_CERT` in `config.h` to validate against ISRG Root X1 (`certs.h`)
- **Single-stream** — one request handled to completion before the next is read
- **4 KB REQUEST cap** — oversized frames answered with RESET, not a crash
- **Response bodies are streamed, not buffered** — read from the backend and relayed as DATA frames on the fly (`readBackendChunk`/`readBackendLine` in `tunnel_client.cpp`), so response size isn't capped by device RAM the way `MAX_RESPONSE_BODY` caps the request body
- **Requests are still handled one at a time** — a browser's parallel asset fetches queue up and get served sequentially, not concurrently. To stop a slow multi-request page load from killing the whole tunnel connection, `serviceTunnelKeepalive()` is polled from inside the backend-read idle loops during response streaming: it answers/consumes any PING or PONG already queued from the server and sends the client's own scheduled PING, while leaving any other queued frame (e.g. another REQUEST from a parallel fetch) untouched for the outer loop to process once the current request finishes
- **`capabilities = 0x00`** — no HTTP/2 or TLS backend support advertised

**CONNECT payload:** `version(1) + caps(1) + u16-len + tunnel_id + u16-len + api_key`

**CONFIG payload:** `u16 domain_count` + per-domain `u16 domain_id + u8 flags + u16-str domain + u16-str local_host`. Each domain's `local_host` is stored in a small routing table (`g_domain_backends`, up to `MAX_TUNNEL_DOMAINS`), keyed by `domain_id`; incoming REQUEST frames carry `domain_id` and are forwarded to that domain's own backend. Domains beyond `MAX_TUNNEL_DOMAINS` fall back to the first domain's backend.

**REQUEST handling:**
- Parse `domain_id(u16)`, `method(u8-str)`, `path(u16-str)`, headers `(u16 count + u8-str name + u16-str value)*`
- If `FLAG_HAS_BODY (0x01)`: read DATA frames synchronously until `FLAG_END_STREAM (0x01)`
- Forward as HTTP/1.1 with `Connection: close` to local backend (TCP)
- Read response headers (buffered, small), then send the `RESPONSE` frame
- Stream the body straight from the backend into `DATA` frames (`DATA_CHUNK_SIZE` per read), dechunking on the fly for `Transfer-Encoding: chunked`; last `DATA` frame carries `FLAG_END_STREAM`. Body is never fully buffered, so there's no size cap here (see `MAX_RESPONSE_BODY` note above)

## Proxy server setup

The proxy server (`../rust-rpxy`) must have plain-TCP tunnel port enabled:

```toml
# /etc/tunnel-proxy/config.toml
tunnel_port = 8779   # plain TCP, no TLS — required for ESP32
```

The ESP32 always connects to the plain-TCP port. TLS on the *proxy→browser* side is handled by the proxy server itself, transparent to the ESP32.

## Building for other ESP32 boards

Change `board` in `platformio.ini`. Common targets:

```ini
board = esp32dev          # generic ESP32 DevKit
board = esp32-s3-devkitc-1
board = lolin32
board = az-delivery-devkit-v4
```

## Throughput

Measured ESP32-S3 → proxy (Frankfurt), RSSI -48 dBm, relaying a 1 MB response
from a LAN backend:

| Config | Result |
|--------|--------|
| Modem sleep on (Arduino default) | ~22 KB/s, wildly variable (20-60 s for 1 MB) |
| `WiFi.setSleep(false)` | **~48 KB/s, consistent** |

`WiFi.setSleep(false)` in `main.cpp`'s `connectNetwork()` is the single biggest
win — the default `WIFI_PS_MIN_MODEM` parks the radio between DTIM beacons,
adding beacon latency to every TCP round-trip.

Profiling (`REQUEST #n profile:` log line) shows the split clearly: reading from
the local backend is ~1 ms/chunk, writing to the tunnel is ~82 ms per 4 KB — so
the relay is bound almost entirely by ESP32 TCP upload to the proxy. Throughput
scales exactly with bytes regardless of `DATA_CHUNK_SIZE`, so it's
bandwidth-bound, not per-frame-bound; tuning chunk size gains nothing.

**Practical implication:** ~48 KB/s means a 2 MB single-page app takes ~45 s to
load through an ESP32 tunnel, made worse by single-stream (requests are served
one at a time, so a browser's parallel fetches queue). The ESP32 is well suited
to its own small status/control pages; front heavy web apps with the full
`tunnel-client` instead (same content loads in ~1.5 s there).

## Memory budget

| Buffer | Size | Location |
|--------|------|----------|
| `g_frame_buf` | 4 KB | global (REQUEST payload) |
| `g_body_buf` | 32 KB | global (REQUEST body only, browser→backend) |
| `g_drain_buf` | 256 B | global |
| `resp_payload` | 4 KB | static local in `handleRequest` (response headers) |
| `stream_buf` | 2 KB | static local in `handleRequest` (response body streaming, reused per chunk — response size is unbounded, not buffered) |
| `g_domain_backends` | ~1 KB (`MAX_TUNNEL_DOMAINS` × 132 B) | global (per-domain backend routing table) |

Total static: ~43 KB. ESP32 has ~300 KB available heap — well within budget. `String` objects used for HTTP header parsing are short-lived and freed after each request. Response bodies stream through the fixed 2 KB `stream_buf` regardless of total size — a 1 MB backend response costs the same RAM as a 1 KB one, just more DATA frames.

## API key format

`<subscriptionId>_<salt>` — same format as the full `tunnel-client`. The server extracts `subscriptionId` by splitting on the first `_`.
