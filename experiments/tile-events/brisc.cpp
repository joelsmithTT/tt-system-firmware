// Sends N tile event records to ARC, announcing each with a token to the MSI catcher
// or, with PARAM_PMM, a message to the PMM mailbox.
#include <stdint.h>
#include "layout.h"

#define RD(a) (*(volatile uint32_t *)(a))
#define WR(a, v) (*(volatile uint32_t *)(a) = (v))
#define FENCE() __asm__ volatile("fence rw, rw" ::: "memory")

#define NOC0 0xFFB20000u
#define NOC_TARG_ADDR_LO 0x000u
#define NOC_TARG_ADDR_MID 0x004u
#define NOC_TARG_ADDR_HI 0x008u
#define NOC_RET_ADDR_LO 0x00Cu
#define NOC_RET_ADDR_MID 0x010u
#define NOC_RET_ADDR_HI 0x014u
#define NOC_PACKET_TAG 0x018u
#define NOC_CTRL 0x01Cu
#define NOC_AT_LEN_BE 0x020u
#define NOC_CMD_CTRL 0x040u
#define NOC_COUNTER_CLEAR 0x060u
#define NOC_ID_LOGICAL 0x148u
#define NOC_CMD_BUF_OFFSET 0x800u
#define NOC_COUNTER_BASE 0x200u
#define NOC_MST_REQS_OUTSTANDING_ID0 16u
#define NOC_CTRL_WR_NONPOSTED 0x12u
#define WR_BUF (1u * NOC_CMD_BUF_OFFSET)

static void wait_outstanding()
{
    while (RD(NOC0 + NOC_COUNTER_BASE + NOC_MST_REQS_OUTSTANDING_ID0 * 4) & 0xFFu) {
    }
}

extern "C" void brisc_main()
{
    const uint32_t n = RD(PARAM_N);
    const uint32_t marker = RD(PARAM_MARKER);
    const uint32_t sync = RD(PARAM_SYNC);
    const uint32_t pmm = RD(PARAM_PMM);
    const uint32_t me = RD(NOC0 + NOC_ID_LOGICAL) & 0xFFFu;

    WR(NOC0 + 0x100u, RD(NOC0 + 0x100u) | (1u << 16)); // NIU_CFG_0: enable command FIFOs, as tt-lab does
    WR(NOC0 + WR_BUF + NOC_TARG_ADDR_MID, 0u);
    WR(NOC0 + WR_BUF + NOC_TARG_ADDR_HI, me);
    WR(NOC0 + WR_BUF + NOC_RET_ADDR_MID, 0u);
    WR(NOC0 + WR_BUF + NOC_PACKET_TAG, 0u);
    WR(NOC0 + WR_BUF + NOC_CTRL, NOC_CTRL_WR_NONPOSTED);
    WR(NOC0 + NOC_COUNTER_CLEAR, 0xFFFFu); // counters are stale from before reset, as tt-lab does

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t rec = RECORDS + RECORD_STRIDE * i;
        const uint32_t tok = TOKBUF + 64u * i;

        WR(rec + 4, 0u); // type TEST
        WR(rec + 8, 0xC0FFEEu);
        WR(rec + 12, marker);
        WR(rec + 16, i);
        WR(rec + 20, me);
        WR(rec, RECORD_MAGIC); // last: the record is complete
        if (pmm) {
            // PMM keeps one message per tile: wait for ARC to ack the previous record.
            uint32_t spins = 0;
            while (i && RD(rec - RECORD_STRIDE) == RECORD_MAGIC && ++spins < ACK_SPINS) {
            }
            if (spins == ACK_SPINS) {
                WR(DONE, DONE_STUCK | (i - 1));
                return;
            }
            WR(tok, (me & 0x3Fu) << 26 | (me >> 6) << 20 | 1u << 12 | rec >> 9);
        } else {
            WR(tok, 0x80000000u | ((rec >> 4) << 12) | me);
        }
        FENCE();

        WR(NOC0 + WR_BUF + NOC_TARG_ADDR_LO, tok);
        WR(NOC0 + WR_BUF + NOC_RET_ADDR_LO, pmm ? ARC_PMM : ARC_FIFO);
        WR(NOC0 + WR_BUF + NOC_RET_ADDR_HI, ARC_COORD);
        WR(NOC0 + WR_BUF + NOC_AT_LEN_BE, 4u);
        WR(NOC0 + WR_BUF + NOC_CMD_CTRL, 1u);
        while (RD(NOC0 + WR_BUF + NOC_CMD_CTRL)) {
        }
        if (sync) {
            wait_outstanding();
        }
    }
    wait_outstanding();
    WR(DONE, n);
}
