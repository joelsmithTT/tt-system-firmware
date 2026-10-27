/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DEVICE_EVENTS_H
#define DEVICE_EVENTS_H

#include <stdint.h>

/**
 * @defgroup device_events Device Events
 * @brief Asynchronous device-to-host event notification.
 *
 * A ring of event records lives in CSM and is published through telemetry
 * (@ref TAG_EVENT_RING_ADDR). ARC firmware appends records; the host reads them and
 * writes back its read pointer, the only field it writes. After appending, firmware
 * sends the host doorbell MSI, which carries no payload; the host reads the ring to
 * find out what is new.
 *
 * See @ref ttfw_device_events for the host-side protocol.
 * @{
 */

/** @brief Identifies the event ring ("TEVR") */
#define EVENT_RING_MAGIC       0x54455652
#define EVENT_RING_VERSION     1
#define EVENT_RING_NUM_ENTRIES 64

/** @brief Firmware event types are 0x0000-0x7FFF; 0x8000-0xFFFF belong to the host driver. */
#define EVENT_TYPE_FW_MAX 0x7FFF

/** @brief Event type reserved for test messages */
#define EVENT_TYPE_TEST 0x0000

/**
 * @brief Reserved for rate limiting (not implemented).
 *
 * data[0] is the suppressed type and data[1] the number of records suppressed.
 */
#define EVENT_TYPE_EVENTS_SUPPRESSED 0x0001

/**
 * @brief Encode a tile as an event source: its translated NOC coordinates, packed as in
 * NOC_NODE_ID.
 */
#define EVENT_SOURCE_TILE(x, y) ((uint16_t)((x) | ((y) << 6)))

/** @brief Most records one @ref TT_SMC_MSG_TEST_EVENT_RECORD may append */
#define EVENT_RING_TEST_MAX_REPEAT 1024

/** @brief One record in the event ring */
struct event_ring_entry {
	uint32_t seq;       /**< Firmware sequence number of this record */
	uint16_t type;      /**< Event type */
	uint16_t source;    /**< For a tile, @ref EVENT_SOURCE_TILE; else type-specific */
	uint32_t cause;     /**< Type-specific subcode */
	uint32_t data[3];   /**< Type-specific data */
	uint64_t timestamp; /**< ARC reference clock (50 MHz, 20 ns ticks) since boot */
};

/**
 * @brief Event ring, at the CSM address in @ref TAG_EVENT_RING_ADDR
 *
 * A producer/consumer ring. Firmware appends at @c wptr and the host reads from @c rptr;
 * both are free-running and record @c s lives in entries[s % num_entries]. When the ring is
 * full, firmware drops the new record and counts it in @c overflow rather than wait.
 */
struct event_ring {
	uint32_t magic;       /**< @ref EVENT_RING_MAGIC */
	uint32_t version;     /**< @ref EVENT_RING_VERSION */
	uint32_t entry_size;  /**< sizeof(struct event_ring_entry) */
	uint32_t num_entries; /**< @ref EVENT_RING_NUM_ENTRIES, a power of 2 */
	/** Sequence number the next record will get. Written by firmware; 0 at boot. */
	uint32_t wptr;
	/** Sequence number of the next record the host will read. Written by the host. */
	volatile uint32_t rptr;
	/** Records dropped because the ring was full. Written by firmware; wraps. */
	uint32_t overflow;
	uint32_t reserved; /**< Pads the header to 32 bytes */
	struct event_ring_entry entries[EVENT_RING_NUM_ENTRIES];
};

/**
 * @brief Append a record to the event ring, then send the doorbell MSI.
 *
 * If the ring is full the record is dropped and counted in @c overflow; the doorbell is
 * sent either way. Safe to call from any context, including ISRs.
 *
 * @retval 0 on success, including a dropped record.
 * @retval -EINVAL if @p type is not a firmware type.
 */
int event_ring_append(uint16_t type, uint16_t source, uint32_t cause, const uint32_t data[3]);

/**
 * @brief Append a record to the event ring without sending the doorbell MSI.
 *
 * Lets a batch of records share one MSI: call @ref event_doorbell after the last.
 * Safe to call from any context, including ISRs.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p type is not a firmware type.
 */
int event_ring_append_no_doorbell(uint16_t type, uint16_t source, uint32_t cause,
				  const uint32_t data[3]);

/** @brief Send the doorbell MSI. Safe to call from any context, including ISRs. */
void event_doorbell(void);

/** @} */

#endif
