/*
 * The S20+'s Bluetooth: the BT side of the Broadcom BCM4375 (its Wi-Fi side is on PCIe, redwood_bcm4375.c), an
 * HCI controller on UART1, and iOS's bluetoothd talking to it (PLANS/06-wireless.md, B1-B3). Boot-arg bt=1.
 *
 * The hardware (the phone's device tree, exynos990-y2s_eur_open_21.dts "bluetooth" + exynos9830.dts uart@10840000;
 * the z3s Linux port's BLUETOOTH.md for what made it work on the same SoC):
 *   UART1           0x10840000, SPI 416, a USI v2 block (USI_CON +0xc4, USI_OPTION +0xc8), FIFOs of 256
 *   its USI mode    SYSREG_PERIC1 0x10721000 ("usi_uart_bt"): 1 = UART
 *   its clocks      CMU_PERIC1 0x10700000: MUX_CLKCMU_PERIC1_UART_BT_USER +0x610 (bit 4: 0 OSC 26 MHz, 1 the 400 MHz
 *                   PERIC1 IP clock), DIV_CLK_PERIC1_UART_BT +0x1800 (bits 3:0 + 1), gates +0x2008/+0x203c/+0x2058
 *   its pins        gpq0-0 RXD, -1 TXD, -2 RTS, -3 CTS, function 2 (ALIVE pinctrl 0x15850000, bank +0xa0). RTS is
 *                   held low as a GPIO, as z3s does (this UART's automatic flow control does not drive it); CTS is
 *                   honoured by the UART (UMCON AFC)
 *   BT_EN           gpm16-0 (CMGP pinctrl 0x15c30000, bank +0x200)
 *   BT device wake  gpg1-1 (PERIC1 pinctrl 0x10730000, bank +0xc0): held high, the chip never sleeps
 *   BT host wake    gpa2-3: not used
 *
 * Bring-up, in the kernel (bluetoothd only talks HCI; Apple's BlueTool, which would boot the chip, must not run on a
 * chip it does not know -- userland/src/bluetool_stub.c answers for it):
 *   BT_EN cycled; HCI_Reset at 115200; Broadcom 0xfc18 to BT_FAST baud; 0xfc2e (download minidriver); every record
 *   of the .hcd (0xfc4c write RAM ...); 0xfc4e (launch RAM); 250 ms; back at 115200; HCI_Reset; 0xfc18 again;
 *   0xfc27 (no sleep); 0xfc01 (the BD address); Read Local Version, for the log.
 *
 * Then iOS's side (PLANS/06-wireless.md, "B3 findings"):
 *   - an IOService named "bluetooth" with local-mac-address, transport-encoding 1 (H4) and transport-speed:
 *     bluetoothd's "is there Bluetooth" gate, its address, its transport (iokit/Kernel/RedwoodWLAN.cpp);
 *   - a kernel control named com.apple.uart.bluetooth: bluetoothd's "POSIX" UART driver is not a tty but a
 *     PF_SYSTEM socket to a kernel control of that name (BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth), with
 *     termios through getsockopt/setsockopt(SYSPROTO_CONTROL, TIOCGETA/TIOCSETA). What it writes goes out of the
 *     UART; what comes in goes to it. One client at a time.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <stdarg.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/redwood_bbi2c.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);       /* (osfmk/kern/thread.h, MACH_KERNEL_PRIVATE) */
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_line(const char *line);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context);
extern void RedwoodBluetoothStart(const uint8_t *address, uint32_t speed);       /* iokit/Kernel/RedwoodWLAN.cpp */

#define UART_PHYS       0x10840000U
#define UART_INTID      (416 + 32)
#define SW_CONF_PHYS    0x10721000U
#define CMU_PERIC1      0x10700000U
#define PINCTRL_CMGP    0x15c30000U

/* the UART (Samsung's serial_s3c.h; exynos: UFSTAT counts of 8 bits, FRACVAL) */
#define ULCON           0x00
#define UCON            0x04
#define UFCON           0x08
#define UMCON           0x0c
#define UTRSTAT         0x10
#define UERSTAT         0x14
#define UFSTAT          0x18
#define UTXH            0x20
#define URXH            0x24
#define UBRDIV          0x28
#define UFRACVAL        0x2c
#define UINTP           0x30
#define UINTS           0x34
#define UINTM           0x38
#define USI_CON         0xc4
#define USI_OPTION      0xc8
#define UFSTAT_RXCOUNT(v)       ((v) & 0xff)
#define UFSTAT_RXFULL           (1u << 8)
#define UFSTAT_TXCOUNT(v)       (((v) >> 16) & 0xff)
#define UFSTAT_TXFULL           (1u << 24)
#define UTRSTAT_TXE             (1u << 2)
#define UINT_RXD                (1u << 0)
#define UINT_ERROR              (1u << 1)
#define FIFO                    256

#define BT_SLOW         115200
#define BT_FAST         3000000
#define CTL_NAME        "com.apple.uart.bluetooth"
#define RING            (64u << 10)

static volatile uint8_t *uart, *sw_conf, *cmu;
static volatile uint8_t *bank_q0, *bank_m16, *bank_g1;
static uint32_t uart_clock_hz;
static uint32_t bt_speed;
static bool bt_ready;
static uint8_t bt_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };

/* what came in, for the client (and for the bring-up's own reads) */
static LCK_GRP_DECLARE(bt_lck_grp, "redwood-bt");
static LCK_MTX_DECLARE(bt_lock, &bt_lck_grp);
static LCK_MTX_DECLARE(bt_tx_lock, &bt_lck_grp);
static uint8_t *ring;
static volatile uint32_t ring_head, ring_tail;  /* head: next byte written by the reader; tail: next one taken */
static uint64_t rx_bytes, tx_bytes, rx_overruns, rx_dropped, irqs;
static kern_ctl_ref bt_ctl;
static volatile uint32_t bt_unit;               /* the connected client's unit, 0: none */
static int bt_event;

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(uart + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(uart + off) = v;
}

static void
bt_log(const char *fmt, ...) __printflike(1, 2);

static void
bt_log(const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	loader_log_line(line);
}

static void
pin_out(volatile uint8_t *bank, uint32_t pin, uint32_t level)
{
	uint32_t dat = *(volatile uint32_t *)(bank + REDWOOD_GPIO_DAT);

	*(volatile uint32_t *)(bank + REDWOOD_GPIO_DAT) = (dat & ~(1u << pin)) | ((level & 1) << pin);
	redwood_gpio_func(bank, pin, REDWOOD_GPIO_OUTPUT);
}

/* ---- the UART ---- */

static void
uart_baud(uint32_t baud)
{
	uint32_t div = uart_clock_hz / baud;

	/* (Samsung's s3c24xx_serial_set_termios: the nearer of div and div + 1, UBRDIV = div / 16 - 1, FRACVAL = div % 16) */
	if ((div & 15) != 15) {
		uint32_t rd_baud = uart_clock_hz / div, ru_baud = uart_clock_hz / (div + 1);

		if ((rd_baud > baud ? rd_baud - baud : baud - rd_baud) > (ru_baud > baud ? ru_baud - baud : baud - ru_baud)) {
			div++;
		}
	}
	/* what is in the transmitter goes out at the old speed first */
	for (int i = 0; i < 10000 && !(rd(UTRSTAT) & UTRSTAT_TXE); i++) {
		delay(10);
	}
	wr(UBRDIV, div / 16 - 1);
	wr(UFRACVAL, div & 15);
	bt_speed = baud;
}

static void
uart_reset_fifos(void)
{
	wr(UFCON, rd(UFCON) | (3u << 1));
	for (int i = 0; i < 1000 && (rd(UFCON) & (3u << 1)); i++) {
		delay(1);
	}
}

/* clocks, the USI's mode, pins, and the UART itself: 8N1, FIFOs, CTS flow control, interrupts for what comes in */
static bool
uart_setup(void)
{
	uint32_t mux, div, g1, g2, g3;

	cmu = (volatile uint8_t *)ml_io_map(CMU_PERIC1, 0x4000);
	sw_conf = (volatile uint8_t *)ml_io_map(SW_CONF_PHYS & ~0xfffU, 0x1000) + (SW_CONF_PHYS & 0xfffU);
	uart = (volatile uint8_t *)ml_io_map(UART_PHYS, 0x1000);
	/* the gates on (CG_VAL bit 21 with MANUAL bit 20: forced on, whatever automatic gating would do) */
	{
		static const uint32_t gates[] = { 0x2008, 0x203c, 0x2058 };

		for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
			volatile uint32_t *g = (volatile uint32_t *)(cmu + gates[i]);

			*g = *g | (1u << 21) | (1u << 20);
		}
	}
	mux = *(volatile uint32_t *)(cmu + 0x610);
	div = *(volatile uint32_t *)(cmu + 0x1800);
	g1 = *(volatile uint32_t *)(cmu + 0x2008);
	g2 = *(volatile uint32_t *)(cmu + 0x203c);
	g3 = *(volatile uint32_t *)(cmu + 0x2058);
	uart_clock_hz = ((mux >> 4) & 1 ? 400000000U : 26000000U) / ((div & 0xf) + 1);
	bt_log("bt: UART1 clock: mux %08x (%s), div %08x -> %u Hz; gates %08x %08x %08x; QCH %08x\n", mux,
	    (mux >> 4) & 1 ? "PERIC1 IP 400 MHz" : "OSC 26 MHz", div, uart_clock_hz, g1, g2, g3,
	    *(volatile uint32_t *)(cmu + 0x302c));

	/* the USI as a UART, out of reset, its clock requested continuously (z3s: "clkreq-on"; without it Rx is dead) */
	*(volatile uint32_t *)sw_conf = 1;
	wr(USI_CON, 1);
	delay(1);
	wr(USI_CON, 0);
	delay(1);
	wr(USI_OPTION, 1u << 1);

	/* pins: RXD, TXD, CTS to the UART; RTS a GPIO held low (we are always ready to take bytes) */
	bank_q0 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, 0xa0);
	redwood_gpio_func(bank_q0, 0, 2);
	redwood_gpio_func(bank_q0, 1, 2);
	redwood_gpio_func(bank_q0, 3, 2);
	pin_out(bank_q0, 2, 0);
	redwood_gpio_pull(bank_q0, 0, REDWOOD_GPIO_PULL_UP);
	redwood_gpio_pull(bank_q0, 3, REDWOOD_GPIO_PULL_UP);

	wr(UINTM, 0xf);                         /* all masked while it is set up */
	wr(UINTP, 0xf);
	wr(ULCON, 3);                           /* 8N1 */
	/* UCON: Rx and Tx by the CPU (interrupt/polling), Rx timeout interrupt (8 frames), Rx error interrupt, level-triggered */
	wr(UCON, (1u << 0) | (1u << 2) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (3u << 12));
	/* UFCON: FIFOs on, Rx trigger 32 bytes, Tx trigger 4 */
	wr(UFCON, (1u << 0) | (4u << 4) | (1u << 8));
	uart_reset_fifos();
	wr(UMCON, 1u << 4);                     /* AFC: Tx waits for CTS */
	uart_baud(BT_SLOW);
	return true;
}

/* ---- what comes in ---- */

static uint32_t
ring_count(void)
{
	return ring_head - ring_tail;
}

/* the FIFO into the ring (the reader thread; never from the interrupt) */
static void
uart_drain(void)
{
	uint32_t st, n;

	while ((n = UFSTAT_RXCOUNT(st = rd(UFSTAT))) != 0 || (st & UFSTAT_RXFULL)) {
		if (n == 0) {
			n = FIFO;
		}
		while (n-- > 0) {
			uint8_t c = (uint8_t)rd(URXH);

			if (ring_count() < RING) {
				ring[ring_head % RING] = c;
				ring_head++;
			} else {
				rx_dropped++;
			}
			rx_bytes++;
		}
	}
	if (rd(UERSTAT) & 1) {
		rx_overruns++;
	}
}

static void
bt_irq(__unused void *context, __unused uint32_t intid)
{
	wr(UINTM, rd(UINTM) | UINT_RXD | UINT_ERROR);
	wr(UINTP, UINT_RXD | UINT_ERROR);
	irqs++;
	thread_wakeup((event_t)&bt_event);
}

/* to the client, as much as it will take */
static void
bt_deliver(void)
{
	uint32_t unit = bt_unit;

	while (unit != 0 && ring_count() > 0) {
		uint32_t at = ring_tail % RING, n = ring_count();
		errno_t e;

		if (n > RING - at) {
			n = RING - at;
		}
		if (n > 4096) {
			n = 4096;
		}
		e = ctl_enqueuedata(bt_ctl, unit, ring + at, n, 0);
		if (e != 0) {
			break;                          /* (full: the rest the next time round) */
		}
		ring_tail += n;
	}
	if (unit == 0 && !bt_ready) {
		return;                                 /* the bring-up takes its own bytes */
	}
	if (unit == 0 && ring_count() > 0) {
		rx_dropped += ring_count();
		ring_tail = ring_head;                  /* nobody to give them to */
	}
}

/*
 * The interrupt wakes it; every 100 ms besides, so that nothing is left behind a missed one -- counted:
 * bt_rescued, the looks that found bytes the interrupt had not brought (it was every 2 ms: some 500
 * wake-ups a second with nothing to do, the cpuidle wake record, 27 October).
 */
static uint64_t bt_rescued, bt_safety_looks;

static void
bt_reader(__unused void *arg, __unused wait_result_t wr_)
{
	wait_result_t why = THREAD_AWAKENED;

	for (;;) {
		uint32_t before = ring_count();

		lck_mtx_lock(&bt_lock);
		uart_drain();
		lck_mtx_unlock(&bt_lock);
		if (why == THREAD_TIMED_OUT) {
			bt_safety_looks++;
			if (ring_count() != before) {
				bt_rescued++;
				printf("bt: the 100 ms look found %u bytes no interrupt had brought (%llu times of %llu looks)\n",
				    ring_count() - before, bt_rescued, bt_safety_looks);
			}
		}
		bt_deliver();
		wr(UINTM, rd(UINTM) & ~(UINT_RXD | UINT_ERROR));
		assert_wait_timeout((event_t)&bt_event, THREAD_UNINT, 100, NSEC_PER_MSEC);
		why = thread_block(THREAD_CONTINUE_NULL);
	}
}

/* ---- what goes out ---- */

static void
uart_write(const uint8_t *p, uint32_t n)
{
	lck_mtx_lock(&bt_tx_lock);
	while (n > 0) {
		uint32_t room = FIFO - UFSTAT_TXCOUNT(rd(UFSTAT));

		if (rd(UFSTAT) & UFSTAT_TXFULL) {
			room = 0;
		}
		if (room == 0) {
			delay(20);
			continue;
		}
		while (room-- > 0 && n > 0) {
			wr(UTXH, *p++);
			n--;
			tx_bytes++;
		}
	}
	lck_mtx_unlock(&bt_tx_lock);
}

/* ---- HCI for the bring-up ---- */

/* bytes from the ring, waiting up to ms for them */
static bool
ring_take(uint8_t *to, uint32_t n, uint32_t ms)
{
	uint64_t deadline = mach_absolute_time(), now;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &now);
	deadline += now;
	while (ring_count() < n) {
		if (mach_absolute_time() > deadline) {
			return false;
		}
		IOSleep(1);
	}
	for (uint32_t i = 0; i < n; i++) {
		to[i] = ring[ring_tail % RING];
		ring_tail++;
	}
	return true;
}

/* one command, and its Command Complete (or Status); returns the status byte, or -1. reply: its parameters after the status */
static int
hci_cmd(uint16_t opcode, const uint8_t *params, uint8_t len, uint8_t *reply, uint32_t reply_max, uint32_t ms)
{
	uint8_t pkt[3 + 255 + 1], hdr[3], ev[260];

	pkt[0] = 0x01;
	pkt[1] = (uint8_t)opcode;
	pkt[2] = (uint8_t)(opcode >> 8);
	pkt[3] = len;
	memcpy(pkt + 4, params, len);
	uart_write(pkt, 4u + len);
	for (int tries = 0; tries < 8; tries++) {
		uint8_t type;

		if (!ring_take(&type, 1, ms)) {
			return -1;
		}
		if (type != 0x04) {
			continue;                       /* (not an event: should not happen during bring-up) */
		}
		if (!ring_take(hdr, 2, ms) || !ring_take(ev, hdr[1], ms)) {
			return -1;
		}
		if (hdr[0] == 0x0e && hdr[1] >= 4 && (ev[1] | ev[2] << 8) == opcode) {
			if (reply != NULL) {
				uint32_t n = hdr[1] - 4u;

				memcpy(reply, ev + 4, n < reply_max ? n : reply_max);
			}
			return ev[3];
		}
		if (hdr[0] == 0x0f && hdr[1] >= 4 && (ev[2] | ev[3] << 8) == opcode) {
			return ev[0];
		}
	}
	return -1;
}

static bool
bt_set_speed(uint32_t baud)
{
	uint8_t p[6] = { 0, 0, (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24) };
	int st = hci_cmd(0xfc18, p, sizeof(p), NULL, 0, 500);

	if (st != 0) {
		bt_log("bt: 0xfc18 (UART speed %u): status %d\n", baud, st);
		return false;
	}
	uart_baud(baud);
	IOSleep(10);
	return true;
}

/* the chip powered, patched and configured; true: ready for bluetoothd */
static bool
bt_bringup(void)
{
	static const char hcd_name[] = "bcm4375B1-semco.hcd";
	const uint8_t *hcd;
	uint64_t hcd_size = 0, at;
	uint8_t reply[64];
	uint32_t records = 0;
	int st;
	uint64_t t0 = mach_absolute_time(), ns;

	hcd = redwood_image_file("fw", hcd_name, &hcd_size, NULL);
	if (hcd == NULL || hcd_size < 3) {
		bt_log("bt: no fw/%s in the image: the chip is not patched (userland/build.sh)\n", hcd_name);
	}
	/* BT_EN cycled, the chip awake */
	bank_m16 = redwood_gpio_bank(PINCTRL_CMGP, 0x200);
	bank_g1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, 0xc0);
	pin_out(bank_g1, 1, 1);
	pin_out(bank_m16, 0, 0);
	IOSleep(20);
	pin_out(bank_m16, 0, 1);
	IOSleep(150);
	uart_reset_fifos();
	ring_head = ring_tail = 0;

	st = hci_cmd(0x0c03, NULL, 0, NULL, 0, 1000);
	if (st != 0) {
		bt_log("bt: no answer to HCI_Reset at %u baud (status %d; %llu bytes in, %llu irqs, UFSTAT %08x, UERSTAT %08x, "
		    "UMSTAT %08x, UTRSTAT %08x)\n", BT_SLOW, st, rx_bytes, irqs, rd(UFSTAT), rd(UERSTAT), rd(0x1c), rd(UTRSTAT));
		return false;
	}
	if (hci_cmd(0x1001, NULL, 0, reply, sizeof(reply), 500) == 0) {
		bt_log("bt: the chip answers at %u baud: HCI %u rev %04x, LMP %u, manufacturer %u, LMP subversion %04x\n",
		    BT_SLOW, reply[0], reply[1] | reply[2] << 8, reply[3], reply[4] | reply[5] << 8, reply[6] | reply[7] << 8);
	}
	if (hcd != NULL && hcd_size >= 3) {
		if (!bt_set_speed(BT_FAST)) {
			bt_log("bt: downloading the patchram at %u baud\n", BT_SLOW);
		}
		if ((st = hci_cmd(0xfc2e, NULL, 0, NULL, 0, 1000)) != 0) {
			bt_log("bt: 0xfc2e (download minidriver): status %d\n", st);
			return false;
		}
		IOSleep(50);
		for (at = 0; at + 3 <= hcd_size;) {
			uint16_t op = hcd[at] | hcd[at + 1] << 8;
			uint8_t len = hcd[at + 2];

			if (at + 3 + len > hcd_size) {
				break;
			}
			st = hci_cmd(op, hcd + at + 3, len, NULL, 0, 1000);
			if (op == 0xfc4e) {
				break;                  /* launch RAM: its answer may not come; the chip restarts */
			}
			if (st != 0) {
				bt_log("bt: patchram record %u (%04x) at %llu: status %d\n", records, op, at, st);
				return false;
			}
			records++;
			at += 3u + len;
		}
		IOSleep(250);
		uart_baud(BT_SLOW);
		uart_reset_fifos();
		ring_head = ring_tail = 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		bt_log("bt: patchram %s: %u records written, launched (%llu ms)\n", hcd_name, records, ns / NSEC_PER_MSEC);
		if ((st = hci_cmd(0x0c03, NULL, 0, NULL, 0, 1000)) != 0) {
			bt_log("bt: no answer to HCI_Reset after the patchram (status %d)\n", st);
			return false;
		}
	}
	if (!bt_set_speed(BT_FAST)) {
		uart_baud(BT_SLOW);
	}
	{
		uint8_t sleep[12] = { 0 };      /* 0xfc27 Set Sleep Mode: mode 0, no sleep */

		(void)hci_cmd(0xfc27, sleep, sizeof(sleep), NULL, 0, 500);
	}
	{
		uint8_t a[6];

		for (int i = 0; i < 6; i++) {
			a[i] = bt_address[5 - i];
		}
		if ((st = hci_cmd(0xfc01, a, 6, NULL, 0, 500)) != 0) {
			bt_log("bt: 0xfc01 (BD address): status %d\n", st);
		}
	}
	if (hci_cmd(0x1001, NULL, 0, reply, sizeof(reply), 500) == 0) {
		bt_log("bt: patched: HCI %u rev %04x, LMP %u, manufacturer %u, LMP subversion %04x; at %u baud\n", reply[0],
		    reply[1] | reply[2] << 8, reply[3], reply[4] | reply[5] << 8, reply[6] | reply[7] << 8, bt_speed);
	}
	if (hci_cmd(0x1009, NULL, 0, reply, sizeof(reply), 500) == 0) {
		bt_log("bt: BD address %02x:%02x:%02x:%02x:%02x:%02x\n", reply[5], reply[4], reply[3], reply[2], reply[1], reply[0]);
	}
	return true;
}

/* ---- the kernel control for bluetoothd ---- */

#define TIOCGETA_64     0x40487413u     /* _IOR('t', 19, struct termios), a 64-bit process's termios: 72 bytes */
#define TIOCSETA_64     0x80487414u
#define TIOCSETAW_64    0x80487415u
#define TIOCSETAF_64    0x80487416u

static errno_t
bt_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo)
{
	if (bt_unit != 0) {
		return EBUSY;
	}
	*unitinfo = NULL;
	lck_mtx_lock(&bt_lock);
	ring_tail = ring_head;                  /* nothing old for a new client */
	lck_mtx_unlock(&bt_lock);
	bt_unit = sac->sc_unit;
	bt_log("bt: %s: a client connected (unit %u)\n", CTL_NAME, sac->sc_unit);
	return 0;
}

static errno_t
bt_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	if (bt_unit == unit) {
		bt_unit = 0;
	}
	bt_log("bt: %s: the client went (unit %u; %llu bytes in, %llu out, %llu dropped, %llu overruns)\n", CTL_NAME, unit,
	    rx_bytes, tx_bytes, rx_dropped, rx_overruns);
	return 0;
}

static errno_t
bt_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	for (mbuf_t n = m; n != NULL; n = mbuf_next(n)) {
		if (mbuf_len(n) > 0) {
			uart_write((const uint8_t *)mbuf_data(n), (uint32_t)mbuf_len(n));
		}
	}
	mbuf_freem(m);
	return 0;
}

static errno_t
bt_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, __unused void *data,
    __unused size_t len)
{
	uint32_t o = (uint32_t)opt;

	/* the speed is the kernel's: the chip's is changed with 0xfc18 only, and was at bring-up */
	return o == TIOCSETA_64 || o == TIOCSETAW_64 || o == TIOCSETAF_64 ? 0 : EINVAL;
}

static errno_t
bt_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	if ((uint32_t)opt == TIOCGETA_64 && data != NULL && *len >= 72) {
		uint64_t *t = (uint64_t *)data;

		bzero(data, 72);
		t[0] = 0x4;                             /* c_iflag: IGNPAR */
		t[1] = 0;                               /* c_oflag */
		t[2] = 0x300 | 0x800 | 0x8000 | 0x10000 | 0x20000;     /* c_cflag: CS8 CREAD CLOCAL CCTS_OFLOW CRTS_IFLOW */
		t[3] = 0;                               /* c_lflag */
		t[7] = bt_speed;                        /* c_ispeed (after c_cc[20] and padding: offset 56) */
		t[8] = bt_speed;                        /* c_ospeed */
		*len = 72;
		return 0;
	}
	return EINVAL;
}

static void
bt_start(__unused void *arg, __unused wait_result_t w)
{
	struct kern_ctl_reg reg;
	thread_t reader;
	errno_t e;

	if (kernel_thread_start_priority(bt_reader, NULL, 93, &reader) != KERN_SUCCESS) {
		bt_log("bt: no reader thread\n");
		return;
	}
	thread_deallocate(reader);
	if (gic_v3_spi_attach(UART_INTID, false, bt_irq, NULL) != KERN_SUCCESS) {
		bt_log("bt: UART1's interrupt (GIC %u) not attached: polled every 2 ms\n", UART_INTID);
	}
	wr(UINTM, rd(UINTM) & ~(UINT_RXD | UINT_ERROR));
	if (!bt_bringup()) {
		bt_log("bt: the controller did not come up: no Bluetooth\n");
		return;
	}
	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, CTL_NAME, sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = 64 << 10;
	reg.ctl_recvsize = 256 << 10;
	reg.ctl_connect = bt_connect;
	reg.ctl_disconnect = bt_disconnect;
	reg.ctl_send = bt_send;
	reg.ctl_setopt = bt_setopt;
	reg.ctl_getopt = bt_getopt;
	e = ctl_register(&reg, &bt_ctl);
	if (e != 0) {
		bt_log("bt: kernel control %s not registered (%d)\n", CTL_NAME, e);
		return;
	}
	bt_ready = true;
	RedwoodBluetoothStart(bt_address, bt_speed);
	bt_log("bt: ready: kernel control %s (H4 at %u baud), IOService \"bluetooth\" for bluetoothd\n", CTL_NAME, bt_speed);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, bt_rx_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &rx_bytes, "bytes from the BT controller");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, bt_tx_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &tx_bytes, "bytes to the BT controller");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, bt_rx_dropped, CTLFLAG_RD | CTLFLAG_LOCKED, &rx_dropped, "bytes nobody took");

void redwood_exynos_bt_init(void);

void
redwood_exynos_bt_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bt", &on, sizeof(on)) || on == 0) {
		return;
	}
	ring = kalloc_data(RING, Z_WAITOK | Z_ZERO);
	if (ring == NULL || !uart_setup()) {
		return;
	}
	if (kernel_thread_start(bt_start, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
