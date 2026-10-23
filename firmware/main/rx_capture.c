#include "rx_capture.h"
#include <string.h>

void rx_capture_init(rx_capture_t *capture, kq_sym_t *storage, size_t capacity)
{
    capture->symbols = storage;
    capture->capacity = capacity;
    rx_capture_reset(capture);
    if (!storage || capacity == 0) capture->failed = true;
}

void rx_capture_reset(rx_capture_t *capture)
{
    capture->count = 0;
    capture->complete = false;
    capture->overflow = false;
    capture->failed = !capture->symbols || capture->capacity == 0;
}

bool rx_capture_append(rx_capture_t *capture, const kq_sym_t *symbols,
                       size_t count, bool is_last)
{
    if (capture->failed || capture->complete || (count != 0 && !symbols)) {
        capture->failed = true;
        return false;
    }
    if (capture->overflow) {
        capture->complete = is_last;
        return false;
    }

    if (count > capture->capacity - capture->count) {
        capture->overflow = true;
        capture->complete = is_last;
        return false;
    }
    if (count != 0) {
        memcpy(capture->symbols + capture->count, symbols, count * sizeof(*symbols));
        capture->count += count;
    }
    capture->complete = is_last;
    return true;
}

void rx_capture_mark_event_lost(rx_capture_t *capture)
{
    capture->failed = true;
    capture->complete = false;
}

size_t rx_capture_snapshot(const rx_capture_t *capture, kq_sym_t *out,
                           size_t out_capacity)
{
    if (!capture->complete || capture->failed || capture->overflow ||
        !out || out_capacity < capture->count)
        return 0;
    memcpy(out, capture->symbols, capture->count * sizeof(*out));
    return capture->count;
}