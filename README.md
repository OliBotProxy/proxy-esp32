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
# Generic ESP32-S3 module — WiFi + plain TCP (the default env)
pio run -e esp32-s3 --target upload
pio device monitor   # 115200 baud, optional — watch boot logs

# Original ESP32 DevKit and similar — WiFi + plain TCP
pio run -e esp32dev --target upload

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

### Pointing a domain at the device's *own* demo server

If the backend is the demo server running on the ESP32 itself, set **Local IP**
to `127.0.0.1:80`, not the device's WiFi address.

Using the device's own LAN IP makes it connect back to itself *through the
router*, and many consumer routers don't reflect a station's traffic back to it
(no hairpin/NAT-loopback) — the connection just times out and the proxy reports
the backend as unreachable. `127.0.0.1` stays inside the device's own network
stack, always works, and is immune to DHCP changes.

Use the LAN IP only when the backend is a *different* machine.

### Handling a changing local IP

When the backend is another machine on your network, its IP comes from DHCP and
can change, silently breaking the tunnel domain's `Local IP` setting. Two ways
to avoid this:

- **Reserve the IP** for that device's MAC address in your router's DHCP
  settings (recommended — set once, never think about it again), or
- **Re-check and update** the `Local IP` field in the dashboard whenever it
  changes (the serial monitor always shows the current IP on boot/reconnect).

## Limitations

Measured on an ESP32-S3 at RSSI -48 dBm relaying to a proxy in Frankfurt.

| Limit | Value | Why |
|---|---|---|
| **Throughput** | ~50-65 KB/s | ESP32 TCP upload to the proxy. Reading the local backend is ~100× faster; the tunnel write is >90% of the time. |
| **Concurrency** | 1 request at a time | Single-stream by design (one shared backend socket, fixed buffers). A browser's parallel fetches queue up and are served in order. |
| **Request headers** | 4 KB (`MAX_REQUEST_PAYLOAD`) | Larger REQUEST frames are answered with RESET. Very large cookie sets can hit this. |
| **Request body** | 32 KB (`MAX_RESPONSE_BODY`) | Uploads (POST/PUT bodies) are buffered. Bigger bodies are truncated. |
| **Response body** | unlimited | Streamed to the tunnel as it's read, never fully buffered — response size costs no extra RAM. |
| **Domains per tunnel** | 8 (`MAX_TUNNEL_DOMAINS`) | Each gets its own backend. Extra domains fall back to the first one's backend. |

These are platform realities, not bugs. For comparison, the same page that takes
~35 s through an ESP32 loads in ~1.5 s through the desktop
[`tunnel-client`](https://github.com/OliBotProxy/rust-client).

> **Power note:** the firmware calls `WiFi.setSleep(false)`. WiFi modem sleep
> (the Arduino default) parks the radio between beacons and cuts throughput to
> roughly a third, with very erratic timing — so it's disabled deliberately.
> The trade-off is higher idle current: run these devices on mains power, not
> a battery.

## Recommendations

**Serve small pages.** At ~50-65 KB/s, every 64 KB costs about a second. A
lightweight status/control page (a few tens of KB) feels instant; a modern
JS-framework single-page app of 2 MB takes over half a minute. This is what the
ESP32 is good at — device dashboards, sensor readouts, config forms, REST
endpoints. Front a heavy web app with the full `tunnel-client` instead.

**Enable gzip on your backend.** Compressed responses pass straight through the
tunnel, and text assets (HTML/CSS/JS/JSON) typically shrink 60-80% — the single
cheapest way to multiply effective speed.

**Don't ship source maps.** `.js.map` files are often several times larger than
the code they describe, and browsers fetch them automatically whenever DevTools
is open — including maps that never appear in the Network tab. One real example:
a 2.1 MB app carried 9 MB of source maps, so a page load transferred 11 MB
instead of 2 MB and everything queued behind it stalled for minutes. Build with
source maps off (e.g. `build.sourcemap: false` in Vite) or don't deploy the
`.map` files.

**Keep asset counts low.** Because requests are served one at a time, ten small
files cost ten sequential round-trips. Bundling helps more here than on a normal
server.

**Reserve the device's IP** in your router's DHCP settings so the dashboard's
`Local IP` never goes stale.

**Watch the serial log when tuning.** Each request logs a profile line with
size, duration, throughput, free heap, and RSSI:

```
REQUEST #12 done: GET /app.js -> 200 (485616 B, 7317ms, 66KB/s, heap=186996)
REQUEST #12 profile: 119 chunks, read=110ms write=7109ms (avg read=0ms write=59ms per chunk), rssi=-50 dBm
```

A high `write=` relative to `read=` is normal — it means the tunnel upload is
the limit. A poor `rssi` (worse than about -70 dBm) will show up as
proportionally lower throughput.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Device won't join WiFi | Wrong SSID/password entered during provisioning — hold BOOT 3s to reset and retry |
| Tunnel connects but domain shows a proxy error | `Local IP` in the dashboard doesn't match the device's current IP (see above) |
| `Backend unreachable` in the serial log, pointing at the device's own IP | Router won't loop traffic back to the sender — use `127.0.0.1:80` instead (see above) |
| Demo page loads but looks broken/blank | Confirm the domain's port is `80`, matching the demo server |
| Nothing on the serial monitor | Check baud rate is `115200` |
| Some requests sit "pending" forever in the browser | Not a hang — they're queued behind a large transfer. Requests are served one at a time; check the serial log for what's currently streaming. Source maps are the usual culprit (see Recommendations). |
| Everything is slow from *every* browser at once | Same cause: one device, one request at a time, so a big transfer for one client blocks all the others |
| Page loads fine with DevTools closed, crawls with it open | DevTools is fetching source maps out of band — they don't all show in the Network tab |
| POST/upload silently truncated | Request bodies are capped at 32 KB (`MAX_RESPONSE_BODY`) |
| Occasional RESET on requests with many cookies | REQUEST headers over 4 KB (`MAX_REQUEST_PAYLOAD`) are rejected |

## More detail

See [`CLAUDE.md`](./CLAUDE.md) for the protocol internals, memory budget,
build variants, and source file layout.
