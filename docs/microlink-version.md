# MicroLink version

Repository: https://github.com/CamM2325/microlink

Integration method: vendor copy under `components/microlink/`.

Commit: `216da3300f0493b0860247d43f7af5ce29df63a5`

Date reviewed: 2026-05-08

Reason for selection: MicroLink is the preferred embedded Tailscale client for this integration. It is linked only in the ESP32-S3 N16R8 profile and is started through the firmware-owned `tailscale_manager` wrapper.

Verification note: the vendor copy received in this workspace does not include `.git` metadata or a submodule entry, so the commit is recorded from the local patch headers already present in `components/microlink/CMakeLists.txt`. Re-verifying the exact upstream tree should be done before committing or releasing.

## Local changes

- `components/microlink/CMakeLists.txt`
  - Removed the old `json` component dependency; ESP-IDF 6 provides cJSON via managed component `espressif/cjson`.
  - Added `REQUIRED_IDF_TARGETS esp32s3`.
  - Builds cellular/network-switch sources only when their Kconfig options are enabled.
- `components/microlink/idf_component.yml`
  - Adds managed dependency `espressif/cjson`.
- `components/microlink/include/microlink_internal.h`
  - Removed direct `mbedtls/entropy.h` and `mbedtls/ctr_drbg.h` usage for mbedTLS 4 / TF-PSA.
- `components/microlink/include/microlink.h`
  - Added `control_host` to `microlink_config_t` so the firmware-owned Tailscale config can select an alternate control plane without enabling MicroLink's internal HTTPD.
- `components/microlink/src/ml_derp.c`
  - Removed per-connection CTR-DRBG setup; mbedTLS 4 TLS uses the PSA RNG initialized by ESP-IDF.
- `components/microlink/src/microlink.c`
  - Applies `microlink_config_t.control_host` to the internal `ctrl_host` override before NVS config overrides.
- `components/microlink/src/ml_noise.c`
  - Replaced removed low-level `mbedtls/chacha20.h` / `mbedtls/chachapoly.h` APIs with PSA AEAD (`PSA_ALG_CHACHA20_POLY1305`).
- `components/microlink/src/nacl_box.c`
  - Adjusted exact protocol string constants to compile cleanly with GCC 15.
- `components/wireguard_lwip/CMakeLists.txt`
  - Local ESP-IDF component wrapper for the WireGuard lwIP backend.
- `components/wireguard_lwip/src/wireguard.c`
   - Adjusted exact protocol string constants to compile cleanly with GCC 15 while preserving effective byte lengths.
- `components/microlink/src/ml_coord.c` (Fase 9)
   - Changed control plane port from HTTP/80 to HTTPS/443 (`ML_CTRL_PORT`).
   - Wrapped coord socket with mbedTLS TLS before HTTP Upgrade.
   - All coord send/recv operations are TLS-aware.
   - (Fase 10) DERP connect is now async (no 15s blocking wait).
   - (Fase 10) `do_fetch_peers()` failure is non-fatal — coord continues to long-poll.
- `components/microlink/include/microlink_internal.h` (Fase 10)
   - `ML_CTRL_PROTOCOL_VER` updated from 131 to 138 (CurrentCapabilityVersion).
- `sdkconfig.defaults.s3-n16r8` (Fase 10)
   - Added `CONFIG_MBEDTLS_CHACHA20_C=y` and `CONFIG_MBEDTLS_CHACHAPOLY_C=y`.
   - Without these, `psa_aead_encrypt(PSA_ALG_CHACHA20_POLY1305)` fails silently and the Noise msg1 is sent with garbage encryption.

## Build status

Verified on 2026-05-08 with ESP-IDF `v6.1-dev-2938-g12f36a021f`:

```text
idf.py -B build_s3 reconfigure
idf.py -B build_s3 build
```

Result:

```text
build_s3/esp-idf/microlink/libmicrolink.a      1.4M
build_s3/esp-idf/wireguard_lwip/libwireguard_lwip.a 424K
wifi_repeater.bin binary size 0xf1ae0 bytes
Smallest app partition is 0x7e0000 bytes
0x6ee520 bytes (88%) free
```

MicroLink is compiled but not linked into the final application yet because no runtime code references it. This is intentional for Fase 2.

## Runtime link status

Fase 7 links MicroLink from `main/tailscale_manager.c`, still with Tailscale disabled by default.

Verified on 2026-05-09 (Fase 10 — after Noise handshake fix):

```text
idf.py -B build_s3 build
wifi_repeater.bin binary size 0x1130f0 bytes
Smallest app partition is 0x7e0000 bytes
0x6ccf10 bytes (86%) free

Noise handshake: complete (IK v138, ChaCha20-Poly1305 via PSA)
Register: complete (VPN IP 100.81.77.58 assigned)
MapResponse: complete (22KB, 13 Noise frames, 20+ DERP regions parsed)
DERP: pending (conn=0, connects async)
Status: "connecting" (not yet stable "connected" — Fase 11)
```
