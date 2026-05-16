# TODO — Optimización Tailscale en ESP32-S3 (repetidor WiFi, 1-2 STA)

Objetivo: maximizar throughput, minimizar latencia y reducir tiempo de conexión
para uso real con Tailscale, sin perder funcionalidad.

Mejora estimada acumulada: **2×–5× throughput TCP**, **−40 a −60 % latencia**,
**−50 % tiempo de conexión inicial**.

---

## Tier 1 — sdkconfig.defaults (riesgo nulo)

Aplicar todo de golpe → `idf.py reconfigure && idf.py build` → smoke test.

- [x] **CPU @ 240 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`).
      Crypto Noise/WG escala lineal. +30 % throughput WG.
- [x] **Compilador app `-O2` (PERF)** (`CONFIG_COMPILER_OPTIMIZATION_PERF=y`).
      Quita `-Og` debug. +25 % throughput hot path C.
- [x] **mbedTLS `-O2` PERF** (`CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_PERF=y`).
      ChaCha20-Poly1305 PSA al 100 %. +30 % throughput WG.
- [x] **Assertion level mínimo** (`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y`).
      +8 % throughput general.
- [x] **I-cache 32 KB / 8-ways / 32 B line**
      (`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y`). El hot path Noise + WG
      no cabe en 16 KB. +10 % throughput WG.
- [x] **TCP_WND y TCP_SND_BUF a 65535**
      (`CONFIG_LWIP_TCP_WND_DEFAULT=65535`,
      `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535`).
      Limpia el cuello brutal del BDP. +250-500 % throughput TCP single-stream.
- [x] **AMPDU BA window 32**
      (`CONFIG_ESP_WIFI_TX_BA_WIN=32`, `CONFIG_ESP_WIFI_RX_BA_WIN=32`).
      Más agregación 802.11n. +15 % WiFi. Requirió subir
      `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16` (constraint
      `RX_BA_WIN ≤ 2 × STATIC_RX_BUFFER_NUM`).
- [x] Build OK. Binario 0x127850 = 1.21 MB (–48 KB vs antes; asserts SILENT
      ahorra strings y compensa el inlining de `-O2`).
- [ ] Smoke test en hardware: web UI accesible, asociación STA, NAPT al WAN
      funciona, Tailscale conecta.

### Cambios colaterales aplicados en Tier 1

- [x] `target_compile_options(... -Wno-error=stringop-truncation)` en
      `components/microlink/CMakeLists.txt`. Bajo `-O2`, GCC promueve
      `-Wstringop-truncation` a error en patrones `strncpy(dst, src,
      sizeof(dst)-1)` (intencionales en código vendor). Silenciado a nivel
      del componente, sin tocar el código.

## Tier 2 — Cambios funcionales (riesgo bajo, validar uno a uno)

- [ ] **Activar zero-copy WG RX** (`CONFIG_ML_ZERO_COPY_WG=y`).
      Quita el round-trip BSD socket → `recvfrom()` → wireguardif_input para
      cada paquete WG. +50–100 % throughput WG, −30 ms RTT.
      Validar: si hay panic en `tcpip_thread`, revertir y diagnosticar.
- [ ] **MSS clamping en netif WireGuard** (≈30 líneas en `ml_wg_mgr.c` o un
      hook en `wireguardif_output`). Reescribir TCP MSS en SYN/SYN-ACK que
      atraviesen el túnel para evitar fragmentación IP dentro de WG (MTU 1280).
      Sin esto, si fragmentaba: hasta +500 %. Si no fragmentaba: 0 %.
- [ ] **Auditar TCP proxy** (`wifi_manager.c`, sección "TCP Proxy").
      Si NAPT cubre todos los port-forwards configurables, eliminar el proxy
      user-space. Si hace falta para el caso "misma LAN" (input traffic), dejarlo.

## Tier 3 — Ajustes finos (opcionales, sin tocar a menos que medidas lo pidan)

- [ ] WiFi `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 10→16 si llega tráfico
      a ráfagas y se ven `RX buffer full` en logs.
- [ ] `CONFIG_LWIP_TCP_RECVMBOX_SIZE` 16→32 (más mbox por socket TCP).
- [ ] Pinning explícito de `tcpip_task` y tareas microlink a cores opuestos
      (ya está en el header v2 de microlink, verificar runtime).
- [ ] `CONFIG_FREERTOS_HZ=1000` (de 100). +precisión en sleeps cortos del
      coord task. Trade-off: +overhead context switch, normalmente +1-2 %
      útil en cargas con `vTaskDelay(1)`.

## Validación

- [ ] `iperf3 -c <peer-tailscale>` desde el cliente STA, baseline antes de
      tocar nada (capturar Mbps + RTT con `ping`).
- [ ] Repetir tras cada Tier para confirmar el porcentaje real.
- [ ] Stress: 24h de uplink continuo para detectar leaks PSRAM.
- [ ] Verificar que la VPN se mantiene estable tras reconexiones WiFi.

## Notas

- ESP32-S3 no acelera ChaCha20-Poly1305 (sí AES/SHA/RSA/ECC). Techo realista
  WireGuard a 240 MHz: **~6 Mbps**. Si el caso de uso pide más, no hay opción
  hardware en S3.
- Wireguard MTU efectivo recomendado: 1280. TCP MSS efectivo: 1240.
- Toda la cadena se valida con `idf.py build` y mensaje de tamaño binario;
  la partición app permite hasta 0x7e0000 bytes (~8 MB), hoy está en ~1.2 MB.
