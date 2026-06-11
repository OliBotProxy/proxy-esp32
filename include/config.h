#pragma once

// ── WiFi credentials ──────────────────────────────────────────────────────────
#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// ── Tunnel configuration ──────────────────────────────────────────────────────
// Regional API hosts: api-us.oli.bot  |  api-eu.oli.bot  |  api-asia.oli.bot
#define TUNNEL_API_HOST "api-us.oli.bot"
#define TUNNEL_API_PORT 443

// Tunnel ID and API key from the proxy-admin dashboard
#define TUNNEL_ID      "your-tunnel-id"
#define TUNNEL_API_KEY "your-subscriptionId_your-salt"

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
// Max REQUEST frame payload accepted (4 KB per ESP32 subset spec)
#define MAX_REQUEST_PAYLOAD 4096
// Max response body buffered before streaming as DATA frames
#define MAX_RESPONSE_BODY   32768
// DATA frame chunk size
#define DATA_CHUNK_SIZE     2048

// ── Ethernet (USE_ETHERNET builds) ───────────────────────────────────────────
// ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER,
// and ETH_CLK_MODE are normally defined by the board's pins_arduino.h
// (e.g. esp32-p4-evboard uses TLK110 PHY on MDC=31, MDIO=52, POWER=51).
// Only define them here if targeting a custom board that doesn't set them.

// ── TLS tunnel cert validation (USE_TLS_TUNNEL builds) ───────────────────────
// By default TLS is encrypted but the server cert is not verified (setInsecure).
// Define TUNNEL_TLS_VERIFY_CERT to validate against the ISRG Root X1 CA in certs.h.
// #define TUNNEL_TLS_VERIFY_CERT
