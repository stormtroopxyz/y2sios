/*
 * The touchscreen of the Galaxy S20+ (y2s, Exynos 990): a Samsung System LSI
 * touch controller (S6SY79A, Samsung's "sec_ts" driver, firmware
 * "tsp_sec/y79a_y2.bin") at I2C address 0x48 on the pins of HSI2C_15.
 *
 * It is NOT an STMicroelectronics FingerTipS part: the stock kernel runs
 * drivers/input/touchscreen/sec_ts/y79a (Samsung's open-source Exynos 990
 * kernel), and the command set and the 16-byte event records below are that
 * driver's.
 *
 * Its two supplies come from the S2DOS05 display PMIC (I2C 0x60 on the pins of
 * HSI2C_4): LDO2 "tsp_io" (1.8 V) and LDO3 "tsp_avdd" (3.0 V). They are marked
 * regulator-boot-on, but the bootloader does not leave them on in our boot path:
 * with them off the controller clamps its bus low. So the rails are turned on
 * first.
 *
 * Both buses are bit-banged on their GPIOs. The HSI2C controllers are not used:
 * HSI2C_15 sits in reset with its clock gated in CMU_PERIC1, and the pinctrl
 * blocks are always on. Lines are driven open-drain (low = output 0,
 * high = input, the board's pull-ups raise it), so a slave may stretch the clock
 * and nothing ever fights a slave that holds a line low.
 *
 * From the phone's device tree (dtb.img + dtbo.img):
 *
 *   hsi2c_4  @ 0x10590000  pins gpp1-0 (SCL) / gpp1-1 (SDA)  PERIC0 pinctrl 0x10430000, bank +0x20
 *            s2dos05_pmic@60: ldo2 = tsp_io 1.8 V, ldo3 = tsp_avdd 3.0 V
 *   hsi2c_15 @ 0x108D0000  pins gpp5-6 (SCL) / gpp5-7 (SDA)  PERIC1 pinctrl 0x10730000, bank +0x00
 *            touchscreen@48: "sec,sec_ts", attention gpa1-0 (low = events pending)
 *   gpa1                   ALIVE pinctrl 0x15850000, bank +0x20
 *
 * Boot arguments (nothing is touched unless touch_ts is given):
 *
 *   touch_ts=1       power, probe, then poll and post touches (redwood_touch_post)
 *   touch_ts=2       power and probe, then panic with a one-line summary of what
 *                    was found (the panic is what saves the log on this phone)
 *   touch_ts=3       power and probe only, no polling, no panic
 *   touch_pwr=N      0 leave the PMIC alone, 1 turn the two rails on (default),
 *                    2 power-cycle them first (forces a fresh controller boot)
 *   touch_attn=0     ignore the attention pin and read the event stack every poll
 *   touch_pullup=1   also enable the SoC's internal pull-ups on the bus pins
 *   touch_poll_us=N  how often the attention pin is looked at (default 8000 us)
 *   touch_flip=N     bit 0 mirrors X, bit 1 mirrors Y
 *   touch_trace=1    print every raw event
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

/* ------------------------------------------------------------------------ */
/* GPIO (Exynos 990 pinctrl, bsd/sys/redwood_bbi2c.h)                        */

#define BANK_GPP1           0x20U       /* PERIC0 */
#define BANK_GPP5           0x00U       /* PERIC1 */
#define BANK_GPA1           0x20U       /* ALIVE  */
#define ATTN_PIN            0U          /* gpa1-0 */

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* ------------------------------------------------------------------------ */
/* What was found, kept for the panic message (the only reliable log here)   */

static char summary[1024];

static void note(const char *format, ...) __printflike(1, 2);

static void
note(const char *format, ...)
{
	char line[192];
	va_list ap;
	size_t used;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("sec_ts: %s\n", line);

	used = strlen(summary);
	if (used + 4 < sizeof(summary)) {
		snprintf(summary + used, sizeof(summary) - used, "%s%s", used ? " | " : "", line);
	}
}

/* ------------------------------------------------------------------------ */
/* Bit-banged, open-drain I2C (bsd/dev/redwood_bbi2c.c)                       */

static bool
bus_setup(struct redwood_bbi2c *b, bool pullup)
{
	char line[192];
	bool ok = redwood_bbi2c_setup(b, pullup, line, sizeof(line));

	note("%s", line);
	return ok;
}

static bool
i2c_xfer(struct redwood_bbi2c *b, uint8_t addr, const uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen)
{
	return redwood_bbi2c_xfer(b, addr, wbuf, wlen, rbuf, rlen);
}

/* ------------------------------------------------------------------------ */
/* The S2DOS05 display PMIC: the touch controller's two rails                */
/* (Samsung drivers/regulator/s2dos05.c, include/linux/regulator/s2dos05.h)  */

#define S2DOS05_ADDR          0x60U
#define S2DOS05_REG_DEV_ID    0x00U
#define S2DOS05_REG_EN        0x03U
#define S2DOS05_REG_LDO2_CFG  0x05U
#define S2DOS05_REG_LDO3_CFG  0x06U
#define S2DOS05_REG_ID_PGM    0x61U
#define S2DOS05_EN_LDO2       (1U << 1)  /* tsp_io   */
#define S2DOS05_EN_LDO3       (1U << 2)  /* tsp_avdd */
#define S2DOS05_VSEL_MASK     0x7fU
#define S2DOS05_LDO2_1V8      0x0cU      /* 1.5 V + 12 x 25 mV */
#define S2DOS05_LDO3_3V0      0x0cU      /* 2.7 V + 12 x 25 mV */

static struct redwood_bbi2c pmic_bus = { .name = "pmic bus gpp1-0/1", .scl = 0, .sda = 1, .half_us = 4 };

static bool
pmic_rd(uint8_t reg, uint8_t *value)
{
	return i2c_xfer(&pmic_bus, S2DOS05_ADDR, &reg, 1, value, 1);
}

static bool
pmic_wr(uint8_t reg, uint8_t value)
{
	uint8_t buf[2] = { reg, value };

	return i2c_xfer(&pmic_bus, S2DOS05_ADDR, buf, 2, NULL, 0);
}

/* Only bits are ever set or cleared in EN: its other rails feed the display. */
static bool
pmic_en_update(uint8_t set, uint8_t clear)
{
	uint8_t en;

	if (!pmic_rd(S2DOS05_REG_EN, &en)) {
		return false;
	}
	return pmic_wr(S2DOS05_REG_EN, (uint8_t)((en | set) & ~clear));
}

static bool
touch_power(uint32_t how, bool pullup)
{
	uint8_t id = 0, pgm = 0, en = 0, l2 = 0, l3 = 0;
	bool ok;

	pmic_bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP1);
	if (!bus_setup(&pmic_bus, pullup)) {
		note("pmic bus is not usable");
		redwood_bbi2c_restore(&pmic_bus);
		return false;
	}

	ok = pmic_rd(S2DOS05_REG_DEV_ID, &id) && pmic_rd(S2DOS05_REG_ID_PGM, &pgm) &&
	    pmic_rd(S2DOS05_REG_EN, &en) && pmic_rd(S2DOS05_REG_LDO2_CFG, &l2) &&
	    pmic_rd(S2DOS05_REG_LDO3_CFG, &l3);
	note("s2dos05: %s, id %#x pgm %#x EN %#x LDO2 %#x LDO3 %#x",
	    ok ? "answers" : "NO ANSWER", id, pgm, en, l2, l3);
	if (!ok) {
		redwood_bbi2c_restore(&pmic_bus);
		return false;
	}

	if (how == 2 && (en & (S2DOS05_EN_LDO2 | S2DOS05_EN_LDO3))) {
		/* off in the reverse order of sec_ts_power(), so the controller boots afresh */
		pmic_en_update(0, S2DOS05_EN_LDO3);
		sleep_us(4000);
		pmic_en_update(0, S2DOS05_EN_LDO2);
		sleep_us(30000);
		pmic_rd(S2DOS05_REG_EN, &en);
	}

	/* A rail's voltage is only ever set while it is off. */
	if (!(en & S2DOS05_EN_LDO2) && (l2 & S2DOS05_VSEL_MASK) != S2DOS05_LDO2_1V8) {
		pmic_wr(S2DOS05_REG_LDO2_CFG, (uint8_t)((l2 & ~S2DOS05_VSEL_MASK) | S2DOS05_LDO2_1V8));
	}
	if (!(en & S2DOS05_EN_LDO3) && (l3 & S2DOS05_VSEL_MASK) != S2DOS05_LDO3_3V0) {
		pmic_wr(S2DOS05_REG_LDO3_CFG, (uint8_t)((l3 & ~S2DOS05_VSEL_MASK) | S2DOS05_LDO3_3V0));
	}

	/* tsp_io first, then tsp_avdd 1 ms later (sec_ts_power()) */
	if (!(en & S2DOS05_EN_LDO2)) {
		pmic_en_update(S2DOS05_EN_LDO2, 0);
		sleep_us(1000);
	}
	if (!(en & S2DOS05_EN_LDO3)) {
		pmic_en_update(S2DOS05_EN_LDO3, 0);
	}

	pmic_rd(S2DOS05_REG_EN, &en);
	pmic_rd(S2DOS05_REG_LDO2_CFG, &l2);
	pmic_rd(S2DOS05_REG_LDO3_CFG, &l3);
	ok = (en & (S2DOS05_EN_LDO2 | S2DOS05_EN_LDO3)) == (S2DOS05_EN_LDO2 | S2DOS05_EN_LDO3);
	note("s2dos05: now EN %#x LDO2 %#x LDO3 %#x: rails %s", en, l2, l3, ok ? "on" : "NOT ON");
	redwood_bbi2c_restore(&pmic_bus);
	return ok;
}

/* ------------------------------------------------------------------------ */
/* The controller (Samsung drivers/input/touchscreen/sec_ts/y79a/sec_ts.h)   */

#define TS_ADDR                   0x48U
#define TS_CMD_SENSE_ON           0x10U
#define TS_READ_FW_INTEGRITY      0x21U
#define TS_READ_DEVICE_ID         0x22U
#define TS_READ_PANEL_INFO        0x23U
#define TS_CMD_SET_TOUCHFUNCTION  0x30U
#define TS_READ_ID                0x52U
#define TS_READ_BOOT_STATUS       0x55U
#define TS_READ_ONE_EVENT         0x60U
#define TS_READ_ALL_EVENT         0x61U
#define TS_CMD_CLEAR_EVENT_STACK  0x62U
#define TS_READ_FW_VERSION        0xa3U
#define TS_READ_TS_STATUS         0xafU

#define TS_EVENT_SIZE             16U
#define TS_MAX_EVENTS             32U

/* byte 0: eid in bits 1:0, tid / status type in bits 5:2, touch state in bits 7:6 */
#define TS_EID_COORDINATE         0U
#define TS_EID_STATUS             1U
#define TS_EID_EMPTY              3U

#define TS_STYPE_ERR              1U
#define TS_STYPE_INFO             2U
#define TS_STYPE_VENDOR_INFO      7U
#define TS_ACK_BOOT_COMPLETE      0x00U

#define TS_STATUS_BOOT_MODE       0x10U
#define TS_STATUS_APP_MODE        0x20U

#define TS_ACTION_PRESS           1U
#define TS_ACTION_MOVE            2U
#define TS_ACTION_RELEASE         3U

#define TS_TOUCHTYPE_NORMAL       0U
#define TS_TOUCHTYPE_GLOVE        3U
#define TS_TOUCHTYPE_PALM         5U
#define TS_TOUCHTYPE_WET          6U

/* SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC: touch | palm | wet, a little-endian u16 */
#define TS_DEFAULT_FUNCTIONS      0x0061U

#define TS_MAX_FINGERS            10U

static struct redwood_bbi2c ts_bus = { .name = "touch bus gpp5-6/7", .scl = 6, .sda = 7, .half_us = 2 };
static volatile uint8_t *attn_bank;
static uint32_t use_attn = 1;
static uint32_t touch_trace;
static uint32_t flip;
static uint32_t max_x = 4095, max_y = 4095;  /* sec,max_coords; replaced by the panel info */

static struct {
	bool     down;
	uint32_t x, y;
} finger[TS_MAX_FINGERS];

static bool
ts_read(uint8_t cmd, uint8_t *buf, uint32_t len)
{
	return i2c_xfer(&ts_bus, TS_ADDR, &cmd, 1, buf, len);
}

static bool
ts_write(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	uint8_t buf[8];
	uint32_t i;

	if (len + 1 > sizeof(buf)) {
		return false;
	}
	buf[0] = cmd;
	for (i = 0; i < len; i++) {
		buf[i + 1] = data[i];
	}
	return i2c_xfer(&ts_bus, TS_ADDR, buf, len + 1, NULL, 0);
}

/* The controller pulls its attention line low while it has events queued. */
static bool
attn_pending(void)
{
	return !use_attn || redwood_gpio_get(attn_bank, ATTN_PIN) == 0;
}

static uint32_t
scale(uint32_t v, uint32_t max, uint32_t axis)
{
	uint32_t out = v >= max ? 65535 : v * 65535 / max;

	if (flip & (axis == 0 ? 1 : 2)) {
		out = 65535 - out;
	}
	return out;
}

static void
release_all(void)
{
	uint32_t i;

	for (i = 0; i < TS_MAX_FINGERS; i++) {
		if (finger[i].down) {
			finger[i].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, i, finger[i].x, finger[i].y);
		}
	}
}

static bool
ts_start_sensing(void)
{
	uint8_t functions[2] = { TS_DEFAULT_FUNCTIONS & 0xff, TS_DEFAULT_FUNCTIONS >> 8 };
	bool ok;

	ok = ts_write(TS_CMD_SET_TOUCHFUNCTION, functions, sizeof(functions));
	return ts_write(TS_CMD_SENSE_ON, NULL, 0) && ok;
}

static void
handle_event(const uint8_t *e)
{
	uint32_t eid = e[0] & 0x3;
	uint32_t kind = (e[0] >> 2) & 0xf;
	uint32_t slot, action, ttype, x, y;

	if (touch_trace) {
		printf("sec_ts: event %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
	}

	if (eid == TS_EID_STATUS) {
		/* a watchdog reset: the controller rebooted and must be told to sense again */
		if (kind == TS_STYPE_INFO && e[1] == TS_ACK_BOOT_COMPLETE && e[2] == 0x20) {
			printf("sec_ts: the controller reset itself, sensing again\n");
			release_all();
			ts_start_sensing();
		} else if (kind == TS_STYPE_ERR) {
			printf("sec_ts: error status %02x %02x %02x %02x\n", e[1], e[2], e[3], e[4]);
			release_all();
		}
		return;
	}
	if (eid != TS_EID_COORDINATE || kind == 0) {
		return;
	}

	slot = kind - 1;
	if (slot >= TS_MAX_FINGERS) {
		return;
	}
	action = e[0] >> 6;
	ttype = ((uint32_t)(e[6] >> 6) << 2) | (e[7] >> 6);
	if (ttype != TS_TOUCHTYPE_NORMAL && ttype != TS_TOUCHTYPE_GLOVE &&
	    ttype != TS_TOUCHTYPE_PALM && ttype != TS_TOUCHTYPE_WET) {
		return;
	}
	x = scale(((uint32_t)e[1] << 4) | (e[3] >> 4), max_x, 0);
	y = scale(((uint32_t)e[2] << 4) | (e[3] & 0xf), max_y, 1);

	switch (action) {
	case TS_ACTION_PRESS:
	case TS_ACTION_MOVE:
		if (!finger[slot].down) {
			finger[slot].down = true;
			finger[slot].x = x;
			finger[slot].y = y;
			redwood_touch_post(REDWOOD_TOUCH_DOWN, slot, x, y);
		} else if (finger[slot].x != x || finger[slot].y != y) {
			finger[slot].x = x;
			finger[slot].y = y;
			redwood_touch_post(REDWOOD_TOUCH_MOVE, slot, x, y);
		}
		break;
	case TS_ACTION_RELEASE:
		if (finger[slot].down) {
			finger[slot].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, slot, finger[slot].x, finger[slot].y);
		}
		break;
	default:
		break;
	}
}

/* sec_ts_read_event(): one event, which says how many more to read at once. */
static uint32_t
read_events(void)
{
	static uint8_t rest[TS_EVENT_SIZE * (TS_MAX_EVENTS - 1)];
	uint8_t first[TS_EVENT_SIZE];
	uint32_t left, i;

	if (!ts_read(TS_READ_ONE_EVENT, first, sizeof(first)) || first[0] == 0) {
		return 0;
	}
	if ((first[0] & 0x3) == TS_EID_EMPTY) {
		ts_write(TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
		release_all();
		return 0;
	}
	left = first[7] & 0x1f;
	if (left > TS_MAX_EVENTS - 1) {
		ts_write(TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
		release_all();
		return 0;
	}
	if (left != 0 && !ts_read(TS_READ_ALL_EVENT, rest, left * TS_EVENT_SIZE)) {
		left = 0;
	}
	handle_event(first);
	for (i = 0; i < left; i++) {
		handle_event(rest + i * TS_EVENT_SIZE);
	}
	return left + 1;
}

/* sec_ts_wait_for_ready(SEC_TS_ACK_BOOT_COMPLETE): up to 2 s. */
static bool
wait_for_boot(uint8_t last[TS_EVENT_SIZE])
{
	uint32_t i, kind;

	for (i = 0; i <= 100; i++) {
		if (attn_pending() && ts_read(TS_READ_ONE_EVENT, last, TS_EVENT_SIZE)) {
			kind = (last[0] >> 2) & 0xf;
			if ((kind == TS_STYPE_INFO || kind == TS_STYPE_VENDOR_INFO) &&
			    last[1] == TS_ACK_BOOT_COMPLETE) {
				return true;
			}
		}
		sleep_us(20000);
	}
	return false;
}

static bool
ts_probe(void)
{
	uint8_t ev[TS_EVENT_SIZE] = { 0 }, devid[5] = { 0 }, integrity = 0, boot = 0;
	uint8_t status[4] = { 0 }, id[3] = { 0 }, panel[11] = { 0 }, fw[4] = { 0 };
	uint32_t px, py;
	bool ready, ok;

	ready = wait_for_boot(ev);
	note("boot event %s: %02x %02x %02x %02x, attn %u",
	    ready ? "seen" : "NOT seen", ev[0], ev[1], ev[2], ev[3], redwood_gpio_get(attn_bank, ATTN_PIN));

	ok = ts_read(TS_READ_DEVICE_ID, devid, sizeof(devid));
	ok = ts_read(TS_READ_FW_INTEGRITY, &integrity, 1) && ok;
	ok = ts_read(TS_READ_BOOT_STATUS, &boot, 1) && ok;
	ok = ts_read(TS_READ_TS_STATUS, status, sizeof(status)) && ok;
	ok = ts_read(TS_READ_ID, id, sizeof(id)) && ok;
	ok = ts_read(TS_READ_FW_VERSION, fw, sizeof(fw)) && ok;
	ok = ts_read(TS_READ_PANEL_INFO, panel, sizeof(panel)) && ok;
	note("%s: dev %02x%02x%02x%02x%02x id %02x%02x%02x fw %02x.%02x.%02x.%02x integrity %#x boot %#x status %02x %02x %02x %02x",
	    ok ? "answers" : "NO ANSWER", devid[0], devid[1], devid[2], devid[3], devid[4], id[0], id[1], id[2],
	    fw[0], fw[1], fw[2], fw[3], integrity, boot, status[0], status[1], status[2], status[3]);
	if (!ok) {
		return false;
	}

	px = ((uint32_t)panel[0] << 8) | panel[1];
	py = ((uint32_t)panel[2] << 8) | panel[3];
	if (px > 1 && py > 1) {
		max_x = px - 1;
		max_y = py - 1;
	}
	note("panel %ux%u (display %ux%u, %u tx x %u rx)", max_x + 1, max_y + 1,
	    ((uint32_t)panel[4] << 8) | panel[5], ((uint32_t)panel[6] << 8) | panel[7], panel[8], panel[9]);

	if (boot == TS_STATUS_BOOT_MODE || !(integrity & 0x80)) {
		note("the controller is in its bootloader / has no valid firmware: not sensing");
		return false;
	}
	ok = ts_start_sensing();
	note("sense on -> %d", ok);
	return ok;
}

/* ------------------------------------------------------------------------ */

static void
sec_ts_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 8000, mode = 0, pwr = 1, pullup = 0;
	uint64_t reads = 0, events = 0;

	PE_parse_boot_argn("touch_ts", &mode, sizeof(mode));
	PE_parse_boot_argn("touch_pwr", &pwr, sizeof(pwr));
	PE_parse_boot_argn("touch_attn", &use_attn, sizeof(use_attn));
	PE_parse_boot_argn("touch_pullup", &pullup, sizeof(pullup));
	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 1000 || poll_us > 100000) {
		poll_us = 8000;
	}

	attn_bank = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, BANK_GPA1);
	ts_bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, BANK_GPP5);

	/* The attention line as a plain input; the board pulls it up to tsp_io. */
	redwood_gpio_func(attn_bank, ATTN_PIN, REDWOOD_GPIO_INPUT);
	redwood_gpio_pull(attn_bank, ATTN_PIN, REDWOOD_GPIO_PULL_NONE);

	/*
	 * A few tries: a bit-banged transfer that is held up (by whatever else the boot is doing on the
	 * other cores) can go unanswered once -- seen 24 Oct, the S2DOS05 silent for one boot. Only in
	 * the diagnostic mode (touch_ts=2) does a failure end the run with the summary.
	 */
	const char *failed = NULL;

	for (unsigned attempt = 1; attempt <= 5; attempt++) {
		failed = NULL;
		if (pwr != 0 && !touch_power(pwr, pullup != 0)) {
			failed = "could not power the touch controller";
		} else {
			sleep_us(100000);   /* TOUCH_POWER_ON_DWORK_TIME */
			if (!bus_setup(&ts_bus, pullup != 0)) {
				failed = "the touch bus is held low (no power?)";
			} else if (!ts_probe()) {
				failed = "probe failed";
			}
		}
		if (failed == NULL) {
			break;
		}
		printf("sec_ts: attempt %u: %s\n", attempt, failed);
		summary[0] = 0;
		sleep_us(200000);
	}
	if (failed != NULL) {
		if (mode == 2) {
			panic("sec_ts: %s: %s", failed, summary);
		}
		printf("sec_ts: %s after 5 attempts: no touch screen this run\n", failed);
		return;
	}
	if (mode == 2) {
		panic("sec_ts: probe ok (touch_ts=2): %s", summary);
	}
	if (mode == 3) {
		return;
	}
	printf("sec_ts: polling every %u us (%s)\n", poll_us,
	    use_attn ? "when the attention line is low" : "always");

	for (;;) {
		if (attn_pending()) {
			uint32_t n = read_events();

			reads++;
			events += n;
			if (touch_trace && (reads % 500) == 0) {
				printf("sec_ts: %llu reads, %llu events, %u stretch timeouts\n",
				    reads, events, ts_bus.stretch_timeouts);
			}
		}
		sleep_us(poll_us);
	}
}

void
redwood_sec_ts_init(void)
{
	uint32_t mode = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touch_ts", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("touch_trace", &touch_trace, sizeof(touch_trace));
	if (kernel_thread_start(sec_ts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
