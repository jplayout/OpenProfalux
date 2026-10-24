#pragma once
#include "pfx_radio.h"
/* Start a worker; does not wait for a physical radio or change Zigbee storage. */
void pfx_runtime_start(void);
pfx_result_t pfx_runtime_submit(pfx_request_t request);
