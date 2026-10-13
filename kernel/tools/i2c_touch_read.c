/*
 * i2c_touch_read — userspace burst reader for the device's touch controller.
 *
 * Reads a register window from the touch controller over /dev/i2c-N and prints
 * every change while a human touches the glass.
 *
 * WHY I2C_RDWR AND NOT I2C_SLAVE:
 *   panjit_ts is normally bound to this address.  I2C_SLAVE returns -EBUSY when
 *   a kernel driver claims the address (i2c-dev.c: i2cdev_check_addr), but the
 *   I2C_RDWR path carries the address inside each message and performs no busy
 *   check at all (i2cdev_ioctl_rdwr).  So this tool needs no unbind.
 *   [measured from the 4.14.52 source]
 *
 * WHY A COMBINED TRANSFER:
 *   Two messages in one ioctl (write the register offset, then read) issue a
 *   repeated START with no intervening STOP — the same transaction shape the
 *   in-tree cy8ctmg110_read_regs() uses.  A write() followed by a read() would
 *   be two transactions and may not address the register the same way.
 *
 * The expected register map, from the in-tree Cypress sibling driver
 * (drivers/input/touchscreen/cy8ctmg110_ts.c), is X1=3, Y1=5, X2=7, Y2=9,
 * FINGERS=11, GESTURE=12, each coordinate big-endian 16-bit.  Whether the part
 * actually fitted here uses that same map is exactly what this tool measures —
 * so the decode is printed alongside the raw bytes, never instead of them.
 *
 * Nothing here writes to the controller.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

/*
 * Self-declared to match include/uapi/linux/i2c.h and i2c-dev.h byte for byte,
 * so this compiles against any cross toolchain whether or not it ships the
 * kernel's i2c uapi headers.  Verified against the 4.14.52 tree.
 */
struct arca_i2c_msg {
	uint16_t addr;
	uint16_t flags;
	uint16_t len;
	uint8_t *buf;
};

struct arca_i2c_rdwr_ioctl_data {
	struct arca_i2c_msg *msgs;
	uint32_t nmsgs;
};

#define ARCA_I2C_M_RD 0x0001
#define ARCA_I2C_RDWR 0x0707

#define MAX_LEN 32

/*
 * Kernel input_event as this 32-bit ARM target lays it out: struct timeval is
 * two 4-byte longs here, so the whole record is 16 bytes.  Self-declared for the
 * same reason as the i2c structs above.
 */
struct arca_input_event {
	long tv_sec;
	long tv_usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

#define ARCA_EV_KEY 0x01
#define ARCA_EV_ABS 0x03
#define ARCA_ABS_X 0x00
#define ARCA_ABS_Y 0x01
#define ARCA_BTN_TOUCH 0x14a

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	/* long long: (tv_sec * 1000) overflows a 32-bit long on this target. */
	return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/* One combined write-then-read transfer.  Returns 0, or -errno. */
static int burst_read(int fd, int addr, int reg, uint8_t *out, int len)
{
	uint8_t cmd = (uint8_t)reg;
	struct arca_i2c_msg msgs[2];
	struct arca_i2c_rdwr_ioctl_data arg;

	memset(msgs, 0, sizeof(msgs));
	msgs[0].addr = (uint16_t)addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &cmd;

	msgs[1].addr = (uint16_t)addr;
	msgs[1].flags = ARCA_I2C_M_RD;
	msgs[1].len = (uint16_t)len;
	msgs[1].buf = out;

	arg.msgs = msgs;
	arg.nmsgs = 2;

	if (ioctl(fd, ARCA_I2C_RDWR, &arg) < 0)
		return -errno;
	return 0;
}

static void print_bytes(const uint8_t *b, int len)
{
	int i;
	for (i = 0; i < len; i++)
		printf("%02x ", b[i]);
}

/* Decode under the cy8ctmg110 assumption.  reg_base is the register `b[0]` is. */
static void print_decode(const uint8_t *b, int len, int reg_base)
{
	int i_x1 = 3 - reg_base, i_y1 = 5 - reg_base;
	int i_x2 = 7 - reg_base, i_y2 = 9 - reg_base;
	int i_f = 11 - reg_base;

	if (i_x1 >= 0 && i_y1 + 1 < len)
		printf(" X1=%4d Y1=%4d",
		       (b[i_x1] << 8) | b[i_x1 + 1],
		       (b[i_y1] << 8) | b[i_y1 + 1]);
	if (i_x2 >= 0 && i_y2 + 1 < len)
		printf(" X2=%4d Y2=%4d",
		       (b[i_x2] << 8) | b[i_x2 + 1],
		       (b[i_y2] << 8) | b[i_y2 + 1]);
	if (i_f >= 0 && i_f < len)
		printf(" fingers=%d", b[i_f]);
}

static void usage(const char *me)
{
	printf("Usage: %s [options]\n", me);
	printf("  --bus N          I2C bus number (default 2).  NEVER use bus 1: PMIC.\n");
	printf("  --addr 0xNN      slave address (default 0x03)\n");
	printf("  --reg N          first register of the burst (default 3)\n");
	printf("  --len N          bytes to burst (default 9, max %d)\n", MAX_LEN);
	printf("  --seconds N      run time (default 30; 0 = until killed)\n");
	printf("  --interval-ms N  poll period (default 20)\n");
	printf("  --all            print every sample, not only changes\n");
	printf("  --help           this text\n");
}

int main(int argc, char **argv)
{
	int bus = 2, addr = 0x03, reg = 3, len = 9;
	int seconds = 30, interval_ms = 20, print_all = 0, do_scan = 0;
	const char *evdev_path = NULL;
	int evfd = -1;
	long long ev_events = 0, ev_touch_events = 0;
	char path[64];
	int fd, i, j, rc;
	uint8_t cur[MAX_LEN], prev[MAX_LEN], dump[MAX_LEN];
	uint8_t bmin[MAX_LEN], bmax[MAX_LEN];
	int changed[MAX_LEN];
	long long samples = 0, changes = 0, errors = 0;
	int consec_err = 0, first_err = 0, have_prev = 0;
	int max_fingers = 0, nonzero_finger_samples = 0;
	long long t0, tnext_beat;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
		else if (!strcmp(argv[i], "--all")) print_all = 1;
		else if (!strcmp(argv[i], "--scan")) do_scan = 1;
		else if (!strcmp(argv[i], "--evdev")) evdev_path = argv[++i];
		else if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); return 2; }
		else if (!strcmp(argv[i], "--bus")) bus = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--addr")) addr = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--reg")) reg = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--len")) len = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--seconds")) seconds = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--interval-ms")) interval_ms = (int)strtol(argv[++i], NULL, 0);
		else { fprintf(stderr, "unknown option %s\n", argv[i]); usage(argv[0]); return 2; }
	}

	if (len < 1 || len > MAX_LEN) { fprintf(stderr, "--len out of range\n"); return 2; }
	if (bus == 1) {
		fprintf(stderr, "refusing bus 1: it carries the PMIC and pv02_app 5 can hang it\n");
		return 2;
	}
	if (interval_ms < 1) interval_ms = 1;

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	printf("i2c_touch_read: %s addr=0x%02x reg=%d len=%d interval=%dms seconds=%d\n",
	       path, addr, reg, len, interval_ms, seconds);

	/*
	 * --scan: which addresses on this bus answer at all?  Uses a bare
	 * one-byte READ with no register pointer written first, which is the
	 * gentlest probe (it never writes to the device).  Do not run this on
	 * bus 1: the guard above refuses that bus outright.
	 */
	if (do_scan) {
		int a, found = 0;
		uint8_t one;
		struct arca_i2c_msg m;
		struct arca_i2c_rdwr_ioctl_data sa;

		printf("scanning %s for responding addresses (read-only probe)\n", path);
		for (a = 0x03; a <= 0x77; a++) {
			memset(&m, 0, sizeof(m));
			m.addr = (uint16_t)a;
			m.flags = ARCA_I2C_M_RD;
			m.len = 1;
			m.buf = &one;
			sa.msgs = &m;
			sa.nmsgs = 1;
			if (ioctl(fd, ARCA_I2C_RDWR, &sa) >= 0) {
				printf("  0x%02x responds (first byte 0x%02x)\n", a, one);
				found++;
			}
		}
		printf("scan done: %d address(es) responded\n", found);
		if (found == 0)
			printf("  ZERO responders means the probe itself is suspect,"
			       " not that the bus is empty.\n");
		close(fd);
		return 0;
	}

	/*
	 * Instrument validation before the trace: dump the whole documented
	 * register window once.  A NAK shows up as an ioctl error here rather
	 * than as a screenful of plausible zeros later.
	 */
	rc = burst_read(fd, addr, 0, dump, 13);
	if (rc < 0) {
		printf("BASELINE regs 0..12: READ FAILED (%s)\n", strerror(-rc));
		printf("  A failure here means the address or bus is wrong, or the\n");
		printf("  controller is not answering — not that there is no touch.\n");
	} else {
		printf("BASELINE regs 0..12: ");
		print_bytes(dump, 13);
		printf("\n");
	}
	/*
	 * Optional second instrument.  Reading evdev alongside the register poll
	 * is what makes a trace self-validating: if evdev shows touch events and
	 * the registers stayed static at the same moment, the register map is
	 * wrong (or something drained them).  If evdev is silent too, the glass
	 * simply was not touched and the trace is void rather than informative.
	 */
	if (evdev_path) {
		evfd = open(evdev_path, O_RDONLY | O_NONBLOCK);
		if (evfd < 0)
			printf("evdev %s: OPEN FAILED (%s) — the touch-witness column is"
			       " MISSING, not negative\n", evdev_path, strerror(errno));
		else
			printf("evdev witness: %s open\n", evdev_path);
	}

	printf("Touch the glass now. Columns: t(ms) raw bytes, then the cy8ctmg110 decode.\n");
	fflush(stdout);

	for (i = 0; i < MAX_LEN; i++) { bmin[i] = 0xff; bmax[i] = 0x00; changed[i] = 0; }

	t0 = now_ms();
	tnext_beat = t0 + 2000;

	for (;;) {
		long long t = now_ms();
		struct timespec slp;

		if (seconds > 0 && t - t0 >= (long long)seconds * 1000LL)
			break;

		rc = burst_read(fd, addr, reg, cur, len);
		if (rc < 0) {
			errors++;
			consec_err++;
			if (!first_err) {
				first_err = 1;
				printf("%7lld  READ ERROR: %s\n", t - t0, strerror(-rc));
				fflush(stdout);
			}
			if (consec_err >= 50) {
				printf("50 consecutive read errors — giving up.\n");
				break;
			}
			goto nap;
		}
		consec_err = 0;
		samples++;

		/* Drain whatever evdev has, so touch events land beside the samples. */
		if (evfd >= 0) {
			struct arca_input_event ev;
			int n = 0;
			while (n < 64 && read(evfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				n++;
				ev_events++;
				if ((ev.type == ARCA_EV_ABS &&
				     (ev.code == ARCA_ABS_X || ev.code == ARCA_ABS_Y)) ||
				    (ev.type == ARCA_EV_KEY && ev.code == ARCA_BTN_TOUCH)) {
					ev_touch_events++;
					printf("%7lld  EVDEV %-9s = %d\n", t - t0,
					       ev.type == ARCA_EV_KEY ? "BTN_TOUCH" :
					       (ev.code == ARCA_ABS_X ? "ABS_X" : "ABS_Y"),
					       ev.value);
					fflush(stdout);
				}
			}
		}

		for (i = 0; i < len; i++) {
			if (cur[i] < bmin[i]) bmin[i] = cur[i];
			if (cur[i] > bmax[i]) bmax[i] = cur[i];
		}
		if (11 - reg >= 0 && 11 - reg < len) {
			int f = cur[11 - reg];
			if (f > max_fingers) max_fingers = f;
			if (f != 0) nonzero_finger_samples++;
		}

		if (!have_prev || memcmp(cur, prev, (size_t)len) != 0 || print_all) {
			if (have_prev)
				for (i = 0; i < len; i++)
					if (cur[i] != prev[i]) changed[i] = 1;
			if (have_prev || !print_all) changes++;
			printf("%7lld  ", t - t0);
			print_bytes(cur, len);
			print_decode(cur, len, reg);
			printf("\n");
			fflush(stdout);
			tnext_beat = t + 2000;
		} else if (t >= tnext_beat) {
			printf("%7lld  (unchanged) ", t - t0);
			print_bytes(cur, len);
			printf("\n");
			fflush(stdout);
			tnext_beat = t + 2000;
		}

		memcpy(prev, cur, (size_t)len);
		have_prev = 1;

nap:
		slp.tv_sec = 0;
		slp.tv_nsec = (long)interval_ms * 1000000L;
		nanosleep(&slp, NULL);
	}

	printf("\n--- summary ---\n");
	printf("samples=%lld  changed-samples=%lld  read-errors=%lld\n",
	       samples, changes, errors);
	printf("per-byte over the run (reg: min max changed?)\n");
	for (i = 0; i < len; i++) {
		if (bmax[i] < bmin[i])
			printf("  reg %2d: (never read)\n", reg + i);
		else
			printf("  reg %2d: %02x %02x %s\n", reg + i, bmin[i], bmax[i],
			       changed[i] ? "CHANGED" : "static");
	}
	printf("max fingers byte (reg 11) = %d, samples with fingers!=0 = %d\n",
	       max_fingers, nonzero_finger_samples);

	j = 0;
	for (i = 0; i < len; i++) if (changed[i]) j++;

	printf("\n--- verdict ---\n");
	if (evdev_path)
		printf("evdev witness: %lld events total, %lld of them touch events\n",
		       ev_events, ev_touch_events);

	if (samples > 0 && j == 0) {
		if (evdev_path && evfd >= 0 && ev_touch_events > 0) {
			printf("REGISTER MAP IS WRONG (or something drained the registers):\n");
			printf("  evdev saw %lld touch events, so the glass WAS touched and the\n",
			       ev_touch_events);
			printf("  controller WAS reporting — yet regs %d..%d never moved.\n",
			       reg, reg + len - 1);
			printf("  Widen the window (--reg 0 --len 32) to find where the data lives.\n");
		} else if (evdev_path && evfd >= 0) {
			printf("TRACE IS VOID, not negative: evdev saw no touch events either, so\n");
			printf("  the glass was not touched during this window. Nothing is learned\n");
			printf("  about the register map. Re-run with the glass actually touched.\n");
		} else {
			printf("AMBIGUOUS: not one byte changed across %lld samples, and there was\n",
			       samples);
			printf("  no evdev witness, so 'never touched' and 'wrong registers' cannot\n");
			printf("  be told apart. Re-run with --evdev /dev/input/event0.\n");
		}
	} else if (samples > 0) {
		printf("LIVE DATA: %d of %d bytes in regs %d..%d changed during the run.\n",
		       j, len, reg, reg + len - 1);
	}

	if (evfd >= 0)
		close(evfd);


	close(fd);
	return 0;
}
