idf_component_register(
    SRCS
        "src/aes67.c"
        "src/aes67_ptp.c"
        "src/aes67_rtp.c"
        "src/aes67_sap.c"
        "src/aes67_sdp.c"
        "src/aes67_session.c"
        "src/aes67_audio.c"
        "src/aes67_convert.c"
        "src/aes67_net.c"
        "src/aes67_mdns.c"
        "src/aes67_hw_timer.c"
        "src/aes67_clock.c"
        "src/aes67_eth_rx.c"
        "src/aes67_webui.c"
        # Bundled third-party: a minimal IEEE-1588 PTP daemon (Apache NuttX
        # ptpd, ported to ESP-IDF by Espressif) and the EMAC time wrapper from
        # the ESP-IDF ethernet/ptp example. Vendored here so the component is
        # self-contained on the Component Registry. See third_party/*/README.md
        # and the SPDX headers for provenance and licensing.
        "third_party/ptpd/ptpd.c"
        "third_party/esp_eth_time/esp_eth_time.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
        "third_party/ptpd"
        "third_party/ptpd/include"
        "third_party/esp_eth_time"
    PRIV_REQUIRES
        esp_eth
        esp_netif
        esp_timer
        esp_driver_i2s
        mdns
        lwip
        esp_psram
        esp_mm
        esp_http_server
        esp_event
        json
)

# One source of truth for the version: the component manifest. The macros
# AES67_VERSION_MAJOR/MINOR/PATCH and AES67_VERSION_STRING come from here.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml" AES67_VERSION_LINE REGEX "^version:")
string(REGEX REPLACE "version: *\"?([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\1;\\2;\\3" AES67_VERSION_PARTS "${AES67_VERSION_LINE}")
list(GET AES67_VERSION_PARTS 0 AES67_VERSION_MAJOR)
list(GET AES67_VERSION_PARTS 1 AES67_VERSION_MINOR)
list(GET AES67_VERSION_PARTS 2 AES67_VERSION_PATCH)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    AES67_VERSION_MAJOR=${AES67_VERSION_MAJOR}
    AES67_VERSION_MINOR=${AES67_VERSION_MINOR}
    AES67_VERSION_PATCH=${AES67_VERSION_PATCH}
    AES67_VERSION_STRING="${AES67_VERSION_MAJOR}.${AES67_VERSION_MINOR}.${AES67_VERSION_PATCH}")

# The web page is embedded gzipped. Generate the archive in the build
# directory from web/index.html, so it is never stale and never tracked.
set(AES67_WEB_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${AES67_WEB_GZ}"
    COMMAND gzip -9 -n -c "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" > "${AES67_WEB_GZ}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    COMMENT "Compressing web/index.html"
    VERBATIM
)
add_custom_target(aes67_web_gz DEPENDS "${AES67_WEB_GZ}")
add_dependencies(${COMPONENT_LIB} aes67_web_gz)
set_property(SOURCE "${AES67_WEB_GZ}" PROPERTY GENERATED TRUE)
target_add_binary_data(${COMPONENT_LIB} "${AES67_WEB_GZ}" BINARY)

# Every non-static function must have a prototype in a header, so the
# public surface is what the headers say. The vendored sources are exempt.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wmissing-prototypes)
set_source_files_properties(
    third_party/esp_eth_time/esp_eth_time.c
    PROPERTIES COMPILE_OPTIONS "-Wno-missing-prototypes"
)
# ptpd.c is a third-party port with intentionally-unused helpers behind
# Kconfig; silence that one warning for the bundled file only.
set_source_files_properties(
    third_party/ptpd/ptpd.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-missing-prototypes"
)

