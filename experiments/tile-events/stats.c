// stats: print ARC's tile-event forwarding counters (TT_SMC_MSG_TEST_TILE_EVENTS).
#include "common.h"

int main(void)
{
	uint32_t r[8];

	open_dev();
	unsigned st = smc(0x92, 0, r);
	printf("status %u: MSI tokens %u, records %u, bad %u, queue_full %u, overflows %u | "
	       "PMM irq76 %u, irq77 %u, records %u, bad %u\n",
	       st, r[1], r[2], r[3], r[4], r[5], r[6] & 0xFFFF, r[6] >> 16, r[7] & 0xFFFF, r[7] >> 16);
	return st != 0;
}
