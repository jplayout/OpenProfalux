/* Checked receive-only transport port. Register values are generated from the legacy driver. */
#include "pfx_radio.h"
#include <string.h>
#include "radio_profile_generated.h"

static pfx_result_t transfer(pfx_cc_t *cc, const uint8_t *tx, uint8_t *rx, size_t n)
{
    if (!cc->opened) return PFX_NOT_READY;
    /* STX is never permitted, even through an accidental future caller. */
    if (tx[0] == 0x35) return PFX_TX_LOCKED;
    pfx_result_t result = cc->bus.transfer(cc->bus.ctx, tx, rx, n);
    if (result != PFX_OK) cc->state = PFX_CC_ERROR;
    return result;
}
static pfx_result_t strobe(pfx_cc_t *cc, uint8_t command)
{
    uint8_t value = 0;
    return transfer(cc, &command, &value, 1);
}
static pfx_result_t read_reg(pfx_cc_t *cc, uint8_t addr, bool status, uint8_t *value)
{
    uint8_t tx[] = {addr | (status ? 0xc0 : 0x80), 0}, rx[2] = {0};
    pfx_result_t r = transfer(cc, tx, rx, sizeof(tx));
    if (r == PFX_OK) *value = rx[1];
    return r;
}
static pfx_result_t write_reg(pfx_cc_t *cc, uint8_t addr, uint8_t value)
{
    for (unsigned i = 0; i < 6; i++) {
        uint8_t tx[] = {addr, value}, rx[2], actual = 0;
        pfx_result_t r = transfer(cc, tx, rx, sizeof(tx));
        if (r != PFX_OK) return r;
        r = read_reg(cc, addr, false, &actual);
        if (r != PFX_OK) return r;
        if (actual == value) return PFX_OK;
        cc->bus.delay_us(cc->bus.ctx, 1000);
    }
    cc->state = PFX_CC_ERROR;
    return PFX_SPI_ERROR;
}
static pfx_result_t wait_idle(pfx_cc_t *cc)
{
    for (unsigned i = 0; i < 100; i++) {
        uint8_t state;
        pfx_result_t r = read_reg(cc, 0x35, true, &state);
        if (r != PFX_OK) return r;
        if ((state & 0x1f) == 1) { cc->state = PFX_CC_IDLE; return PFX_OK; }
        cc->bus.delay_us(cc->bus.ctx, 100);
    }
    cc->state = PFX_CC_ERROR;
    return PFX_TIMEOUT;
}
pfx_result_t pfx_cc_init(pfx_cc_t *cc, pfx_bus_t bus)
{
    memset(cc, 0, sizeof(*cc)); cc->bus = bus;
    if (!bus.open || !bus.transfer || !bus.delay_us || !bus.close) return PFX_INVALID;
    pfx_result_t r = bus.open(bus.ctx);
    if (r != PFX_OK) { cc->state = PFX_CC_ERROR; return r; }
    cc->opened = true;
    r = strobe(cc, 0x30); /* SRES, never STX, no PA configuration. */
    if (r == PFX_OK) {
        bus.delay_us(bus.ctx, 1000);
        r = read_reg(cc, 0x30, true, &cc->part);
    }
    if (r == PFX_OK) r = read_reg(cc, 0x31, true, &cc->version);
    if (r == PFX_OK && (cc->part != 0 || (cc->version != 0x04 && cc->version != 0x14))) r = PFX_ABSENT;
    if (r == PFX_OK) r = pfx_cc_sleep(cc);
    if (r != PFX_OK) { bus.close(bus.ctx); cc->opened = false; cc->state = PFX_CC_ERROR; }
    return r;
}
pfx_result_t pfx_cc_wake(pfx_cc_t *cc)
{
    pfx_result_t r = strobe(cc, 0x36); /* CS assertion wakes SPWD; SIDLE quiesces RX. */
    return r == PFX_OK ? wait_idle(cc) : r;
}
pfx_result_t pfx_cc_sleep(pfx_cc_t *cc)
{
    pfx_result_t r = pfx_cc_wake(cc);
    if (r == PFX_OK) r = strobe(cc, 0x39); /* SPWD takes effect at CS rising edge. */
    if (r == PFX_OK) cc->state = PFX_CC_SLEEP;
    return r;
}
pfx_result_t pfx_cc_receive(pfx_cc_t *cc, uint32_t khz)
{
    if (khz != 868350 && khz != 868425) return PFX_INVALID;
    pfx_result_t r = pfx_cc_wake(cc);
    for (size_t i = 0; r == PFX_OK && i < sizeof(pfx_rx_profile)/sizeof(pfx_rx_profile[0]); i++)
        r = write_reg(cc, pfx_rx_profile[i].reg, pfx_rx_profile[i].value);
    uint32_t word = ((uint64_t)khz * 65536 + 13000) / 26000;
    for (unsigned i = 0; r == PFX_OK && i < 3; i++) r = write_reg(cc, 0x0d + i, word >> (16 - 8*i));
    if (r == PFX_OK) r = strobe(cc, 0x3a); /* SFRX in IDLE */
    if (r == PFX_OK) r = strobe(cc, 0x34); /* SRX */
    if (r == PFX_OK) {
        bool ready = false;
        for (unsigned i = 0; i < 100; i++) {
            uint8_t state;
            r = read_reg(cc, 0x35, true, &state);
            if (r != PFX_OK) break;
            if ((state & 0x1f) == 0x0d) { ready = true; break; }
            cc->bus.delay_us(cc->bus.ctx, 100);
        }
        if (r == PFX_OK && !ready) r = PFX_TIMEOUT;
    }
    if (r == PFX_OK) { cc->frequency_khz = khz; cc->state = PFX_CC_RX; }
    else { (void)pfx_cc_sleep(cc); cc->state = PFX_CC_ERROR; }
    return r;
}
pfx_result_t pfx_cc_transmit(pfx_cc_t *cc, const char *bits)
{
    (void)cc; (void)bits;
    return PFX_TX_LOCKED;
}
void pfx_cc_close(pfx_cc_t *cc)
{
    if (cc->opened) { (void)pfx_cc_sleep(cc); cc->bus.close(cc->bus.ctx); }
    cc->opened = false; cc->state = PFX_CC_OFF;
}
