#include "pfx_radio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t regs[64], part, version, marc;
    unsigned opened, closed, transfers, stx, sleeps, rx, writes;
    unsigned fail_at;
    bool fail_open, high_bus, zero_bus, bad_write, stuck_idle, stuck_rx;
    pfx_result_t error;
} fake_t;
static pfx_result_t open_bus(void *ctx) { fake_t *f=ctx; f->opened++; return f->fail_open ? PFX_SPI_ERROR : PFX_OK; }
static void close_bus(void *ctx) { ((fake_t *)ctx)->closed++; }
static void delay_bus(void *ctx,uint32_t us) { (void)ctx; (void)us; }
static pfx_result_t transfer_bus(void *ctx,const uint8_t *tx,uint8_t *out,size_t n)
{
    fake_t *f=ctx; f->transfers++;
    if(f->fail_at && f->transfers==f->fail_at) return f->error;
    memset(out,0,n);
    if(n==1) {
        if(tx[0]==0x35) { f->stx++; assert(!"STX reached bus"); }
        if(tx[0]==0x30 || tx[0]==0x36) f->marc=f->stuck_idle ? 0 : 1;
        if(tx[0]==0x39) f->sleeps++;
        if(tx[0]==0x34) { f->marc=f->stuck_rx ? 1 : 0x0d; f->rx++; }
    } else if(tx[0]&0x80) {
        uint8_t addr=tx[0]&0x3f;
        out[1]=addr==0x30 ? f->part : addr==0x31 ? f->version : addr==0x35 ? f->marc : f->regs[addr];
        if(f->high_bus) out[1]=0xff;
        if(f->zero_bus) out[1]=0;
    } else { if(!f->bad_write) f->regs[tx[0]]=tx[1]; f->writes++; }
    return PFX_OK;
}
static pfx_bus_t bus(fake_t *f) { return (pfx_bus_t){f,open_bus,transfer_bus,delay_bus,close_bus}; }
static fake_t fresh(void) { return (fake_t){.version=0x14,.marc=1,.error=PFX_SPI_ERROR}; }
static void frame(char out[67],uint32_t serial,uint8_t button)
{
    memset(out,'0',66);out[66]=0;
    for(unsigned i=0;i<28;i++) out[32+i]=(serial&(1u<<i)) ? '1':'0';
    for(unsigned i=0;i<4;i++) out[60+i]=(button&(1u<<i)) ? '1':'0';
}
typedef struct { pfx_config_t durable; unsigned saves; bool fail, cut_before_commit; } disk_t;
static pfx_result_t save(void *ctx,const pfx_config_t *c)
{
    disk_t *d=ctx; d->saves++;
    if(d->fail || d->cut_before_commit) return PFX_STORAGE_ERROR;
    uint8_t bytes[PFX_CONFIG_BYTES];pfx_config_encode(c,bytes);
    assert(pfx_config_decode(&d->durable,bytes,sizeof(bytes))==PFX_OK);
    return PFX_OK;
}
int main(void)
{
    pfx_cc_t cc; fake_t f=fresh();
    f.fail_open=true; assert(pfx_cc_init(&cc,bus(&f))==PFX_SPI_ERROR); assert(!cc.opened);
    f=fresh();f.high_bus=true;assert(pfx_cc_init(&cc,bus(&f))==PFX_ABSENT);assert(f.closed==1);
    f=fresh();f.zero_bus=true;assert(pfx_cc_init(&cc,bus(&f))==PFX_ABSENT);
    f=fresh();f.version=0x99;assert(pfx_cc_init(&cc,bus(&f))==PFX_ABSENT);
    f=fresh();f.fail_at=1;f.error=PFX_TIMEOUT;assert(pfx_cc_init(&cc,bus(&f))==PFX_TIMEOUT);
    f=fresh();f.fail_at=2;assert(pfx_cc_init(&cc,bus(&f))==PFX_SPI_ERROR);
    f=fresh();f.stuck_idle=true;assert(pfx_cc_init(&cc,bus(&f))==PFX_TIMEOUT);
    puts("PASS absent module (floating high/zero/unknown ID), SPI init/read/ready timeout, no abort");
    f=fresh();assert(pfx_cc_init(&cc,bus(&f))==PFX_OK);
    assert(cc.state==PFX_CC_SLEEP && f.sleeps==1 && f.rx==0 && f.stx==0 && f.writes==0);
    assert(pfx_cc_wake(&cc)==PFX_OK && cc.state==PFX_CC_IDLE);
    assert(pfx_cc_sleep(&cc)==PFX_OK && cc.state==PFX_CC_SLEEP);
    assert(pfx_cc_receive(&cc,0)==PFX_INVALID);
    assert(pfx_cc_receive(&cc,868350)==PFX_OK && cc.state==PFX_CC_RX);
    assert(f.regs[0x0d]==0x21 && f.regs[0x0e]==0x65); // 26 MHz crystal, explicit 868.35 MHz
    assert(pfx_cc_sleep(&cc)==PFX_OK);
    assert(pfx_cc_transmit(&cc,"any")==PFX_TX_LOCKED && f.stx==0);
    f.bad_write=true;assert(pfx_cc_receive(&cc,868425)==PFX_SPI_ERROR);
    f.bad_write=false; f.stuck_rx=true;assert(pfx_cc_receive(&cc,868425)==PFX_TIMEOUT);
    f.stuck_rx=false;
    pfx_cc_close(&cc);assert(!cc.opened && cc.state==PFX_CC_OFF);
    assert(pfx_cc_wake(&cc)==PFX_NOT_READY);
    puts("PASS init never TX/RX, sleep/wake/RX, checked register writes, RX timeout, TX lock");
    pfx_gateway_t g={.cc=&cc};pfx_request_t request={10,PFX_OPEN,100};
    assert(pfx_gateway_command(&g,request)==PFX_SIMULATED); // Boot works with absent CC1101.
    g.mode=PFX_RADIO_DIAGNOSTIC;assert(pfx_gateway_command(&g,request)==PFX_NOT_READY);
    f=fresh();assert(pfx_cc_init(&cc,bus(&f))==PFX_OK);
    assert(pfx_gateway_command(&g,request)==PFX_NO_DATA);
    assert(pfx_gateway_command(&g,(pfx_request_t){9,0,0})==PFX_INVALID);
    pfx_queue_t queue={0};
    for(unsigned i=0;i<PFX_QUEUE_SIZE;i++) assert(pfx_queue_push(&queue,(pfx_request_t){10+i%4,i%3,i})==PFX_OK);
    assert(pfx_queue_push(&queue,request)==PFX_QUEUE_FULL);
    assert(pfx_queue_push(&queue,(pfx_request_t){10,3,0})==PFX_INVALID);
    for(unsigned i=0;i<PFX_QUEUE_SIZE;i++) { pfx_request_t out;assert(pfx_queue_pop(&queue,&out));assert(out.received_us==i); }
    assert(!pfx_queue_pop(&queue,&request));
    for(unsigned i=0;i<100;i++) { assert(pfx_queue_push(&queue,(pfx_request_t){13,2,i})==PFX_OK);assert(pfx_queue_pop(&queue,&request));assert(request.received_us==i); }
    puts("PASS Zigbee simulation, missing mapping, all endpoints, queue bursts/full/FIFO/wrap");
    disk_t disk={0};g.save=save;g.save_ctx=&disk;
    assert(pfx_learn_begin(&g,10,0,61,868350,0)==PFX_INVALID);
    assert(pfx_learn_begin(&g,10,0,2,0,0)==PFX_INVALID);
    assert(pfx_learn_begin(&g,10,0,2,868350,0)==PFX_OK);
    assert(pfx_learn_tick(&g,2000000)==PFX_EXPIRED && !g.learning && !disk.saves);
    char bits[67];frame(bits,0x1234567,2);
    for(unsigned ep=10;ep<=13;ep++) for(unsigned cmd=0;cmd<3;cmd++) {
        frame(bits,0x1234567+ep,1u<<cmd);
        assert(pfx_learn_begin(&g,ep,cmd,10,868350,0)==PFX_OK);
        assert(pfx_learn_feed(&g,"bad",10)==PFX_INVALID);
        assert(pfx_learn_feed(&g,bits,1000)==PFX_PENDING);
        bits[65]='1';assert(pfx_learn_feed(&g,bits,2000)==PFX_OK);
        assert(!g.learning);
        assert(pfx_gateway_command(&g,(pfx_request_t){ep,cmd,0})==PFX_TX_LOCKED);
        g.mode=PFX_RADIO_ACTIVE;assert(pfx_gateway_command(&g,(pfx_request_t){ep,cmd,0})==PFX_TX_LOCKED);g.mode=PFX_RADIO_DIAGNOSTIC;
    }
    assert(disk.saves==12 && f.stx==0);
    uint8_t data[PFX_CONFIG_BYTES];pfx_config_encode(&g.config,data);
    pfx_config_t restored={0};assert(pfx_config_decode(&restored,data,sizeof(data))==PFX_OK);
    assert(!memcmp(&restored,&g.config,sizeof(restored)));
    pfx_config_t old=g.config;
    disk.cut_before_commit=true;
    assert(pfx_learn_begin(&g,10,0,10,868350,0)==PFX_OK);
    frame(bits,g.config.binding[0].serial,2); bits[0]='1';
    assert(pfx_learn_feed(&g,bits,1000)==PFX_PENDING);
    assert(pfx_learn_feed(&g,bits,2000)==PFX_STORAGE_ERROR);
    assert(!memcmp(&g.config,&old,sizeof(old)) && !memcmp(&disk.durable,&old,sizeof(old)));
    assert(!g.learning);
    assert(pfx_learn_begin(&g,10,0,10,868350,0)==PFX_OK);
    frame(bits,0x7654321,2);assert(pfx_learn_feed(&g,bits,1000)==PFX_PENDING);
    assert(pfx_learn_feed(&g,bits,2000)==PFX_INVALID);pfx_learn_cancel(&g);
    assert(pfx_binding_clear(&g,9)==PFX_INVALID);
    assert(pfx_binding_clear(&g,10)==PFX_STORAGE_ERROR);
    assert(!memcmp(&g.config,&old,sizeof(old)));
    disk.cut_before_commit=false;
    assert(pfx_binding_clear(&g,10)==PFX_OK && !g.config.binding[0].serial);
    assert(pfx_gateway_command(&g,(pfx_request_t){10,0,0})==PFX_NO_DATA);
    pfx_config_t bad=old;bad.binding[0].frames[0][66]='1';pfx_config_encode(&bad,data);
    assert(pfx_config_decode(&restored,data,sizeof(data))==PFX_INVALID);
    pfx_config_encode(&old,data);data[9]^=1;assert(pfx_config_decode(&restored,data,sizeof(data))==PFX_INVALID);
    assert(pfx_config_decode(&restored,data,sizeof(data)-1)==PFX_INVALID);
    pfx_config_encode(&old,data);data[4]=2;assert(pfx_config_decode(&restored,data,sizeof(data))==PFX_INVALID);
    memset(&bad,0,sizeof(bad));bad.binding[0].frequency_khz=868350;pfx_config_encode(&bad,data);
    assert(pfx_config_decode(&restored,data,sizeof(data))==PFX_INVALID);
    puts("PASS 12 endpoint bindings, confirmed capture, TTL, invalid data, CRC/schema, reboot, failed save/power-cut model");
    cc.state=PFX_CC_ERROR;assert(pfx_gateway_command(&g,(pfx_request_t){10,0,0})==PFX_NOT_READY);
    pfx_cc_close(&cc);assert(f.stx==0);
    puts("PASS radio failure never reports execution; no STX in any scenario");
    return 0;
}
