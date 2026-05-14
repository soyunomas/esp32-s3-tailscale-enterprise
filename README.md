# ESP32-S3 Tailscale WPA2-Enterprise (Advertise Routes)

WiFi Repeater based on **ESP32-S3** with **WPA2-Enterprise** support and embedded **Tailscale** client capable of advertising LAN routes.

[🇪🇸 Versión en Español](README-ESP.md)

All configuration is done via an embedded responsive web UI served directly from the device.

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
- **WPA2-Enterprise** — Supports EAP-PEAP/TTLS configurations commonly used by corporate and university networks (including eduroam).
- **NAPT & Port Forwarding** — Network Address Translation for AP clients and port redirection.
- **Hideable SSID** — Option to hide the AP's SSID.
- **Non-blocking Reconnection** — STA retries with exponential backoff, recovery mode, and always-available captive portal.
- **Configurable Identity** — Custom Hostname and MAC addresses for STA/AP.

### 🛣️ Tailscale with Advertise Routes
- **Embedded Tailscale Client** — Native C/FreeRTOS integration using MicroLink.
- **Advertise Routes (Subnet Router / Gateway SNAT)** — Advertises LAN routes using `Hostinfo.RoutableIPs` and performs SNAT toward the LAN, allowing `Tailscale -> LAN` access without requiring static return routes on the main router.
- **Noise IK Authentication** — Cryptographic handshake with `controlplane.tailscale.com`.
- **DERP Relay** — Global relay connection when direct peer-to-peer connectivity is unavailable.
- **Automatic Reconnection** — Recovers after WiFi changes or connectivity drops.

### 💻 Web UI
- **Responsive UI** — Mobile-first responsive design with no external frameworks.
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

Pre-built binaries may be available in the `firmware/` directory or in GitHub Releases depending on the current release workflow.

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
idf.py -B build_s3 -p /dev/ttyACM0 monitor
# Exit: Ctrl+]
```
