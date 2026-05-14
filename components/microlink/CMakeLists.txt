# --- LOCAL PATCHES (WiFi-Repeater-WPA2-Enterprise vendor copy) --------------
# Changes vs upstream commit 216da3300f0493b0860247d43f7af5ce29df63a5:
#  1. Removed 'json' from REQUIRES (cJSON moved to managed component
#     'espressif/cjson' in ESP-IDF 6.0; declared in idf_component.yml).
#  2. Added REQUIRED_IDF_TARGETS=esp32s3 (this vendor copy is S3-only).
#  3. Build cellular/net-switch sources only when their Kconfig options are
#     enabled; the repeater profile uses WiFi STA/AP only.
# Target-gating itself is done in the project root CMakeLists.txt via
# EXCLUDE_COMPONENTS, NOT inside this file (that pattern is incompatible
# with ESP-IDF 6 Build System v2 dependency resolution).
# See docs/microlink-version.md.
# --- END LOCAL PATCHES ------------------------------------------------------

set(SRCS
    "src/microlink.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/x25519.c"
    "src/ml_zerocopy.c"
    "src/ml_config_httpd.c"
)

if(CONFIG_ML_ENABLE_CELLULAR)
    list(APPEND SRCS
        "src/ml_cellular.c"
        "src/ml_at_socket.c"
    )
endif()

if(CONFIG_ML_ENABLE_NET_SWITCH)
    list(APPEND SRCS
        "src/ml_net_switch.c"
    )
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        lwip
        mbedtls
        driver
        esp_driver_tsens
        esp_http_server
    PRIV_REQUIRES
        wireguard_lwip
    REQUIRED_IDF_TARGETS
        esp32s3
)
