// tiledrive [-p] [-s] N X,Y [X,Y ...]: load brisc.bin onto each Tensix (X,Y)
// (translated), release them together, have each send N tile event records to
// ARC, then report what reached the event ring.
//   -p: announce through the PMM mailbox (the default is the MSI catcher).
//   -s: MSI catcher only: wait for each token write to complete before the next.
#include <time.h>
#include <unistd.h>
#include "common.h"
#include "layout.h"

#define SOFT_RESET 0xFFB121B0u
#define ALL_RESET 0x47800u
#define BRISC_RESET 0x800u
#define MAX_TILES 120

static void stats(const char *when)
{
	uint32_t r[8];

	if (smc(0x92, 0, r)) {
		printf("%s ARC stats: TT_SMC_MSG_TEST_TILE_EVENTS not supported\n", when);
		return;
	}
	printf("%s ARC stats: MSI tokens %u records %u bad %u queue_full %u overflows %u | "
	       "PMM irq76 %u irq77 %u records %u bad %u\n",
	       when, r[1], r[2], r[3], r[4], r[5], r[6] & 0xFFFF, r[6] >> 16, r[7] & 0xFFFF, r[7] >> 16);
}

int main(int argc, char **argv)
{
	int pmm = 0, sync = 0, opt;

	while ((opt = getopt(argc, argv, "ps")) != -1) {
		if (opt == 'p')
			pmm = 1;
		else if (opt == 's')
			sync = 1;
		else
			return 2;
	}
	if (argc - optind < 2) {
		fprintf(stderr, "usage: tiledrive [-p] [-s] N X,Y [X,Y ...]\n");
		return 2;
	}
	unsigned n = atoi(argv[optind++]), nt = 0;
	unsigned tx[MAX_TILES], ty[MAX_TILES];

	if (n > MAX_TOKENS)
		n = MAX_TOKENS;
	for (; optind < argc && nt < MAX_TILES; optind++, nt++) {
		if (sscanf(argv[optind], "%u,%u", &tx[nt], &ty[nt]) != 2 || !coord_ok(tx[nt], ty[nt]) ||
		    (tx[nt] == 8 && ty[nt] == 0)) {
			fprintf(stderr, "%s is not a Tensix tile on every SKU\n", argv[optind]);
			return 1;
		}
	}

	static uint32_t prog[4096];
	FILE *f = fopen("brisc.bin", "rb");
	if (!f)
		die("brisc.bin");
	size_t words = (fread(prog, 1, sizeof(prog), f) + 3) / 4;
	uint32_t marker = 0x7E57u << 16 | (time(NULL) & 0xFFFF);

	open_dev();
	for (unsigned t = 0; t < nt; t++) {
		unsigned x = tx[t], y = ty[t];

		noc(1, x, y, SOFT_RESET, ALL_RESET);
		for (size_t i = 0; i < words; i++)
			noc(1, x, y, 4 * i, prog[i]);
		for (unsigned i = 0; i < n; i++)
			noc(1, x, y, RECORDS + RECORD_STRIDE * i, 0);
		noc(1, x, y, PARAM_N, n);
		noc(1, x, y, PARAM_MARKER, marker);
		noc(1, x, y, PARAM_SYNC, sync);
		noc(1, x, y, PARAM_PMM, pmm);
		noc(1, x, y, DONE, 0);
	}

	stats("before:");

	struct tenstorrent_read_events re = { .argsz = sizeof(re) };
	ioctl(fd, TENSTORRENT_IOCTL_READ_EVENTS, &re); // cursor = now

	for (unsigned t = 0; t < nt; t++)
		noc(1, tx[t], ty[t], SOFT_RESET, ALL_RESET & ~BRISC_RESET);
	unsigned finished = 0, stuck = 0;
	int ms;
	for (ms = 0; ms < 2000 && finished + stuck < nt; ms++) {
		usleep(1000);
		finished = stuck = 0;
		for (unsigned t = 0; t < nt; t++) {
			uint32_t d = noc(0, tx[t], ty[t], DONE, 0);
			finished += d == n;
			stuck += !!(d & DONE_STUCK);
		}
	}
	printf("%u tile(s) via %s: %u finished, %u stuck waiting for an ack, %u timed out (~%d ms)\n", nt,
	       pmm ? "PMM" : sync ? "MSI catcher (sync)" : "MSI catcher", finished, stuck, nt - finished - stuck,
	       ms);
	usleep(200000);
	stats("after: ");

	unsigned acked = 0;
	for (unsigned t = 0; t < nt; t++)
		for (unsigned k = 0; k < n; k++)
			acked += noc(0, tx[t], ty[t], RECORDS + RECORD_STRIDE * k, 0) == 0;
	printf("records acked by ARC (magic cleared): %u/%u\n", acked, n * nt);

	static struct tenstorrent_event ev[4096];
	re = (struct tenstorrent_read_events){ .argsz = sizeof(re), .capacity = 4096, .events_ptr = (uintptr_t)ev };
	if (ioctl(fd, TENSTORRENT_IOCTL_READ_EVENTS, &re))
		die("READ_EVENTS");

	unsigned mine = 0, fw_lost = 0, tiles_seen = 0, inorder = 1, bad_source = 0;
	static int prev[64 * 64];
	memset(prev, -1, sizeof(prev));
	for (unsigned k = 0; k < re.count; k++) {
		if (ev[k].type == TENSTORRENT_EVENT_TYPE_FW_EVENTS_LOST)
			fw_lost += ev[k].data[0];
		if (ev[k].data[0] != marker)
			continue;
		unsigned src = ev[k].source & 0xFFF;
		if (mine < 2)
			printf("  event seq %llu type 0x%x source 0x%03x (x=%u y=%u) cause 0x%x data 0x%x %u 0x%x\n",
			       (unsigned long long)ev[k].seq, ev[k].type, ev[k].source, src & 0x3F, src >> 6,
			       ev[k].cause, ev[k].data[0], ev[k].data[1], ev[k].data[2]);
		bad_source += src != ev[k].data[2];
		if (prev[src] < 0)
			tiles_seen++;
		else if ((int)ev[k].data[1] <= prev[src])
			inorder = 0;
		prev[src] = ev[k].data[1];
		mine++;
	}
	printf("host received %u/%u records from %u/%u tiles via READ_EVENTS, per-tile order %s, "
	       "source mismatches %u; FW_EVENTS_LOST %u; reader lost %u\n",
	       mine, n * nt, tiles_seen, nt, inorder ? "OK" : "WRONG", bad_source, fw_lost, re.lost);

	for (unsigned t = 0; t < nt; t++)
		noc(1, tx[t], ty[t], SOFT_RESET, ALL_RESET);
	return 0;
}
