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
// Verify pin numbers against your specific board schematic.
#ifdef USE_ETHERNET
  #ifndef ETH_PHY_TYPE
    #define ETH_PHY_TYPE  ETH_PHY_LAN8720
  #endif
  #ifndef ETH_PHY_ADDR
    #define ETH_PHY_ADDR  0
  #endif
  #ifndef ETH_PHY_MDC
    #define ETH_PHY_MDC   23
  #endif
  #ifndef ETH_PHY_MDIO
    #define ETH_PHY_MDIO  18
  #endif
  #ifndef ETH_PHY_POWER
    #define ETH_PHY_POWER -1   // -1 = no dedicated power/reset pin
  #endif
  #ifndef ETH_CLK_MODE
    #define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN
  #endif
#endif

// ── TLS tunnel cert validation (USE_TLS_TUNNEL builds) ───────────────────────
// By default TLS is encrypted but the server cert is not verified (setInsecure).
// Define TUNNEL_TLS_VERIFY_CERT to validate against the ISRG Root X1 CA in certs.h.
// #define TUNNEL_TLS_VERIFY_CERT
