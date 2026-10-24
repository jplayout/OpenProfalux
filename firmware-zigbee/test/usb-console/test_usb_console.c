#include "pfx_usb_console.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

static const char *scenario;
static bool installed;
static unsigned installs, switches, unbuffered, block_sets;
static int flags=O_NONBLOCK;
const char *esp_err_to_name(esp_err_t err) { (void)err; return "MOCK_ERROR"; }
void test_log(const char *tag,const char *format,...) { (void)tag; (void)format; }
int test_setvbuf(FILE *stream,char *buf,int mode,size_t size)
{
    assert(stream==stdin && !buf && mode==_IONBF && size==0);unbuffered++;
    return !strcmp(scenario,"buffer-error") ? -1:0;
}
int test_fileno(FILE *stream) { assert(stream==stdin); return 0; }
int test_fcntl(int fd,int command,...)
{
    assert(fd==0);
    if(command==F_GETFL) return !strcmp(scenario,"flags-error") ? -1:flags;
    assert(command==F_SETFL);
    va_list args;va_start(args,command);flags=va_arg(args,int);va_end(args);
    assert(!(flags&O_NONBLOCK));block_sets++;return 0;
}
bool usb_serial_jtag_is_driver_installed(void) { return installed; }
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg)
{
    assert(!installed && unbuffered==1 && block_sets==1);
    assert(cfg->rx_buffer_size==1024 && cfg->tx_buffer_size==2048); installs++;
    if(!strcmp(scenario,"install-error")) return ESP_ERR_NO_MEM;
    installed=true;return ESP_OK;
}
void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t mode) { assert(installed && mode==ESP_LINE_ENDINGS_LF); }
void usb_serial_jtag_vfs_use_driver(void) { assert(installed && !(flags&O_NONBLOCK));switches++; }
int main(int argc,char **argv)
{
    assert(argc==2);scenario=argv[1];installed=!strcmp(scenario,"reuse");
    esp_err_t result=pfx_usb_console_init();
    if(!strcmp(scenario,"install-error")) { assert(result==ESP_ERR_NO_MEM && installs==1 && switches==0); }
    else if(strstr(scenario,"error")) { assert(result==ESP_FAIL && installs==0 && switches==0); }
    else {
        assert(result==ESP_OK && switches==1 && installs==(!strcmp(scenario,"reuse") ? 0u:1u));
        assert(pfx_usb_console_init()==ESP_OK && switches==1 && unbuffered==1 && block_sets==1);
    }
    printf("PASS USB console init: %s\n",scenario);
}
