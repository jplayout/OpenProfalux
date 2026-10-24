#include "sdkconfig.h"
#ifdef CONFIG_OPF_RADIO_CONSOLE
#if !defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#error "Radio console requires native USB Serial/JTAG stdin"
#endif
#include "pfx_usb_console.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

static const char *TAG = "PFX_USB_CONSOLE";

esp_err_t pfx_usb_console_init(void)
{
    /* Single-owner initialization: console_worker is the only caller/reader. */
    static bool ready;
    if (ready) return ESP_OK;

    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        ESP_LOGE(TAG, "Cannot disable stdin buffering");
        return ESP_FAIL;
    }
    int fd = fileno(stdin);
    int flags = fd < 0 ? -1 : fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Cannot configure blocking stdin: fd=%d errno=%d", fd, errno);
        return ESP_FAIL;
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .rx_buffer_size = 1024,
            .tx_buffer_size = 2048,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "USB Serial/JTAG driver installed (RX=1024 TX=2048 bytes)");
    } else {
        ESP_LOGI(TAG, "Reusing installed USB Serial/JTAG driver");
    }
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_use_driver();
    clearerr(stdin);
    ready = true;
    ESP_LOGI(TAG, "Console ready: native USB, interrupt-driven VFS, blocking unbuffered stdin fd=%d", fd);
    return ESP_OK;
}
#endif
