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
| `MAX_RESPONSE_BODY` | `32768` | Max response body buffered |
| `DATA_CHUNK_SIZE` | `2048` | DATA frame chunk size |

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
- **`capabilities = 0x00`** — no HTTP/2 or TLS backend support advertised

**CONNECT payload:** `version(1) + caps(1) + u16-len + tunnel_id + u16-len + api_key`

**CONFIG payload:** `u16 domain_count` + per-domain `u16 domain_id + u8 flags + u16-str domain + u16-str local_host`. Only the first domain's `local_host` is used as the backend address.

**REQUEST handling:**
- Parse `domain_id(u16)`, `method(u8-str)`, `path(u16-str)`, headers `(u16 count + u8-str name + u16-str value)*`
- If `FLAG_HAS_BODY (0x01)`: read DATA frames synchronously until `FLAG_END_STREAM (0x01)`
- Forward as HTTP/1.1 with `Connection: close` to local backend (TCP)
- Read response: parse status + headers, dechunk if needed, cap at `MAX_RESPONSE_BODY`
- Send `RESPONSE` frame then `DATA` frames in `DATA_CHUNK_SIZE` chunks

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

## Memory budget

| Buffer | Size | Location |
|--------|------|----------|
| `g_frame_buf` | 4 KB | global (REQUEST payload) |
| `g_body_buf` | 32 KB | global (request + response body, sequential) |
| `g_drain_buf` | 256 B | global |
| `resp_payload` | 4 KB | static local in `handleRequest` |
| `data_payload` | 2 KB | static local in `handleRequest` |

Total static: ~42 KB. ESP32 has ~300 KB available heap — well within budget. `String` objects used for HTTP header parsing are short-lived and freed after each request.

## API key format

`<subscriptionId>_<salt>` — same format as the full `tunnel-client`. The server extracts `subscriptionId` by splitting on the first `_`.
