#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rx_confirm.h"

#define PFX_ENDPOINT_BASE 10
#define PFX_ENDPOINTS 4
#define PFX_QUEUE_SIZE 8
#define PFX_FRAME_BITS 66

typedef enum {
    PFX_OK, PFX_SIMULATED, PFX_PENDING, PFX_INVALID, PFX_ABSENT,
    PFX_SPI_ERROR, PFX_TIMEOUT, PFX_NOT_READY, PFX_NO_DATA, PFX_TX_LOCKED,
    PFX_STORAGE_ERROR, PFX_QUEUE_FULL, PFX_EXPIRED
} pfx_result_t;
const char *pfx_result_name(pfx_result_t result);
typedef enum { PFX_SIMULATION, PFX_RADIO_DIAGNOSTIC, PFX_RADIO_ACTIVE } pfx_mode_t;
typedef enum { PFX_OPEN, PFX_CLOSE, PFX_STOP } pfx_command_t;
typedef enum { PFX_CC_OFF, PFX_CC_IDLE, PFX_CC_RX, PFX_CC_SLEEP, PFX_CC_ERROR } pfx_cc_state_t;

typedef struct {
    void *ctx;
    pfx_result_t (*open)(void *ctx);
    pfx_result_t (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t size);
    void (*delay_us)(void *ctx, uint32_t us);
    void (*close)(void *ctx);
} pfx_bus_t;
typedef struct {
    pfx_bus_t bus;
    pfx_cc_state_t state;
    uint8_t part, version;
    uint32_t frequency_khz;
    bool opened;
} pfx_cc_t;
pfx_result_t pfx_cc_init(pfx_cc_t *cc, pfx_bus_t bus);
pfx_result_t pfx_cc_wake(pfx_cc_t *cc);
pfx_result_t pfx_cc_sleep(pfx_cc_t *cc);
pfx_result_t pfx_cc_receive(pfx_cc_t *cc, uint32_t frequency_khz);
/* Deliberately no TX implementation in stage 4, including when mode is ACTIVE. */
pfx_result_t pfx_cc_transmit(pfx_cc_t *cc, const char *bits);
void pfx_cc_close(pfx_cc_t *cc);

typedef struct {
    uint32_t serial;
    uint32_t frequency_khz;
    char frames[3][PFX_FRAME_BITS + 1];
} pfx_binding_t;
typedef struct { pfx_binding_t binding[PFX_ENDPOINTS]; } pfx_config_t;
/* Explicit byte encoding, schema + CRC. One NVS blob; never persist native struct padding. */
#define PFX_CONFIG_BYTES (8 + PFX_ENDPOINTS * (8 + 3 * (PFX_FRAME_BITS + 1)) + 4)
void pfx_config_encode(const pfx_config_t *config, uint8_t out[PFX_CONFIG_BYTES]);
pfx_result_t pfx_config_decode(pfx_config_t *config, const uint8_t *data, size_t size);
bool pfx_frame_valid(const char *bits, uint32_t *serial);

typedef struct { uint8_t endpoint, command; int64_t received_us; } pfx_request_t;
typedef struct {
    pfx_request_t items[PFX_QUEUE_SIZE];
    unsigned head, count;
} pfx_queue_t;
/* Caller serializes queue access (short ESP critical sections; no radio/NVS here). */
pfx_result_t pfx_queue_push(pfx_queue_t *queue, pfx_request_t request);
bool pfx_queue_pop(pfx_queue_t *queue, pfx_request_t *request);

typedef pfx_result_t (*pfx_save_fn)(void *ctx, const pfx_config_t *config);
typedef struct {
    pfx_cc_t *cc;
    pfx_mode_t mode;
    pfx_config_t config;
    pfx_save_fn save;
    void *save_ctx;
    bool learning;
    uint8_t learn_endpoint, learn_command;
    int64_t learn_deadline_us;
    uint32_t learn_frequency_khz;
    rx_confirm_t confirm;
} pfx_gateway_t;
pfx_result_t pfx_gateway_command(pfx_gateway_t *gateway, pfx_request_t request);
pfx_result_t pfx_learn_begin(pfx_gateway_t *gateway, uint8_t endpoint, uint8_t command,
                             unsigned seconds, uint32_t frequency_khz, int64_t now_us);
pfx_result_t pfx_learn_feed(pfx_gateway_t *gateway, const char *bits, int64_t now_us);
pfx_result_t pfx_learn_tick(pfx_gateway_t *gateway, int64_t now_us);
void pfx_learn_cancel(pfx_gateway_t *gateway);
pfx_result_t pfx_binding_clear(pfx_gateway_t *gateway, uint8_t endpoint);
