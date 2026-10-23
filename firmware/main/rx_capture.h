#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "keeloq_decode.h"

typedef struct {
    kq_sym_t *symbols;
    size_t capacity;
    size_t count;
    bool complete;
    bool overflow;
    bool failed;
} rx_capture_t;

void rx_capture_init(rx_capture_t *capture, kq_sym_t *storage, size_t capacity);
void rx_capture_reset(rx_capture_t *capture);
bool rx_capture_append(rx_capture_t *capture, const kq_sym_t *symbols,
                       size_t count, bool is_last);
void rx_capture_mark_event_lost(rx_capture_t *capture);
size_t rx_capture_snapshot(const rx_capture_t *capture, kq_sym_t *out,
                           size_t out_capacity);