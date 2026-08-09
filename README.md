# proxy-esp32 — Demo Setup Guide

Turn an ESP32 into a live web server reachable from the internet through an
[oli.bot](https://oli.bot) tunnel — no port forwarding, no static IP. This
guide walks through flashing the firmware, provisioning credentials on the
device itself (nothing secret ever goes in source control), and pointing a
tunnel domain at the device's built-in demo page.

## What you get

The firmware ships with a small demo web server (`main.cpp`) that serves a
status page — uptime, free heap, local IP, chip model — on port 80 of the
device's local IP. That's the "local backend" a tunnel domain forwards
requests to, so it's a quick way to confirm the whole path (browser →
oli.bot proxy → tunnel → device → back) works end to end before wiring up
your own application.

## Prerequisites

- An ESP32 DevKit (WiFi) or a Waveshare ESP32-P4-ETH (Ethernet) board
- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension)
- An oli.bot account with a tunnel created in the dashboard, and its
  **Tunnel ID** + **API Key** (`Dashboard → Client Tunnels`)

## 1. Flash the firmware

No credentials go in the build — `include/config.h` only holds protocol and
timing constants (ports, timeouts, buffer sizes), never secrets.

```bash
# WiFi + plain TCP (ESP32 DevKit and similar)
pio run -e esp32dev --target upload
pio device monitor   # 115200 baud, optional — watch boot logs

# Ethernet + TLS (Waveshare ESP32-P4-ETH)
pio run -e waveshare-esp32p4-eth --target upload
```

## 2. Provision credentials (first boot)

On first boot — or any time stored credentials are missing — the device
starts its own captive-portal setup server instead of connecting to a tunnel.

**WiFi boards:**
1. On your phone/laptop, join the WiFi network the device broadcasts:
   `proxy-esp32-XXXXXX` (the suffix is unique per device — check the serial
   monitor if you're not sure which one).
2. A captive-portal page should open automatically; if not, browse to
   `http://192.168.4.1`.
3. Fill in: your home/office **WiFi SSID + password**, **API Region**
   (US / EU / Asia), your **Tunnel ID**, and your **API Key**
   (format `<subscriptionId>_<salt>`, from the dashboard).
4. Submit. The device saves everything to NVS flash and reboots.

**Ethernet boards:** skip the WiFi fields — connect the Ethernet cable first,
then open the IP the serial monitor prints (`Open: http://<device-ip>`) and
fill in the same Tunnel ID / API Key / Region fields.

To re-provision later (new WiFi network, wrong tunnel, etc.), hold the
**BOOT button** (GPIO0) for 3 seconds at boot — this wipes stored credentials
and drops the device back into setup mode.

## 3. Confirm the tunnel connects

Watch the serial monitor (`pio device monitor`) after the reboot — you
should see it join the network, fetch the proxy address, and log
`Session established. Backend: <ip>:<port>` (or the TLS equivalent). It also
starts the demo web server and logs the local address it's listening on,
e.g.:

```
Demo web server listening on 192.168.0.233:80
```

## 4. Point a tunnel domain at the device

In the oli.bot dashboard (`Client Tunnels → your tunnel → domains`), add or
edit a domain and set **Local IP** to the device's address from step 3, port
`80` — e.g. `192.168.0.233:80`.

Then visit that domain in a browser. You should see the demo's "It works!"
status page, served live by the ESP32.

### Handling a changing local IP

The device's local IP comes from your router's DHCP and can change over
time, which would silently break the tunnel domain's `Local IP` setting.
Two ways to avoid this:

- **Reserve the IP** for the device's MAC address in your router's DHCP
  settings (recommended — set once, never think about it again), or
- **Re-check and update** the `Local IP` field in the dashboard whenever it
  changes (the serial monitor always shows the current IP on boot/reconnect).

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Device won't join WiFi | Wrong SSID/password entered during provisioning — hold BOOT 3s to reset and retry |
| Tunnel connects but domain shows a proxy error | `Local IP` in the dashboard doesn't match the device's current IP (see above) |
| Demo page loads but looks broken/blank | Confirm the domain's port is `80`, matching the demo server |
| Nothing on the serial monitor | Check baud rate is `115200` |

## More detail

See [`CLAUDE.md`](./CLAUDE.md) for the protocol internals, memory budget,
build variants, and source file layout.
