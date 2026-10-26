/*
 * I2C bit-banged on two Exynos 990 GPIOs (open-drain: low = output 0, high = input, the
 * board's pull-ups raise it), and access to the pinctrl banks. Used where the HSI2C
 * controllers are not set up (their clocks are not ours to turn on): the touch controller,
 * the S2DOS05 display PMIC, the MAX77705 charger and fuel gauge. See bsd/dev/redwood_bbi2c.c.
 */
#ifndef _SYS_REDWOOD_BBI2C_H_
#define _SYS_REDWOOD_BBI2C_H_

#ifdef KERNEL
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Exynos 990 pinctrl blocks (0x20 bytes per bank) */
#define REDWOOD_PINCTRL_PERIC0  0x10430000U     /* gpp0..gpp4, gpg0 */
#define REDWOOD_PINCTRL_PERIC1  0x10730000U     /* gpp5..gpp9, gpc0, gpg1, gpb0..2 */
#define REDWOOD_PINCTRL_ALIVE   0x15850000U     /* gpa0..gpa4, gpq0 */

#define REDWOOD_GPIO_CON        0x00U           /* 4 bits per pin */
#define REDWOOD_GPIO_DAT        0x04U
#define REDWOOD_GPIO_PUD        0x08U           /* 2 bits per pin: 0 none, 1 down, 3 up */

#define REDWOOD_GPIO_INPUT      0x0U
#define REDWOOD_GPIO_OUTPUT     0x1U
#define REDWOOD_GPIO_PULL_NONE  0x0U
#define REDWOOD_GPIO_PULL_UP    0x3U

/* A bank's registers: the block mapped once, whoever asks. */
volatile uint8_t *redwood_gpio_bank(uint32_t block_phys, uint32_t bank_offset);
void redwood_gpio_func(volatile uint8_t *bank, uint32_t pin, uint32_t func);
void redwood_gpio_pull(volatile uint8_t *bank, uint32_t pin, uint32_t pull);
uint32_t redwood_gpio_get(volatile uint8_t *bank, uint32_t pin);
void redwood_gpio_set(volatile uint8_t *bank, uint32_t pin, uint32_t value);

struct redwood_bbi2c {
	const char             *name;
	volatile uint8_t       *bank;
	uint32_t                scl, sda;
	uint32_t                half_us;
	uint32_t                saved_con, saved_pud;
	uint32_t                stretch_timeouts;
};

/*
 * Take the pins (open-drain), free a stuck bus, check that an address nobody owns is not
 * acked. `report` gets one line about what was found. False: the bus is not usable.
 */
bool redwood_bbi2c_setup(struct redwood_bbi2c *bus, bool pullup, char *report, size_t report_size);
/* The pins' functions and pulls as they were before setup (the HSI2C controller's). */
void redwood_bbi2c_restore(struct redwood_bbi2c *bus);
/* Write `wlen` bytes, then (if `rlen`) a repeated start and read `rlen`. False on a NACK. */
bool redwood_bbi2c_xfer(struct redwood_bbi2c *bus, uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
    uint8_t *rbuf, uint32_t rlen);
/* SMBus-style register access */
bool redwood_bbi2c_read_reg(struct redwood_bbi2c *bus, uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len);
bool redwood_bbi2c_write_reg(struct redwood_bbi2c *bus, uint8_t addr, uint8_t reg, uint8_t value);
#endif /* KERNEL */

#endif /* _SYS_REDWOOD_BBI2C_H_ */
