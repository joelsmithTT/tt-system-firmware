/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "device_events.h"

#include <errno.h>

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#include "noc2axi.h"
#include "pcie.h"
#include "telemetry.h"
#include "timer.h"

BUILD_ASSERT(sizeof(struct event_ring_entry) == 32, "event ring entry is 32 bytes");
BUILD_ASSERT(offsetof(struct event_ring, entries) == 32, "event ring header is 32 bytes");
BUILD_ASSERT(IS_POWER_OF_TWO(EVENT_RING_NUM_ENTRIES), "event ring size is a power of 2");

/* CSM is the SMC's RAM, so the ring is visible to the host where it is. */
static struct event_ring ring = {
	.magic = EVENT_RING_MAGIC,
	.version = EVENT_RING_VERSION,
	.entry_size = sizeof(struct event_ring_entry),
	.num_entries = EVENT_RING_NUM_ENTRIES,
};

/* Serializes appends; callers may be ISRs. */
static struct k_spinlock event_lock;

void event_doorbell(void)
{
	SendHostDoorbellMsi();
}

/*
 * Firmware never writes an entry the host has not released by advancing rptr, so the host
 * can read [rptr, wptr) without racing us. An rptr that is not within the last
 * num_entries records is a host bug; treat the ring as full rather than trust it.
 */
static uint32_t event_ring_append_locked(uint16_t type, uint16_t source, uint32_t cause,
					 const uint32_t data[3])
{
	uint32_t s = ring.wptr;
	struct event_ring_entry *e = &ring.entries[s % EVENT_RING_NUM_ENTRIES];

	if (s - ring.rptr >= EVENT_RING_NUM_ENTRIES) {
		ring.overflow++;
		return s;
	}

	e->type = type;
	e->source = source;
	e->cause = cause;
	e->data[0] = data[0];
	e->data[1] = data[1];
	e->data[2] = data[2];
	e->timestamp = TimerTimestamp();
	e->seq = s;

	barrier_dmem_fence_full();
	ring.wptr = s + 1;

	return s + 1;
}

int event_ring_append_no_doorbell(uint16_t type, uint16_t source, uint32_t cause,
				  const uint32_t data[3])
{
	k_spinlock_key_t key;

	if (type > EVENT_TYPE_FW_MAX) {
		return -EINVAL;
	}

	key = k_spin_lock(&event_lock);
	event_ring_append_locked(type, source, cause, data);
	k_spin_unlock(&event_lock, key);

	return 0;
}

int event_ring_append(uint16_t type, uint16_t source, uint32_t cause, const uint32_t data[3])
{
	int ret = event_ring_append_no_doorbell(type, source, cause, data);

	if (ret == 0) {
		barrier_dmem_fence_full();
		event_doorbell();
	}

	return ret;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_TEST_EVENT_RECORD messages
 *
 * @see test_event_record_rqst
 */
static uint8_t test_event_record_handler(const union request *request, struct response *response)
{
	const struct test_event_record_rqst *rqst = &request->test_event_record;
	uint32_t data[3] = {rqst->data[0], rqst->data[1], 0};
	uint32_t wptr = ring.wptr;

	if (rqst->type != EVENT_TYPE_TEST) {
		return EINVAL;
	}

	/* repeat 0 sends only the doorbell, so the host can test an MSI with nothing new. */
	if (rqst->repeat > EVENT_RING_TEST_MAX_REPEAT) {
		return EINVAL;
	}

	/* Lock per record so a long batch does not hold off interrupts. */
	for (uint32_t i = 0; i < rqst->repeat; i++) {
		k_spinlock_key_t key = k_spin_lock(&event_lock);

		data[2] = rqst->data[2] + i;
		wptr = event_ring_append_locked(rqst->type, rqst->source, rqst->cause, data);

		k_spin_unlock(&event_lock, key);
	}

	barrier_dmem_fence_full();
	event_doorbell();

	response->data[1] = wptr;
	return 0;
}

/*
 * EXPERIMENT: tile events.  A tile writes a struct tile_event_record into its
 * own L1, then writes a token to ARC's MSI catcher FIFO (8,0):0x800B0000:
 *   bit 31      1 (0 is the host's message queue doorbell)
 *   bits 30:12  record L1 address >> 4
 *   bits 11:0   the tile's translated NOC coordinates, x | y << 6
 * ARC copies the record into the event ring, with the tile as source, and
 * acks by clearing the record's magic.  A token is only a hint: the FIFO
 * holds 64 and stock firmware flushes it on overflow.
 */
#define TILE_TOKEN_FLAG    BIT(31)
#define TILE_RECORD_MAGIC  0x54455654 /* "TEVT" */
#define TILE_EVENT_TLB     15

struct tile_event_record {
	uint32_t magic;
	uint16_t type;
	uint16_t reserved;
	uint32_t cause;
	uint32_t data[3];
};

static struct {
	uint32_t tokens, records, bad, queue_full, overflows;
} tile_stats;

K_MSGQ_DEFINE(tile_tokens, sizeof(uint32_t), 128, 4);

static bool tile_coords_ok(uint32_t x, uint32_t y)
{
	/*
	 * Translated Tensix columns present on every SKU: x 1-7 and 10-14, y 2-11.
	 * x 15-16 are fused off on p100, and a NOC access to a missing tile can hang.
	 */
	return ((x >= 1 && x <= 7) || (x >= 10 && x <= 14)) && y >= 2 && y <= 11;
}

static void tile_event_work_handler(struct k_work *work)
{
	uint32_t token;

	ARG_UNUSED(work);
	while (k_msgq_get(&tile_tokens, &token, K_NO_WAIT) == 0) {
		uint32_t x = token & 0x3F, y = (token >> 6) & 0x3F;
		uint32_t addr = ((token >> 12) & 0x7FFFF) << 4;
		uint32_t w[5];

		if (!tile_coords_ok(x, y)) {
			tile_stats.bad++;
			continue;
		}

		NOC2AXITlbSetup(0, TILE_EVENT_TLB, x, y, addr);
		for (int i = 0; i < 5; i++) {
			w[i] = NOC2AXIRead32(0, TILE_EVENT_TLB, addr + 4 * i);
		}
		if (w[0] != TILE_RECORD_MAGIC || (w[1] & 0xFFFF) > EVENT_TYPE_FW_MAX) {
			tile_stats.bad++;
			continue;
		}

		uint32_t data[3] = {w[3], w[4], NOC2AXIRead32(0, TILE_EVENT_TLB, addr + 20)};

		event_ring_append(w[1] & 0xFFFF, EVENT_SOURCE_TILE(x, y), w[2], data);
		NOC2AXIWrite32(0, TILE_EVENT_TLB, addr, 0);
		tile_stats.records++;
	}
}

static K_WORK_DEFINE(tile_event_work, tile_event_work_handler);

void msi_catcher_token(uint32_t token)
{
	tile_stats.tokens++;
	if (!(token & TILE_TOKEN_FLAG)) {
		tile_stats.bad++;
		return;
	}
	if (k_msgq_put(&tile_tokens, &token, K_NO_WAIT) != 0) {
		tile_stats.queue_full++;
	}
	k_work_submit(&tile_event_work);
}

void msi_catcher_overflowed(void)
{
	tile_stats.overflows++;
}

/* EXPERIMENT: TT_SMC_MSG_TEST_TILE_EVENTS (0x92) returns tile_stats in data[1..5]. */
static uint8_t test_tile_events_handler(const union request *request, struct response *response)
{
	ARG_UNUSED(request);
	response->data[1] = tile_stats.tokens;
	response->data[2] = tile_stats.records;
	response->data[3] = tile_stats.bad;
	response->data[4] = tile_stats.queue_full;
	response->data[5] = tile_stats.overflows;
	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_TEST_TILE_EVENTS, test_tile_events_handler);

static int device_events_init(void)
{
	UpdateTelemetryEventRing((uint32_t)&ring);
	return 0;
}
/* Publish alongside the rest of telemetry. */
SYS_INIT(device_events_init, POST_KERNEL, init_telemetry_PRIO);

REGISTER_MESSAGE(TT_SMC_MSG_TEST_EVENT_RECORD, test_event_record_handler);
