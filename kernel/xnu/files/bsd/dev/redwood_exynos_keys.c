/*
 * The Galaxy S20+'s (y2s, Exynos 990) power and volume keys.
 *
 * All three are plain GPIOs of the ALIVE pinctrl, low while pressed (the phone's device tree,
 * gpio_keys, and the UEFI's y2sPkg KeypadDeviceLib, which reads them the same way):
 *
 *   volume up    gpa0-3        volume down  gpa0-4        power  gpa2-4
 *
 * They are polled every 16 ms and reported as redwood_touch_post(REDWOOD_BUTTON_DOWN/UP, ...),
 * which backboardd turns into the HID events of the real keys (userland inject.c).
 *
 * A key is believed only after it has been seen released once (a key held while the system
 * starts is not a press), and only a state seen twice running counts (debounce).
 *
 * Volume down + power held together for 3 s hands control back to the loader, which saves the
 * log -- a clean end of a run, a few seconds before the PMIC's own reset (about 7 s) would
 * cut the power.
 *
 * Boot args: keys=1 on (default: follows touch_ts), keys=0 off, keys_escape=0 no escape.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern void loader_return(void);
extern void redwood_var_save_on_return(void);  /* bsd/dev/redwood_ufs.c */
extern void redwood_sync_filesystems(void);     /* bsd/dev/redwood_ufs.c */

#define POLL_MS         16
#define ESCAPE_MS       3000
#define BANK_GPA0       0x00U
#define BANK_GPA2       0x40U

static struct key {
	const char         *name;
	uint32_t            bank_offset, pin;
	uint32_t            record;            /* REDWOOD_BUTTON_* */
	volatile uint8_t   *bank;
	bool                live;              /* seen released: believed from then on */
	bool                last;              /* the look before this one */
	bool                down;              /* as reported */
	uint64_t            since;
	uint32_t            presses;
} keys[] = {
	{ .name = "power", .bank_offset = BANK_GPA2, .pin = 4, .record = REDWOOD_BUTTON_LOCK },
	{ .name = "volume up", .bank_offset = BANK_GPA0, .pin = 3, .record = REDWOOD_BUTTON_VOLUME_UP },
	{ .name = "volume down", .bank_offset = BANK_GPA0, .pin = 4, .record = REDWOOD_BUTTON_VOLUME_DOWN },
};

#define KEY_POWER       0
#define KEY_VOLUME_DOWN 2

static void
key_look(struct key *k, bool pressed)
{
	uint64_t now = mach_absolute_time(), held;
	bool steady = pressed == k->last;

	k->last = pressed;
	if (!steady) {
		return;
	}
	if (!k->live) {
		if (!pressed) {
			k->live = true;
			printf("keys: %s is released: believed from here on\n", k->name);
		}
		return;
	}
	if (pressed == k->down) {
		return;
	}
	k->down = pressed;
	if (pressed) {
		k->since = now;
		k->presses++;
		printf("keys: %s pressed\n", k->name);
	} else {
		absolutetime_to_nanoseconds(now - k->since, &held);
		printf("keys: %s released after %llu ms\n", k->name, held / NSEC_PER_MSEC);
	}
	redwood_touch_post(pressed ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, k->record, 0, 0);
}

static void
keys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t escape = 1, i;
	uint64_t both_since = 0, ms;

	PE_parse_boot_argn("keys_escape", &escape, sizeof(escape));
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		keys[i].bank = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, keys[i].bank_offset);
		keys[i].last = redwood_gpio_get(keys[i].bank, keys[i].pin) == 0;
	}
	printf("keys: power %s, volume up %s, volume down %s (polled every %u ms)\n",
	    keys[0].last ? "down" : "up", keys[1].last ? "down" : "up", keys[2].last ? "down" : "up", POLL_MS);

	for (;;) {
		for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			key_look(&keys[i], redwood_gpio_get(keys[i].bank, keys[i].pin) == 0);
		}
		if (escape && keys[KEY_POWER].down && keys[KEY_VOLUME_DOWN].down) {
			uint64_t now = mach_absolute_time();

			if (both_since == 0) {
				both_since = now;
			}
			absolutetime_to_nanoseconds(now - both_since, &ms);
			if (ms / NSEC_PER_MSEC >= ESCAPE_MS) {
				printf("keys: volume down + power held %u ms: back to the loader\n", ESCAPE_MS);
				redwood_var_save_on_return();   /* the data volume into the flash first (var_store=1) */
				redwood_sync_filesystems();     /* and what HFS+ on userdata holds (bsd/dev/redwood_ufs.c) */
				loader_return();
			}
		} else {
			both_since = 0;
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

void
redwood_exynos_keys_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("keys", &on, sizeof(on))) {
		PE_parse_boot_argn("touch_ts", &on, sizeof(on));
	}
	if (on == 0) {
		return;
	}
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
