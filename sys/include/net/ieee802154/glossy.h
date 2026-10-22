/*
 * Copyright (C) 2026 HAW Hamburg
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/**
 * @defgroup net_glossy Glossy
 * @ingroup  net
 * @brief    Glossy flooding on top of the IEEE 802.15.4 radio HAL
 *
 * @{
 *
 * @file
 * @brief  Glossy flooding interface
 *
 * @author Stepan Konoplev <stepan_konoplev@gmx.de>
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "byteorder.h"
#include "event.h"
#include "iolist.h"
#include "net/eui64.h"
#include "net/ieee802154.h"
#include "net/ieee802154/radio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Glossy forward declaration
 */
typedef struct glossy glossy_t;

/**
 * @brief Internal Glossy FSM state machine events
 */
typedef enum {
    GLOSSY_FSM_EV_ENTRY,        /**< FSM calls entry function of a state */
    GLOSSY_FSM_EV_EXIT,         /**< FSM calls exit function of a state */
    GLOSSY_FSM_EV_START,        /**< The upper layer starts Glossy */
    GLOSSY_FSM_EV_STOP,         /**< The upper layer stops Glossy */
    GLOSSY_FSM_EV_RX_START,     /**< The radio detected a valid SFD */
    GLOSSY_FSM_EV_RX_DONE,      /**< The radio received a valid frame with valid CRC */
    GLOSSY_FSM_EV_CRC_ERROR,    /**< The radio detected an invalid CRC */
    GLOSSY_FSM_EV_TX_START,     /**< The radio transmitted a valid SFD
                                     @note This feature may be only optional */
    GLOSSY_FSM_EV_TX_DONE,      /**< The radio finished transmitting a frame */
    GLOSSY_FSM_EV_NUMOF,        /**< Number of Glossy FSM events */
} glossy_fsm_ev_t;

/**
 * @brief Internal Glossy FSM event processing return status
 */
typedef enum {
    GLOSSY_FSM_RETURN_HANDLED,      /**< Event was handled */
    GLOSSY_FSM_RETURN_TRANSITION,   /**< Event has caused a transition */
    GLOSSY_FSM_RETURN_IGNORED,      /**< Event was ignored, no action was performed */
} glossy_fsm_return_t;

/**
 * @brief Glossy events reported to the upper layer
 */
typedef enum {
    GLOSSY_EV_FLOOD_DONE,   /**< Flood finished */
} glossy_ev_t;

/**
 * @brief Event callback of the upper layer
 *
 * @note Called from the Glossy thread context.
 *
 * @param[in] glossy Glossy descriptor that reports the event
 * @param[in] ev     The event that occurred
 */
typedef void (*glossy_cb_t)(glossy_t *glossy, glossy_ev_t ev);

/**
 * @brief Internal Glossy FSM state handler
 *
 * @param[in] glossy Glossy descriptor
 * @param[in] event  Event to process in the current state
 *
 * @return Result of the event processing
 */
typedef glossy_fsm_return_t (*glossy_fsm_state_cb_t)(glossy_t *glossy, glossy_fsm_ev_t event);

/**
 * @brief Internal Glossy FSM
 */
typedef struct {
    glossy_fsm_state_cb_t fsm_state_cb;     /**< current state of the Glossy FSM */
    uint8_t busy_status;                    /**< flag if the Glossy FSM is in use or not */
} glossy_fsm_t;

/**
 * @brief Glossy descriptor
 */
struct glossy {
    ieee802154_dev_t radio;                     /**< 802.15.4 HAL descriptor */
    glossy_cb_t cb;                             /**< event callback of the upper layer */
    uint32_t numof_tx;                          /**< current number of transmissions */
    bool is_initiator;                          /**< flag if the node is the flood initiator */
    ieee802154_phy_conf_t phy_conf;             /**< PHY configuration */
    eui64_t ext_addr;                           /**< IEEE 802.15.4 extended address */
    network_uint16_t short_addr;                /**< IEEE 802.15.4 short address */
    uint16_t panid;                             /**< IEEE 802.15.4 PAN ID */
    const iolist_t *psdu;                       /**< stores the current PSDU */
    iolist_t rx_iol;                            /**< iolist wrapping @ref glossy_t::rx_buf */
    uint8_t rx_buf[IEEE802154_FRAME_LEN_MAX];   /**< stores received frame */
    size_t rx_len;                              /**< stores length of received frame */
    ieee802154_rx_info_t rx_info;               /**< stores LQI and RSSI of received frame */
    glossy_fsm_t fsm;                           /**< Glossy FSM */
    event_queue_t evq;                          /**< event queue of the Glossy thread */
    event_t ev;                                 /**< event posted from the radio ISR */
    uint8_t isr_flags;                          /**< pending radio events, accessed atomically */
};

/**
 * @brief Initialize a Glossy descriptor and the underlying radio
 *
 * @param[out] glossy     Glossy descriptor to initialize
 * @param[in]  cb         Callback for events reported to the upper layer
 * @param[in]  short_addr IEEE 802.15.4 short address of the node
 * @param[in]  ext_addr   IEEE 802.15.4 extended address of the node
 *
 * @retval 0  on success
 * @retval <0 negative errno on error
 */
int glossy_init(glossy_t *glossy, glossy_cb_t cb,
                const network_uint16_t *short_addr, const eui64_t *ext_addr);

/**
 * @brief Load the frame that is sent during the next flood
 *
 * @param[in] glossy Glossy descriptor
 * @param[in] psdu   PSDU to flood, wrapped in an iolist
 *
 * @retval 0  on success
 * @retval <0 negative errno on error
 */
int glossy_load_frame(glossy_t *glossy, const iolist_t *psdu);

/**
 * @brief Start a flood
 *
 * The result is reported via @ref GLOSSY_EV_FLOOD_DONE.
 *
 * @param[in] glossy    Glossy descriptor
 * @param[in] initiator true if this node initiates the flood,
 *                      false if it only participates
 *
 * @retval 0  on success
 * @retval <0 negative errno on error
 */
int glossy_start(glossy_t *glossy, bool initiator);

/**
 * @brief Stops Glossy FSM
 *
 * @param[in] glossy Glossy descriptor
 *
 * @retval 0  on success
 * @retval <0 negative errno on error
 */
int glossy_stop(glossy_t *glossy);

/**
 * @brief Read the frame received during the last flood
 *
 * Only valid inside the @ref GLOSSY_EV_FLOOD_DONE callback.
 *
 * @param[in]  glossy   Glossy descriptor
 * @param[out] buf      buffer to write to, may be NULL to only query the length
 * @param[in]  max_size size of @p buf
 * @param[out] info     RX info, may be NULL
 *
 * @return number of bytes written
 * @retval -ENOBUFS if @p buf is too small
 * @retval -ENOENT  if the node was initiator and did not receive anything
 */
int glossy_read(glossy_t *glossy, uint8_t *buf, size_t max_size,
                ieee802154_rx_info_t *info);

/**
 * @brief Print the current state of the Glossy FSM
 *
 * @param[in] glossy Glossy descriptor
 */
void print_glossy_state(glossy_t *glossy);

#ifdef __cplusplus
}
#endif

/** @} */
