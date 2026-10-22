#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "atomic_utils.h"
#include "errno.h"
#include "net/ieee802154/glossy.h"
#include "iolist.h"
#include "net/ieee802154.h"
#include "net/ieee802154/radio.h"
#include "net/netopt.h"
#include "ztimer.h"

#ifndef CONFIG_GLOSSY_PRIO
#  define CONFIG_GLOSSY_PRIO       (THREAD_PRIORITY_MAIN - 5)
#endif

#ifndef CONFIG_GLOSSY_STACKSIZE
#  define CONFIG_GLOSSY_STACKSIZE  (THREAD_STACKSIZE_DEFAULT)
#endif

#ifndef CONFIG_GLOSSY_MAX_N_TX
#  define CONFIG_GLOSSY_MAX_N_TX 3
#endif

#define MAX_N_TX CONFIG_GLOSSY_MAX_N_TX

static char _stack[CONFIG_GLOSSY_STACKSIZE];

#define GLOSSY_FLAG_RX_START    (1U << 0)
#define GLOSSY_FLAG_RX_DONE     (1U << 1)
#define GLOSSY_FLAG_CRC_ERROR   (1U << 2)
#define GLOSSY_FLAG_TX_START    (1U << 3)
#define GLOSSY_FLAG_TX_DONE     (1U << 4)
#define GLOSSY_FLAG_START       (1U << 5)
#define GLOSSY_FLAG_STOP        (1U << 6)

static glossy_fsm_return_t _fsm_state_off(glossy_t *glossy, glossy_fsm_ev_t event);
static glossy_fsm_return_t _fsm_state_idle(glossy_t *glossy, glossy_fsm_ev_t event);
static glossy_fsm_return_t _fsm_state_wait(glossy_t *glossy, glossy_fsm_ev_t event);
static glossy_fsm_return_t _fsm_state_receive(glossy_t *glossy, glossy_fsm_ev_t event);
static glossy_fsm_return_t _fsm_state_start_transmitting(glossy_t *glossy, glossy_fsm_ev_t event);
static glossy_fsm_return_t _fsm_state_transmitting(glossy_t *glossy, glossy_fsm_ev_t event);
static glossy_fsm_return_t _fsm_state_transmitted(glossy_t *glossy, glossy_fsm_ev_t event);
static glossy_fsm_return_t _fsm_state_invalid(glossy_t *glossy, glossy_fsm_ev_t event);

#define ENABLE_DEBUG 0
#include "debug.h"

typedef struct {
    glossy_fsm_state_cb_t state;
    const char *name;
} glossy_fsm_state_mapping_t;

static const glossy_fsm_state_mapping_t states_mapping[] = {
    { _fsm_state_invalid, "INVALID" },
    { _fsm_state_off, "OFF" },
    { _fsm_state_idle, "IDLE" },
    { _fsm_state_wait, "WAIT" },
    { _fsm_state_receive, "RECEIVE" },
    { _fsm_state_start_transmitting, "TX_START" },
    { _fsm_state_transmitting, "TX" },
    { _fsm_state_transmitted, "TX_END" },
};

static char *str_ev[] = {
    "ENTRY",
    "EXIT",
    "START",
    "RX_START",
    "RX_DONE",
    "CRC_ERROR",
    "TX_START",
    "TX_DONE",
};

static int _glossy_reset_context(glossy_t *glossy)
{
    glossy->numof_tx = 0;
    glossy->is_initiator = false;
    glossy->rx_len       = 0;
    glossy->psdu         = NULL;
    return 0;
}

MAYBE_UNUSED
static const char *get_state_name(glossy_fsm_state_cb_t state)
{
    for (uint8_t i = 0; i < ARRAY_SIZE(states_mapping); i++) {
        if (states_mapping[i].state == state) {
            return states_mapping[i].name;
        }
    }
    return "UNKNOWN";
}

void glossy_print_state(glossy_t *glossy)
{
    printf("[glossy] state: %s\n", get_state_name(glossy->fsm.fsm_state_cb));
}

static void _glossy_print_state_transition(glossy_fsm_state_cb_t old, glossy_fsm_state_cb_t new,
                                           glossy_fsm_ev_t ev)
{
    DEBUG("[glossy] %s--(%s)->%s\n", get_state_name(old), str_ev[ev], get_state_name(new));
}

static inline glossy_fsm_return_t _state_transition(glossy_fsm_t *fsm,
                                                    glossy_fsm_state_cb_t new_state)
{
    fsm->fsm_state_cb = new_state;
    return GLOSSY_FSM_RETURN_TRANSITION;
}

static glossy_fsm_return_t _fsm_state_invalid(glossy_t *glossy, glossy_fsm_ev_t ev)
{
    (void)glossy;
    DEBUG("[glossy] INVALID: ev=%s\n", str_ev[ev]);
    assert(false);
    switch (ev) {
        case GLOSSY_FSM_EV_ENTRY:
        case GLOSSY_FSM_EV_EXIT:
        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }
}

static glossy_fsm_return_t  _fsm_state_off(glossy_t *glossy, glossy_fsm_ev_t event)
{
    ieee802154_dev_t *radio = &glossy->radio;

    DEBUG("[glossy] OFF: ev=%s\n", str_ev[event]);

    switch (event) {
        case GLOSSY_FSM_EV_ENTRY:
            ieee802154_radio_off(radio);
            if (glossy->cb) {
                glossy->cb(glossy, GLOSSY_EV_FLOOD_DONE);
            }
            _glossy_reset_context(glossy);
            return GLOSSY_FSM_RETURN_HANDLED;

        case GLOSSY_FSM_EV_EXIT:
            ieee802154_radio_request_on(radio);
            return GLOSSY_FSM_RETURN_HANDLED;

        case GLOSSY_FSM_EV_START:
            return _state_transition(&glossy->fsm, _fsm_state_idle);

        case GLOSSY_FSM_EV_STOP:
            return _state_transition(&glossy->fsm, _fsm_state_off);

        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }
}

static glossy_fsm_return_t _fsm_state_idle(glossy_t *glossy, glossy_fsm_ev_t event)
{
    ieee802154_dev_t *radio = &glossy->radio;

    DEBUG("[glossy] IDLE: ev=%s\n", str_ev[event]);

    switch (event) {
        case GLOSSY_FSM_EV_ENTRY:
            while (ieee802154_radio_confirm_on(radio) == -EAGAIN) {}

            /* Configure address filter */
            ieee802154_radio_config_addr_filter(radio, IEEE802154_AF_PANID, &glossy->panid);
            ieee802154_radio_config_addr_filter(radio, IEEE802154_AF_SHORT_ADDR,&glossy->short_addr);
            ieee802154_radio_config_addr_filter(radio, IEEE802154_AF_EXT_ADDR, &glossy->ext_addr);

            /* Apply Glossy's PHY Configuration */
            ieee802154_radio_config_phy(radio, &glossy->phy_conf);

            /* Make sure to deactivate CSMA-CA because we want to send directly */
            ieee802154_radio_set_csma_params(radio, NULL, -1);

            /* Set CCA Configuration to ensure the radio is in a valid state, event if cca
             * is not used */
            ieee802154_radio_set_cca_mode(radio, IEEE802154_CCA_MODE_ED_THRESHOLD);
            ieee802154_radio_set_cca_threshold(radio, CONFIG_IEEE802154_CCA_THRESH_DEFAULT);

            if(ieee802154_radio_set_frame_filter_mode(radio, IEEE802154_FILTER_PROMISC) != 0) {
                /* Promiscious mode is prefereble but since it's only optional mode
                 * fall back to accept all valid frame that matches the address */
                ieee802154_radio_set_frame_filter_mode(radio, IEEE802154_FILTER_ACCEPT);
            }

            ieee802154_radio_config_src_address_match(radio, IEEE802154_SRC_MATCH_EN, false);

            /* NOTE: AUTO-ACK is disabled by not set the ACK Bit when transmitting a frame */
            glossy_fsm_state_cb_t next_state = glossy->is_initiator ?
                                              _fsm_state_start_transmitting : _fsm_state_wait;

            return _state_transition(&glossy->fsm, next_state);
        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }
}

static glossy_fsm_return_t _fsm_state_wait(glossy_t *glossy, glossy_fsm_ev_t event)
{
    ieee802154_dev_t *radio = &glossy->radio;

    DEBUG("[glossy] WAIT: ev=%s\n", str_ev[event]);

    switch (event) {
        case GLOSSY_FSM_EV_ENTRY:
            while (ieee802154_radio_set_rx(radio) < 0) {}
            return GLOSSY_FSM_RETURN_HANDLED;
        case GLOSSY_FSM_EV_RX_START:
            /* TODO add radio_read_rx_timestamp
             * radio_read_rx_timestamp(radio, &buf);
             * */
            return _state_transition(&glossy->fsm, _fsm_state_receive);
        case GLOSSY_FSM_EV_STOP:
            return _state_transition(&glossy->fsm, _fsm_state_off);
        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }
}

static glossy_fsm_return_t _fsm_state_receive(glossy_t *glossy, glossy_fsm_ev_t event)
{
    ieee802154_dev_t *radio = &glossy->radio;

    DEBUG("[glossy] RECEIVE: ev=%s\n", str_ev[event]);

    switch (event) {
        case GLOSSY_FSM_EV_ENTRY:
            return GLOSSY_FSM_RETURN_IGNORED;
        case GLOSSY_FSM_EV_RX_DONE:
            {
                while (ieee802154_radio_set_idle(radio, false) < 0) {}

                int len = ieee802154_radio_len(radio);
                if (len < 0) {
                    return _state_transition(&glossy->fsm, _fsm_state_wait);
                }
                glossy->rx_len = len;
                DEBUG("[glossy] rx_len=%d\n", len);

                ieee802154_radio_read(radio, glossy->rx_buf, glossy->rx_len, &glossy->rx_info);

                glossy->rx_iol.iol_base = glossy->rx_buf;
                glossy->rx_iol.iol_len  = glossy->rx_len;
                glossy->rx_iol.iol_next = NULL;
                glossy->psdu = &glossy->rx_iol;

                return _state_transition(&glossy->fsm, _fsm_state_start_transmitting);
            }
        case GLOSSY_FSM_EV_CRC_ERROR:
            while (ieee802154_radio_set_idle(radio, false) < 0) {}
            /* flush the radio buffer */
            ieee802154_radio_read(radio, NULL, 0, NULL);
            return _state_transition(&glossy->fsm, _fsm_state_wait);
        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }}

static glossy_fsm_return_t _fsm_state_start_transmitting(glossy_t *glossy, glossy_fsm_ev_t event)
{
    ieee802154_dev_t *radio = &glossy->radio;

    DEBUG("[glossy] TX_START: ev=%s\n", str_ev[event]);

    switch (event) {
        case GLOSSY_FSM_EV_ENTRY:
            /* For now transmitt directly
             * TODO add timed transmission
             * */
            /* Make sure radio is in idle before writing the frame */
            while (ieee802154_radio_set_idle(radio, true) < 0) {}
            ieee802154_radio_write(radio, glossy->psdu);
            while (ieee802154_radio_request_transmit(radio) < 0) {}
            return GLOSSY_FSM_RETURN_HANDLED;
        case GLOSSY_FSM_EV_TX_START:
            /* TODO add radio_read_tx_timestamp
             * radio_read_tx_timestamp(radio, &buf);
             * */
            return _state_transition(&glossy->fsm, _fsm_state_transmitting);
        case GLOSSY_FSM_EV_TX_DONE:
            return _state_transition(&glossy->fsm, _fsm_state_transmitted);
        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }
}

static glossy_fsm_return_t _fsm_state_transmitting(glossy_t *glossy, glossy_fsm_ev_t event)
{
    DEBUG("[glossy] TX: ev=%s\n", str_ev[event]);

    switch (event) {
        case GLOSSY_FSM_EV_TX_DONE:
            return _state_transition(&glossy->fsm, _fsm_state_transmitted);
        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }
}

static glossy_fsm_return_t _fsm_state_transmitted(glossy_t *glossy, glossy_fsm_ev_t event)
{
    ieee802154_dev_t *radio = &glossy->radio;

    DEBUG("[glossy] TX_END: ev=%s\n", str_ev[event]);

    switch (event) {
        case GLOSSY_FSM_EV_ENTRY:
            glossy->numof_tx++;
            while (ieee802154_radio_confirm_transmit(radio, NULL) < 0) {}
            DEBUG("[glossy] numof_tx=%u/%u\n", (unsigned)glossy->numof_tx, (unsigned)MAX_N_TX);
            if (glossy->numof_tx < MAX_N_TX) {
                return _state_transition(&glossy->fsm, _fsm_state_start_transmitting);
            }
            return _state_transition(&glossy->fsm, _fsm_state_off);
        default:
            return GLOSSY_FSM_RETURN_IGNORED;
    }
}

static int ieee802154_glossy_process_ev(glossy_t *glossy, glossy_fsm_ev_t ev)
{
    glossy_fsm_state_cb_t last_state = glossy->fsm.fsm_state_cb;
    glossy_fsm_ev_t last_event = ev;
    glossy_fsm_return_t res;

    uint8_t was_busy =  atomic_fetch_or_u8(&glossy->fsm.busy_status, true);

    if (was_busy) {
        DEBUG("[glossy] BUSY, event %s dropped\n", str_ev[ev]);
        return -EBUSY;
    }

    while ((res = (*glossy->fsm.fsm_state_cb)(glossy, last_event)) == GLOSSY_FSM_RETURN_TRANSITION) {
        _glossy_print_state_transition(last_state, glossy->fsm.fsm_state_cb, last_event);

        if (_fsm_state_invalid == glossy->fsm.fsm_state_cb) {
            _glossy_print_state_transition(last_state, glossy->fsm.fsm_state_cb, ev);
            assert(false);
        }

        res = (*last_state)(glossy, GLOSSY_FSM_EV_EXIT);
        /* state shouldn`t make a transition on exit event */
        assert(res != GLOSSY_FSM_RETURN_TRANSITION);
        last_state = glossy->fsm.fsm_state_cb;
        last_event = GLOSSY_FSM_EV_ENTRY;
    }

    atomic_store_u8(&glossy->fsm.busy_status, false);
    return res;
}

static void _hal_cb(ieee802154_dev_t *dev, ieee802154_trx_ev_t status)
{
    glossy_t *glossy = container_of(dev, glossy_t, radio);
    uint8_t flag;

    DEBUG("[glossy] hal_cb: status=%d\n", (int)status);

    switch (status) {
        case IEEE802154_RADIO_INDICATION_RX_START:
            flag = GLOSSY_FLAG_RX_START;
            break;
        case IEEE802154_RADIO_INDICATION_RX_DONE:
            flag = GLOSSY_FLAG_RX_DONE;
            break;
        case IEEE802154_RADIO_INDICATION_CRC_ERROR:
            flag = GLOSSY_FLAG_CRC_ERROR;
            break;
        case IEEE802154_RADIO_INDICATION_TX_START:
            flag = GLOSSY_FLAG_TX_START;
            break;
        case IEEE802154_RADIO_CONFIRM_TX_DONE:
            flag = GLOSSY_FLAG_TX_DONE;
            break;
        default:
            return;
    }

    atomic_fetch_or_u8(&glossy->isr_flags, flag);
    event_post(&glossy->evq, &glossy->ev);
}

static void _ev_handler(event_t *ev)
{
    glossy_t *glossy = container_of(ev, glossy_t, ev);
    uint8_t flags;

    while ((flags = atomic_fetch_and_u8(&glossy->isr_flags, 0)) != 0) {
        DEBUG("[glossy] ev_handler: flags=0x%02x\n", flags);
        if (flags & GLOSSY_FLAG_START) {
            ieee802154_glossy_process_ev(glossy, GLOSSY_FSM_EV_START);
        }
        if (flags & GLOSSY_FLAG_STOP) {
            ieee802154_glossy_process_ev(glossy, GLOSSY_FSM_EV_STOP);
        }
        if (flags & GLOSSY_FLAG_RX_START) {
            ieee802154_glossy_process_ev(glossy, GLOSSY_FSM_EV_RX_START);
        }
        if (flags & GLOSSY_FLAG_CRC_ERROR) {
            ieee802154_glossy_process_ev(glossy, GLOSSY_FSM_EV_CRC_ERROR);
        }
        if (flags & GLOSSY_FLAG_RX_DONE) {
            ieee802154_glossy_process_ev(glossy, GLOSSY_FSM_EV_RX_DONE);
        }
        if (flags & GLOSSY_FLAG_TX_START) {
            ieee802154_glossy_process_ev(glossy, GLOSSY_FSM_EV_TX_START);
        }
        if (flags & GLOSSY_FLAG_TX_DONE) {
            ieee802154_glossy_process_ev(glossy, GLOSSY_FSM_EV_TX_DONE);
        }
    }
}

static void *_glossy_thread(void *arg)
{
    glossy_t *glossy = arg;

    event_queue_init(&glossy->evq);
    event_loop(&glossy->evq);
    return NULL;
}

int glossy_init(glossy_t *glossy, glossy_cb_t cb,
                const network_uint16_t *short_addr, const eui64_t *ext_addr)
{
    DEBUG("[glossy] glossy_init()\n");

    /* Configure PHY */
    glossy->phy_conf = (ieee802154_phy_conf_t){
        .phy_mode = CONFIG_IEEE802154_DEFAULT_PHY_MODE,
        .channel  = CONFIG_IEEE802154_DEFAULT_CHANNEL,
        .page     = 0,
        .pow      = CONFIG_IEEE802154_DEFAULT_TXPOWER,
    };

    /* generate EUI-64 and short address */
    memcpy(&glossy->ext_addr, ext_addr, sizeof(eui64_t));
    memcpy(&glossy->short_addr, short_addr, sizeof(network_uint16_t));
    glossy->panid = CONFIG_IEEE802154_DEFAULT_PANID;

    /* Configure FSM */
    glossy->fsm.fsm_state_cb = _fsm_state_off;
    glossy->fsm.busy_status = false;

    /* event handling */
    glossy->ev.handler = _ev_handler;
    glossy->isr_flags  = 0;
    glossy->radio.cb   = _hal_cb;
    glossy->cb = cb;

    /* frame buffers */
    glossy->psdu   = NULL;
    glossy->rx_len = 0;

    /* Reset context data */
    _glossy_reset_context(glossy);
    ieee802154_radio_off(&glossy->radio);

    thread_create(_stack,
                sizeof(_stack),
                CONFIG_GLOSSY_PRIO,
                THREAD_CREATE_STACKTEST,
                _glossy_thread,
                glossy,
                "glossy");
    return 0;
}

int glossy_load_frame(glossy_t *glossy, const iolist_t *psdu)
{
    DEBUG("[glossy] glossy_load_frame()\n");
    glossy->psdu = psdu;
    return 0;
}

int glossy_start(glossy_t *glossy, bool initiator)
{
    DEBUG("[glossy] glossy_start(initiator=%d)\n", (int)initiator);
    glossy->is_initiator = initiator;

    atomic_fetch_or_u8(&glossy->isr_flags, GLOSSY_FLAG_START);
    event_post(&glossy->evq, &glossy->ev);

    return 0;
}

int glossy_stop(glossy_t *glossy)
{
    DEBUG("[glossy] glossy_stop\n");

    atomic_fetch_or_u8(&glossy->isr_flags, GLOSSY_FLAG_STOP);
    event_post(&glossy->evq, &glossy->ev);

    return 0;
}

int glossy_read(glossy_t *glossy, uint8_t *buf, size_t max_size,
                ieee802154_rx_info_t *info)
{
    DEBUG("[glossy] glossy_read(): rx_len=%u\n", (unsigned)glossy->rx_len);

    if (!buf) {
        return -EINVAL;
    }

    if (max_size < glossy->rx_len) {
        return -ENOBUFS;
    }

    memcpy(buf, glossy->rx_buf, glossy->rx_len);

    if (info) {
        *info = glossy->rx_info;
    }

    return glossy->rx_len;
}
