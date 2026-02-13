/*
 * SPDX-FileCopyrightText: 2026 HAW Hamburg
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "container.h"
#include "mutex.h"
#include "ztimer.h"

#include "mac_pib.h"
#include "mac_internal_priv.h"

#define ENABLE_DEBUG 0
#include "debug.h"

static void _tx_finish(ieee802154_mac_t *mac, ieee802154_mac_indirect_q_t *indirect_q, int slot,
                       int status, ieee802154_tx_info_t *info);

static void _tx_finish(ieee802154_mac_t *mac, ieee802154_mac_indirect_q_t *indirect_q, int slot,
                       int status, ieee802154_tx_info_t *info)
{
    if (ieee802154_mac_tx_empty(&indirect_q->q[slot])) {
        mac->indirect_q.busy = false;
        return;
    }
    bool frame_pending = info && info->frame_pending;
    if (frame_pending || mac->scan_active || mac->assoc_pending) {
        mac->cbs.rx_request(mac);
    }
    if (!mac->is_coordinator && frame_pending &&
        !mac->scan_active) {
        mac->poll_rx_active = true;
        mac->poll_rx_deadline = ieee802154_indirect_get_deadline(mac);
    }
    ieee802154_mac_tx_desc_t *d = ieee802154_mac_tx_peek(&indirect_q->q[slot]);
    d->tx_state = IEEE802154_TX_STATE_DONE;
    DEBUG("IEEE802154 MAC: TX state DONE (handle=%u, status=%d)\n", d->handle, status);
    if (d->indirect &&
        ((status == TX_STATUS_MEDIUM_BUSY) || (status == TX_STATUS_NO_ACK))) {
        d->tx_state = IEEE802154_TX_STATE_QUEUED;
        mac->indirect_q.busy = false;
        if (mac->is_coordinator || mac->scan_active || mac->assoc_pending) {
            mac->cbs.rx_request(mac);
        }
        return;
    }
    if ((d->type == IEEE802154_FCF_TYPE_DATA) && mac->cbs.data_confirm) {
        mac->cbs.data_confirm(mac->cbs.mac, d->handle, status);
    }

    ieee802154_pib_value_t rx_on;
    ieee802154_mac_mlme_get(mac, IEEE802154_PIB_RX_ON_WHEN_IDLE, &rx_on);
    if (rx_on.v.b && mac->cbs.rx_request) {
        mac->cbs.rx_request(mac);
    }
    d->in_use = false;
    ieee802154_mac_tx_pop(&indirect_q->q[slot]);
    ieee802154_mac_handle_indirectq_auto_free(mac, indirect_q, slot);
    mac->indirect_q.busy = false;
}

void ieee802154_mac_tick(ieee802154_mac_t *mac)
{
    mutex_lock(&mac->indirect_q.lock);
    mac->indirect_q.tick++;
    if (mac->assoc_pending &&
        ieee802154_mac_frame_is_expired(mac->indirect_q.tick, mac->assoc_deadline_tick)) {
        mac->assoc_pending = false;
        //(void)ieee802154_mac_fsm_process_ev_ctx(mac, IEEE802154_MAC_FSM_EV_ASSOC_TIMEOUT, NULL);
        //(void)ieee802154_set_idle(&mac->submac);
    }
    if (mac->poll_rx_active &&
        ieee802154_mac_frame_is_expired(mac->indirect_q.tick, mac->poll_rx_deadline) &&
        !mac->is_coordinator && !mac->scan_active) {
        mac->poll_rx_active = false;
        (void)ieee802154_set_idle(&mac->submac);
    }
    for (unsigned i = 0; i < IEEE802154_MAC_TX_INDIRECTQ_SIZE; i++) {
        ieee802154_mac_txq_t *txq = &mac->indirect_q.q[i];
        if (ieee802154_mac_tx_empty(txq) || (txq->deadline_tick == NULL)) {
            continue;
        }
        if (ieee802154_mac_frame_is_expired(mac->indirect_q.tick, *txq->deadline_tick)) {
            _tx_finish(mac, &mac->indirect_q, i, -ETIMEDOUT, NULL);
        }
    }
    ztimer_set(ZTIMER_MSEC, &mac->tick, (uint32_t)IEEE802154_MAC_TICK_INTERVAL_MS);
    mutex_unlock(&mac->indirect_q.lock);
}

void ieee802154_mac_tx_finish_current(ieee802154_mac_t *mac, int status, ieee802154_tx_info_t *info)
{
    _tx_finish(mac, &mac->indirect_q, mac->indirect_q.current_slot, status, info);
}

void ieee802154_mac_ack_timeout_fired(ieee802154_mac_t *mac)
{
    mutex_lock(&mac->submac_lock);
    ieee802154_submac_ack_timeout_fired(&mac->submac);
    mutex_unlock(&mac->submac_lock);
}

void ieee802154_submac_ack_timer_set(ieee802154_submac_t *submac)
{
    ieee802154_mac_t *mac = container_of(submac, ieee802154_mac_t, submac);

    ztimer_set(ZTIMER_USEC, &mac->ack_timer, (uint32_t)submac->ack_timeout_us);
}

void ieee802154_submac_ack_timer_cancel(ieee802154_submac_t *submac)
{
    ieee802154_mac_t *mac = container_of(submac, ieee802154_mac_t, submac);

    ztimer_remove(ZTIMER_USEC, &mac->ack_timer);
}
