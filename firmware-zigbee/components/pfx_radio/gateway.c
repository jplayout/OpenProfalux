#include "pfx_radio.h"
#include <string.h>

const char *pfx_result_name(pfx_result_t r)
{
    static const char *names[] = {"OK", "SIMULATION", "PENDING", "INVALID", "ABSENT",
        "SPI_ERROR", "TIMEOUT", "NOT_READY", "NO_DATA", "TX_LOCKED", "STORAGE_ERROR", "QUEUE_FULL", "EXPIRED"};
    return (unsigned)r < sizeof(names)/sizeof(names[0]) ? names[r] : "UNKNOWN";
}
bool pfx_frame_valid(const char *bits, uint32_t *serial)
{
    if (!bits || strnlen(bits, PFX_FRAME_BITS + 1) != PFX_FRAME_BITS) return false;
    for (unsigned i = 0; i < PFX_FRAME_BITS; i++) if (bits[i] != '0' && bits[i] != '1') return false;
    uint32_t s = 0;
    for (unsigned i = 0; i < 28; i++) if (bits[32+i] == '1') s |= 1u << i;
    if (!s || s == 0x0fffffff || !memchr(bits+60, '1', 4)) return false;
    if (serial) *serial = s;
    return true;
}
static bool valid_request(pfx_request_t r)
{
    return r.endpoint >= PFX_ENDPOINT_BASE && r.endpoint < PFX_ENDPOINT_BASE + PFX_ENDPOINTS && r.command <= PFX_STOP;
}
pfx_result_t pfx_queue_push(pfx_queue_t *q, pfx_request_t r)
{
    if (!valid_request(r)) return PFX_INVALID;
    if (q->count == PFX_QUEUE_SIZE) return PFX_QUEUE_FULL;
    q->items[(q->head + q->count) % PFX_QUEUE_SIZE] = r; q->count++;
    return PFX_OK;
}
bool pfx_queue_pop(pfx_queue_t *q, pfx_request_t *r)
{
    if (!q->count) return false;
    *r = q->items[q->head]; q->head = (q->head+1) % PFX_QUEUE_SIZE; q->count--;
    return true;
}
pfx_result_t pfx_gateway_command(pfx_gateway_t *g, pfx_request_t r)
{
    if (!valid_request(r) || g->mode < PFX_SIMULATION || g->mode > PFX_RADIO_ACTIVE) return PFX_INVALID;
    if (g->mode == PFX_SIMULATION) return PFX_SIMULATED;
    if (!g->cc || !g->cc->opened || g->cc->state == PFX_CC_ERROR) return PFX_NOT_READY;
    const pfx_binding_t *b = &g->config.binding[r.endpoint - PFX_ENDPOINT_BASE];
    uint32_t serial;
    if (!pfx_frame_valid(b->frames[r.command], &serial) || serial != b->serial ||
        (b->frequency_khz != 868350 && b->frequency_khz != 868425)) return PFX_NO_DATA;
    /* ACTIVE is a reserved future state, not an authorization to emit in stage 4. */
    return PFX_TX_LOCKED;
}
void pfx_learn_cancel(pfx_gateway_t *g)
{
    g->learning = false;
    rx_confirm_reset(&g->confirm);
}
pfx_result_t pfx_learn_tick(pfx_gateway_t *g, int64_t now)
{
    if (g->learning && now >= g->learn_deadline_us) { pfx_learn_cancel(g); return PFX_EXPIRED; }
    return PFX_PENDING;
}
pfx_result_t pfx_learn_begin(pfx_gateway_t *g, uint8_t ep, uint8_t cmd, unsigned seconds, uint32_t khz, int64_t now)
{
    if (!valid_request((pfx_request_t){.endpoint=ep,.command=cmd}) || !seconds || seconds>60 || now<0 ||
        (khz!=868350 && khz!=868425)) return PFX_INVALID;
    if (g->mode == PFX_SIMULATION || !g->cc || !g->cc->opened || g->cc->state == PFX_CC_ERROR) return PFX_NOT_READY;
    if (g->learning) return PFX_NOT_READY;
    rx_confirm_reset(&g->confirm);
    g->learn_endpoint=ep; g->learn_command=cmd; g->learn_frequency_khz=khz;
    g->learn_deadline_us=now+(int64_t)seconds*1000000; g->learning=true;
    return PFX_OK;
}
pfx_result_t pfx_learn_feed(pfx_gateway_t *g, const char *bits, int64_t now)
{
    if (!g->learning) return PFX_NOT_READY;
    if (pfx_learn_tick(g, now) == PFX_EXPIRED) return PFX_EXPIRED;
    uint32_t serial;
    if (!pfx_frame_valid(bits, &serial)) return PFX_INVALID;
    if (!rx_confirm_push(&g->confirm, bits, PFX_FRAME_BITS, now)) return PFX_PENDING;
    pfx_config_t candidate = g->config;
    pfx_binding_t *b=&candidate.binding[g->learn_endpoint-PFX_ENDPOINT_BASE];
    if (b->serial && (b->serial!=serial || b->frequency_khz!=g->learn_frequency_khz)) return PFX_INVALID;
    b->serial=serial; b->frequency_khz=g->learn_frequency_khz;
    memcpy(b->frames[g->learn_command], g->confirm.full, PFX_FRAME_BITS+1);
    /* One checked durable write before publishing the new mapping in RAM. */
    pfx_result_t r=g->save ? g->save(g->save_ctx,&candidate) : PFX_STORAGE_ERROR;
    if (r == PFX_OK) { g->config=candidate; pfx_learn_cancel(g); }
    else { pfx_learn_cancel(g); return PFX_STORAGE_ERROR; }
    return r;
}
pfx_result_t pfx_binding_clear(pfx_gateway_t *g, uint8_t endpoint)
{
    if (endpoint < PFX_ENDPOINT_BASE || endpoint >= PFX_ENDPOINT_BASE + PFX_ENDPOINTS) return PFX_INVALID;
    if (g->learning) return PFX_NOT_READY;
    pfx_config_t candidate = g->config;
    memset(&candidate.binding[endpoint - PFX_ENDPOINT_BASE], 0, sizeof(candidate.binding[0]));
    if (!g->save || g->save(g->save_ctx, &candidate) != PFX_OK) return PFX_STORAGE_ERROR;
    g->config = candidate;
    return PFX_OK;
}
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc=~0u;
    while(n--) { crc^=*p++; for(unsigned i=0;i<8;i++) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1u))); }
    return ~crc;
}
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=v>>(8*i); }
static uint32_t get32(const uint8_t *p) { uint32_t v=0; for(unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v; }
void pfx_config_encode(const pfx_config_t *c, uint8_t out[PFX_CONFIG_BYTES])
{
    memset(out,0,PFX_CONFIG_BYTES); memcpy(out,"PFX4",4); put32(out+4,1);
    size_t at=8;
    for(unsigned i=0;i<PFX_ENDPOINTS;i++) {
        put32(out+at,c->binding[i].serial); at+=4;
        put32(out+at,c->binding[i].frequency_khz); at+=4;
        memcpy(out+at,c->binding[i].frames,sizeof(c->binding[i].frames)); at+=sizeof(c->binding[i].frames);
    }
    put32(out+at,crc32(out,at));
}
pfx_result_t pfx_config_decode(pfx_config_t *c,const uint8_t *data,size_t size)
{
    if(!c || !data || size!=PFX_CONFIG_BYTES || memcmp(data,"PFX4",4) || get32(data+4)!=1 || get32(data+size-4)!=crc32(data,size-4)) return PFX_INVALID;
    pfx_config_t temp={0}; size_t at=8;
    for(unsigned i=0;i<PFX_ENDPOINTS;i++) {
        pfx_binding_t *b=&temp.binding[i]; b->serial=get32(data+at); at+=4;
        b->frequency_khz=get32(data+at); at+=4;
        memcpy(b->frames,data+at,sizeof(b->frames)); at+=sizeof(b->frames);
        bool any=false;
        for(unsigned j=0;j<3;j++) {
            if(!b->frames[j][0]) {
                for(unsigned k=0;k<sizeof(b->frames[j]);k++) if(b->frames[j][k]) return PFX_INVALID;
                continue;
            }
            uint32_t s;
            if(!pfx_frame_valid(b->frames[j],&s) || s!=b->serial) return PFX_INVALID;
            any=true;
        }
        if(any ? (b->frequency_khz!=868350 && b->frequency_khz!=868425) : (b->serial || b->frequency_khz)) return PFX_INVALID;
    }
    *c=temp; return PFX_OK;
}
