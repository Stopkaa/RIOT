#include "periph/gpio.h"
#include "test_utils/expect.h"

#include "errno.h"

#include "luid.h"
#include "mutex.h"
#include "shell.h"
#include "event/thread.h"
#include "ztimer.h"

#include "net/l2util.h"
#include "net/ieee802154.h"
#include "net/eui_provider.h"
#include "net/ieee802154/radio.h"
#include "net/ieee802154/glossy.h"

#include "test_common.h"

static glossy_t _glossy;
static eui64_t _long_addr;
static network_uint16_t _short_addr;

static uint8_t glossy_payload[] = { 'R', 'I', 'O', 'T', 0,0,0,0 };
static uint8_t glossy_hdr[] = {
    0x41, 0x88,   /* FCF: Data, PAN-ID compression, dst/src short, no AR */
    0x00,         /* seq */
    0x23, 0x00,   /* dst PAN 0x0023 (CONFIG_IEEE802154_DEFAULT_PANID) */
    0xff, 0xff,   /* dst: Broadcast */
    0x00, 0x00,   /* src short addr */
};

static iolist_t glossy_iol_payload = {
    .iol_base = glossy_payload,
    .iol_len  = sizeof(glossy_payload),
    .iol_next = NULL,
};

static iolist_t _glossy_frame = {
    .iol_base = glossy_hdr,
    .iol_len  = sizeof(glossy_hdr),
    .iol_next = &glossy_iol_payload,
};

struct _reg_container {
    int count;
};

static int glossy_print_current_state(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    print_glossy_state(&_glossy);
    return 0;
}

static int send_glossy_test_pkt(int argc, char **argv)
{
    (void) argv;
    if (argc != 1) {
        puts("Usage: send_test_pkt)");
        return 1;
    }
    glossy_stop(&_glossy);
    glossy_load_frame(&_glossy, &_glossy_frame);
    puts("start glossy flood\n");
    return glossy_start(&_glossy, true);
}

static void _glossy_cb(glossy_t *glossy, glossy_ev_t event)
{
    puts("glossy flooding done");
    (void) event;

    glossy_start(glossy, false);
}

static ieee802154_dev_t *_reg_callback(ieee802154_dev_type_t type, void *opaque)
{
    struct _reg_container *reg = opaque;

    printf("Trying to register ");
    switch (type) {
    case IEEE802154_DEV_TYPE_CC2538_RF:
        printf("cc2538_rf");
        break;
    case IEEE802154_DEV_TYPE_NRF802154:
        printf("nrf52840");
        break;
    case IEEE802154_DEV_TYPE_SOCKET_ZEP:
        printf("socket_zep");
        break;
     case IEEE802154_DEV_TYPE_KW2XRF:
        printf("kw2xrf");
        break;
     case IEEE802154_DEV_TYPE_KW41ZRF:
        printf("kw41zrf");
        break;
    case IEEE802154_DEV_TYPE_MRF24J40:
        printf("mrf24j40");
        break;
    case IEEE802154_DEV_TYPE_ESP_IEEE802154:
        printf("esp_ieee802154");
        break;
    }

    puts(".");
    if (reg->count > 0) {
        puts("For the moment this test only supports one radio");
        return NULL;
    }

    puts("Success");
    return &_glossy.radio;
}

static void _register_radio(void)
{
    struct _reg_container reg = {0};
    ieee802154_hal_test_init_devs(_reg_callback, &reg);
}

static int _init_glossy(void)
{
    luid_base(&_long_addr, sizeof(_long_addr));
    eui64_set_local(&_long_addr);
    eui64_clear_group(&_long_addr);
    eui_short_from_eui64(&_long_addr, &_short_addr);

    _register_radio();

    int res = glossy_init(&_glossy, &_glossy_cb, &_short_addr, &_long_addr);

    if (res < 0) {
        return res;
    }

    return glossy_start(&_glossy, false);
}

static const shell_command_t shell_commands[] = {
    { "glossy_test_send", "starts glossy flood", send_glossy_test_pkt},
    { "print_state", "print current glossy state", glossy_print_current_state},
     { NULL, NULL, NULL }
};

int main(void)
{
    _init_glossy();

    /* start the shell */
    puts("Initialization successful - starting the shell now");

    char line_buf[SHELL_DEFAULT_BUFSIZE];
    shell_run(shell_commands, line_buf, SHELL_DEFAULT_BUFSIZE);

    return 0;
}
