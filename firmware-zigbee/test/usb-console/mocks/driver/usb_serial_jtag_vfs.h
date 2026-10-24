#pragma once
typedef enum { ESP_LINE_ENDINGS_LF } esp_line_endings_t;
void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t mode);
void usb_serial_jtag_vfs_use_driver(void);
