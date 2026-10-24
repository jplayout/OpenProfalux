#pragma once
#include "esp_err.h"
/* Called once by the sole stdin reader, before its first getchar(). No USB reset. */
esp_err_t pfx_usb_console_init(void);
