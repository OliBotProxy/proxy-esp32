#pragma once

// ── Tunnel API port ───────────────────────────────────────────────────────────
// WiFi credentials, tunnel ID, API key, and API host are stored in NVS flash.
// On first boot (or after reset) the device enters provisioning mode to collect them.
#define TUNNEL_API_PORT 443

// Plain-TCP tunnel port (server must have tunnel_port configured, default 8779)
#define TUNNEL_PROXY_PORT_DEFAULT 8779
// TLS tunnel port (server tunnel_port_tls, default 8778)
#define TUNNEL_PROXY_TLS_PORT_DEFAULT 8778

// Reconnect delay after a session ends (milliseconds)
#define RECONNECT_DELAY_MS 3000

// ── Keepalive ─────────────────────────────────────────────────────────────────
#define PING_INTERVAL_MS 30000   // send PING every 30 s
#define PONG_TIMEOUT_MS  10000   // reconnect if no PONG within 10 s after PING

// ── Buffers ───────────────────────────────────────────────────────────────────
// Max domains from one CONFIG frame that get their own backend routing entry.
// Extra domains beyond this count still work but fall back to the first domain's backend.
#define MAX_TUNNEL_DOMAINS  8
// Max REQUEST frame payload accepted (4 KB per ESP32 subset spec)
#define MAX_REQUEST_PAYLOAD 4096
// Max REQUEST body buffered (browser -> backend). Response bodies (backend ->
// browser) are streamed straight into DATA frames and aren't size-limited by
// this constant — see readBackendChunk()/readBackendLine() in tunnel_client.cpp.
#define MAX_RESPONSE_BODY   32768
// DATA frame chunk size
#define DATA_CHUNK_SIZE     2048

// ── Ethernet (USE_ETHERNET builds) ───────────────────────────────────────────
// ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER,
// and ETH_CLK_MODE are normally defined by the board's pins_arduino.h
// (e.g. esp32-p4-evboard uses TLK110 PHY on MDC=31, MDIO=52, POWER=51).
// Only define them here if targeting a custom board that doesn't set them.

// ── Provisioning ─────────────────────────────────────────────────────────────
// GPIO held LOW for 3 s at boot clears NVS and enters provisioning mode.
// Set to -1 to disable the reset pin.
#define PROVISION_RESET_PIN   0          // GPIO0 = BOOT button on most boards
// SoftAP SSID prefix (WiFi builds). A 6-hex chip-ID suffix is appended.
#define PROVISION_AP_PREFIX   "proxy-esp32"

// ── TLS tunnel cert validation (USE_TLS_TUNNEL builds) ───────────────────────
// By default TLS is encrypted but the server cert is not verified (setInsecure).
// Define TUNNEL_TLS_VERIFY_CERT to validate against the ISRG Root X1 CA in certs.h.
// #define TUNNEL_TLS_VERIFY_CERT
