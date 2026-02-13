/*
 * SPDX-FileCopyrightText: 2026 HAW Hamburg
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#pragma once

/**
 * @{
 *
 * @file
 * @author Moritz Holzer <moritz.holzer@haw-hamburg.de>
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "mutex.h"

#include "net/ieee802154/mac.h"


/**
 * @brief IEEE 802.15.4 MAC thread event type.
 */
typedef enum {
    IEEE802154_MAC_EV_INIT,             /**< INIT event */
    IEEE802154_MAC_EV_REQ,
    IEEE802154_MAC_EV_RADIO_TX_DONE,    /**< TX done event */
    IEEE802154_MAC_EV_RADIO_RX_DONE,    /**< RX done event */
    IEEE802154_MAC_EV_RADIO_CRC_ERR,    /**< CRC error event */
    IEEE802154_MAC_EV_SUBMAC_BH,        /**< bootom half event */
    IEEE802154_MAC_EV_ACK_TIMEOUT,      /**< ACK timeout event */
    IEEE802154_MAC_EV_TX,               /**< wakeup event */
} ieee802154_mac_ev_t;

/**
 * @brief IEEE 802.15.4 MAC FSM events.
 */
typedef enum {
    IEEE802154_MAC_FSM_EV_SCAN_START,
    IEEE802154_MAC_FSM_EV_SCAN_TIMER,
    IEEE802154_MAC_FSM_EV_SCAN_DONE,
    IEEE802154_MAC_FSM_EV_ASSOC_REQ_RX,
    IEEE802154_MAC_FSM_EV_ASSOC_RES_RX,
    IEEE802154_MAC_FSM_EV_DISASSOC_RX,
    IEEE802154_MAC_FSM_EV_COORD_START,
    IEEE802154_MAC_FSM_EV_MLME_ASSOC_REQ,
    IEEE802154_MAC_FSM_EV_MLME_POLL,
    IEEE802154_MAC_FSM_EV_MLME_ASSOC_RES,
    IEEE802154_MAC_FSM_EV_ASSOC_TIMEOUT,
    IEEE802154_MAC_FSM_EV_TX_REQUEST,
    IEEE802154_MAC_FSM_EV_SLEEP,
    IEEE802154_MAC_FSM_EV_WAKE,
    IEEE802154_MAC_FSM_EV_RX_BEACON,
    IEEE802154_MAC_FSM_EV_RX_DATA,
    IEEE802154_MAC_FSM_EV_RX_CMD_DATA_REQ,
    IEEE802154_MAC_FSM_EV_RX_CMD_BEACON_REQ,
    IEEE802154_MAC_FSM_EV_RX_CMD_ASSOC_REQ,
    IEEE802154_MAC_FSM_EV_RX_CMD_ASSOC_RES,
    IEEE802154_MAC_FSM_EV_RX_CMD_DISASSOC,
    IEEE802154_MAC_FSM_EV_MCPS_DATA_REQ,
} ieee802154_mac_fsm_ev_t;

typedef struct {
    iolist_t *buf;
    const ieee802154_rx_info_t *info;
    eui64_t src_addr;
    uint8_t src[IEEE802154_LONG_ADDRESS_LEN];
    le_uint16_t src_pan;
    int src_len;
    uint8_t frame_type;
    uint8_t cmd_type;
    uint8_t assoc_status;
    uint16_t assoc_short_addr;
    const void *dst_addr;
    const void *data_dst_addr;
    iolist_t *msdu;
    uint8_t msdu_handle;
    ieee802154_addr_mode_t src_mode;
    ieee802154_addr_mode_t dst_mode;
    uint16_t dst_panid;
    ieee802154_assoc_capability_t capability;
    bool ack_req;
    bool indirect;
    int *result;
} ieee802154_mac_fsm_ctx_t;

/**
 * @brief Initialize internal MAC state.
 */
void ieee802154_init_mac_internal(ieee802154_mac_t *mac);

/**
 * @brief Attach the IEEE 802.15.4 SubMAC callbacks.
 */
void ieee802154_mac_submac_attach(ieee802154_mac_t *mac);

/**
 * @brief Finish the current TX and report status.
 */
void ieee802154_mac_tx_finish_current(ieee802154_mac_t *mac, int status);

/**
 * @brief Attach the IEEE 802.15.4 Radio HAL callbacks.
 */
void ieee802154_mac_radio_attach(ieee802154_mac_t *mac);

/**
 * @brief Transmit a MAC frame to the given destination.
 */
int ieee802154_mac_tx(ieee802154_mac_t *mac, const ieee802154_ext_addr_t *dst_addr);


/**
 * @brief Whether the TX queue is full.
 */
bool ieee802154_mac_tx_full(const ieee802154_mac_txq_t *txq);

/**
 * @brief Whether the TX queue is empty.
 */
bool ieee802154_mac_tx_empty(const ieee802154_mac_txq_t *txq);

/**
 * @brief Returns a descriptor of TX queue head. Caller fills it.
 */
ieee802154_mac_tx_desc_t *ieee802154_mac_tx_reserve(ieee802154_mac_txq_t *txq);

/**
 * @brief Advances TX queue tail to make it visible to the sender.
 */
void ieee802154_mac_tx_commit(ieee802154_mac_txq_t *txq);

/**
 * @brief Returns descriptor of TX queue head (next to send), does not remove.
 */
ieee802154_mac_tx_desc_t *ieee802154_mac_tx_peek(ieee802154_mac_txq_t *txq);

/**
 * @brief Removes TX queue head entry.
 */
void ieee802154_mac_tx_pop(ieee802154_mac_txq_t *txq);

/**
 * @brief Allocate a slot in the indirect queue.
 */
int ieee802154_indirectq_alloc_slot(ieee802154_mac_indirect_q_t *indirect_q);
/**
 * @brief Free a slot in the indirect queue.
 */
void ieee802154_indirectq_free_slot(ieee802154_mac_indirect_q_t *indirect_q, uint8_t slot);

/**
 * @brief Whether if indirect queue is empty
 */
bool ieee802154_indirectq_empty(const ieee802154_mac_indirect_q_t *indirect_q);

/**
 * @brief Get the deadline tick for indirect transmission.
 */
uint16_t ieee802154_indirect_get_deadline(ieee802154_mac_t *mac);
/**
 * @brief Check whether a frame is expired.
 */
bool ieee802154_mac_frame_is_expired(uint16_t now_tick, uint16_t deadline_tick);
/**
 * @brief Update the frame pending bit for a destination address.
 */
void ieee802154_mac_indirect_fp_update(ieee802154_mac_t *mac,
                                       ieee802154_addr_mode_t dst_mode,
                                       const void *dst_addr,
                                       bool pending);
/**
 * @brief Auto-free an indirect queue slot when it expires.
 */
void ieee802154_mac_handle_indirectq_auto_free(ieee802154_mac_t *mac,
                                               ieee802154_mac_indirect_q_t *indirect_q,
                                               uint8_t slot);
/**
 * @brief Find an indirect queue slot by destination address.
 */
int ieee802154_mac_indirectq_search_slot(ieee802154_mac_t *mac,
                                         ieee802154_addr_mode_t dst_mode,
                                         const void *dst_addr);
/**
 * @brief Get a slot index for the given destination address.
 */
int ieee802154_mac_indirectq_get_slot(ieee802154_mac_t *mac,
                                      ieee802154_addr_mode_t dst_mode,
                                      const void *dst_addr);
/**
 * @brief Enqueue a data frame into the MAC TX queue.
 */
int ieee802154_mac_map_push(ieee802154_mac_t *mac,
                            uint8_t frame_type,
                            ieee802154_addr_mode_t src_mode,
                            ieee802154_addr_mode_t dst_mode,
                            const uint16_t *dst_panid,
                            const void *dst_addr,
                            iolist_t *msdu,
                            const uint8_t *msdu_handle,
                            bool ack_req,
                            bool indirect);
/**
 * @brief Enqueue a data request command.
 */
int ieee802154_mac_enqueue_data_request(ieee802154_mac_t *mac,
                                        ieee802154_addr_mode_t dst_mode,
                                        uint16_t *dst_panid,
                                        const void *dst_addr);
/**
 * @brief Enqueue an association request command.
 */
/**
 * @brief Enequeu a beacon request command.
 */
int ieee802154_mac_enqueue_beacon_request(ieee802154_mac_t *mac);

/**
 * @brief Process the active scan timer in thread context.
 */
void ieee802154_mac_scan_timer_process(ieee802154_mac_t *mac);

/**
 * @brief Handle a MAC FSM event and apply state transitions.
 */
int ieee802154_mac_fsm_process_ev_ctx(ieee802154_mac_t *mac, ieee802154_mac_fsm_ev_t ev,
                                      const ieee802154_mac_fsm_ctx_t *ctx);

int ieee802154_mac_fsm_request(ieee802154_mac_t *mac, ieee802154_mac_fsm_ev_t ev,
                               const ieee802154_mac_fsm_ctx_t *ctx);


#ifdef __cplusplus
}
#endif

/** @} */
