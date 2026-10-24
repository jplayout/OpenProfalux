/* ESP-IDF 5.5 port. Single worker owns radio, NVS, learning and control requests. */
#include "pfx_runtime.h"
#include "rx_capture.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "soc/soc_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define PIN_MOSI 18
#define PIN_MISO 20
#define PIN_SCLK 19
#define PIN_CSN 2
#define PIN_GDO0 3
#define SPI_HOST SPI2_HOST
static const char *TAG="PFX_RADIO";
static pfx_cc_t s_cc;
static pfx_gateway_t s_gateway;
static pfx_queue_t s_queue;
static TaskHandle_t s_worker;
static portMUX_TYPE s_queue_mux=portMUX_INITIALIZER_UNLOCKED;
static spi_device_handle_t s_spi;
static bool s_bus_open;
static esp_pm_lock_handle_t s_pm_lock;
static bool s_learning_lock;
static bool s_storage_fault;
static rmt_channel_handle_t s_rx;
static rmt_symbol_word_t *s_work;
static kq_sym_t *s_symbols;
static rx_capture_t s_capture;
static volatile bool s_capture_done;
static portMUX_TYPE s_capture_mux=portMUX_INITIALIZER_UNLOCKED;
typedef struct { int kind, mode, endpoint, command, seconds; uint32_t khz; } control_t;
static QueueHandle_t s_control;

static pfx_result_t spi_error(esp_err_t err)
{
    if(err==ESP_OK) return PFX_OK;
    ESP_LOGE(TAG,"SPI/GPIO failure: %s",esp_err_to_name(err));
    return err==ESP_ERR_TIMEOUT ? PFX_TIMEOUT : PFX_SPI_ERROR;
}
static void bus_close(void *ctx)
{
    (void)ctx;
    if(s_spi) { (void)spi_bus_remove_device(s_spi); s_spi=NULL; }
    if(s_bus_open) { (void)spi_bus_free(SPI_HOST); s_bus_open=false; }
    (void)gpio_set_level(PIN_CSN,1);
}
static pfx_result_t bus_open(void *ctx)
{
    (void)ctx;
    if(s_spi || s_bus_open) return PFX_NOT_READY;
    esp_err_t err=gpio_set_level(PIN_CSN,1);
    gpio_config_t cs={.pin_bit_mask=1ULL<<PIN_CSN,.mode=GPIO_MODE_OUTPUT};
    if(err==ESP_OK) err=gpio_config(&cs);
    gpio_config_t miso={.pin_bit_mask=1ULL<<PIN_MISO,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    if(err==ESP_OK) err=gpio_config(&miso);
    spi_bus_config_t bus={.miso_io_num=PIN_MISO,.mosi_io_num=PIN_MOSI,.sclk_io_num=PIN_SCLK,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=32};
    if(err==ESP_OK) {
        err=spi_bus_initialize(SPI_HOST,&bus,SPI_DMA_DISABLED);
        if(err==ESP_OK) s_bus_open=true;
    }
    spi_device_interface_config_t dev={.clock_speed_hz=1000000,.mode=0,.spics_io_num=-1,.queue_size=1};
    if(err==ESP_OK) err=spi_bus_add_device(SPI_HOST,&dev,&s_spi);
    if(err!=ESP_OK) { bus_close(NULL); return spi_error(err); }
    /* TI manual reset CS timing; SRES and CHIP_RDYn wait follow in transfer. */
    gpio_set_level(PIN_CSN,0); esp_rom_delay_us(10);
    gpio_set_level(PIN_CSN,1); esp_rom_delay_us(40);
    return PFX_OK;
}
static pfx_result_t bus_transfer(void *ctx,const uint8_t *tx,uint8_t *rx,size_t n)
{
    (void)ctx;
    if(!s_spi) return PFX_NOT_READY;
    if(!n || tx[0]==0x35) return PFX_TX_LOCKED;
    esp_err_t err=esp_pm_lock_acquire(s_pm_lock);
    if(err!=ESP_OK) return spi_error(err);
    err=gpio_set_level(PIN_CSN,0);
    int64_t deadline=esp_timer_get_time()+10000;
    while(err==ESP_OK && gpio_get_level(PIN_MISO)) {
        if(esp_timer_get_time()>=deadline) { err=ESP_ERR_TIMEOUT; break; }
        esp_rom_delay_us(10);
    }
    if(err==ESP_OK) {
        spi_transaction_t t={.length=8*n,.tx_buffer=tx,.rx_buffer=rx};
        err=spi_device_polling_transmit(s_spi,&t);
        /* SRES has its own CHIP_RDYn period while CS remains low. */
        if(err==ESP_OK && tx[0]==0x30) {
            deadline=esp_timer_get_time()+10000;
            while(gpio_get_level(PIN_MISO)) {
                if(esp_timer_get_time()>=deadline) { err=ESP_ERR_TIMEOUT; break; }
                esp_rom_delay_us(10);
            }
        }
    }
    esp_err_t cs_err=gpio_set_level(PIN_CSN,1);
    (void)esp_pm_lock_release(s_pm_lock);
    return spi_error(err!=ESP_OK ? err : cs_err);
}
static void bus_delay(void *ctx,uint32_t us) { (void)ctx; esp_rom_delay_us(us); }
static pfx_result_t store_save(void *ctx,const pfx_config_t *config)
{
    (void)ctx;
    if(s_storage_fault) return PFX_STORAGE_ERROR;
    uint8_t data[PFX_CONFIG_BYTES]; pfx_config_encode(config,data);
    nvs_handle_t h;
    esp_err_t err=nvs_open("pfx_zb_radio",NVS_READWRITE,&h);
    if(err!=ESP_OK) return PFX_STORAGE_ERROR;
    err=nvs_set_blob(h,"bindings_v1",data,sizeof(data));
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    if(err!=ESP_OK) {
        /* Commit errors may be ambiguous; don't overwrite again until a clean reload. */
        s_storage_fault=true;
        ESP_LOGE(TAG,"Mapping save failed: %s; persistence disabled until reboot",esp_err_to_name(err));
    }
    return err==ESP_OK ? PFX_OK : PFX_STORAGE_ERROR;
}
static void store_load(void)
{
    nvs_handle_t h;
    esp_err_t err=nvs_open("pfx_zb_radio",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND) return;
    if(err!=ESP_OK) { s_storage_fault=true; ESP_LOGE(TAG,"Mapping namespace: %s",esp_err_to_name(err)); return; }
    uint8_t data[PFX_CONFIG_BYTES]; size_t size=sizeof(data);
    err=nvs_get_blob(h,"bindings_v1",data,&size); nvs_close(h);
    if(err==ESP_ERR_NVS_NOT_FOUND) return;
    if(err!=ESP_OK || pfx_config_decode(&s_gateway.config,data,size)!=PFX_OK) {
        s_storage_fault=true; ESP_LOGE(TAG,"Invalid/unreadable radio mapping; no automatic reset or overwrite");
    } else ESP_LOGI(TAG,"Radio bindings restored (separate NVS namespace)");
}
static bool rx_callback(rmt_channel_handle_t channel,const rmt_rx_done_event_data_t *event,void *ctx)
{
    (void)channel; (void)ctx;
    portENTER_CRITICAL_ISR(&s_capture_mux);
    rx_capture_append(&s_capture,(const kq_sym_t *)event->received_symbols,event->num_symbols,event->flags.is_last);
    if(event->flags.is_last) s_capture_done=true;
    portEXIT_CRITICAL_ISR(&s_capture_mux);
    return false;
}
static bool stop_learning(void)
{
    pfx_learn_cancel(&s_gateway);
    bool released=true;
    if(s_rx) {
        esp_err_t disabled=rmt_disable(s_rx);
        esp_err_t deleted=rmt_del_channel(s_rx);
        if(deleted==ESP_OK) s_rx=NULL;
        else {
            released=false;
            ESP_LOGE(TAG,"RMT cleanup failed (disable=%s delete=%s); buffers/PM lock quarantined until retry or reboot",
                     esp_err_to_name(disabled),esp_err_to_name(deleted));
        }
    }
    if(released) {
        free(s_work); free(s_symbols); s_work=NULL; s_symbols=NULL;
        s_capture_done=false;
    }
    if(s_cc.opened) {
        pfx_result_t r=pfx_cc_sleep(&s_cc);
        ESP_LOGI(TAG,"CC1101 sleep: %s",pfx_result_name(r));
        if(r!=PFX_OK) pfx_cc_close(&s_cc);
    }
    if(released && s_learning_lock) { esp_pm_lock_release(s_pm_lock); s_learning_lock=false; }
    return released;
}
static esp_err_t arm_capture(void)
{
    rx_capture_reset(&s_capture); s_capture_done=false;
    rmt_receive_config_t cfg={.signal_range_min_ns=3000,.signal_range_max_ns=10000000,.flags.en_partial_rx=true};
    return rmt_receive(s_rx,s_work,KEELOQ_CAP_SYMBOLS*sizeof(*s_work),&cfg);
}
static pfx_result_t start_learning(control_t c)
{
    if(s_storage_fault || s_gateway.learning || s_rx) return PFX_NOT_READY;
    pfx_result_t r=pfx_learn_begin(&s_gateway,c.endpoint,c.command,c.seconds,c.khz,esp_timer_get_time());
    if(r!=PFX_OK) return r;
    esp_err_t err=esp_pm_lock_acquire(s_pm_lock);
    if(err==ESP_OK) s_learning_lock=true;
    if(err==ESP_OK) {
        r=pfx_cc_receive(&s_cc,c.khz);
        if(r!=PFX_OK) { stop_learning(); return r; }
        s_work=calloc(KEELOQ_CAP_SYMBOLS,sizeof(*s_work));
        s_symbols=calloc(KEELOQ_CAP_SYMBOLS,sizeof(*s_symbols));
        if(!s_work || !s_symbols) err=ESP_ERR_NO_MEM;
    }
    if(err==ESP_OK) {
        _Static_assert(sizeof(kq_sym_t)==sizeof(rmt_symbol_word_t),"RMT symbol layout");
        rx_capture_init(&s_capture,s_symbols,KEELOQ_CAP_SYMBOLS);
        rmt_rx_channel_config_t cfg={.gpio_num=PIN_GDO0,.clk_src=RMT_CLK_SRC_DEFAULT,
            .resolution_hz=1000000,.mem_block_symbols=SOC_RMT_MEM_WORDS_PER_CHANNEL};
        err=rmt_new_rx_channel(&cfg,&s_rx);
    }
    if(err==ESP_OK) {
        rmt_rx_event_callbacks_t cb={.on_recv_done=rx_callback};
        err=rmt_rx_register_event_callbacks(s_rx,&cb,NULL);
    }
    if(err==ESP_OK) err=rmt_enable(s_rx);
    if(err==ESP_OK) err=arm_capture();
    if(err!=ESP_OK) { ESP_LOGE(TAG,"RMT start: %s",esp_err_to_name(err)); stop_learning(); return PFX_NOT_READY; }
    ESP_LOGW(TAG,"LEARN endpoint=%d command=%d expires=%d s RX=%" PRIu32 " kHz; TX forbidden",c.endpoint,c.command,c.seconds,c.khz);
    return PFX_OK;
}
static void poll_learning(void)
{
    if(pfx_learn_tick(&s_gateway,esp_timer_get_time())==PFX_EXPIRED) {
        ESP_LOGI(TAG,"LEARN expired"); stop_learning(); return;
    }
    portENTER_CRITICAL(&s_capture_mux); bool done=s_capture_done; portEXIT_CRITICAL(&s_capture_mux);
    if(!done) return;
    /* RMT transaction is terminal; no rearm until worker finishes decoding. */
    char bits[PFX_FRAME_BITS+1]={0};
    if(s_capture.complete && !s_capture.failed && !s_capture.overflow) {
        int n=keeloq_decode(s_symbols,s_capture.count,bits,sizeof(bits),NULL);
        if(n==PFX_FRAME_BITS) {
            pfx_result_t r=pfx_learn_feed(&s_gateway,bits,esp_timer_get_time());
            ESP_LOGI(TAG,"LEARN capture: %s",pfx_result_name(r));
        }
    }
    if(!s_gateway.learning) { stop_learning(); return; }
    esp_err_t err=arm_capture();
    if(err!=ESP_OK) { ESP_LOGE(TAG,"RMT rearm: %s",esp_err_to_name(err)); stop_learning(); }
}
static void change_mode(int mode)
{
    if(!stop_learning()) return;
    pfx_cc_close(&s_cc);
    s_gateway.mode=mode;
    if(mode!=PFX_SIMULATION) {
        pfx_bus_t bus={.open=bus_open,.close=bus_close,.transfer=bus_transfer,.delay_us=bus_delay};
        pfx_result_t r=pfx_cc_init(&s_cc,bus);
        ESP_LOGI(TAG,"CC1101 probe=%s part=0x%02x version=0x%02x; startup continues",pfx_result_name(r),s_cc.part,s_cc.version);
    }
    ESP_LOGW(TAG,"mode=%d (0=SIMULATION 1=RADIO_DIAGNOSTIC 2=RADIO_ACTIVE); TX permanently locked in stage 4",mode);
}
static void radio_worker(void *arg)
{
    (void)arg;
    s_gateway.cc=&s_cc; s_gateway.save=store_save;
    store_load();
    esp_err_t err=esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP,0,"pfx_radio",&s_pm_lock);
    if(err!=ESP_OK) { ESP_LOGE(TAG,"PM lock unavailable: %s; simulation only",esp_err_to_name(err)); s_gateway.mode=PFX_SIMULATION; }
    else change_mode(CONFIG_OPF_RADIO_MODE);
    for(;;) {
        ulTaskNotifyTake(pdTRUE,s_gateway.learning ? pdMS_TO_TICKS(50) : portMAX_DELAY);
        control_t ctl;
        while(xQueueReceive(s_control,&ctl,0)==pdTRUE) {
            if(ctl.kind==0) {
                if(s_pm_lock) change_mode(ctl.mode);
                else ESP_LOGE(TAG,"Radio mode unavailable: no PM lock");
            }
            else if(ctl.kind==1) {
                pfx_result_t r=start_learning(ctl);
                ESP_LOGI(TAG,"LEARN request: %s",pfx_result_name(r));
            } else if(ctl.kind==2) stop_learning();
            else if(ctl.kind==3) {
                pfx_result_t r=pfx_binding_clear(&s_gateway,ctl.endpoint);
                ESP_LOGI(TAG,"CLEAR radio binding endpoint=%d: %s",ctl.endpoint,pfx_result_name(r));
            } else {
                ESP_LOGI(TAG,"STATUS mode=%d CC-state=%d storage_fault=%d learning=%d TX=LOCKED",
                         s_gateway.mode,s_cc.state,s_storage_fault,s_gateway.learning);
                for(unsigned i=0;i<PFX_ENDPOINTS;i++) {
                    const pfx_binding_t *b=&s_gateway.config.binding[i];
                    ESP_LOGI(TAG,"binding endpoint=%u serial=0x%07" PRIx32 " frequency=%" PRIu32 " frames=%d/%d/%d",
                        PFX_ENDPOINT_BASE+i,b->serial,b->frequency_khz,!!b->frames[0][0],!!b->frames[1][0],!!b->frames[2][0]);
                }
            }
        }
        pfx_request_t request;
        for(unsigned batch=0;batch<PFX_QUEUE_SIZE;batch++) {
            portENTER_CRITICAL(&s_queue_mux); bool found=pfx_queue_pop(&s_queue,&request); portEXIT_CRITICAL(&s_queue_mux);
            if(!found) break;
            pfx_result_t r=pfx_gateway_command(&s_gateway,request);
            const char *names[]={"OPEN","CLOSE","STOP"};
            ESP_LOGI(TAG,"%s endpoint=%u command=%s received_us=%" PRId64 "; no RF transmission",
                pfx_result_name(r),request.endpoint,names[request.command],request.received_us);
        }
        if(s_gateway.learning) poll_learning();
        portENTER_CRITICAL(&s_queue_mux); bool pending=s_queue.count!=0; portEXIT_CRITICAL(&s_queue_mux);
        if(pending) xTaskNotifyGive(s_worker);
    }
}
#ifdef CONFIG_OPF_RADIO_CONSOLE
static void console_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,"Local commands: mode 0|1|2 ; learn <ep> OPEN|CLOSE|STOP <seconds 1..60> 868350|868425 ; cancel ; clear <ep> ; status");
    char line[96]; size_t used=0; bool overflow=false;
    for(;;) {
        int ch=getchar();
        if(ch==EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if(ch!='\n' && ch!='\r') {
            if(used<sizeof(line)-1) line[used++]=ch; else overflow=true;
            continue;
        }
        line[used]=0;
        if(!used && !overflow) continue;
        control_t c={0}; char cmd[8],extra; bool valid=false;
        if(!overflow && sscanf(line,"mode %d %c",&c.mode,&extra)==1 && c.mode>=0 && c.mode<=2) valid=true;
        else if(!overflow && sscanf(line,"learn %d %7s %d %" SCNu32 " %c",&c.endpoint,cmd,&c.seconds,&c.khz,&extra)==4) {
            c.kind=1; c.command=!strcmp(cmd,"OPEN") ? 0 : !strcmp(cmd,"CLOSE") ? 1 : !strcmp(cmd,"STOP") ? 2 : -1;
            valid=c.endpoint>=10 && c.endpoint<=13 && c.command>=0 && c.seconds>=1 && c.seconds<=60 && (c.khz==868350 || c.khz==868425);
        } else if(!overflow && !strcmp(line,"cancel")) { c.kind=2; valid=true; }
        else if(!overflow && sscanf(line,"clear %d %c",&c.endpoint,&extra)==1 && c.endpoint>=10 && c.endpoint<=13) { c.kind=3; valid=true; }
        else if(!overflow && !strcmp(line,"status")) { c.kind=4; valid=true; }
        if(!valid) ESP_LOGW(TAG,"Invalid local command");
        else if(xQueueSend(s_control,&c,0)!=pdTRUE) ESP_LOGW(TAG,"Local control queue full");
        else xTaskNotifyGive(s_worker);
        used=0; overflow=false;
    }
}
#endif
void pfx_runtime_start(void)
{
    s_control=xQueueCreate(4,sizeof(control_t));
    configASSERT(s_control);
    configASSERT(xTaskCreate(radio_worker,"pfx_radio",6144,NULL,4,&s_worker)==pdPASS);
#ifdef CONFIG_OPF_RADIO_CONSOLE
    configASSERT(xTaskCreate(console_worker,"pfx_console",3072,NULL,2,NULL)==pdPASS);
#endif
}
pfx_result_t pfx_runtime_submit(pfx_request_t request)
{
    if(!s_worker) return PFX_NOT_READY;
    portENTER_CRITICAL(&s_queue_mux); pfx_result_t r=pfx_queue_push(&s_queue,request); portEXIT_CRITICAL(&s_queue_mux);
    if(r==PFX_OK) xTaskNotifyGive(s_worker);
    return r;
}
