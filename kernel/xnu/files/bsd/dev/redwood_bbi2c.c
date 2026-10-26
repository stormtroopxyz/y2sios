/*
 * I2C bit-banged on two GPIOs of the Exynos 990, and shared access to its pinctrl banks
 * (bsd/sys/redwood_bbi2c.h).
 *
 * Open-drain: a line is pulled low by making its pin an output (its DAT bit is kept 0) and
 * let go by making it an input, so the board's pull-up raises it. Nothing is ever driven
 * high, a slave may hold SCL low for as long as it needs (clock stretching), and a slave that
 * holds SDA low is never fought.
 *
 * Several drivers use pins of the same bank (gpp1: the S2DOS05 on pins 0/1, the MAX77705 on
 * pins 6/7), so every read-modify-write of a bank register is done under one spin lock.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/redwood_bbi2c.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define PINCTRL_SIZE    0x1000U

static LCK_GRP_DECLARE(gpio_lck_grp, "redwood-gpio");
static LCK_SPIN_DECLARE(gpio_lock, &gpio_lck_grp);

static struct {
	uint32_t        phys;
	vm_offset_t     virt;
} blocks[8];

volatile uint8_t *
redwood_gpio_bank(uint32_t block_phys, uint32_t bank_offset)
{
	vm_offset_t virt = 0, mapped;
	unsigned i;

	lck_spin_lock(&gpio_lock);
	for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]) && blocks[i].phys != 0; i++) {
		if (blocks[i].phys == block_phys) {
			virt = blocks[i].virt;
			break;
		}
	}
	lck_spin_unlock(&gpio_lock);
	if (virt != 0) {
		return (volatile uint8_t *)(virt + bank_offset);
	}

	mapped = ml_io_map(block_phys, PINCTRL_SIZE);
	lck_spin_lock(&gpio_lock);
	for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		if (blocks[i].phys == block_phys) {
			virt = blocks[i].virt;          /* someone else mapped it meanwhile */
			break;
		}
		if (blocks[i].phys == 0) {
			blocks[i].phys = block_phys;
			blocks[i].virt = virt = mapped;
			break;
		}
	}
	lck_spin_unlock(&gpio_lock);
	return virt != 0 ? (volatile uint8_t *)(virt + bank_offset) : (volatile uint8_t *)(mapped + bank_offset);
}

static inline uint32_t
bank_rd(volatile uint8_t *bank, uint32_t off)
{
	return *(volatile uint32_t *)(bank + off);
}

static inline void
bank_wr(volatile uint8_t *bank, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(bank + off) = value;
}

static void
bank_update(volatile uint8_t *bank, uint32_t off, uint32_t mask, uint32_t value)
{
	lck_spin_lock(&gpio_lock);
	bank_wr(bank, off, (bank_rd(bank, off) & ~mask) | (value & mask));
	lck_spin_unlock(&gpio_lock);
}

void
redwood_gpio_func(volatile uint8_t *bank, uint32_t pin, uint32_t func)
{
	bank_update(bank, REDWOOD_GPIO_CON, 0xfu << (pin * 4), (func & 0xf) << (pin * 4));
}

void
redwood_gpio_pull(volatile uint8_t *bank, uint32_t pin, uint32_t pull)
{
	bank_update(bank, REDWOOD_GPIO_PUD, 0x3u << (pin * 2), (pull & 0x3) << (pin * 2));
}

uint32_t
redwood_gpio_get(volatile uint8_t *bank, uint32_t pin)
{
	return (bank_rd(bank, REDWOOD_GPIO_DAT) >> pin) & 1;
}

/* the pin an output at `value' (its level set before it is switched to output) */
void
redwood_gpio_set(volatile uint8_t *bank, uint32_t pin, uint32_t value)
{
	bank_update(bank, REDWOOD_GPIO_DAT, 1u << pin, (value ? 1u : 0u) << pin);
	redwood_gpio_func(bank, pin, REDWOOD_GPIO_OUTPUT);
}

/* ------------------------------------------------------------------------ */

static void
bb_wait(struct redwood_bbi2c *b)
{
	delay_for_interval(b->half_us, NSEC_PER_USEC);
}

static void
line_low(struct redwood_bbi2c *b, uint32_t pin)
{
	redwood_gpio_func(b->bank, pin, REDWOOD_GPIO_OUTPUT);      /* DAT for the pin was preset to 0 */
}

static void
line_release(struct redwood_bbi2c *b, uint32_t pin)
{
	redwood_gpio_func(b->bank, pin, REDWOOD_GPIO_INPUT);
}

/* Let SCL go high and wait while a slave stretches it. */
static void
scl_release(struct redwood_bbi2c *b)
{
	uint32_t i;

	line_release(b, b->scl);
	for (i = 0; i < 1000; i++) {
		if (redwood_gpio_get(b->bank, b->scl)) {
			return;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	b->stretch_timeouts++;
}

/* A (repeated) start: works from idle and from SCL low after a byte. */
static void
bb_start(struct redwood_bbi2c *b)
{
	line_release(b, b->sda);
	bb_wait(b);
	scl_release(b);
	bb_wait(b);
	line_low(b, b->sda);
	bb_wait(b);
	line_low(b, b->scl);
	bb_wait(b);
}

static void
bb_stop(struct redwood_bbi2c *b)
{
	line_low(b, b->sda);
	bb_wait(b);
	scl_release(b);
	bb_wait(b);
	line_release(b, b->sda);
	bb_wait(b);
}

static bool
bb_write_byte(struct redwood_bbi2c *b, uint8_t byte)
{
	bool ack;
	int i;

	for (i = 7; i >= 0; i--) {
		if ((byte >> i) & 1) {
			line_release(b, b->sda);
		} else {
			line_low(b, b->sda);
		}
		bb_wait(b);
		scl_release(b);
		bb_wait(b);
		line_low(b, b->scl);
	}
	line_release(b, b->sda);
	bb_wait(b);
	scl_release(b);
	bb_wait(b);
	ack = redwood_gpio_get(b->bank, b->sda) == 0;
	line_low(b, b->scl);
	bb_wait(b);
	return ack;
}

static uint8_t
bb_read_byte(struct redwood_bbi2c *b, bool ack)
{
	uint8_t value = 0;
	int i;

	line_release(b, b->sda);
	for (i = 0; i < 8; i++) {
		bb_wait(b);
		scl_release(b);
		bb_wait(b);
		value = (uint8_t)((value << 1) | redwood_gpio_get(b->bank, b->sda));
		line_low(b, b->scl);
	}
	if (ack) {
		line_low(b, b->sda);
	} else {
		line_release(b, b->sda);
	}
	bb_wait(b);
	scl_release(b);
	bb_wait(b);
	line_low(b, b->scl);
	line_release(b, b->sda);
	return value;
}

bool
redwood_bbi2c_xfer(struct redwood_bbi2c *b, uint8_t addr, const uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen)
{
	uint32_t i;
	bool ok;

	bb_start(b);
	ok = bb_write_byte(b, (uint8_t)(addr << 1));
	for (i = 0; ok && i < wlen; i++) {
		ok = bb_write_byte(b, wbuf[i]);
	}
	if (ok && rlen != 0) {
		bb_start(b);
		ok = bb_write_byte(b, (uint8_t)((addr << 1) | 1));
		for (i = 0; ok && i < rlen; i++) {
			rbuf[i] = bb_read_byte(b, i + 1 < rlen);
		}
	}
	bb_stop(b);
	return ok;
}

bool
redwood_bbi2c_read_reg(struct redwood_bbi2c *b, uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len)
{
	return redwood_bbi2c_xfer(b, addr, &reg, 1, buf, len);
}

bool
redwood_bbi2c_write_reg(struct redwood_bbi2c *b, uint8_t addr, uint8_t reg, uint8_t value)
{
	uint8_t buf[2] = { reg, value };

	return redwood_bbi2c_xfer(b, addr, buf, sizeof(buf), NULL, 0);
}

bool
redwood_bbi2c_setup(struct redwood_bbi2c *b, bool pullup, char *report, size_t report_size)
{
	uint32_t i, sda_before;
	bool ghost;

	b->saved_con = bank_rd(b->bank, REDWOOD_GPIO_CON);
	b->saved_pud = bank_rd(b->bank, REDWOOD_GPIO_PUD);

	line_release(b, b->scl);
	line_release(b, b->sda);
	bank_update(b->bank, REDWOOD_GPIO_DAT, (1u << b->scl) | (1u << b->sda), 0);
	if (pullup) {
		redwood_gpio_pull(b->bank, b->scl, REDWOOD_GPIO_PULL_UP);
		redwood_gpio_pull(b->bank, b->sda, REDWOOD_GPIO_PULL_UP);
	}
	bb_wait(b);
	sda_before = redwood_gpio_get(b->bank, b->sda);

	/* a slave left in the middle of a byte holds SDA low: clock it out, then stop */
	for (i = 0; i < 9 && !redwood_gpio_get(b->bank, b->sda); i++) {
		line_low(b, b->scl);
		bb_wait(b);
		scl_release(b);
		bb_wait(b);
	}
	line_low(b, b->scl);
	bb_wait(b);
	bb_stop(b);

	/* an address nobody owns must not be acked: if it is, SDA reads low whatever happens */
	ghost = redwood_bbi2c_xfer(b, 0x55, NULL, 0, NULL, 0);
	if (report != NULL) {
		snprintf(report, report_size, "%s: CON %#x PUD %#x, SCL %u SDA %u (was %u, %u clocks to free), ghost ack %d",
		    b->name, b->saved_con, b->saved_pud, redwood_gpio_get(b->bank, b->scl), redwood_gpio_get(b->bank, b->sda),
		    sda_before, i, ghost);
	}
	return !ghost && redwood_gpio_get(b->bank, b->sda) && redwood_gpio_get(b->bank, b->scl);
}

void
redwood_bbi2c_restore(struct redwood_bbi2c *b)
{
	uint32_t mask = (0xfu << (b->scl * 4)) | (0xfu << (b->sda * 4));
	uint32_t pmask = (0x3u << (b->scl * 2)) | (0x3u << (b->sda * 2));

	bank_update(b->bank, REDWOOD_GPIO_CON, mask, b->saved_con);
	bank_update(b->bank, REDWOOD_GPIO_PUD, pmask, b->saved_pud);
}
