// SPDX-License-Identifier: GPL-2.0-only
/*
 * Touch driver for the Cypress CY8CTMG120 on the Arca, bound to the
 * vendor DT node `tsc_panjit@03` (compatible "panjit_ts") that our image
 * keeps unchanged.
 *
 * Adapted from drivers/input/touchscreen/cy8ctmg110_ts.c (Samuli Konttila,
 * Aava Mobile), whose register map this part shares byte for byte
 * (SYSTEM_ANALYSIS.md#33-touch). The host handshake is NOT that driver's:
 * it is the vendor kernel's panjit_ts, read out of its disassembly —
 *   - probe: reset pulse; reg0 = 0x00; clear reg1 until it reads 0; then,
 *     with the IRQ requested, reg0 = 0x08 (the base driver's 0x10 wedges
 *     the part, measured);
 *   - the IRQ is level-low, whatever the node's falling-edge cell says;
 *   - while a finger is down the host polls every 10 ms and writes
 *     reg1 = 0x00 after each read. Without that write the part reports one
 *     touch and then stalls (measured);
 *   - on lift, reg0 = 0x08 and reg1 = 0x00 again, then the IRQ re-arms.
 * Coordinates are 12-bit 0..4095; a value with bits above 0x0fff is an
 * "unresolved" marker, handled in the IRQ thread.
 *
 * Event order per frame is ABS_X, ABS_Y, BTN_TOUCH, SYN_REPORT, the order
 * native_apps/common/touch_input.c relies on. The input device is named
 * "panjit_ts" because userspace matches that name.
 *
 * Both fingers also go out as type-B MT slots (slot 0 = X1/Y1, slot 1 =
 * X2/Y2, valid while byte 8's count covers them), ahead of the legacy
 * events. The slots are reported by hand: input_mt_sync_frame()'s pointer
 * emulation emits BTN_TOUCH before ABS_X/ABS_Y, which breaks that order.
 * With two fingers the part reports the corners of their bounding box —
 * (min X, min Y) and (max X, max Y) — not the fingers: top-right plus
 * bottom-left reads the same as top-left plus bottom-right (measured).
 * No ABS_PRESSURE: the vendor's was a constant 255/0 and nothing reads it.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/of.h>

#define DRIVER_NAME	"panjit_ts"

#define REG_MODE	0
#define REG_ACK		1
#define REG_X1		3	/* X1 Y1 X2 Y2 big-endian 16-bit, then fingers */
#define BURST_LEN	9
#define REG_PROF	0x13	/* per-electrode deltas: 27 columns, then 15 rows */
#define PROF_NX		27
#define PROF_NY		15
#define READ_LEN	(REG_PROF + PROF_NX + PROF_NY - REG_X1)
#define PITCH4_X	626	/* 4 x raw units per column, fitted (measured) */
#define PITCH4_Y	1155	/* 4 x raw units per row, fitted (measured) */
#define PEAK_MIN	20	/* a finger's peak is ~60..150; noise stays < 20 */
#define AMP_W		150	/* weight of a peak-height mismatch, per %^2 */
#define DR_MAX		48	/* scans (~3 s) a merged axis may coast on velocity */
#define MODE_RUN	0x08
#define COORD_NONE	0xffff
#define COORD_MASK	0x0fff
#define COORD_MAX	4095
#define POLL_MS		10
#define MAX_FINGERS	2
#define REGDUMP_MAX	255
#define REGDUMP_ROW	32	/* %*ph prints at most 64 bytes */

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "log every burst that changes, as raw bytes, and the pairing chosen");

static bool regdump;
module_param(regdump, bool, 0644);
MODULE_PARM_DESC(regdump, "also log registers 0..regdump_len-1 whenever they change");

static int regdump_len = 32;
module_param(regdump_len, int, 0644);
MODULE_PARM_DESC(regdump_len, "registers regdump reads, 1..255");

static bool reset_after_mt = true;
module_param(reset_after_mt, bool, 0644);
MODULE_PARM_DESC(reset_after_mt, "reset the part after each two-finger touch");

struct cy8_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset;
	char phys[32];
};

static int cy8_reset(struct cy8_ts *ts);

static int cy8_dist2(int ax, int ay, int bx, int by)
{
	return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

/*
 * Registers 0x13.. hold one signal byte per electrode, 27 columns then 15
 * rows (mapped against a single finger's X1/Y1, measured): the part senses
 * each axis as a profile, and a finger is a peak on each. Up to two peaks
 * of one profile, the two tallest, in position order: pos at the 3-point
 * centroid in raw units, amp the peak's height. Returns how many.
 */
static int cy8_peaks(const u8 *p, int n, int pitch4, int *pos, int *amp)
{
	int i, j, found = 0;

	for (i = 0; i < n; i++) {
		int l = i ? p[i - 1] : 0, r = i + 1 < n ? p[i + 1] : 0;
		int at;

		if (p[i] < PEAK_MIN || p[i] < l || p[i] <= r)
			continue;
		at = pitch4 * ((i - 1) * l + i * p[i] + (i + 1) * r) /
		     (4 * (l + p[i] + r));
		if (found < 2) {
			j = found++;
		} else {
			j = amp[0] < amp[1] ? 0 : 1;
			if (p[i] <= amp[j])
				continue;
		}
		pos[j] = clamp(at, 0, COORD_MAX);
		amp[j] = p[i];
	}
	if (found == 2 && pos[0] > pos[1]) {
		swap(pos[0], pos[1]);
		swap(amp[0], amp[1]);
	}
	return found;
}

/*
 * One axis of a two-finger frame: v[0] <= v[1], and each end's peak height.
 * The part's own value is used where a profile peak confirms it (below);
 * X2 is the unresolved marker through most two-finger touches (measured),
 * and the profile fills it. One peak after last scan's ends (prev,
 * prev[0] < 0 if none) were 5+ electrodes apart is a lifting finger whose
 * peak faded first, and its end holds last scan's value; from closer it
 * is a merge (at 3 apart, a crossing at speed, measured). *near: the ends
 * are closer than 2.5 electrodes, where the two centroids pull together
 * and say little about which finger is which. False if neither source has
 * the axis.
 */
static bool cy8_axis(const u8 *prof, int n, int pitch4, int clo, int chi,
		     const int *prev, int *v, int *a, bool *near)
{
	int pos[2], amp[2];
	int found = cy8_peaks(prof, n, pitch4, pos, amp);
	int near_raw = pitch4 * 5 / 8;
	bool lo_ok = !(clo & ~COORD_MASK), hi_ok = !(chi & ~COORD_MASK);

	if (found == 1) {
		pos[1] = pos[0];
		amp[1] = amp[0];
		if (prev[0] >= 0 && prev[1] - prev[0] >= pitch4 * 5 / 4) {
			int gone = abs(pos[0] - prev[0]) <= abs(pos[0] - prev[1]);

			pos[gone] = prev[gone];
		}
	} else if (!found) {
		if (!lo_ok || !hi_ok)
			return false;
		amp[0] = amp[1] = 1;
	}
	/*
	 * A resolved-looking value can still be stale: Y2 held one reading
	 * for 2 s while both fingers moved (measured). Trust the part only
	 * where a profile peak agrees with it to within one electrode.
	 */
	if (found && lo_ok && abs(clo - pos[0]) > pitch4 / 4)
		lo_ok = false;
	if (found && hi_ok && abs(chi - pos[1]) > pitch4 / 4)
		hi_ok = false;
	v[0] = lo_ok ? clo : pos[0];
	v[1] = hi_ok ? chi : pos[1];
	a[0] = amp[0];
	a[1] = amp[1];
	if (v[0] > v[1]) {
		swap(v[0], v[1]);
		swap(a[0], a[1]);
	}
	*near = v[1] - v[0] < near_raw;
	return true;
}

/*
 * The part reports two fingers as the corners of their bounding box, so a
 * finger pair is either (x0,y0)+(x1,y1) or (x0,y1)+(x1,y0), in either slot
 * order. Score each of the four by distance from where each slot is
 * predicted to be, plus how badly it pairs peak heights: a firmer or wider
 * finger is taller on both axes (measured, thumb against fingertip), so the
 * taller X peak belongs with the taller Y peak. Tracking dominates while the
 * fingers are apart; height decides at a landing and at a crossing, where
 * the distances tie. Two equal fingers crossing is still a coin toss.
 */
static int cy8_pair(int *x, int *y, const int *ax, const int *ay,
		    const int *px, const int *py, const bool *pv)
{
	static const int pick[4][4] = {	/* slot0 x,y ; slot1 x,y (indices) */
		{ 0, 0, 1, 1 }, { 1, 1, 0, 0 }, { 0, 1, 1, 0 }, { 1, 0, 0, 1 },
	};
	int bx[2] = { x[0], x[1] }, by[2] = { y[0], y[1] };
	int fx = 100 * ax[0] / (ax[0] + ax[1]);
	int fy = 100 * ay[0] / (ay[0] + ay[1]);
	int k, best = 0, best_cost = INT_MAX;

	for (k = 0; k < 4; k++) {
		int m = k < 2 ? fx - fy : fx - (100 - fy);
		int cost = AMP_W * m * m;

		if (pv[0])
			cost += cy8_dist2(bx[pick[k][0]], by[pick[k][1]], px[0], py[0]);
		if (pv[1])
			cost += cy8_dist2(bx[pick[k][2]], by[pick[k][3]], px[1], py[1]);
		if (cost < best_cost) {
			best_cost = cost;
			best = k;
		}
	}
	x[0] = bx[pick[best][0]];
	y[0] = by[pick[best][1]];
	x[1] = bx[pick[best][2]];
	y[1] = by[pick[best][3]];
	return best;
}

static int cy8_read(struct cy8_ts *ts, u8 reg, u8 *buf, u8 len)
{
	struct i2c_msg msg[2] = {
		{ .addr = ts->client->addr, .len = 1, .buf = &reg },
		{ .addr = ts->client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret = i2c_transfer(ts->client->adapter, msg, 2);

	if (ret == 2)
		return 0;
	return ret < 0 ? ret : -EIO;
}

static int cy8_write(struct cy8_ts *ts, u8 reg, u8 val)
{
	u8 w[2] = { reg, val };
	int ret = i2c_master_send(ts->client, w, sizeof(w));

	if (ret == sizeof(w))
		return 0;
	return ret < 0 ? ret : -EIO;
}

/* Poll until the finger lifts; the IRQ stays masked (ONESHOT) meanwhile. */
static irqreturn_t cy8_irq_thread(int irq, void *dev_id)
{
	struct cy8_ts *ts = dev_id;
	struct input_dev *input = ts->input;
	bool down = false, two = false;
	u8 b[READ_LEN], prev[READ_LEN] = { 0 }, last[BURST_LEN] = { 0 };
	u8 regs[REGDUMP_MAX], last_regs[REGDUMP_MAX] = { 0 };
	int rlen = clamp(regdump_len, 1, REGDUMP_MAX);
	int held_x[MAX_FINGERS] = { -1, -1 }, held_y[MAX_FINGERS] = { -1, -1 };
	int px[MAX_FINGERS] = { 0 }, py[MAX_FINGERS] = { 0 };
	bool pv[MAX_FINGERS] = { false, false };
	int vx[MAX_FINGERS] = { 0 }, vy[MAX_FINGERS] = { 0 };
	int ex[2] = { -1, -1 }, ey[2] = { -1, -1 };	/* last scan's axis ends */
	int i, k = -1, passing = 0, drx = 0, dry = 0;
	bool changed;

	for (;;) {
		int x[MAX_FINGERS], y[MAX_FINGERS];
		bool on[MAX_FINGERS], lx = false, ly = false;

		if (cy8_read(ts, REG_X1, b, sizeof(b))) {
			dev_err_ratelimited(&ts->client->dev, "burst read failed\n");
			break;
		}
		/*
		 * Every change, unthrottled: a rate limit drops the poses. A
		 * two-finger burst is logged after pairing, on the same line as
		 * the pairing chosen, so one line is one frame.
		 */
		changed = debug && memcmp(b, last, sizeof(last));
		if (changed && b[8] < 2)
			dev_info(&ts->client->dev, "%*ph\n", BURST_LEN, b);
		memcpy(last, b, sizeof(last));
		if (regdump && !cy8_read(ts, REG_MODE, regs, rlen) &&
		    memcmp(regs, last_regs, rlen)) {
			for (i = 0; i < rlen; i += REGDUMP_ROW)
				dev_info(&ts->client->dev, "regs %02x: %*ph\n", i,
					 min(REGDUMP_ROW, rlen - i), regs + i);
			memcpy(last_regs, regs, rlen);
		}
		/*
		 * Count 0 with a real X1 is not a lift: the part sends one such
		 * burst whenever a finger joins or leaves a touch (measured). A
		 * lift reads all 0xff. Skip the former, bounded in case it sticks.
		 */
		if (!b[8] && (b[0] != 0xff || b[1] != 0xff) && ++passing <= 10) {
			cy8_write(ts, REG_ACK, 0);
			msleep(POLL_MS);
			continue;
		}
		if (!b[8])
			break;
		passing = 0;
		cy8_write(ts, REG_ACK, 0);
		if (b[8] >= 2)
			two = true;
		/*
		 * The part rewrites the burst and the profiles every 60 ms each,
		 * 30 ms out of phase (measured), several polls apart. A poll that
		 * brings nothing new to the source this frame is built from is
		 * not a scan: counted as one it halves the velocity toward zero.
		 */
		if (b[8] >= 2 ? !memcmp(b + BURST_LEN, prev + BURST_LEN,
					READ_LEN - BURST_LEN) :
				!memcmp(b, prev, BURST_LEN)) {
			msleep(POLL_MS);
			continue;
		}
		memcpy(prev, b, sizeof(b));

		for (i = 0; i < MAX_FINGERS; i++) {
			x[i] = (b[4 * i] << 8) | b[4 * i + 1];
			y[i] = (b[4 * i + 2] << 8) | b[4 * i + 3];
			on[i] = i < b[8] && x[i] != COORD_NONE && y[i] != COORD_NONE;
			/*
			 * A coordinate with bits above 0x0fff set is unresolved, not
			 * large: X2 reads exactly 0x408e for seconds at a time while
			 * a second finger is down (measured). Hold the slot's last
			 * value; a slot with none yet is not reported until it has.
			 */
			if (on[i] && (x[i] & ~COORD_MASK))
				x[i] = held_x[i] >= 0 ? held_x[i] : COORD_MAX;
			if (on[i] && (y[i] & ~COORD_MASK))
				y[i] = held_y[i];
			if (x[i] < 0 || y[i] < 0)
				on[i] = false;
			held_x[i] = on[i] ? x[i] : -1;
			held_y[i] = on[i] ? y[i] : -1;
		}
		if (b[8] >= 2) {
			const u8 *prof = b + REG_PROF - REG_X1;
			int ax[2], ay[2], qx[MAX_FINGERS], qy[MAX_FINGERS];

			on[0] = on[1] =
				cy8_axis(prof, PROF_NX, PITCH4_X, (b[0] << 8) | b[1],
					 (b[4] << 8) | b[5], ex, x, ax, &lx) &&
				cy8_axis(prof + PROF_NX, PROF_NY, PITCH4_Y,
					 (b[2] << 8) | b[3], (b[6] << 8) | b[7],
					 ey, y, ay, &ly);
			if (on[0]) {
				ex[0] = x[0];
				ex[1] = x[1];
				ey[0] = y[0];
				ey[1] = y[1];
			}
			/* Heights from peaks that overlap are not a finger's. */
			if (lx || ly)
				ax[0] = ax[1] = ay[0] = ay[1] = 1;
			for (i = 0; i < MAX_FINGERS; i++) {
				qx[i] = px[i] + vx[i];
				qy[i] = py[i] + vy[i];
			}
			k = on[0] ? cy8_pair(x, y, ax, ay, qx, qy, pv) : -1;
			/* Neither source has an axis: hold the last frame. */
			for (i = 0; k < 0 && i < MAX_FINGERS; i++) {
				x[i] = px[i];
				y[i] = py[i];
				on[i] = pv[i];
			}
		} else if (on[0] && pv[1] &&
			   (!pv[0] || cy8_dist2(x[0], y[0], px[1], py[1]) <
				      cy8_dist2(x[0], y[0], px[0], py[0]))) {
			/* One finger down, and it is slot 1's: keep it there. */
			x[1] = x[0];
			y[1] = y[0];
			on[1] = true;
			on[0] = false;
		}
		if (b[8] < 2)
			ex[0] = ey[0] = -1;
		if (changed && b[8] >= 2)
			dev_info(&ts->client->dev,
				 "%*ph k=%d%s%s s0=%d,%d s1=%d,%d v0=%d,%d v1=%d,%d\n",
				 BURST_LEN, b, k, lx ? " nx" : "", ly ? " ny" : "",
				 x[0], y[0], x[1], y[1],
				 vx[0], vy[0], vx[1], vy[1]);

		drx = lx ? drx + 1 : 0;
		dry = ly ? dry + 1 : 0;
		for (i = 0; i < MAX_FINGERS; i++) {
			input_mt_slot(input, i);
			input_mt_report_slot_state(input, MT_TOOL_FINGER, on[i]);
			if (on[i]) {
				input_report_abs(input, ABS_MT_POSITION_X, x[i]);
				input_report_abs(input, ABS_MT_POSITION_Y, y[i]);
			}
			/*
			 * Velocity, halved each scan: without it a pair crossing in
			 * one axis is a tie, the two corners sitting equally far
			 * either side of where the slot was. While that axis's ends
			 * are near (cy8_axis) they say little about which finger is
			 * where, so the prediction coasts on the velocity instead,
			 * for up to DR_MAX scans.
			 */
			if (on[i] && pv[i]) {
				if (lx && drx <= DR_MAX) {
					px[i] += vx[i];
				} else {
					vx[i] = (vx[i] + x[i] - px[i]) / 2;
					px[i] = x[i];
				}
				if (ly && dry <= DR_MAX) {
					py[i] += vy[i];
				} else {
					vy[i] = (vy[i] + y[i] - py[i]) / 2;
					py[i] = y[i];
				}
			} else {
				vx[i] = vy[i] = 0;
				px[i] = x[i];
				py[i] = y[i];
			}
			pv[i] = on[i];
		}
		/* The legacy pointer follows slot 0, or slot 1 left alone. */
		i = on[0] ? 0 : 1;
		if (on[i]) {
			input_report_abs(input, ABS_X, x[i]);
			input_report_abs(input, ABS_Y, y[i]);
			input_report_key(input, BTN_TOUCH, 1);
			down = true;
		}
		input_sync(input);
		msleep(POLL_MS);
	}

	cy8_write(ts, REG_MODE, MODE_RUN);
	cy8_write(ts, REG_ACK, 0);
	for (i = 0; i < MAX_FINGERS; i++) {
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}
	if (down)
		input_report_key(input, BTN_TOUCH, 0);
	input_sync(input);
	/*
	 * After a two-finger touch ends, every later one reads X2 as the
	 * unresolved marker until the part is reset; single touches do not
	 * cause this (measured, two rounds). Costs ~120 ms blind after lift.
	 */
	if (two && reset_after_mt && cy8_reset(ts) >= 0)
		cy8_write(ts, REG_MODE, MODE_RUN);
	return IRQ_HANDLED;
}

/*
 * Reset pulse, reg0 = 0, then clear reg1 until it reads 0. Returns the
 * number of reads that took, or a negative errno; reg0 = MODE_RUN is the
 * caller's.
 */
static int cy8_reset(struct cy8_ts *ts)
{
	u8 ack = 0xff;
	int err = 0, i;

	if (ts->reset) {
		gpiod_set_value_cansleep(ts->reset, 1);
		msleep(20);
		gpiod_set_value_cansleep(ts->reset, 0);
		msleep(100);
	}
	cy8_write(ts, REG_MODE, 0);
	for (i = 0; i < 10; i++) {
		err = cy8_read(ts, REG_ACK, &ack, 1);
		if (!err && !ack)
			break;
		cy8_write(ts, REG_ACK, 0);
	}
	return err ? err : i + 1;
}

static int cy8_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct cy8_ts *ts;
	int err, reads;

	if (client->irq <= 0) {
		dev_err(dev, "no IRQ in the DT node\n");
		return -EINVAL;
	}

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;

	/* reset-gpios is active-high; pulse it so a reload resets the part too. */
	ts->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset))
		return PTR_ERR(ts->reset);

	reads = err = cy8_reset(ts);
	if (err < 0) {
		dev_err(dev, "controller does not answer: %d\n", err);
		return -ENXIO;
	}

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	snprintf(ts->phys, sizeof(ts->phys), "%s/input0", dev_name(dev));
	ts->input->name = DRIVER_NAME;
	ts->input->phys = ts->phys;
	ts->input->id.bustype = BUS_I2C;

	input_set_capability(ts->input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(ts->input, ABS_X, 0, COORD_MAX, 0, 0);
	input_set_abs_params(ts->input, ABS_Y, 0, COORD_MAX, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, COORD_MAX, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, COORD_MAX, 0, 0);
	err = input_mt_init_slots(ts->input, MAX_FINGERS, INPUT_MT_DIRECT);
	if (err)
		return err;

	err = input_register_device(ts->input);
	if (err)
		return err;

	err = devm_request_threaded_irq(dev, client->irq, NULL, cy8_irq_thread,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					DRIVER_NAME, ts);
	if (err) {
		dev_err(dev, "request irq %d: %d\n", client->irq, err);
		return err;
	}

	cy8_write(ts, REG_MODE, MODE_RUN);
	i2c_set_clientdata(client, ts);
	dev_info(dev, "irq %d, reset %s, reg1 cleared after %d reads\n",
		 client->irq, ts->reset ? "gpio" : "none", reads);
	return 0;
}

static const struct i2c_device_id cy8_idtable[] = {
	{ DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cy8_idtable);

static const struct of_device_id cy8_of_match[] = {
	{ .compatible = "panjit_ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, cy8_of_match);

static struct i2c_driver cy8_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = cy8_of_match,
	},
	.id_table = cy8_idtable,
	.probe = cy8_probe,
};
module_i2c_driver(cy8_driver);

MODULE_DESCRIPTION("CY8CTMG120 touchscreen on the Arca");
MODULE_LICENSE("GPL v2");
