// Host helpers over the KMD's NOC_READ/NOC_WRITE and SMC_MSG ioctls.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ioctl.h" // tt-kmd's uapi header; build.sh points -I at KMD_DIR

static int fd;

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static void open_dev(void)
{
	fd = open("/dev/tenstorrent/0", O_RDWR);
	if (fd < 0)
		die("open /dev/tenstorrent/0");
}

// Translated coordinates that exist on every Blackhole SKU: ARC, and Tensix
// x 1-7 and 10-14 (15-16 are fused off on p100), y 2-11.  A NOC access to a
// tile that isn't there can hang the host.
static int coord_ok(unsigned x, unsigned y)
{
	if (x == 8 && y == 0)
		return 1;
	return ((x >= 1 && x <= 7) || (x >= 10 && x <= 14)) && y >= 2 && y <= 11;
}

static __attribute__((unused)) uint32_t noc(int wr, unsigned x, unsigned y, uint64_t addr, uint32_t value)
{
	struct tenstorrent_noc_io io = { .argsz = sizeof(io), .x = x, .y = y, .width = 4, .addr = addr, .value = value };

	if (!coord_ok(x, y)) {
		fprintf(stderr, "refusing NOC access to (%u,%u): not on every SKU\n", x, y);
		exit(1);
	}
	if (ioctl(fd, wr ? TENSTORRENT_IOCTL_NOC_WRITE : TENSTORRENT_IOCTL_NOC_READ, &io))
		die("noc");
	return io.value;
}

static __attribute__((unused)) unsigned smc(uint32_t w0, uint32_t w1, uint32_t rsp[8])
{
	struct tenstorrent_smc_msg msg = { .argsz = sizeof(msg), .flags = TENSTORRENT_SMC_MSG_POST };

	msg.message[0] = w0;
	msg.message[1] = w1;
	if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg))
		die("SMC_MSG post");
	for (int i = 0; i < 5000; i++) {
		memset(&msg, 0, sizeof(msg));
		msg.argsz = sizeof(msg);
		msg.flags = TENSTORRENT_SMC_MSG_POLL;
		if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg) == 0) {
			memcpy(rsp, msg.message, 32);
			return msg.message[0] & 0xFF;
		}
		if (errno != EAGAIN)
			die("SMC_MSG poll");
		usleep(1000);
	}
	fprintf(stderr, "SMC_MSG: no response\n");
	exit(1);
}
