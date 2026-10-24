set(LEGACY "${CMAKE_CURRENT_LIST_DIR}/../../../firmware/main")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    execute_process(COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/extract-radio-config.py"
        "${LEGACY}/cc1101.c" "${CMAKE_CURRENT_BINARY_DIR}/radio_profile_generated.h"
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LEGACY}/cc1101.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../tools/extract-radio-config.py")
endif()
idf_component_register(SRCS "cc1101_port.c" "gateway.c" "esp_radio.c" "usb_console.c"
    "${LEGACY}/rx_capture.c" "${LEGACY}/keeloq_decode.c" "${LEGACY}/rx_confirm.c"
    INCLUDE_DIRS "include" "${LEGACY}"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_rmt esp_timer esp_pm nvs_flash esp_driver_usb_serial_jtag vfs)
