// tiledrive X Y N [sync]: load brisc.bin onto Tensix (X,Y) (translated), have
// it send N tile event tokens to ARC, then report what reached the event ring.
//   sync: BRISC waits for each token write to complete before the next.
#include <time.h>
#include "common.h"
#include "layout.h"

#define SOFT_RESET 0xFFB121B0u
#define ALL_RESET 0x47800u
#define BRISC_RESET 0x800u

static void stats(const char *when)
{
	uint32_t r[8];

	if (smc(0x92, 0, r)) {
		printf("%s ARC stats: TT_SMC_MSG_TEST_TILE_EVENTS not supported\n", when);
		return;
	}
	printf("%s ARC stats: tokens %u, records %u, bad %u, queue_full %u, overflows %u\n", when, r[1], r[2], r[3],
	       r[4], r[5]);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: tiledrive X Y N [sync]\n");
		return 2;
	}
	unsigned x = atoi(argv[1]), y = atoi(argv[2]), n = atoi(argv[3]);
	int sync = argc > 4 && atoi(argv[4]);
	static uint32_t prog[4096];
	FILE *f = fopen("brisc.bin", "rb");
	if (!f)
		die("brisc.bin");
	size_t words = (fread(prog, 1, sizeof(prog), f) + 3) / 4;
	uint32_t marker = 0x7E57u << 16 | (time(NULL) & 0xFFFF);

	if (n > MAX_TOKENS)
		n = MAX_TOKENS;
	open_dev();
	if (!coord_ok(x, y) || (x == 8 && y == 0)) {
		fprintf(stderr, "(%u,%u) is not a Tensix tile on every SKU\n", x, y);
		return 1;
	}

	noc(1, x, y, SOFT_RESET, ALL_RESET);
	for (size_t i = 0; i < words; i++)
		noc(1, x, y, 4 * i, prog[i]);
	for (unsigned i = 0; i < n; i++)
		noc(1, x, y, RECORDS + 32 * i, 0);
	noc(1, x, y, PARAM_N, n);
	noc(1, x, y, PARAM_MARKER, marker);
	noc(1, x, y, PARAM_SYNC, sync);
	noc(1, x, y, DONE, 0);

	stats("before:");

	struct tenstorrent_read_events re = { .argsz = sizeof(re) };
	ioctl(fd, TENSTORRENT_IOCTL_READ_EVENTS, &re); // cursor = now

	noc(1, x, y, SOFT_RESET, ALL_RESET & ~BRISC_RESET);
	int i;
	for (i = 0; i < 1000 && noc(0, x, y, DONE, 0) != n; i++)
		usleep(1000);
	printf("BRISC at (%u,%u) %s after ~%d ms (sync=%d)\n", x, y, i < 1000 ? "finished" : "TIMED OUT", i, sync);
	usleep(200000);
	stats("after: ");

	unsigned acked = 0;
	for (unsigned k = 0; k < n; k++)
		acked += noc(0, x, y, RECORDS + 32 * k, 0) == 0;
	printf("records acked by ARC (magic cleared): %u/%u\n", acked, n);

	static struct tenstorrent_event ev[512];
	re = (struct tenstorrent_read_events){ .argsz = sizeof(re), .capacity = 512, .events_ptr = (uintptr_t)ev };
	if (ioctl(fd, TENSTORRENT_IOCTL_READ_EVENTS, &re))
		die("READ_EVENTS");
	unsigned mine = 0, inorder = 1, prev = 0, fw_lost = 0, gaps = 0, first = 0;
	for (unsigned k = 0; k < re.count; k++) {
		if (ev[k].type == TENSTORRENT_EVENT_TYPE_FW_EVENTS_LOST)
			fw_lost += ev[k].data[0];
		if (ev[k].data[0] != marker)
			continue;
		if (mine < 2)
			printf("  event seq %llu type 0x%x source 0x%03x (x=%u y=%u) cause 0x%x data 0x%x %u 0x%x\n",
			       (unsigned long long)ev[k].seq, ev[k].type, ev[k].source, ev[k].source & 0x3F,
			       ev[k].source >> 6, ev[k].cause, ev[k].data[0], ev[k].data[1], ev[k].data[2]);
		if (mine && ev[k].data[1] <= prev)
			inorder = 0;
		if (mine && ev[k].data[1] > prev + 1)
			gaps++;
		if (mine == 0)
			first = ev[k].data[1];
		prev = ev[k].data[1];
		mine++;
	}
	printf("host received %u/%u records via READ_EVENTS, in %s with %u gaps (first index %u, last %u); "
	       "FW_EVENTS_LOST %u; reader lost %u\n",
	       mine, n, inorder ? "order" : "the WRONG ORDER", gaps, mine ? first : 0, prev, fw_lost, re.lost);

	noc(1, x, y, SOFT_RESET, ALL_RESET);
	return 0;
}
