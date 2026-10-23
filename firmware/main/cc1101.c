/*
 * CC1101 driver — Profalux 868.425 MHz OOK
 *
 * NOTE: This is a functional skeleton. Config registers below are
 * validated against SmartRF Studio output for OOK 868.425 MHz ~1550 baud
 * asynchronous serial mode with GDO0 output on TX and GDO0 input on RX.
 */
#include "cc1101.h"
#include "radio_profile.h"
#include "hardware_config.h"
#include <string.h>
#include <esp_log.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_rom_sys.h>
#include "soc/soc_caps.h"   /* SOC_RMT_MEM_WORDS_PER_CHANNEL : portabilite RMT (S3/C3) */
#include <esp_timer.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <esp_rom_gpio.h>      /* rendre GDO0 au GPIO apres une emission RMT */
#include "soc/gpio_sig_map.h"  /* SIG_GPIO_OUT_IDX */
#include <esp_heap_caps.h>
#include "keeloq_decode.h"   /* decodeur OOK HCS/KeeLoq, unite pure testable sur hote */
#include "rx_capture.h"      /* copie bornee des fragments RMT, testable sur hote */

static const char *TAG = "cc1101";
/* ── Switch DEBUG capture RX (runtime, togglable depuis l'UI/MQTT) ──────────
 * Quand ON : logge CHAQUE paquet capte en ecoute (symboles RMT + RSSI + bits
 * decodes + classification KeeLoq), y compris les trames <64 bits normalement
 * jetees. Sert a diagnostiquer "ca ne capte pas" : distingue antenne morte /
 * reception mais decode KO / pas du Profalux. OFF par defaut (log verbeux). */
static volatile bool s_rx_debug = false;
void cc1101_set_rx_debug(bool on) { s_rx_debug = on; ESP_LOGW(TAG, "DEBUG capture RX %s", on ? "ON" : "OFF"); }
bool cc1101_get_rx_debug(void)    { return s_rx_debug; }
/* Gain RX reglable (AGCCTRL2) : plafond de gain LNA. 0x27=defaut (-9 dB). Plus GRAND
 * = moins de gain (0x37, 0x3F=-17 dB) -> l'AGC ne remonte plus sur le bruit dans les
 * creux OOK (moins de fausses transitions), au prix de la sensibilite aux signaux faibles. */
static volatile uint8_t s_rx_gain = 0x27;
void cc1101_set_rx_gain(uint8_t v) { s_rx_gain = v; ESP_LOGW(TAG, "RX gain AGCCTRL2=0x%02X", v); }
uint8_t cc1101_get_rx_gain(void)   { return s_rx_gain; }
/* Timing d'EMISSION : TE (temps elementaire), 400 us par defaut, le nominal de la datasheet HCS300
 * (plage garantie 260-660 us). Le TE d'une vraie telecommande depend de son oscillateur RC ; les
 * moteurs le mesurent sur chaque trame et acceptent toute la plage. On n'imite donc pas le TE de la
 * telecommande captee (issue #64), mais le reglage reste possible (Systeme > Appareil). On derive
 * bit '1'=TE, '0'=2*TE, preambule=TE, entete=10 TE. */
static volatile uint32_t s_tx_te = TX_TE_DEFAULT_US;
/* Bit de repetition (bit 65 de la trame KeeLoq). Mesure 2026-10-10 au YARD Stick One sur les
 * murales Eveno (HCS30x) : la 1re trame d'un appui porte RPT=0, toutes les repetitions RPT=1
 * (meme code tournant), ~18 ms de silence entre trames. Le rejeu envoyait jusqu'ici N trames
 * identiques (le bit tel qu'enregistre, 0 pour une trame adoptee) a 2 ms d'intervalle. Option,
 * desactivee par defaut : les moteurs testes acceptent les deux ; elle sert aux moteurs qui
 * tiendraient compte du bit, et a un maintien fidele (appui long). */
#define TX_RPT_GAP_MS 18
static volatile bool s_tx_rpt = false;
void cc1101_set_tx_repeat_bit(bool on) { s_tx_rpt = on; ESP_LOGW(TAG, "TX bit de repetition %s", on ? "ON" : "OFF"); }
bool cc1101_get_tx_repeat_bit(void)    { return s_tx_rpt; }
/* Emission par le RMT (timing materiel) au lieu du bit-bang (esp_rom_delay_us + GPIO,
 * preemptible par le WiFi, voir issue upstream #37). Option, desactivee par defaut. */
static volatile bool s_tx_rmt = false;
void cc1101_set_tx_rmt(bool on) { s_tx_rmt = on; ESP_LOGW(TAG, "TX par RMT %s", on ? "ON" : "OFF"); }
bool cc1101_get_tx_rmt(void)    { return s_tx_rmt; }
void cc1101_set_tx_te(uint32_t te) { if (te >= 200 && te <= 900) { s_tx_te = te; ESP_LOGW(TAG, "TX TE=%u us", (unsigned)te); } }
uint32_t cc1101_get_tx_te(void)    { return s_tx_te; }
/* TE (us) mesure sur la DERNIERE trame decodee (periode de bit / 3). Diagnostic seulement
 * (te= sur la ligne RX) : il ne regle plus l'emission. */
static volatile int s_last_te = 455;
int cc1101_last_te(void) { return s_last_te; }
/* Diagnostic radio expose a l'UI : detection de la puce + resultat du self-test TX. */
static int     s_tx_ok   = -1;      /* -1=non teste, 0=HS, 1=OK (puce passe en TX) */
static uint8_t s_partnum = 0xFF;    /* CC1101 attendu : PARTNUM 0x00, VERSION 0x04/0x14 */
static uint8_t s_version = 0xFF;
/* GDO0 libre (issue #60) : -1 = non teste, 1 = libre, 0 = tenu par un autre composant
 * (s_gdo0_lvl = niveau auquel il est tenu). */
static int s_gdo0 = -1, s_gdo0_lvl = -1;
int cc1101_gdo0_state(int *level) { if (level) *level = s_gdo0_lvl; return s_gdo0; }
void cc1101_get_diag(int *tx_ok, uint8_t *partnum, uint8_t *version) {
    if (tx_ok)   *tx_ok   = s_tx_ok;
    if (partnum) *partnum = s_partnum;
    if (version) *version = s_version;
}
int g_tx_marc = -1;   /* MARCSTATE lu juste apres le dernier STX (0x13=TX). Diagnostic expose via HTTP. */
static spi_device_handle_t s_spi = NULL;
static rmt_channel_handle_t s_cap = NULL;
static QueueHandle_t s_capq = NULL;
#define CAP_SYMBOLS KEELOQ_CAP_SYMBOLS   /* source unique (keeloq_decode.h) */
_Static_assert(sizeof(kq_sym_t) == sizeof(rmt_symbol_word_t), "kq_sym_t doit avoir la meme disposition que rmt_symbol_word_t");
typedef struct {
    const rmt_symbol_word_t *symbols;
    size_t num_symbols;
    bool is_last;
    bool overflow;
    bool failed;
} cc_cap_done_t;

/* Le buffer RMT est reutilisable des le retour du callback partiel. Le callback
 * copie donc chaque fragment dans ce stockage borne avant de rendre la main; le
 * consommateur ne voit que le marqueur terminal, jamais un pointeur RMT. */
#if SOC_RMT_SUPPORT_RX_PINGPONG
static kq_sym_t s_cap_history[CAP_SYMBOLS];
static rx_capture_t s_cap_capture;
#endif
/* Buffer RMT de travail. DMA-capable/aligne seulement quand le SoC le demande. */
static rmt_symbol_word_t *s_capbuf = NULL;
static size_t s_capbuf_sz = 0;
/* RX et auto-capture partagent l'unique canal RMT s_cap sur GDO0 (memoire RMT limitee). */
static cc1101_rx_cb_t     s_rx_cb = NULL;
static TaskHandle_t       s_rx_task = NULL;
static volatile bool      s_rx_running = false;

/* --- Registers CC1101 --- */
#define CC_IOCFG2   0x00
#define CC_IOCFG0   0x02
#define CC_FIFOTHR  0x03
#define CC_PKTLEN   0x06
#define CC_PKTCTRL0 0x08
#define CC_FREQ2    0x0D
#define CC_FREQ1    0x0E
#define CC_FREQ0    0x0F
#define CC_MDMCFG4  0x10
#define CC_MDMCFG3  0x11
#define CC_MDMCFG2  0x12
#define CC_MDMCFG1  0x13
#define CC_MDMCFG0  0x14
#define CC_DEVIATN  0x15
#define CC_MCSM0    0x18
#define CC_FOCCFG   0x19
#define CC_WORCTRL  0x20
#define CC_FSCAL3   0x23
#define CC_FSCAL2   0x24
#define CC_FSCAL1   0x25
#define CC_FSCAL0   0x26
#define CC_TEST2    0x2C
#define CC_TEST1    0x2D
#define CC_TEST0    0x2E
#define CC_PARTNUM  0x30
#define CC_FREQEST  0x32   /* offset de frequence estime du signal recu (signed) */
#define CC_RSSI     0x34
#define CC_MARCSTATE 0x35   /* etat machine radio (0x0D=RX, 0x13=TX, 0x01=IDLE) */
#define CC_MCSM1    0x17
#define CC_FREND1   0x21
#define CC_FREND0   0x22    /* PA_POWER : bit0..2 = index PATABLE utilise */
#define CC_PATABLE  0x3E

/* Strobes */
#define CC_SRES     0x30
#define CC_SIDLE    0x36
#define CC_STX      0x35
#define CC_SRX      0x34
#define CC_SFTX     0x3B
#define CC_SFRX     0x3A

/* Config array: OOK 868.425 MHz ~1550 baud asynchronous mode */
static const struct { uint8_t reg, val; } s_regs[] = {
    {CC_IOCFG2,   0x0D},  /* Serial data output */
    {CC_IOCFG0,   0x0D},  /* Serial data output on GDO0 */
    {CC_FIFOTHR,  0x47},
    {CC_PKTLEN,   0xFF},
    {CC_PKTCTRL0, 0x32},  /* Async serial, no CRC, infinite packet */
    {CC_FREQ2,    0x21},  /* 868.425 MHz (mesure 2026-08-10, etait 868.35 = hors bande) */
    {CC_FREQ1,    0x66},
    {CC_FREQ0,    0xA5},
    {CC_MDMCFG4,  0xC6},  /* RX BW 102 kHz (mesure) */
    {CC_MDMCFG3,  0x83},
    {CC_MDMCFG2,  0x30},  /* OOK/ASK modulation, no preamble/sync */
    {CC_MDMCFG1,  0x22},
    {CC_MDMCFG0,  0xF8},
    {CC_DEVIATN,  0x15},
    {CC_MCSM1,    0x3C},   /* reste en RX apres reception (comme le sniffer) */
    {CC_MCSM0,    0x18},   /* FS_AUTOCAL: calibre le PLL a IDLE->TX/RX */
    {CC_FREND1,   0xB6},   /* front analogique (ref sniffer qui capte) */
    {CC_FREND0,   0x11},   /* PA_POWER=1 => OOK utilise PATABLE[0] et [1] */
    {CC_FOCCFG,   0x14},
    {CC_WORCTRL,  0xFB},
    {CC_FSCAL3,   0xE9},
    {CC_FSCAL2,   0x2A},
    {CC_FSCAL1,   0x00},
    {CC_FSCAL0,   0x1F},
    {CC_TEST2,    0x81},
    {CC_TEST1,    0x35},
    {CC_TEST0,    0x09},
};

static void cs_low(void)  { gpio_set_level(CC1101_PIN_CS, 0); }
static void cs_high(void) { gpio_set_level(CC1101_PIN_CS, 1); }

static void spi_xfer(uint8_t *tx, uint8_t *rx, size_t n) {
    spi_transaction_t t = { .length = 8 * n, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_spi, &t);
}

/* Ecritures VERIFIEES. Mesure 2026-10-09 (D1 mini ESP32 + module CC1101) : apres une rafale
 * de ~5-10 acces SPI (lectures comprises), le CC1101 ignorait les ecritures pendant moins
 * d'1 ms. Il repondait bien en mode ecriture (octet d'etat 0x0F) mais ne gardait pas la
 * valeur ; lectures et strobes passaient. Independant de la frequence SPI et du DMA. Sans
 * verification, l'init perdait au hasard MCSM1/MCSM0/FREND0/TEST* et le PATABLE : puissance
 * d'emission et modulation fausses, sans aucun message. Le defaut a disparu apres une
 * coupure d'alimentation (puce en surconsommation, elle chauffait) : la verification sert a
 * le rendre VISIBLE (spi_retries/spi_fails dans /api/diag) et a s'en remettre.
 * On relit donc chaque registre de config et on reessaie apres 1 ms. */
static uint32_t s_spi_retries, s_spi_fails;
void cc1101_spi_stats(uint32_t *retries, uint32_t *fails) {
    if (retries) *retries = s_spi_retries;
    if (fails)   *fails   = s_spi_fails;
}

static void write_raw(uint8_t addr, uint8_t val) {
    uint8_t tx[2] = {addr, val}, rx[2];
    cs_low(); spi_xfer(tx, rx, 2); cs_high();
}

void cc1101_write_reg(uint8_t addr, uint8_t val) {
    for (int i = 0; ; i++) {
        write_raw(addr, val);
        if (addr > 0x2E || cc1101_read_reg(addr) == val) return;   /* au-dela : pas relisible */
        if (i == 5) {
            s_spi_fails++;
            ESP_LOGE(TAG, "ecriture registre 0x%02X=0x%02X non prise apres %d essais", addr, val, i + 1);
            return;
        }
        s_spi_retries++;
        esp_rom_delay_us(1000);
    }
}

uint8_t cc1101_read_reg(uint8_t addr) {
    uint8_t tx[2] = {addr | 0x80, 0}, rx[2];
    cs_low(); spi_xfer(tx, rx, 2); cs_high();
    return rx[1];
}

static void strobe(uint8_t s) {
    uint8_t tx = s, rx;
    cs_low(); spi_xfer(&tx, &rx, 1); cs_high();
}

int cc1101_init(void) {
    /* GPIO CS + GDO0 */
    gpio_config_t io_cs = { .pin_bit_mask = 1ULL << CC1101_PIN_CS,
                            .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io_cs);
    cs_high();

    gpio_config_t io_gdo = { .pin_bit_mask = 1ULL << CC1101_PIN_GDO0,
                             .mode = GPIO_MODE_INPUT_OUTPUT };
    gpio_config(&io_gdo);

    /* SPI init */
    spi_bus_config_t bus = {
        .miso_io_num = CC1101_PIN_MISO, .mosi_io_num = CC1101_PIN_MOSI,
        .sclk_io_num = CC1101_PIN_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CC1101_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = CC1101_SPI_FREQ_HZ, .mode = 0,
        .spics_io_num = -1, .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(CC1101_SPI_HOST, &dev, &s_spi));

    /* Reset + configure */
    strobe(CC_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));
    for (size_t i = 0; i < sizeof(s_regs) / sizeof(s_regs[0]); i++) {
        cc1101_write_reg(s_regs[i].reg, s_regs[i].val);
    }
    /* PATABLE (burst) pour OOK : index0=eteint (bit 0), index1=puissance (bit 1). */
    for (int i = 0; ; i++) {   /* ecriture verifiee (relue en burst), voir cc1101_write_reg */
        uint8_t tx[3] = { CC_PATABLE | 0x40, 0x00, 0xC0 }, rx[3];
        cs_low(); spi_xfer(tx, rx, 3); cs_high();
        uint8_t tr[3] = { CC_PATABLE | 0xC0, 0, 0 }, rr[3];
        cs_low(); spi_xfer(tr, rr, 3); cs_high();
        if (rr[1] == 0x00 && rr[2] == 0xC0) break;
        if (i == 5) {
            s_spi_fails++;
            ESP_LOGE(TAG, "PATABLE non pris (lu %02X,%02X) : puissance d'emission fausse", rr[1], rr[2]);
            break;
        }
        s_spi_retries++;
        esp_rom_delay_us(1000);
    }
    uint8_t partnum = cc1101_read_reg(CC_PARTNUM | 0x40);
    uint8_t version = cc1101_read_reg(0x31 | 0x40);   /* VERSION : ~0x04/0x14 sur un vrai CC1101 */
    uint8_t marc    = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
    s_partnum = partnum; s_version = version;          /* pour le diagnostic UI */
    ESP_LOGI(TAG, "PARTNUM=0x%02X VERSION=0x%02X (attendu 0x00/0x04-0x14) MARCSTATE=0x%02X", partnum, version, marc);
    return (partnum == 0x00) ? 0 : -1;
}

/* Synchronous bit-bang OOK on GDO0. Codage PWM HCS30x mesure le 2026-08-10 :
 *   Te=455us, periode de bit = 3*Te = 1365us.
 *   symbole H455 L910 (H<L)  et  symbole H910 L455 (H>L).
 * POLARITE (quel symbole = logique 1) NON etablie avec une seule telecommande :
 * on applique la convention "bit=1 si H<L" du decode, A CONFIRMER empiriquement
 * (si l'appairage echoue, inverser PFX_TX_INVERT). */
#ifndef PFX_TX_INVERT
#define PFX_TX_INVERT 0
#endif
static void ook_bit(bool one) {
    if (PFX_TX_INVERT) one = !one;
    if (one) {  /* symbole H455 L910 */
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(455);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(910);
    } else {    /* symbole H910 L455 */
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(910);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(455);
    }
}

/* Frequence porteuse (profil radio). Ecrite en IDLE ; le synthetiseur se recalibre au
 * prochain passage en RX/TX (MCSM0 FS_AUTOCAL). Relue pour verifier : sur certains montages
 * le CC1101 ignore une ecriture juste apres une rafale d'acces SPI, d'ou 3 essais espaces. */
static uint32_t s_freq_khz = PROFALUX_FREQ_HZ / 1000;   /* celle de s_regs au boot */
uint32_t cc1101_get_freq_khz(void) { return s_freq_khz; }

int cc1101_set_freq_khz(uint32_t khz) {
    uint32_t w = radio_cc1101_freq_word(khz);
    uint8_t f2 = (w >> 16) & 0xFF, f1 = (w >> 8) & 0xFF, f0 = w & 0xFF;
    strobe(CC_SIDLE);
    for (int i = 0; i < 3; i++) {
        cc1101_write_reg(CC_FREQ2, f2); cc1101_write_reg(CC_FREQ1, f1); cc1101_write_reg(CC_FREQ0, f0);
        if (cc1101_read_reg(CC_FREQ2) == f2 && cc1101_read_reg(CC_FREQ1) == f1 && cc1101_read_reg(CC_FREQ0) == f0) {
            s_freq_khz = khz;
            ESP_LOGI(TAG, "frequence %u kHz (FREQ %02X %02X %02X)", (unsigned)khz, f2, f1, f0);
            return 0;
        }
        esp_rom_delay_us(1000);
    }
    ESP_LOGE(TAG, "frequence %u kHz NON prise (FREQ relu %02X %02X %02X)", (unsigned)khz,
             cc1101_read_reg(CC_FREQ2), cc1101_read_reg(CC_FREQ1), cc1101_read_reg(CC_FREQ0));
    return -1;
}

/* Auto-test d'emission : passe en TX, lit MARCSTATE (0x13 = porteuse ON),
 * module ~300 ms puis revient a IDLE. Prouve que la puce emet, sans recepteur. */
/* GDO0 est-il libre pour l'emission ? Sur une carte non standard, le fil peut rejoindre un autre
 * composant (Ebyte E07-900MBL : le STM8 de la carte, issue #59). Le CC1101 lache GDO0 (haute
 * impedance), l'ESP32 le lit en entree avec son tirage interne vers le haut puis vers le bas :
 * s'il suit les deux, rien ne le pilote ; s'il reste bloque, un autre composant le tient.
 * On ne force jamais le fil : forcer et relire sa propre broche ne voit pas une sortie plus
 * faible que celle de l'ESP32 (mesure, issue #60). Limite : ne voit ni un tampon a sens unique,
 * ni un fil coupe. */
static void gdo0_free_test(void) {
    strobe(CC_SIDLE);
    cc1101_write_reg(CC_IOCFG0, 0x2E);   /* GDO0 haute impedance cote CC1101 */
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CC1101_PIN_GDO0, GPIO_PULLUP_ONLY);   esp_rom_delay_us(200);
    int hi = gpio_get_level(CC1101_PIN_GDO0);
    gpio_set_pull_mode(CC1101_PIN_GDO0, GPIO_PULLDOWN_ONLY); esp_rom_delay_us(200);
    int lo = gpio_get_level(CC1101_PIN_GDO0);
    gpio_set_pull_mode(CC1101_PIN_GDO0, GPIO_FLOATING);
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(CC1101_PIN_GDO0, 0);
    cc1101_write_reg(CC_IOCFG0, 0x0D);   /* configuration normale */
    s_gdo0 = (hi == 1 && lo == 0) ? 1 : 0;
    s_gdo0_lvl = s_gdo0 ? -1 : hi;
    if (s_gdo0) ESP_LOGI(TAG, "GDO0 libre (suit les tirages haut et bas)");
    else ESP_LOGW(TAG, "GDO0 tenu a %d par un autre composant : l'emission n'atteindra pas le CC1101", hi);
}

int cc1101_tx_selftest(void) {
    gdo0_free_test();
    strobe(CC_STX);
    esp_rom_delay_us(1000);
    uint8_t m1 = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
    for (int i = 0; i < 300; i++) {   /* porteuse modulee 1kHz pendant ~300ms */
        gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(500);
        gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(500);
    }
    uint8_t m2 = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
    strobe(CC_SIDLE);
    ESP_LOGI(TAG, "TX SELFTEST: MARCSTATE post-STX=0x%02X, pendant modulation=0x%02X "
                  "(0x13=TX porteuse ON, 0x01=IDLE=rien)", m1, m2);
    s_tx_ok = (m1 == 0x13 || m2 == 0x13) ? 1 : 0;   /* pour le diagnostic UI */
    return s_tx_ok ? 0 : -1;
}

int cc1101_tx_ook_frame(const uint8_t *frame, size_t bits) {
    strobe(CC_STX);
    esp_rom_delay_us(800);   /* laisse le PLL se caler (FS_AUTOCAL) avant de moduler */

    /* Preuve d'emission : lit MARCSTATE une fois par boot. 0x13=TX (porteuse ON). */
    static bool s_marc_logged = false;
    if (!s_marc_logged) {
        s_marc_logged = true;
        uint8_t m = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
        ESP_LOGI(TAG, "TX MARCSTATE=0x%02X (0x13=TX porteuse ON, 0x01=IDLE=rien emis)", m);
    }

    /* Preambule: 23 alternances de Te (mesure) */
    for (int i = 0; i < PROFALUX_PREAMBLE_ELEMENTS; i++) {
        gpio_set_level(CC1101_PIN_GDO0, (i & 1) == 0);  /* H,L,H,L... */
        esp_rom_delay_us(PROFALUX_TE_US);
    }
    /* Header: silence ~4450us low */
    gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(PROFALUX_HEADER_US);

    /* Data bits MSB first */
    for (size_t i = 0; i < bits; i++) {
        bool b = (frame[i / 8] >> (7 - (i % 8))) & 1;
        ook_bit(b);
    }
    gpio_set_level(CC1101_PIN_GDO0, 0);
    esp_rom_delay_us(2000);  /* Inter-frame gap */
    strobe(CC_SIDLE);
    return 0;
}

/* ── Emission par le RMT ──
 * Meme forme d'onde que le bit-bang ci-dessous, mais chaque duree est tenue par le
 * materiel : une interruption (WiFi) ne peut plus allonger un TE et casser la trame.
 * Le silence entre deux trames est un symbole, lui aussi exact.
 *
 * Memoire RMT : sur ESP32, le canal de reception s_cap prend les 512 symboles, c'est-a-dire
 * les 8 blocs ; il ne reste rien pour un canal d'emission permanent. La reception est de
 * toute facon coupee pendant une emission : on la libere, on cree un canal d'emission de
 * SOC_RMT_MEM_WORDS_PER_CHANNEL symboles (l'encodeur le recharge au fil de l'eau), on emet,
 * puis on rend GDO0 au GPIO et on recree la reception a l'identique. */
#define TX_SYM_MAX (PROFALUX_PREAMBLE_ELEMENTS / 2 + 1 + 80)   /* preambule + entete + 80 bits */
_Static_assert(PROFALUX_PREAMBLE_ELEMENTS % 2 == 1, "le dernier element haut du preambule est suivi de l'entete");
static rmt_symbol_word_t s_txsym[2][TX_SYM_MAX];   /* 1re trame, et repetitions (sous le mutex radio) */

/* Une trame en symboles RMT (1 tic = 1 us). rpt65 = valeur forcee du bit 65, ou 0 pour le
 * laisser tel quel ; gap = silence apres la trame. Renvoie le nombre de symboles. */
static size_t tx_frame_syms(rmt_symbol_word_t *sy, const char *bits, int n, char rpt65,
                            uint32_t te, uint32_t te2, uint32_t hdr, uint32_t gap) {
    size_t k = 0;
    for (int i = 0; i + 1 < PROFALUX_PREAMBLE_ELEMENTS; i += 2)   /* H,L,H,L... (Te chacun) */
        sy[k++] = (rmt_symbol_word_t){ .duration0 = te, .level0 = 1, .duration1 = te, .level1 = 0 };
    sy[k++] = (rmt_symbol_word_t){ .duration0 = te, .level0 = 1, .duration1 = hdr, .level1 = 0 };
    for (int i = 0; i < n && k < TX_SYM_MAX; i++) {
        const char b = (rpt65 && i == 65) ? rpt65 : bits[i];
        sy[k++] = (b == '1')
            ? (rmt_symbol_word_t){ .duration0 = te,  .level0 = 1, .duration1 = te2, .level1 = 0 }
            : (rmt_symbol_word_t){ .duration0 = te2, .level0 = 1, .duration1 = te,  .level1 = 0 };
    }
    sy[k - 1].duration1 += gap;   /* bas du dernier bit + silence (< 32767 us : tient sur 15 bits) */
    return k;
}

/* Emet la rafale par le RMT. Appele en TX (STX fait), reception desactivee. Renvoie 0 si
 * la rafale est partie ; sinon -1, rien n'a ete emis et l'appelant passe au bit-bang.
 * Dans les deux cas, la reception est recreee et active en sortie. */
static int tx_rmt_burst(const char *bits, int n, int repeats, bool rpt,
                        uint32_t te, uint32_t te2, uint32_t hdr, uint32_t gap) {
    if (s_cap) { rmt_del_channel(s_cap); s_cap = NULL; }   /* deja desactive par l'appelant */
    rmt_channel_handle_t tx = NULL;
    rmt_encoder_handle_t enc = NULL;
    rmt_tx_channel_config_t tc = {
        .gpio_num = CC1101_PIN_GDO0, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 1000000,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL, .trans_queue_depth = 4,
    };
    rmt_copy_encoder_config_t ec = {};   /* structure vide */
    int rc = -1;
    if (rmt_new_tx_channel(&tc, &tx) != ESP_OK) { tx = NULL; ESP_LOGW(TAG, "TX RMT : canal KO, bit-bang"); goto out; }
    if (rmt_new_copy_encoder(&ec, &enc) != ESP_OK) { enc = NULL; ESP_LOGW(TAG, "TX RMT : encodeur KO, bit-bang"); goto out; }
    if (rmt_enable(tx) != ESP_OK) { ESP_LOGW(TAG, "TX RMT : activation KO, bit-bang"); goto out; }
    /* 1re trame (RPT force a 0 si l'option est active), puis les repetitions (RPT a 1). */
    size_t n0 = tx_frame_syms(s_txsym[0], bits, n, rpt ? '0' : 0, te, te2, hdr, gap);
    size_t n1 = tx_frame_syms(s_txsym[1], bits, n, rpt ? '1' : 0, te, te2, hdr, gap);
    rmt_transmit_config_t xc = { .loop_count = 0, .flags.eot_level = 0 };
    const int64_t t0 = esp_timer_get_time();
    rc = 0;
    for (int r = 0; r < repeats && rc == 0; r++) {   /* en file : les trames s'enchainent sans trou */
        if (rmt_transmit(tx, enc, r ? s_txsym[1] : s_txsym[0], (r ? n1 : n0) * sizeof(rmt_symbol_word_t), &xc) != ESP_OK)
            rc = r ? -2 : -1;   /* -2 : une partie est partie, on n'en remet pas une couche en bit-bang */
    }
    if (rmt_tx_wait_all_done(tx, 5000) != ESP_OK) ESP_LOGW(TAG, "TX RMT : fin de rafale non confirmee");
    /* Duree mesuree : la rafale dure repeats x (trame + silence), au tic pres. Un ecart
     * signale un probleme d'emission. */
    else ESP_LOGW(TAG, "TX RMT : %d trame(s) en %lld ms (attendu ~%lu ms)", repeats,
                  (long long)((esp_timer_get_time() - t0) / 1000),
                  (unsigned long)((uint64_t)repeats * (PROFALUX_PREAMBLE_ELEMENTS * te + hdr
                                  + (uint64_t)n * (te + te2) + gap) / 1000));
    rmt_disable(tx);
out:
    if (enc) rmt_del_encoder(enc);
    if (tx) rmt_del_channel(tx);
    /* GDO0 rendu au GPIO (sortie bas) pour le bit-bang et l'auto-test, puis reception recreee. */
    gpio_set_level(CC1101_PIN_GDO0, 0);
    esp_rom_gpio_connect_out_signal(CC1101_PIN_GDO0, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);
    if (cc1101_capture_init() != 0) ESP_LOGE(TAG, "TX RMT : reception non recreee");
    return rc == -2 ? 0 : rc;
}

/* Rejoue une trame brute (chaine de bits '0'/'1' en ordre du fil) en OOK :
 * preambule + entete + symboles HCS30x. Sert au replay d'une trame captee. */
int cc1101_tx_raw_bits(const char *bits, int n, int repeats) {
    /* UNE seule session TX (un STX, FS_AUTOCAL calibre le PLL), puis `repeats` trames dos-a-dos
     * = une vraie rafale de telecommande. Evite un STX/calibration par trame et rend l'emission
     * plus fiable (le bit-bang reste sensible a la preemption, d'ou plusieurs trames). */
    if (repeats < 1) repeats = 1;
    /* CRITIQUE : couper la capture RMT pendant le bit-bang. Sinon la RMT ecoute GDO0
     * (l'ecoute permanente l'a armee), capte notre propre TX, deborde ("hw buffer too
     * small") et son ISR PREEMPTE le bit-bang -> timing 455/910 us deforme -> trame
     * invalide -> le moteur l'ignore. Critique en environnement bruite (RMT deborde en
     * continu -> TOUTES les repetitions corrompues). On re-arme apres le SIDLE. */
    if (s_cap) rmt_disable(s_cap);
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);
    strobe(CC_STX);
    esp_rom_delay_us(800);   /* laisse le PLL se caler + la PA monter avant de moduler */
    g_tx_marc = cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F;
    /* Preuve d'emission sur le serie : MARCSTATE 0x13 = la puce est bien en TX (elle module
     * l'antenne). Loggue avant le bit-bang, au point exact ou la trame part. */
    ESP_LOGW(TAG, "TX RAW: emission de %d trame(s) x%d bits, MARCSTATE=0x%02X (%s)",
             repeats, n, g_tx_marc, g_tx_marc == 0x13 ? "TX ACTIF" : "PAS EN TX !");
    /* Timing derive du TE reglable : bit '1'=TE haut/2TE bas, '0'=2TE haut/TE bas,
     * preambule=TE, entete = 10 TE comme la telecommande (l'ancien 4450 x TE / 455 = 9,78 TE
     * raccourcissait l'entete de ~100 us a TE 448). */
    const uint32_t te = s_tx_te, te2 = 2 * s_tx_te;
    const uint32_t hdr = PROFALUX_HEADER_TE * s_tx_te;
    const bool rpt = s_tx_rpt && n >= 66;   /* bit 65 = RPT : 0 sur la 1re trame, 1 ensuite */
    if (s_tx_rmt) {
        if (tx_rmt_burst(bits, n, repeats, rpt, te, te2, hdr, rpt ? TX_RPT_GAP_MS * 1000 : 2000) == 0) {
            strobe(CC_SIDLE);   /* reception deja recreee et active */
            return 0;
        }
        if (s_cap) rmt_disable(s_cap);   /* repli bit-bang : meme etat qu'avant */
    }
    for (int r = 0; r < repeats; r++) {
        for (int i = 0; i < PROFALUX_PREAMBLE_ELEMENTS; i++) {
            gpio_set_level(CC1101_PIN_GDO0, (i & 1) == 0);
            esp_rom_delay_us(te);
        }
        gpio_set_level(CC1101_PIN_GDO0, 0);
        esp_rom_delay_us(hdr);
        for (int i = 0; i < n; i++) {
            const char b = (rpt && i == 65) ? (r == 0 ? '0' : '1') : bits[i];
            if (b == '1') { gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(te);
                                  gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(te2); }
            else                { gpio_set_level(CC1101_PIN_GDO0, 1); esp_rom_delay_us(te2);
                                  gpio_set_level(CC1101_PIN_GDO0, 0); esp_rom_delay_us(te); }
        }
        gpio_set_level(CC1101_PIN_GDO0, 0);
        if (rpt) vTaskDelay(pdMS_TO_TICKS(TX_RPT_GAP_MS));   /* silence de la telecommande, porteuse coupee */
        else     esp_rom_delay_us(2000);   /* gap inter-trame */
    }
    strobe(CC_SIDLE);
    if (s_cap) rmt_enable(s_cap);   /* re-arme la capture RMT pour l'ecoute permanente suivante */
    return 0;
}

/* ── Auto-capture : le RMT lit GDO0 pendant qu'on bit-bang (verifie la trame). ── */
static bool IRAM_ATTR cc_cap_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *e, void *ctx) {
    BaseType_t hp = pdFALSE;
#if SOC_RMT_SUPPORT_RX_PINGPONG
    rx_capture_append(&s_cap_capture, (const kq_sym_t *)e->received_symbols,
                      e->num_symbols, e->flags.is_last);
    if (e->flags.is_last) {
        cc_cap_done_t done = {
            .symbols = NULL,
            .num_symbols = s_cap_capture.count,
            .is_last = true,
            .overflow = s_cap_capture.overflow,
            .failed = s_cap_capture.failed,
        };
        if (xQueueSendFromISR((QueueHandle_t)ctx, &done, &hp) != pdTRUE)
            rx_capture_mark_event_lost(&s_cap_capture);
    }
#else
    if (e->flags.is_last) {
        cc_cap_done_t done = {
            .symbols = e->received_symbols,
            .num_symbols = e->num_symbols,
            .is_last = true,
        };
        xQueueSendFromISR((QueueHandle_t)ctx, &done, &hp);
    }
#endif
    return hp == pdTRUE;
}

static void cc_cap_prepare(void) {
#if SOC_RMT_SUPPORT_RX_PINGPONG
    rx_capture_reset(&s_cap_capture);
#endif
    if (s_capq) xQueueReset(s_capq);
}

static size_t cc_cap_snapshot(const cc_cap_done_t *done, const rmt_symbol_word_t **symbols) {
    if (!done->is_last || done->failed) return 0;
#if SOC_RMT_SUPPORT_RX_PINGPONG
    if (done->overflow) {
        ESP_LOGW(TAG, "RMT RX: capture abandonnee apres saturation du buffer");
        return 0;
    }
    if (!s_cap_capture.complete || s_cap_capture.failed ||
        s_cap_capture.count != done->num_symbols) return 0;
    *symbols = (const rmt_symbol_word_t *)s_cap_history;
    return s_cap_capture.count;
#else
    if (!done->symbols || done->num_symbols > CAP_SYMBOLS) return 0;
    *symbols = done->symbols;
    return done->num_symbols;
#endif
}

static int cc_cap_restart(void) {
    esp_err_t err = rmt_disable(s_cap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT RX: desactivation de reprise impossible: %s", esp_err_to_name(err));
        return -1;
    }
    err = rmt_enable(s_cap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT RX: reactivation impossible: %s", esp_err_to_name(err));
        return -1;
    }
    cc_cap_prepare();
    return 0;
}

int cc1101_capture_init(void) {
    if (s_cap) return 0;
    if (!s_capq) s_capq = xQueueCreate(1, sizeof(cc_cap_done_t));
    if (!s_capq) { ESP_LOGE(TAG, "RMT RX: allocation de la file impossible"); return -1; }
    rmt_rx_channel_config_t c = {0};
    c.clk_src = RMT_CLK_SRC_DEFAULT; c.resolution_hz = 1000000;
    /* mem_block_symbols configure la memoire materielle du canal. Avec
     * en_partial_rx, le driver ESP-IDF copie les fragments dans le buffer logiciel;
        * cc_cap_cb les copie ensuite dans son stockage borne avant reutilisation.
     *
     *   cible    words/canal   SUPPORT_DMA   SUPPORT_RX_PINGPONG
     *   esp32         64        absent        absent
     *   esp32s3       48        1             1
     *   esp32c3       48        absent        1
     *   esp32c6       48        absent        1
     *
      * Le nombre de mots est la memoire materielle du canal, pas la taille du
      * buffer applicatif fourni a rmt_receive().
     *
     * - ESP32 classic : pas de DMA, mais 512 symboles tiennent en multi-blocs.
        * - S3 : with_dma=true, le buffer logiciel est dimensionne pour CAP_SYMBOLS.
        * - C3/C6 : sans DMA mais avec ping-pong RX, en_partial_rx fournit les segments. */
#if defined(CONFIG_IDF_TARGET_ESP32)
    c.mem_block_symbols = 512;    /* ESP32 : pas de DMA mais 512 en multi-blocs */
#elif SOC_RMT_SUPPORT_DMA
    c.flags.with_dma    = true;   /* S3 seul (le C3 n'a pas le DMA, il ne passe jamais
                                   * ici) : seulement 48 words/canal -> ping-pong bogue
                                   * (#13419) sur une trame >48 symb. Le DMA stream vers un
                                   * grand buffer et evite ce chemin. mem_block_symbols petit
                                   * avec DMA corrompt (#12564) -> on prend tout le buffer. */
    c.mem_block_symbols = CAP_SYMBOLS;
#else
    /* C3/C6 : memoire materielle minimale, collecte longue via ping-pong. */
    c.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#endif
    c.gpio_num = CC1101_PIN_GDO0;
    if (rmt_new_rx_channel(&c, &s_cap) != ESP_OK) { ESP_LOGW(TAG, "RMT capture init KO"); s_cap = NULL; return -1; }
    /* Le buffer DMA du S3 exige alignement et RAM interne. Les autres SoC n'ont
     * pas a reserver de memoire DMA pour le canal RX. */
    if (!s_capbuf) {
        s_capbuf_sz = CAP_SYMBOLS * sizeof(rmt_symbol_word_t);
#if SOC_RMT_SUPPORT_DMA
        s_capbuf = heap_caps_aligned_alloc(64, s_capbuf_sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
        s_capbuf = heap_caps_aligned_alloc(sizeof(uintptr_t), s_capbuf_sz,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        if (!s_capbuf) { ESP_LOGW(TAG, "RMT capture: allocation du buffer RX impossible"); rmt_del_channel(s_cap); s_cap = NULL; return -1; }
    }
#if SOC_RMT_SUPPORT_RX_PINGPONG
    rx_capture_init(&s_cap_capture, s_cap_history, CAP_SYMBOLS);
#endif
    rmt_rx_event_callbacks_t cb = { .on_recv_done = cc_cap_cb };
    esp_err_t err = rmt_rx_register_event_callbacks(s_cap, &cb, s_capq);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT RX: installation du callback impossible: %s", esp_err_to_name(err));
        rmt_del_channel(s_cap); s_cap = NULL; return -1;
    }
    err = rmt_enable(s_cap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT RX: activation impossible: %s", esp_err_to_name(err));
        rmt_del_channel(s_cap); s_cap = NULL; return -1;
    }
    /* RMT met GDO0 en input : on re-active la sortie pour pouvoir bit-banger */
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);
    return 0;
}

/* Emet la trame ET capture GDO0, decode la forme d'onde en bits (ordre du fil).
 * Retourne le nombre de bits decodes (<0 = erreur). bit=1 si HAUT court (H<680us). */
/* decode_rmt : fin wrapper autour du decodeur PUR keeloq_decode() (unite keeloq_decode.c,
 * testee sur hote dans firmware/test/keeloq). On caste le buffer RMT en kq_sym_t (meme
 * disposition binaire, cf _Static_assert plus haut) et on met a jour le TE mesure
 * (diagnostic). Tout le decodage (de-glitch, scan entetes, vote) vit dans l'unite. */
static int decode_rmt(const rmt_symbol_word_t *raw, size_t n, char *out, int max_bits) {
    int te = 0;
    int r = keeloq_decode((const kq_sym_t *)raw, n, out, max_bits, &te);
    if (te) s_last_te = te;
    return r;
}

int cc1101_tx_and_capture_bits(const uint8_t *frame, size_t bits, char *out_bits, int max_bits) {
    if (!s_cap && cc1101_capture_init() != 0) return -1;
    if (cc_cap_restart() != 0) return -2;
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);   /* on pilote GDO0 (TX) */
    rmt_receive_config_t rc = { .signal_range_min_ns = 2000, .signal_range_max_ns = 6000000 };
#if SOC_RMT_SUPPORT_RX_PINGPONG
    rc.flags.en_partial_rx = true;
#endif
    if (rmt_receive(s_cap, s_capbuf, s_capbuf_sz, &rc) != ESP_OK) {
        cc_cap_restart();
        return -2;
    }
    cc1101_tx_ook_frame(frame, bits);         /* bit-bang GDO0, RMT capture en parallele */
    cc_cap_done_t ev;
    if (xQueueReceive(s_capq, &ev, pdMS_TO_TICKS(500)) != pdTRUE) {
        cc_cap_restart();
        return -3;
    }
    const rmt_symbol_word_t *symbols = NULL;
    size_t n = cc_cap_snapshot(&ev, &symbols);
    if (!n) { cc_cap_restart(); return -4; }
    return decode_rmt(symbols, n, out_bits, max_bits);
}

/* Ecoute (RX) pendant timeout_ms : bascule la puce en RX, capture GDO0 (donnee
 * demodulee) et decode la 1re trame recue. Retourne nbits (<0 = rien / erreur). */
/* Sonde de bruit RX : arme la reception a vide ~800 ms, compte les evenements RMT
 * (= activite bruit OOK) et suit le RSSI. Sert a comparer objectivement un module/antenne :
 * MOINS d'evenements = moins de bruit qui inonde le buffer = meilleur pour l'ecoute continue. */
int cc1101_rx_probe(void) {
    if (!s_cap && cc1101_capture_init() != 0) return -1;
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT);
    cc1101_write_reg(0x02, 0x0D); cc1101_write_reg(0x0B, 0x06); cc1101_write_reg(0x19, 0x14);
    cc1101_write_reg(0x1B, s_rx_gain); cc1101_write_reg(0x1C, 0x00); cc1101_write_reg(0x1D, 0x91);   /* AGC : meme reglage que l'ecoute (rx_listen_bits) */
    strobe(CC_SIDLE); esp_rom_delay_us(200);
    strobe(0x33); vTaskDelay(pdMS_TO_TICKS(3));
    strobe(0x3A); strobe(0x34);
    for (int i = 0; i < 50; i++) { if ((cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F) == 0x0D) break; vTaskDelay(pdMS_TO_TICKS(1)); }
    if (cc_cap_restart() != 0) return -1;
    rmt_receive_config_t rc = { .signal_range_min_ns = 3000, .signal_range_max_ns = 8000000 };
#if SOC_RMT_SUPPORT_RX_PINGPONG
    rc.flags.en_partial_rx = true;
#endif
    int64_t t_end = esp_timer_get_time() + 800 * 1000;
    int nev = 0; int8_t rssi_max = -128, rssi_min = 0;
    if (rmt_receive(s_cap, s_capbuf, s_capbuf_sz, &rc) == ESP_OK) {
        while (esp_timer_get_time() < t_end) {
            int8_t r = cc1101_get_rssi(); if (r > rssi_max) rssi_max = r; if (r < rssi_min) rssi_min = r;
            cc_cap_done_t ev;
            if (xQueueReceive(s_capq, &ev, pdMS_TO_TICKS(40)) != pdTRUE) continue;
            nev++;
            if (cc_cap_restart() != 0) break;
            if (rmt_receive(s_cap, s_capbuf, s_capbuf_sz, &rc) != ESP_OK) break;
        }
    }
    strobe(CC_SIDLE);
    if (cc_cap_restart() != 0) return -1;
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);
    ESP_LOGI(TAG, "RX PROBE bruit: RSSI %d..%d dBm, %d evt RMT en 800ms (moins d'evt = moins de bruit/flood)",
             rssi_min, rssi_max, nev);
    return nev;
}

/* Log DIAG d'une capture (commun aux deux chemins S3/ESP32). sym/nsym = symboles bruts
 * a dumper, nbits/out = resultat du decode. Ne logge que si le switch DEBUG est ON et la
 * capture est "interessante" (entete trouvee ou grosse rafale). */
static void diag_log_capture(const rmt_symbol_word_t *sym, size_t nsym, int nbits, const char *out) {
    if (!(s_rx_debug && (nbits >= 1 || nsym >= 50))) return;
    int8_t drssi = cc1101_get_rssi();
    if (nbits >= 64)
        ESP_LOGW(TAG, "DIAG RX: %u symb, RSSI %d dBm, %d bits => TRAME PROFALUX/KEELOQ complete: %s",
                 (unsigned)nsym, drssi, nbits, out);
    else if (nbits >= 1)
        ESP_LOGW(TAG, "DIAG RX: %u symb, RSSI %d dBm, entete HCS OK mais %d bits (partiel): %s",
                 (unsigned)nsym, drssi, nbits, out);
    else
        ESP_LOGW(TAG, "DIAG RX: %u symb, RSSI %d dBm, PAS d'entete HCS (ou bruit)", (unsigned)nsym, drssi);
    if (drssi > -70) {   /* signal fort : dump des durees brutes des 1res impulsions (us) */
        int8_t fq = (int8_t)cc1101_read_reg(CC_FREQEST | 0x40);
        int fq_khz = (int)fq * 1587 / 1000;
        char dbuf[320]; int p = 0;
        int lim = nsym < 20 ? (int)nsym : 20;
        for (int k = 0; k < lim && p < 300; k++)
            p += snprintf(dbuf + p, sizeof(dbuf) - p, "%c%u %c%u ",
                          sym[k].level0 ? 'H' : 'L', (unsigned)sym[k].duration0,
                          sym[k].level1 ? 'H' : 'L', (unsigned)sym[k].duration1);
        ESP_LOGW(TAG, "DIAG RAW (RSSI %d, FREQEST %d ~%d kHz, %d/%u symb, us): %s",
                 drssi, fq, fq_khz, lim, (unsigned)nsym, dbuf);
    }
}

int cc1101_rx_listen_bits(uint32_t timeout_ms, char *out_bits, int max_bits) {
    /* RX sur GDO0 (GPIO25) comme le sniffer. On REUTILISE l'unique canal RMT s_cap :
     * l'ESP32 n'a pas assez de memoire RMT pour un 2e canal de 512 symboles
     * ("no free rx channels"). GDO0 est prouve OK par le SELFVERIFY au boot. */
    if (!s_cap && cc1101_capture_init() != 0) return -1;
    int r = -3;
    /* GDO0 en INPUT : la CC1101 (RX, IOCFG0=0x0D) sort la donnee demodulee, on la lit */
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT);
    cc1101_write_reg(0x02, 0x0D);   /* IOCFG0 = donnee serie demodulee sur GDO0 */
    cc1101_write_reg(0x0B, 0x06);   /* FSCTRL1 */
    cc1101_write_reg(0x19, 0x14);   /* FOCCFG  */
    cc1101_write_reg(0x1B, s_rx_gain);   /* AGCCTRL2 : plafond de gain RX REGLABLE via l'UI (defaut 0x27)
                                     * et resultat de la calibration. Le 0x04 fige "TI DN022" introduit en
                                     * v0.2.6 degradait la reception en terrain : l'ancienne valeur recoit
                                     * mieux (retours issues #1 eleroy et #59 joha38, rollback = RX revient).
                                     * On restaure donc le reglage. Voir cc1101_set_rx_gain(). */
    cc1101_write_reg(0x1C, 0x00);   /* AGCCTRL1 */
    cc1101_write_reg(0x1D, 0x91);   /* AGCCTRL0 : frontiere de decision ASK/OOK a 8 dB. Le passage a 12 dB
                                     * (0x92) en v0.2.6 ratait des trames faibles a l'enrolement, on revient a 8. */
    strobe(CC_SIDLE); esp_rom_delay_us(200);
    strobe(0x33);     /* SCAL */
    vTaskDelay(pdMS_TO_TICKS(3));
    strobe(0x3A);     /* SFRX */
    strobe(0x34);     /* SRX */
    for (int i = 0; i < 50; i++) {
        if ((cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F) == 0x0D) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGI(TAG, "RX(GDO0): MARCSTATE=0x%02X (0x0D=RX attendu)", cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F);
    /* OOK sans squelch => bruit continu sur GDO0. Sur les SoC ping-pong, les
     * fragments de la transaction sont copies a chaque callback et seul is_last
     * publie le buffer stable au consommateur. L'ESP32 classique conserve son
     * evenement unique delimite par l'idle; en_partial_rx n'y est pas supporte. */
    if (cc_cap_restart() != 0) { r = -2; goto rx_listen_done; }
    int64_t t_end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
#if SOC_RMT_SUPPORT_RX_PINGPONG
    /* 32 ms depasse le silence inter-trame HCS (~16-23 ms). Les callbacks
     * partiels sont copies dans s_cap_history; seul is_last publie la capture. */
    rmt_receive_config_t rc = { .signal_range_min_ns = 3000, .signal_range_max_ns = 32000000,
                                .flags.en_partial_rx = true };
    if (rmt_receive(s_cap, s_capbuf, s_capbuf_sz, &rc) == ESP_OK) {
        while (esp_timer_get_time() < t_end) {
            cc_cap_done_t ev;
            if (xQueueReceive(s_capq, &ev, pdMS_TO_TICKS(100)) != pdTRUE) continue;  /* tjrs en RX */
            const rmt_symbol_word_t *symbols = NULL;
            size_t nsyms = cc_cap_snapshot(&ev, &symbols);
            if (!nsyms) { r = -2; break; }
            int n = decode_rmt(symbols, nsyms, out_bits, max_bits);
            diag_log_capture(symbols, nsyms, n, out_bits);
            if (n >= 64) { r = n; break; }                                  /* vraie trame */
            if (esp_timer_get_time() >= t_end) break;
            if (cc_cap_restart() != 0 || rmt_receive(s_cap, s_capbuf, s_capbuf_sz, &rc) != ESP_OK) break;
        }
    } else r = -2;
#else
    /* ESP32 classique (pas de ping-pong RX) : en_partial_rx INTERDIT. Une reception =
     * un evenement (idle a 8 ms), on decode, on re-arme. decode_rmt (de-glitch + vote)
     * reste commun et tolere le bruit meme sur un seul evenement. */
    rmt_receive_config_t rc = { .signal_range_min_ns = 3000, .signal_range_max_ns = 8000000 };
    if (rmt_receive(s_cap, s_capbuf, s_capbuf_sz, &rc) == ESP_OK) {
        while (esp_timer_get_time() < t_end) {
            cc_cap_done_t ev;
            if (xQueueReceive(s_capq, &ev, pdMS_TO_TICKS(100)) != pdTRUE) continue;  /* tjrs en RX */
            const rmt_symbol_word_t *symbols = NULL;
            size_t nsyms = cc_cap_snapshot(&ev, &symbols);
            if (!nsyms) { r = -2; break; }
            int n = decode_rmt(symbols, nsyms, out_bits, max_bits);
            diag_log_capture(symbols, nsyms, n, out_bits);
            if (n >= 64) { r = n; break; }                                  /* vraie trame */
            if (esp_timer_get_time() >= t_end) break;
            if (cc_cap_restart() != 0 || rmt_receive(s_cap, s_capbuf, s_capbuf_sz, &rc) != ESP_OK) break;
        }
    } else r = -2;
#endif
rx_listen_done:
    strobe(CC_SIDLE);
    if (s_cap && cc_cap_restart() != 0 && r >= 0) r = -2;
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);   /* restaure pour le TX bit-bang */
    return r;
}

/* --- RX --- */
static void rx_task(void *pv) {
    ESP_LOGI(TAG, "RX task started");
    strobe(CC_SRX);
    while (s_rx_running) {
        /* TODO: Sample GDO0 with hardware timer + detect preamble + decode bits.
         * This skeleton just polls RSSI and provides infrastructure.
         * Full implementation: use RMT peripheral or GPIO ISR + circular buffer.
         */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    strobe(CC_SIDLE);
    ESP_LOGI(TAG, "RX task stopped");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

int cc1101_rx_start(cc1101_rx_cb_t cb) {
    if (s_rx_running) return -1;
    s_rx_cb = cb;
    s_rx_running = true;
    xTaskCreate(rx_task, "cc1101_rx", 4096, NULL, 5, &s_rx_task);
    return 0;
}

int cc1101_rx_stop(void) {
    s_rx_running = false;
    return 0;
}

/* Balayage RSSI (issue #51) : pour chaque frequence, IDLE -> FREQ -> RX (calibration auto du PLL,
 * MCSM0 FS_AUTOCAL) puis RSSI max sur dwell_ms, soit au moins une trame entiere de la
 * telecommande. Filtre RX resserre a 58 kHz pendant le balayage (sommet plus net), frequence et
 * filtre restaures a la fin. RSSI en demi-dB (dBm = valeur / 2). L'appelant tient le radio
 * (ecoute permanente en pause). Rend 0, ou -1 si la puce ne passe pas en RX. */
int cc1101_rssi_scan(const uint32_t *khz, int n, int dwell_ms, int16_t *peak_hdb) {
    const uint32_t f0 = s_freq_khz;
    const uint8_t mdm4 = cc1101_read_reg(CC_MDMCFG4);
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT);
    cc1101_write_reg(CC_IOCFG0, 0x0D);
    cc1101_write_reg(0x1B, s_rx_gain);   /* AGCCTRL2, AGCCTRL1, AGCCTRL0 : comme l'ecoute (rx_listen_bits) */
    cc1101_write_reg(0x1C, 0x00);
    cc1101_write_reg(0x1D, 0x91);
    cc1101_write_reg(CC_MDMCFG4, (uint8_t)(0xF0 | (mdm4 & 0x0F)));   /* CHANBW 58 kHz, debit inchange */
    int rc = 0;
    for (int i = 0; i < n; i++) {
        uint32_t w = radio_cc1101_freq_word(khz[i]);
        strobe(CC_SIDLE);
        cc1101_write_reg(CC_FREQ2, (w >> 16) & 0xFF);
        cc1101_write_reg(CC_FREQ1, (w >> 8) & 0xFF);
        cc1101_write_reg(CC_FREQ0, w & 0xFF);
        strobe(0x34);   /* SRX (calibre le PLL au passage IDLE -> RX) */
        int k = 0;
        while ((cc1101_read_reg(CC_MARCSTATE | 0x40) & 0x1F) != 0x0D && k++ < 20) esp_rom_delay_us(100);
        if (k >= 20) { rc = -1; peak_hdb[i] = -256; continue; }
        esp_rom_delay_us(500);   /* RSSI valide apres quelques echantillons */
        int16_t best = -256;
        int64_t t_end = esp_timer_get_time() + (int64_t)dwell_ms * 1000;
        while (esp_timer_get_time() < t_end) {
            int8_t raw = (int8_t)cc1101_read_reg(CC_RSSI | 0x40);
            int16_t hdb = (int16_t)(raw - 148);   /* (raw/2 - 74) dBm, en demi-dB */
            if (hdb > best) best = hdb;
            esp_rom_delay_us(300);
        }
        peak_hdb[i] = best;
        if ((i & 3) == 3) vTaskDelay(1);   /* nourrit le watchdog sur un long balayage */
    }
    strobe(CC_SIDLE);
    cc1101_write_reg(CC_MDMCFG4, mdm4);
    cc1101_set_freq_khz(f0);
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT_OUTPUT);
    return rc;
}

int8_t cc1101_get_rssi(void) {
    uint8_t r = cc1101_read_reg(CC_RSSI | 0x40);
    return (r >= 128) ? (int8_t)((r - 256) / 2 - 74) : (int8_t)(r / 2 - 74);
}
