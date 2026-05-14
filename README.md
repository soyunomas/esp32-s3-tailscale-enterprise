# ESP32-S3 Tailscale WPA2-Enterprise (Advertise Routes)

WiFi Repeater based on **ESP32-S3** with **WPA2-Enterprise** support and embedded **Tailscale** client (capable of advertising LAN routes).

[🇪🇸 Versión en Español](README-ESP.md)

All configuration is done via a **professional web interface** served from the device itself, featuring a responsive dark theme.

<p align="center">
  <a href="img/screenshot1.png"><img src="img/screenshot1.png" width="19%" alt="Dashboard" /></a>
  <a href="img/screenshot2.png"><img src="img/screenshot2.png" width="19%" alt="WiFi Config" /></a>
  <a href="img/screenshot3.png"><img src="img/screenshot3.png" width="19%" alt="Tailscale" /></a>
  <a href="img/screenshot4.png"><img src="img/screenshot4.png" width="19%" alt="Port Forwarding" /></a>
  <a href="img/screenshot5.png"><img src="img/screenshot5.png" width="19%" alt="System" /></a>
</p>

## Main Features

### 🎓 WPA2-Enterprise and WiFi Repeater
- **Simultaneous STA+AP** — Connects to a WiFi network (STA) and creates its own Access Point (AP).
- **WPA2-Enterprise** — Full support for EAP-PEAP/TTLS, perfect for corporate or university networks (like eduroam).
- **NAPT & Port Forwarding** — Network Address Translation for AP clients and port redirection.
- **Hideable SSID** — Option to hide the AP's SSID.
- **Non-blocking Reconnection** — STA retries with exponential backoff, recovery mode, and always-available captive portal.
- **Configurable Identity** — Custom Hostname and MAC addresses for STA/AP.

### 🛣️ Tailscale with Advertise Routes
- **Embedded Tailscale Client** — Native C/FreeRTOS implementation using MicroLink.
- **Advertise Routes (Subnet Router / Gateway SNAT)** — Advertise LAN routes using `Hostinfo.RoutableIPs` and perform SNAT to the LAN, allowing access to your local network without modifying the main router.
- **Noise IK Authentication** — Cryptographic handshake with `controlplane.tailscale.com`.
- **DERP Relay** — Global relay connection when direct peer-to-peer connectivity is unavailable.
- **Automatic Reconnection** — Recovers after WiFi changes or connectivity drops.

### 💻 Web UI
- **Dark Theme** — Modern, mobile-first responsive design, no external frameworks.
- **HTTP Basic Auth** — Configurable credentials via the interface.
- **Full Configuration** — WiFi network, EAP, port forwarding, Tailscale, hostname, MACs, and logging.
- **Log Viewer** — Real-time system logs (32KB circular buffer).
- **Runtime Logging** — Adjustable global and MicroLink/WireGuard log levels without rebooting WiFi.
- **OTA Updates** — Web-based firmware updates with dual partitions and automatic rollback.
- **Factory Reset** — Reset to factory defaults via web or physical BOOT button.

## Hardware

### ESP32-S3 N16R8 (Recommended for Tailscale)

| Component | Detail |
|---|---|
| **Chip** | ESP32-S3 (Xtensa dual-core 240MHz) |
| **Flash** | 16MB |
| **PSRAM** | 8MB octal |
| **WiFi** | 802.11 b/g/n, 2.4GHz |
| **Connector**| USB-C (native + UART) |

## Pre-compiled Binaries (Easy Install)

If you don't want to build from source, you can find pre-compiled binaries in the `firmware/` directory.

### Initial Flash (via USB)

Use `esptool.py` to flash the device via USB for the first time:

```bash
esptool.py -p /dev/ttyACM0 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 firmware/bootloader.bin \
  0x8000 firmware/partition-table.bin \
  0xf000 firmware/ota_data_initial.bin \
  0x20000 firmware/wifi_repeater.bin
```

## Build from Source

### Requirements

- **ESP-IDF v6.1-dev** or higher ([Installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/))

### Build

```bash
source ~/esp/esp-idf/export.sh

idf.py set-target esp32s3
idf.py -B build_s3 build
```

### Flash

```bash
idf.py -B build_s3 -p /dev/ttyACM0 flash
```

### Serial Monitor

```bash
idf.py -p /dev/ttyACM0 monitor
# Exit: Ctrl+]
```

## Usage

1. **Flash** the firmware to the ESP32.
2. **Connect** to the **`ESP32-Repeater`** WiFi network (password: `12345678`).
3. **Open** [http://192.168.4.1](http://192.168.4.1) (or wait for the captive portal).
4. **Login** using username `admin` / password `admin`.
5. Go to **Config** → **Scan Networks** → select the network to repeat.
6. Enter the password (or EAP credentials) and click **Save & Apply**.
7. Check the **Dashboard** for the assigned IP and signal strength.

### Tailscale Configuration (Web UI)

1. Go to **Config** → **Tailscale**.
2. Toggle **Enable Tailscale**.
3. Paste an **Auth Key** from [tailscale.com/admin/settings/keys](https://tailscale.com/admin/settings/keys).
4. Configure **Device Name** (optional).
5. Click **Save Tailscale**.
6. The device will connect in ~60s and appear in your Tailnet.

### Tailscale Routing Modes

| Mode | WPA2-Enterprise Repeater | Port forwarding AP clients | Tailscale access to LAN (no router changes) | Requires return route |
|---|---|---|---|---|
| Repeater / Tailscale node only | Yes | Yes | No | No |
| Tailscale Gateway / SNAT with LAN route | Not usable as repeater | No | Yes | No |

#### Repeater / Tailscale node only
This is the default and safe mode. AP is active, NAPT is active on AP interface. Tailscale is connected as a regular node. No LAN route is advertised.

#### Tailscale Gateway / SNAT (Advertise Routes)
Designed for `Tailscale -> LAN` access without touching the LAN router, using SNAT on the ESP32. Enabling **Expose LAN over Tailscale** advertises the LAN route and forces Gateway/SNAT mode. It disables NAPT on AP and port forwarding, activating NAPT only on the MicroLink WireGuard interface so `Tailscale -> LAN` traffic exits via STA with SNAT.

### Hide AP SSID

In **Config** → **Access Point (AP)** you can enable **Hide AP SSID**. This option only hides the SSID broadcast; it doesn't change the password, channel, DHCP, captive portal, NAPT or port forwarding rules.

### OTA Update

```bash
  curl -u admin:admin -X POST http://192.168.4.1/api/ota \
    --data-binary @firmware/wifi_repeater.bin
```

Or from the **System** → **Firmware Update** tab in the Web UI.

## REST API

The device exposes a comprehensive REST API for monitoring and configuration.
> 🔒 **Security:** All endpoints under `/api/*` require **HTTP Basic Auth** using the admin credentials.

| Endpoint | Method | Description |
|---|:---:|---|
| `/api/status` | `GET` | General system status (STA, RSSI, STA/AP MACs, clients, free heap, uptime). |
| `/api/wifi/state` | `GET` | Detailed STA connection state (`connected`, `retry_count`, `recovery`, `paused`). |
| `/api/wifi/pause` | `POST` | Pauses the upstream STA connection and stops automatic retries. |
| `/api/wifi/resume` | `POST` | Resumes the upstream STA connection and retries. |
| `/api/scan` | `GET` | Scans for nearby WiFi networks and returns the results. |
| `/api/config` | `GET` | Retrieves the current configuration (WiFi, AP, EAP, Port Forwarding, MACs, etc.). |
| `/api/config` | `POST` | Applies and saves a new configuration to NVS memory. |
| `/api/clients` | `GET` | Lists currently connected AP clients (IP and MAC addresses). |
| `/api/ping` | `POST` | Executes an ICMP ping test. Required payload: `{"target":"8.8.8.8"}`. |
| `/api/restart` | `POST` | Reboots the device (Soft Reset). |
| `/api/auth/check` | `GET` | Validates the current HTTP Basic Auth credentials. |
| `/api/auth/change` | `POST` | Changes the HTTP Basic Auth credentials. |
| `/api/ota` | `POST` | Endpoint for uploading Over-The-Air (OTA) firmware binaries. |
| `/api/factory-reset` | `POST` | Restores factory defaults by erasing the NVS. |
| `/api/logs` | `GET` | Retrieves the circular system log buffer as `text/plain`. |
| `/api/loglevel` | `POST` | Adjusts logging levels at runtime (Global and MicroLink overrides). |
| `/api/tailscale/status` | `GET` | Retrieves the current Tailscale connection status and peers. |
| `/api/tailscale/config` | `GET` | Reads the current Tailscale configuration (Auth Key, Hostname, etc.). |
| `/api/tailscale/config` | `POST` | Saves and applies the Tailscale configuration. |

## Internal Architecture

```
┌──────────┐    WiFi AP    ┌──────────────┐    WiFi STA   ┌────────┐
│ Client   │◄─────────────►│  ESP32-S3    │◄─────────────►│ Router │
│ (Mobile) │  192.168.4.x  │  NAPT + DNS  │  192.168.1.x  │  (WAN) │
└──────────┘               └──────┬───────┘               └────────┘
                                  │
                                  │ Tailscale
                                  ▼
                          ┌──────────────┐
                          │   Tailnet    │
                          │ 100.81.77.58 │
                          └──────────────┘
```

## Repository
[https://github.com/soyunomas/esp32-s3-tailscale-enterprise](https://github.com/soyunomas/esp32-s3-tailscale-enterprise)

## Acknowledgements

- **[CamM2325/microlink](https://github.com/CamM2325/microlink)**: Native C implementation of the Tailscale protocol for ESP32.
- **Espressif Systems**: ESP-IDF framework.
- **Tailscale**: VPN mesh network protocols and infrastructure.

## License
MIT