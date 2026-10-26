/*
 * Wi-Fi of the Galaxy S20+ (y2s, Exynos 990), steps W2a and W2b of PLANS/06-wireless.md: the PCIe
 * link to the Broadcom BCM4375 B1, as far as reading the chip's configuration space. The chip is
 * the endpoint of PCIe channel 0, Samsung's "host-v0" controller (compatible "samsung,exynos-pcie"):
 * a DesignWare core with Samsung's glue registers (ELBI), a one-lane Gen2 PHY and its PCS, all in
 * the HSI1 block. Nothing at all is done without the boot-arg:
 *
 *   wifi_pcie=1        read-only: what S-Boot left in the PMU (PHY isolation), the clocks, the
 *                      sysreg, the pins, ELBI/PHY/PCS/DBI and the LTSSM
 *   wifi_pcie=2        the same look, then the link: pins, WLAN_REG_ON, clocks, PMU, PHY
 *                      calibration, the root port's set-up, PERST#, LTSSM, L0 (two tries), then
 *                      the endpoint's id, class and BAR sizes through the iATU's CFG0 window. On a
 *                      failure: the LTSSM's path and history, then LTSSM off, PERST# and
 *                      WLAN_REG_ON low, everything else left as it is.
 *   wifi_pcie_force=1  with either, also read the PHY, PCS and DBI where Samsung never does (the
 *                      PHY isolated by the PMU, lane 0 off, or the block's clock forced off): such a
 *                      read may stall the bus.
 *   wifi_pcie_wait=S   with 2: the link only S seconds after the look (default 20: the system up).
 *   wifi_pcie=3        as 2, then W3: the DMA pool the loader set aside below 4 GB (/chosen
 *                      wifi-dma-base) granted to the PCIe block, 64 KB at a time, through Samsung's
 *                      hypervisor (HVC 0xc6000101, as z3s's brcmfmac s2mpu.c).
 *   wifi_pcie=4        as 3, then W4/W5 (redwood_bcm4375.c): the BARs, the chip's cores, the firmware
 *                      and NVRAM from /fw into its RAM and started, the message rings, the firmware's
 *                      version and address, the CLM, and a scan whose networks go to the log.
 *
 * Every line goes to the loader's log ("wifi: ..."). The work is done on a thread of its own and
 * every wait is bounded; an MMIO access that never completes is the one thing that cannot be.
 *
 * No power domain, no D-TZPC restore: HSI1 is not a switchable domain (the device tree has no
 * pd-hsi1 and cal-if's flexpmu_cal_local_exynos9830.h has only hsi0_on; the UFS this kernel boots
 * from is in HSI1), so it has been on, its TZPC set, since S-Boot. What the PMU has for PCIe ch0 is
 * the PHY isolation bit, PCIE_PHY_CONTROL. Samsung leaves HSI1's PCIe clocks to hardware auto
 * gating (the DT gives the controller none: pcie-clk-num = phy-clk-num = 0); z3s, which has this
 * chip working under mainline Linux on the same SoC, turns the gates on by hand, and so does this.
 *
 * Sources, cited [tag]line (copies in the session's files/wireless-ref/):
 *   [rc]   ExtremeXT/android_kernel_samsung_exynos990 drivers/pci/controller/dwc/
 *          pcie-exynos-host-v0.c, the controller driver Samsung ships for this node
 *   [hdr]  ... pcie-exynos-host-v0.h, its register names
 *   [cal]  ... pcie-exynos9830-v0-cal.c, the PHY (Exynos 9830, ch0, Gen2, 26 MHz reference)
 *   [dw]   ... pcie-designware-host.c (dw_pcie_setup_rc), [dwc] pcie-designware.c (dw_pcie_setup)
 *   [z3s]  Bentlybro/z3s-mainline-linux kernel/drivers/pcie-exynos/pci-exynos.c
 *   [cmu]  ... drivers/soc/samsung/cal-if/exynos9830/cmucal-sfr.c
 *   [dhd]  bcmdhd dhd_custom_exynos.c (WLAN_REG_ON)
 *   [dt]   this phone's dtb.img.0.dts / dtbo.img.0.dts
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <stdarg.h>
#include <sys/redwood_bbi2c.h>
#include <sys/systm.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void loader_log_line(const char *line);

void redwood_exynos_pcie_init(void);
int redwood_wifi_dma_pool(uint64_t *phys, vm_offset_t *virt, size_t *size);

struct redwood_bcm_bus {
	volatile uint8_t *cfg;
	volatile uint8_t *dbi;
	uint32_t mem_phys, mem_size;
	uint64_t bar0_size, bar2_size;
	bool bar0_wide, bar2_wide;
	uint64_t pool_phys;
	vm_offset_t pool_virt;
	size_t pool_mapped;
};
void redwood_bcm4375_run(const struct redwood_bcm_bus *bus);
static struct redwood_bcm_bus bcm_bus;

/* ------------------------------------------------------------------------ */
/* Where things are: pcie@133B0000's reg, reg-names and ranges ([dt]dtb 10911) */

#define ELBI_PHYS               0x133b0000U
#define PHY_PHYS                0x133e0000U
#define PCS_PHYS                0x133d0000U
#define DBI_PHYS                0x133f0000U
#define SYSREG_PHYS             0x13021044U     /* +0xc = 0x13021050, "sysreg + 0x1050" in [cal]91 */
#define CFG_PHYS                0x11ffe000U     /* "config": its first 4 KB is the CFG0 window [dw]351 */
#define MEM_PHYS                0x11000000U     /* ranges: 32-bit memory, PCI address = CPU address */
#define MEM_SIZE                0x00ff0000U
#define PMU_PHYS                0x15860000U
#define CMU_TOP_PHYS            0x1a330000U     /* [cmu]7 */
#define CMU_HSI1_PHYS           0x13000000U     /* [cmu]32 */
#define PINCTRL_HSI1            0x13040000U     /* gpf0 ([dt]dtb 704) */

/* ELBI, Samsung's glue ([hdr]) */
#define ELBI_IRQ_PULSE          0x000U
#define ELBI_SW_WAKE            0x018U          /* bit 1 set: bus number (config TLPs) disabled */
#define ELBI_LTSSM_ENABLE       0x02cU
#define ELBI_RDLH_LINKUP        0x074U          /* [4:0] the LTSSM state, 0x11 = L0 */
#define ELBI_PM_DSTATE          0x088U
#define ELBI_APP_REQ_EXIT_L1    0x0f4U
#define ELBI_HISTORY(i)         (0x138U + 4U * (i))     /* 32 entries, LTSSM in [21:16] */
#define ELBI_LINKDOWN_RST_SEL   0x1b8U
#define ELBI_SOFT_CORE_RESET    0x1d0U
#define ELBI_HISTORY_CHECK      0x274U
#define ELBI_PCS_G_RST          0x288U          /* PCIE_INIT_RSTN */
#define ELBI_CMN_RST            0x28cU          /* PCIE_CMN_RSTN: PCS and PMA common */
#define ELBI_MAC_RST            0x290U          /* PCIE_LANE_RSTN */
#define ELBI_STATE_POWER_S      0x2bcU
#define ELBI_STATE_POWER_M      0x2c0U
#define ELBI_QCH_SEL            0x2c8U

/* PCS */
#define PCS_L1SS_CTRL           0x00cU
#define PCS_CDR                 0x0d0U
#define PCS_ELECIDLE            0x0ecU          /* bit 3: ignore RX electrical idle [cal]33 */
#define PCS_TX_LATENCY          0x0f8U
#define PCS_REFCLK_OUT          0x100U
#define PCS_104                 0x104U

/* DBI: the root port's own configuration space and DesignWare's port logic ([hdr], [dw]) */
#define DBI_ID                  0x000U
#define DBI_CMD                 0x004U
#define DBI_CLASS               0x008U
#define DBI_BAR0                0x010U
#define DBI_BAR1                0x014U
#define DBI_BUSES               0x018U
#define DBI_INTPIN              0x03cU
#define DBI_PMCSR               0x044U          /* PM capability at 0x40 */
#define DBI_LNKCAP              0x07cU          /* PCIe capability at 0x70 */
#define DBI_LNKCTL              0x080U          /* LNKSTA in [31:16] */
#define DBI_LNKCTL2             0x0a0U
#define DBI_PORT_LINK           0x710U
#define DBI_WIDTH_SPEED         0x80cU
#define DBI_MISC                0x8bcU          /* bit 0: DBI_RO_WR_EN */
#define DBI_ATU_VIEWPORT        0x900U
#define DBI_ATU_CR1             0x904U
#define DBI_ATU_CR2             0x908U
#define DBI_ATU_BASE            0x90cU
#define DBI_ATU_UBASE           0x910U
#define DBI_ATU_LIMIT           0x914U
#define DBI_ATU_TARGET          0x918U
#define DBI_ATU_UTARGET         0x91cU
#define DBI_AUX_CLK             0xb40U
#define DBI_L1SUB               0xb44U
#define ATU_ENABLE              (1U << 31)
#define ATU_TYPE_MEM            0U
#define ATU_TYPE_CFG0           4U

/* PMU, sysreg, CMU */
#define PMU_PCIE_PHY_CONTROL    0x718U          /* [hdr]122, the DT's pmu-offset; bit 0: 1 = PHY not isolated */
#define SYSREG_PHY_CONTROL      0x00cU          /* PCIE_WIFI0_PCIE_PHY_CONTROL [hdr]197 */
#define PCS_LANE0_ENABLE        (1U << 12)
#define TOP_MUX_HSI1_PCIE       0x1090U         /* [cmu]144 */
#define TOP_DIV_HSI1_PCIE       0x1888U         /* [cmu]1985 */
#define TOP_GATE_HSI1_PCIE      0x2094U         /* [cmu]852 */
#define HSI1_MUX_PCIE_USER      0x0620U         /* [cmu]267: bit 4 0 = OSCCLK, 1 = CLKCMU_HSI1_PCIE */
#define GATE_PHY_REFCLK         0x2000U         /* [cmu]1346..1355 */
#define GATE_DBI                0x2024U
#define GATE_PHY_APB            0x2028U
#define GATE_ELBI_APB           0x2030U         /* PCIE_SUB_CTRL: [rc]484 dumps ELBI as "SUB_CTRL" */
#define GATE_PCS_APB            0x2034U
#define GATE_MANUAL             (1U << 20)      /* [cmu]5091 */
#define GATE_CG_VAL             (1U << 21)      /* [cmu]5090 */
#define GATE_HWACG              (1U << 28)      /* ENABLE_AUTOMATIC_CLKGATING [cmu]5092 */

/* Pins: bank offsets as Samsung's pinctrl-exynos-arm64.c exynos9830_pin_banks; 4-bit PUD fields */
#define BANK_GPB0               0x0e0U          /* PERIC1 */
#define BANK_GPA0               0x000U          /* ALIVE */
#define BANK_GPA3               0x060U          /* ALIVE */
#define PIN_REG_ON              4U              /* gpb0-4 ([dt]dtbo 2307 wlan, dtb 1548 cfg_wlanen) */
#define PIN_HOST_WAKE           7U              /* gpa0-7 on y2s ([dt]dtb 67); gpa3-7 on z3s */
#define PIN_CLKREQ              0U              /* gpf0-0, function 2, pull-up ([dt]dtb 839) */
#define PIN_PERST               1U              /* gpf0-1, output, high = released ([dt]dtb 849) */

#define LINK_POLL_US            20
#define LINK_POLLS              25000U          /* 500 ms; [rc]1129 gives it 120 ms per try */

static volatile uint8_t *elbi, *phy, *pcs, *dbi, *sysreg, *pmu, *cmu_top, *cmu_hsi1, *cfg;
static volatile uint8_t *gpb0, *gpa0, *gpa3, *gpf0;
static uint32_t force;

/*
 * The PHY's register file for ch0 at a 26 MHz reference, Gen2: 48 common bytes at PHY + 4 * i,
 * 80 transceiver bytes at PHY + 4 * (0x30 + i) ([cal]169, [cal]177; [z3s]111 has the same tables).
 * Index 0x20 (0xc0) and 0x57 - 0x30 (0x7e) are also the "power down clear" values of [cal]104/107.
 */
static const uint8_t phy_cmn[48] = {
	0x01, 0xe1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0c, 0x71, 0x45, 0x65, 0x24, 0x33, 0x18, 0xe3, 0xfc,
	0xd8, 0x05, 0xe6, 0x80, 0x00, 0x00, 0x00, 0x00, 0x60, 0x11, 0x00, 0xa0, 0x05, 0x04, 0x18, 0x88,
	0xc0, 0xff, 0x9b, 0x52, 0x22, 0x30, 0x4f, 0xdc, 0x40, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x80,
};
static const uint8_t phy_trsv[80] = {
	0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xc0, 0xff, 0xff, 0x3f, 0x8c, 0xc8, 0x02, 0x01, 0x88, 0x80,
	0x06, 0x90, 0x6c, 0x66, 0x09, 0x61, 0x42, 0x44, 0xc6, 0x50, 0x0a, 0x33, 0x58, 0xe7, 0x20, 0x22,
	0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x55, 0x15, 0xac, 0xaa, 0x3e, 0x00,
	0x00, 0x00, 0x20, 0x3f, 0x00, 0x03, 0x40, 0x00, 0x10, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* What the look logs of each block */
static const uint16_t elbi_regs[] = { 0x000, 0x00c, 0x018, 0x02c, 0x074, 0x088, 0x0f4, 0x1b8, 0x1c4, 0x1d0, 0x274,
	                              0x288, 0x28c, 0x290, 0x294, 0x2c8 };
static const uint16_t phy_regs[] = { 0x000, 0x004, 0x040, 0x080, 0x0bc, 0x0c0, 0x0d8, 0x15c, 0x1fc };
static const uint16_t pcs_regs[] = { 0x00c, 0x0d0, 0x0ec, 0x0f8, 0x100, 0x104, 0x150, 0x154 };
static const uint16_t dbi_regs[] = { 0x000, 0x004, 0x008, 0x018, 0x07c, 0x080, 0x0a0, 0x710, 0x80c, 0x8bc, 0x8d0,
	                             0x900 };
static const uint16_t sysreg_regs[] = { 0x0, 0x4, 0x8, 0xc };
static const uint16_t top_regs[] = { TOP_MUX_HSI1_PCIE, TOP_DIV_HSI1_PCIE, TOP_GATE_HSI1_PCIE };
/* the user mux, the gates of [cmu]1337..1355 and the Q-channels of [cmu]2513..2518 */
static const uint16_t hsi1_regs[] = { 0x0620, 0x2000, 0x2024, 0x2028, 0x202c, 0x2030, 0x2034, 0x2038, 0x3000, 0x303c,
	                              0x3040, 0x3044, 0x3048, 0x304c };
static const uint16_t hsi1_gates[] = { 0x2000, 0x2024, 0x2028, 0x202c, 0x2030, 0x2034, 0x2038 };

#define N(a)    ((unsigned)(sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------------------------ */

static void wlog(const char *format, ...) __printflike(1, 2);

static void
wlog(const char *format, ...)
{
	char line[320];
	va_list list;
	size_t len;

	strlcpy(line, "wifi: ", sizeof(line));
	va_start(list, format);
	vsnprintf(line + 6, sizeof(line) - 7, format, list);
	va_end(list);
	len = strlen(line);
	line[len] = '\n';
	line[len + 1] = '\0';
	loader_log_line(line);
}

static inline uint32_t
rd(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(volatile uint8_t *base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline void
set_clr(volatile uint8_t *base, uint32_t off, uint32_t clear, uint32_t set)
{
	wr(base, off, (rd(base, off) & ~clear) | set);
}

/* One line for a block: the registers, and whether it answers at all. */
static void
dump(const char *name, volatile uint8_t *base, const uint16_t *offs, unsigned n)
{
	char line[256];
	size_t at;
	uint32_t v, any = 0, all = ~0U;
	unsigned i;

	at = (size_t)snprintf(line, sizeof(line), "%s", name);
	for (i = 0; i < n && at < sizeof(line); i++) {
		v = rd(base, offs[i]);
		any |= v;
		all &= v;
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %x=%08x", (unsigned)offs[i], v);
	}
	wlog("%s%s", line, any == 0 ? "  [all zero: clock gated, or D-TZPC secure]" : all == ~0U ? "  [all ones: no answer]" : "");
}

/*
 * The look at a block, unless Samsung would not touch it now: its PHY isolated by the PMU, or the
 * block's clock forced off (MANUAL set, CG_VAL clear) in CMU_HSI1.
 */
static void
dump_if(const char *name, volatile uint8_t *base, const uint16_t *offs, unsigned n, uint32_t gate, const char *unsafe)
{
	uint32_t g = rd(cmu_hsi1, gate);

	if (unsafe == NULL && (g & GATE_MANUAL) && !(g & GATE_CG_VAL)) {
		unsafe = "its clock is gated off by hand";
	}
	if (unsafe != NULL && !force) {
		wlog("%s not read: %s (wifi_pcie_force=1 reads it anyway)", name, unsafe);
		return;
	}
	dump(name, base, offs, n);
}

static uint32_t
pin_con(volatile uint8_t *bank, uint32_t pin)
{
	return (rd(bank, REDWOOD_GPIO_CON) >> (pin * 4)) & 0xf;
}

/* The level first, then the function: an output never shows a wrong level, not even briefly. */
static void
pin_out(volatile uint8_t *bank, uint32_t pin, uint32_t level)
{
	set_clr(bank, REDWOOD_GPIO_DAT, 1U << pin, (level & 1) << pin);
	redwood_gpio_func(bank, pin, REDWOOD_GPIO_OUTPUT);
}

static uint32_t
ltssm(void)
{
	return rd(elbi, ELBI_RDLH_LINKUP) & 0x1f;
}

/* ------------------------------------------------------------------------ */
/* W2a: the look (only reads)                                                */

static void
look(void)
{
	uint32_t pmu_phy = rd(pmu, PMU_PCIE_PHY_CONTROL), lane0 = rd(sysreg, SYSREG_PHY_CONTROL) & PCS_LANE0_ENABLE;
	const char *isolated = (pmu_phy & 1) ? NULL : "the PMU isolates the PHY (PCIE_PHY_CONTROL bit 0 is 0)";

	wlog("pmu PCIE_PHY_CONTROL(15860718)=%08x: PHY %s", pmu_phy, isolated ? "isolated" : "not isolated");
	dump("cmu_top hsi1_pcie mux/div/gate", cmu_top, top_regs, N(top_regs));
	dump("cmu_hsi1 pcie mux/gates/qch", cmu_hsi1, hsi1_regs, N(hsi1_regs));
	dump("sysreg(13021044)", sysreg, sysreg_regs, N(sysreg_regs));
	wlog("gpio REG_ON gpb0-4 con %x dat %u pud %x | host-wake gpa0-7 con %x dat %u, gpa3-7 con %x dat %u | "
	    "gpf0-0 CLKREQ con %x, gpf0-1 PERST# con %x dat %u", pin_con(gpb0, PIN_REG_ON), redwood_gpio_get(gpb0, PIN_REG_ON),
	    (rd(gpb0, REDWOOD_GPIO_PUD) >> (PIN_REG_ON * 4)) & 0xf, pin_con(gpa0, PIN_HOST_WAKE),
	    redwood_gpio_get(gpa0, PIN_HOST_WAKE), pin_con(gpa3, PIN_HOST_WAKE), redwood_gpio_get(gpa3, PIN_HOST_WAKE),
	    pin_con(gpf0, PIN_CLKREQ), pin_con(gpf0, PIN_PERST), redwood_gpio_get(gpf0, PIN_PERST));
	/* ELBI: Samsung's probe writes it before it lifts the isolation ([rc]2394 -> 1434), so it is safe */
	dump_if("elbi", elbi, elbi_regs, N(elbi_regs), GATE_ELBI_APB, NULL);
	dump_if("phy", phy, phy_regs, N(phy_regs), GATE_PHY_APB, isolated);
	dump_if("pcs", pcs, pcs_regs, N(pcs_regs), GATE_PCS_APB, isolated);
	/* the DBI: Samsung only with the isolation lifted and lane 0 on ([rc]1590..1626) */
	dump_if("dbi", dbi, dbi_regs, N(dbi_regs), GATE_DBI, isolated ? isolated : lane0 ? NULL : "lane 0 is off (sysreg+c bit 12)");
	if (!(rd(cmu_hsi1, GATE_ELBI_APB) & GATE_MANUAL) || (rd(cmu_hsi1, GATE_ELBI_APB) & GATE_CG_VAL) || force) {
		wlog("link: LTSSM %#x (0x11 = L0, 0 = detect quiet), LTSSM_ENABLE %u, D-state %u", ltssm(),
		    rd(elbi, ELBI_LTSSM_ENABLE) & 1, rd(elbi, ELBI_PM_DSTATE) & 7);
	}
}

/* ------------------------------------------------------------------------ */
/* W2b: the link                                                             */

/* exynos_pcie_phy_config() for EP_BCM_WIFI (the DT's ep-device-type 1, [dt]dtbo 8970) */
static void
phy_config(void)
{
	unsigned i;

	/* PCS_HIGH_SPEED [cal]189; PHY mode bits [7:2] = 0x3 << 2, bit 1 clear [cal]190; lane 0 on [cal]195 */
	set_clr(sysreg, 0x0, 0, 1U << 1);
	set_clr(sysreg, SYSREG_PHY_CONTROL, (0xfU << 4) | (0xfU << 2) | (1U << 1), 0x3U << 2);
	set_clr(sysreg, SYSREG_PHY_CONTROL, 0, PCS_LANE0_ENABLE);

	/* pcs_g_rst pulsed 1-0-1 [cal]200 */
	wr(elbi, ELBI_PCS_G_RST, 1);
	delay(10);
	wr(elbi, ELBI_PCS_G_RST, 0);
	delay(10);
	wr(elbi, ELBI_PCS_G_RST, 1);
	delay(10);

	/* the PHY's tables [cal]225 */
	for (i = 0; i < N(phy_cmn); i++) {
		wr(phy, 4 * i, phy_cmn[i]);
	}
	for (i = 0; i < N(phy_trsv); i++) {
		wr(phy, 4 * (0x30 + i), phy_trsv[i]);
	}

	/* tx latency, reference clock out to the endpoint (0x87; 0x89 is Qualcomm's), PRGM_TIMEOUT_L1SS [cal]247..264 */
	wr(pcs, PCS_TX_LATENCY, 0x70);
	wr(pcs, PCS_REFCLK_OUT, 0x87);
	wr(pcs, PCS_104, 0x50);
	set_clr(pcs, PCS_L1SS_CTRL, 1U << 1, 1U << 4);

	/* the MAC, then PCS and PMA common, pulsed 1-0-1 [cal]290, [cal]298 */
	wr(elbi, ELBI_MAC_RST, 1);
	delay(10);
	wr(elbi, ELBI_MAC_RST, 0);
	delay(10);
	wr(elbi, ELBI_MAC_RST, 1);
	delay(10);
	wr(elbi, ELBI_CMN_RST, 1);
	delay(10);
	wr(elbi, ELBI_CMN_RST, 0);
	delay(10);
	wr(elbi, ELBI_CMN_RST, 1);
	delay(100);

	/* CDR reset: [7:6] = 3, 20 us, = 2, = 0 [cal]306 */
	set_clr(pcs, PCS_CDR, 0, 0x3U << 6);
	delay(20);
	set_clr(pcs, PCS_CDR, 0x3U << 6, 0x2U << 6);
	set_clr(pcs, PCS_CDR, 0x3U << 6, 0);

	wlog("step phy: sysreg %08x %08x; phy 0=%02x 4=%02x 80=%02x 15c=%02x (want 01 e1 c0 7e); pcs 100=%02x (want 87)",
	    rd(sysreg, 0), rd(sysreg, SYSREG_PHY_CONTROL), rd(phy, 0) & 0xff, rd(phy, 4) & 0xff, rd(phy, 0x80) & 0xff,
	    rd(phy, 0x15c) & 0xff, rd(pcs, PCS_REFCLK_OUT) & 0xff);
}

/* The controller, then the root port's own configuration: false if the DBI does not answer. */
static bool
core_config(void)
{
	uint32_t id;

	/* a clean DesignWare core after the PHY: soft core reset 0, 20 us, 1 [rc]1064, [z3s]366 */
	wr(elbi, ELBI_SOFT_CORE_RESET, 0);
	delay(20);
	wr(elbi, ELBI_SOFT_CORE_RESET, 1);
	/* L1 exit by hardware, L1 NAK [rc]1077; link-down reset by hand [rc]1082 */
	set_clr(elbi, ELBI_APP_REQ_EXIT_L1, 0, 0x1U | (0x1U << 4));
	wr(elbi, ELBI_LINKDOWN_RST_SEL, 1U << 1);
	/* Q-channel: no clock gating of AXI, APB, PMU while the link trains [rc]1085 (ip-ver 0x983000), [z3s]370 */
	set_clr(elbi, ELBI_QCH_SEL, 0xfffU, 0);
	/* bus number enable: config TLPs may go out [rc]1428 */
	set_clr(elbi, ELBI_SW_WAKE, 1U << 1, 0);
	/* the LTSSM history buffer, for the failure report [rc]2837..2844 */
	wr(elbi, ELBI_HISTORY_CHECK, 0);
	wr(elbi, ELBI_HISTORY_CHECK, 3);
	wr(elbi, ELBI_STATE_POWER_S, 0x200000);
	wr(elbi, ELBI_STATE_POWER_M, 0xffffffff);

	/* Samsung reaches the DBI of a link that is down only with RX electrical idle ignored [rc]1058, [cal]33 */
	set_clr(pcs, PCS_ELECIDLE, 0, 1U << 3);
	id = rd(dbi, DBI_ID);
	if (id == 0 || id == ~0U) {
		set_clr(pcs, PCS_ELECIDLE, 1U << 3, 0);
		wlog("step dbi: the root port's id reads %08x: the DBI does not answer", id);
		return false;
	}
	/* dw_pcie_setup(): one lane [dwc]354, [dwc]376 */
	set_clr(dbi, DBI_PORT_LINK, 0x3fU << 16, 0x1U << 16);
	set_clr(dbi, DBI_WIDTH_SPEED, 0x1fU << 8, 0x1U << 8);
	/* dw_pcie_setup_rc(): no BARs, INTA, buses 0/1/0xff, IO+MEM+MASTER+SERR [dw]679..702 */
	wr(dbi, DBI_MISC, 1);
	wr(dbi, DBI_BAR0, 0);
	wr(dbi, DBI_BAR1, 0);
	set_clr(dbi, DBI_INTPIN, 0xff00U, 0x0100U);
	set_clr(dbi, DBI_BUSES, 0x00ffffffU, 0x00ff0100U);
	set_clr(dbi, DBI_CMD, 0xffffU, 0x0107U);
	/* a PCI-PCI bridge [dw]728, Samsung's ids 144d:ecec [rc]904 (a check that DBI writes land) */
	set_clr(dbi, DBI_CLASS, 0xffff0000U, 0x06040000U);
	wr(dbi, DBI_ID, 0xecec144dU);
	/* directed speed change [dw]734 */
	set_clr(dbi, DBI_WIDTH_SPEED, 0, 1U << 17);
	/*
	 * exynos_pcie_setup_rc() [rc]893: LNKCAP L1 exit 64 us, x1, 5 GT/s [rc]909; aux clock 26 MHz [rc]921;
	 * L1.2 timings [rc]925; PMCSR D0 [rc]928; retrain [rc]931; target speed = the DT's max-link-speed, Gen1 [rc]936
	 */
	set_clr(dbi, DBI_LNKCAP, (0x7U << 15) | (0x3fU << 4) | 0xfU, (0x7U << 15) | (0x1U << 4) | 0x2U);
	wr(dbi, DBI_AUX_CLK, 0x1a);
	wr(dbi, DBI_L1SUB, 0xea);
	wr(dbi, DBI_PMCSR, 0);
	set_clr(dbi, DBI_LNKCTL, 0, 1U << 5);
	set_clr(dbi, DBI_LNKCTL2, 0xfU, 0x1U);
	set_clr(pcs, PCS_ELECIDLE, 1U << 3, 0);        /* [rc]1118 */
	wlog("step dbi: root port id %08x -> %08x (want ecec144d), class %08x, lnkcap %08x, lnkctl2 %08x", id,
	    rd(dbi, DBI_ID), rd(dbi, DBI_CLASS), rd(dbi, DBI_LNKCAP), rd(dbi, DBI_LNKCTL2));
	return true;
}

/* PERST# released, 20 ms for the chip, LTSSM on, then L0 or 500 ms. The states it went through are logged. */
static bool
train(unsigned attempt)
{
	char line[160];
	uint8_t path[40];
	unsigned npath = 0, i, j;
	uint32_t s, last = 0xff;
	size_t at = 0;
	bool up = false;

	pin_out(gpf0, PIN_PERST, 1);                     /* [rc]1071, [z3s]250: 18..20 ms */
	delay(20000);
	wr(elbi, ELBI_LTSSM_ENABLE, 1);                  /* [rc]1125 */
	for (i = 0; i < LINK_POLLS; i++) {
		s = ltssm();
		if (s != last && npath < sizeof(path)) {
			path[npath++] = (uint8_t)s;
		}
		last = s;
		/* up: L0 or a state only reachable from it, 0x0d..0x14 [rc]1132, [z3s]348 */
		if (s >= 0x0d && s <= 0x14) {
			up = true;
			break;
		}
		delay(LINK_POLL_US);
	}
	for (j = 0; j < npath && at < sizeof(line); j++) {
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %02x", (unsigned)path[j]);
	}
	line[at < sizeof(line) ? at : sizeof(line) - 1] = '\0';
	wlog("ltssm (try %u): %s after %u us; path:%s", attempt, up ? "UP" : "not up", i * LINK_POLL_US, line);
	return up;
}

/* exynos_pcie_print_link_history() [rc]875: the hardware's last 32 LTSSM states, oldest first */
static void
history(void)
{
	char line[160];
	size_t at = 0;
	int i;

	for (i = 31; i >= 0 && at < sizeof(line); i--) {
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %02x", (rd(elbi, ELBI_HISTORY((uint32_t)i)) >> 16) & 0x3f);
	}
	line[at < sizeof(line) ? at : sizeof(line) - 1] = '\0';
	wlog("ltssm history:%s", line);
}

/* An outbound iATU region in viewport mode ([rc]962 CFG0 in region 1, [rc]1020 MEM in region 0). */
static void
atu(uint32_t region, uint32_t type, uint32_t base, uint32_t size, uint32_t target)
{
	wr(dbi, DBI_ATU_VIEWPORT, region);
	wr(dbi, DBI_ATU_CR1, type);
	wr(dbi, DBI_ATU_BASE, base);
	wr(dbi, DBI_ATU_UBASE, 0);
	wr(dbi, DBI_ATU_LIMIT, base + size - 1);
	wr(dbi, DBI_ATU_TARGET, target);
	wr(dbi, DBI_ATU_UTARGET, 0);
	wr(dbi, DBI_ATU_CR2, ATU_ENABLE);
}

/* A BAR's size, by writing all ones and reading back (the endpoint's memory decode is off). */
static uint64_t
bar_size(uint32_t off, bool *wide)
{
	uint32_t lo = rd(cfg, off), hi, mlo, mhi = ~0U;
	uint64_t mask;

	*wide = (lo & 0x7) == 0x4;
	wr(cfg, off, ~0U);
	mlo = rd(cfg, off);
	wr(cfg, off, lo);
	if (*wide) {
		hi = rd(cfg, off + 4);
		wr(cfg, off + 4, ~0U);
		mhi = rd(cfg, off + 4);
		wr(cfg, off + 4, hi);
	}
	if ((mlo & ~0xfU) == 0 && (!*wide || mhi == 0)) {
		return 0;               /* not implemented */
	}
	mask = ((uint64_t)mhi << 32) | (mlo & ~0xfU);
	return ~mask + 1;
}

/* After a failure: LTSSM off and PERST# low as exynos_pcie_poweroff() [rc]2958..2962, then WLAN_REG_ON low. */
static void
stop(const char *why, bool trained)
{
	wlog("W2b FAIL: %s; LTSSM %#x. Stopping: LTSSM off, PERST# low, WLAN_REG_ON low", why, ltssm());
	if (trained) {
		history();
		dump("elbi", elbi, elbi_regs, N(elbi_regs));
		dump("pcs", pcs, pcs_regs, N(pcs_regs));
	}
	wr(elbi, ELBI_LTSSM_ENABLE, 0);
	pin_out(gpf0, PIN_PERST, 0);
	pin_out(gpb0, PIN_REG_ON, 0);
}

static bool
link_up(void)
{
	uint32_t v, id = ~0U, cls, i;
	uint64_t bar0, bar2;
	bool up = false, wide0, wide2;
	unsigned attempt;

	/*
	 * Pins, as the controller's "active" state ([dt]dtbo 8964): PERST# an output, low (asserted) while
	 * the chip powers up; CLKREQ# function 2 with a pull-up (4-bit PUD field).
	 */
	pin_out(gpf0, PIN_PERST, 0);
	redwood_gpio_func(gpf0, PIN_CLKREQ, 2);
	set_clr(gpf0, REDWOOD_GPIO_PUD, 0xfU << (PIN_CLKREQ * 4), 0x3U << (PIN_CLKREQ * 4));

	/*
	 * WLAN_REG_ON: low for 20 ms (a chip S-Boot may have left on starts from a real power-on), high,
	 * then 200 ms as bcmdhd's WIFI_TURNON_DELAY before the link is touched [dhd]73, [dhd]192.
	 */
	pin_out(gpb0, PIN_REG_ON, 0);
	delay(20000);
	pin_out(gpb0, PIN_REG_ON, 1);
	delay(200000);
	wlog("step pins: PERST# con %x dat %u, CLKREQ con %x; WLAN_REG_ON con %x dat %u, 200 ms waited; host-wake gpa0-7 dat %u",
	    pin_con(gpf0, PIN_PERST), redwood_gpio_get(gpf0, PIN_PERST), pin_con(gpf0, PIN_CLKREQ), pin_con(gpb0, PIN_REG_ON),
	    redwood_gpio_get(gpb0, PIN_REG_ON), redwood_gpio_get(gpa0, PIN_HOST_WAKE));

	/*
	 * Clocks, as z3s's exynos990_pcie_enable_clocks() [z3s]93: CMU_TOP's CLKCMU_HSI1_PCIE gate on (the
	 * clock its DT gives the controller), CMU_HSI1's PCIe user mux to that clock, then every PCIe gate of
	 * CMU_HSI1. A gate is forced on as mainline's exynos_arm64_init_clocks() leaves them: MANUAL set,
	 * auto gating clear, CG_VAL set ([cmu]5090..5092; z3s sets CG_VAL only, which does nothing while
	 * MANUAL is clear and the hardware gates the clock by itself).
	 */
	set_clr(cmu_top, TOP_GATE_HSI1_PCIE, GATE_HWACG, GATE_MANUAL | GATE_CG_VAL);
	set_clr(cmu_hsi1, HSI1_MUX_PCIE_USER, 0, 1U << 4);
	for (i = 0; i < 100 && (rd(cmu_hsi1, HSI1_MUX_PCIE_USER) & (1U << 16)); i++) {     /* BUSY [cmu]3527 */
		delay(10);
	}
	for (i = 0; i < N(hsi1_gates); i++) {
		set_clr(cmu_hsi1, hsi1_gates[i], GATE_HWACG, GATE_MANUAL | GATE_CG_VAL);
	}
	dump("step clocks: cmu_top", cmu_top, top_regs, N(top_regs));
	dump("step clocks: cmu_hsi1", cmu_hsi1, hsi1_regs, N(hsi1_regs));

	/* PHY isolation off [rc]2817 (poweron), [z3s]135 */
	set_clr(pmu, PMU_PCIE_PHY_CONTROL, 0, 1);
	delay(100);
	wlog("step pmu: PCIE_PHY_CONTROL %08x (want bit 0 set)", rd(pmu, PMU_PCIE_PHY_CONTROL));

	/*
	 * Two tries, as Samsung retries (up to 10, [rc]1140): PHY calibration, controller and root port,
	 * then PERST# released and the LTSSM let go; between them PERST# low and the LTSSM off [rc]1146.
	 */
	for (attempt = 1; attempt <= 2 && !up; attempt++) {
		phy_config();
		if (!core_config()) {
			stop("the DBI does not answer", false);
			return false;
		}
		up = train(attempt);
		if (!up && attempt < 2) {
			wr(elbi, ELBI_LTSSM_ENABLE, 0);
			pin_out(gpf0, PIN_PERST, 0);
			delay(10000);
		}
	}
	if (!up) {
		stop("no link", true);
		return false;
	}

	v = rd(dbi, DBI_LNKCTL) >> 16;
	wlog("link up: LTSSM %#x, Gen%u x%u (want Gen1 x1, the DT's max-link-speed)", ltssm(), v & 0xf, (v >> 4) & 0x3f);
	/* stale interrupt pulses cleared [rc]1171 */
	wr(elbi, ELBI_IRQ_PULSE, rd(elbi, ELBI_IRQ_PULSE));

	/* bus 1 device 0 function 0 behind CFG0 in region 1, the memory window in region 0 [rc]1178 */
	atu(1, ATU_TYPE_CFG0, CFG_PHYS, 0x1000, 1U << 24);
	atu(0, ATU_TYPE_MEM, MEM_PHYS, MEM_SIZE, MEM_PHYS);
	wr(dbi, DBI_ATU_VIEWPORT, 1);
	v = rd(dbi, DBI_ATU_CR2);
	wlog("atu: region 1 CR2 %08x (want 80000000), CR1 %08x, base %08x, target %08x", v, rd(dbi, DBI_ATU_CR1),
	    rd(dbi, DBI_ATU_BASE), rd(dbi, DBI_ATU_TARGET));
	if (v != ATU_ENABLE) {
		stop("the iATU does not take the CFG0 region (viewport mode absent?)", true);
		return false;
	}

	/*
	 * Configuration requests: 100 ms after the link is up (PCIe base spec 6.6.1, for a port of 5 GT/s
	 * or less); then the id, again every 10 ms for up to 1 s while it reads all ones or as a
	 * configuration retry (CRS, 0x0001 for the vendor).
	 */
	delay(100000);
	for (i = 0; i < 100; i++) {
		id = rd(cfg, 0x00);
		if (id != ~0U && (id & 0xffff) != 0x0001) {
			break;
		}
		delay(10000);
	}
	cls = rd(cfg, 0x08);
	wlog("endpoint 01:00.0: id %04x:%04x (want 14e4:4475), class %06x rev %02x, header %02x, subsystem %08x, cmd/status %08x",
	    id & 0xffff, id >> 16, cls >> 8, cls & 0xff, (rd(cfg, 0x0c) >> 16) & 0x7f, rd(cfg, 0x2c), rd(cfg, 0x04));
	if (id != 0x447514e4U) {
		stop(id == ~0U ? "the endpoint does not answer configuration reads" : "not the BCM4375 (14e4:4475)", true);
		return false;
	}
	bar0 = bar_size(0x10, &wide0);
	bar2 = bar_size(0x18, &wide2);
	bcm_bus.bar0_size = bar0;
	bcm_bus.bar2_size = bar2;
	bcm_bus.bar0_wide = wide0;
	bcm_bus.bar2_wide = wide2;
	wlog("endpoint BARs: BAR0 %s %llu KB, BAR2 %s %llu KB", wide0 ? "64-bit" : "32-bit", bar0 >> 10,
	    wide2 ? "64-bit" : "32-bit", bar2 >> 10);
	dump("dbi", dbi, dbi_regs, N(dbi_regs));
	wlog("W2b PASS: link up, BCM4375 14e4:4475 answers; link, clocks and WLAN_REG_ON left on");
	return true;
}

/* ------------------------------------------------------------------------ */
/* W3: memory the chip may reach                                             */

/*
 * The DMA pool the loader set aside below 4 GB (/chosen wifi-dma-base, wifi-dma-size), mapped
 * normal non-cacheable (write combined: the driver memcpy()s through it, which device memory
 * would fault on unaligned). 0 when there is one.
 *
 * Only a window of the pool is mapped into kernel VA, from kernel_map (ml_io_map_unmappable): the
 * 8 MB io_submap behind ml_io_map()/ml_io_map_wcomb() cannot take it (kmem_alloc ... failed with 3
 * @vm_kern.c:178). The S2MPU grants below walk the pool by physical address, not the mapping.
 */
#define POOL_MAP_MAX            (4UL * 1024 * 1024)

static uint64_t pool_phys;
static vm_offset_t pool_virt;
static size_t pool_size;
static size_t pool_mapped;
static bool pool_granted;

int
redwood_wifi_dma_pool(uint64_t *phys, vm_offset_t *virt, size_t *size)
{
	if (pool_virt == 0) {
		DTEntry chosen;
		void const *base = NULL, *bytes = NULL;
		unsigned int base_size = 0, bytes_size = 0;

		if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
		    SecureDTGetProperty(chosen, "wifi-dma-base", &base, &base_size) != kSuccess || base_size != 8 ||
		    SecureDTGetProperty(chosen, "wifi-dma-size", &bytes, &bytes_size) != kSuccess || bytes_size != 8) {
			return -1;
		}
		memcpy(&pool_phys, base, 8);
		memcpy(&pool_size, bytes, 8);
		pool_mapped = pool_size < POOL_MAP_MAX ? pool_size : POOL_MAP_MAX;
		pool_virt = ml_io_map_unmappable((vm_offset_t)pool_phys, pool_mapped, VM_WIMG_WCOMB);
		if (pool_virt == 0) {
			return -1;
		}
	}
	*phys = pool_phys;
	*virt = pool_virt;
	*size = pool_size;
	return pool_granted ? 0 : 1;
}

/* Samsung's hypervisor: the PCIe block (HSI1, S2MPU index 24, VID 1) may reach a 64 KB granule, or not ([s2mpu]) */
#define HVC_S2MPU_WIFI          0xc6000101UL
#define S2MPU_HSI1              ((1UL << 16) | 24UL)
#define S2MPU_GRANULE           0x10000UL
#define S2MPU_READ_WRITE        3UL

static uint64_t
hvc5(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	register uint64_t x4 __asm__("x4") = a4;

	__asm__ volatile ("dsb sy\n\thvc #0\n\tdsb sy" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4) : :
            "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* The whole pool granted to the PCIe block, once: the driver's rings and buffers all come from it. */
static void
grant_pool(void)
{
	uint64_t phys, first = ~0ULL, ret;
	vm_offset_t virt;
	size_t size;
	uint32_t ok = 0, failed = 0;

	if (redwood_wifi_dma_pool(&phys, &virt, &size) < 0) {
		wlog("W3: no DMA pool from the loader (/chosen wifi-dma-base): no grants");
		return;
	}
	for (uint64_t at = phys; at < phys + size; at += S2MPU_GRANULE) {
		ret = hvc5(HVC_S2MPU_WIFI, S2MPU_HSI1, at, S2MPU_GRANULE, S2MPU_READ_WRITE);
		if (ret == 0) {
			ok++;
		} else {
			failed++;
			first = first == ~0ULL ? ret : first;
		}
	}
	pool_granted = failed == 0;
	wlog("W3 %s: DMA pool %llx size %zx (mapped at %lx): %u granules granted, %u refused (first answer %#llx)",
	    failed ? "FAIL" : "PASS", phys, size, (unsigned long)virt, ok, failed, first == ~0ULL ? 0 : first);
}

static void
pcie_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t mode = 0, wait = 20;

	PE_parse_boot_argn("wifi_pcie", &mode, sizeof(mode));
	PE_parse_boot_argn("wifi_pcie_force", &force, sizeof(force));
	PE_parse_boot_argn("wifi_pcie_wait", &wait, sizeof(wait));
	elbi = (volatile uint8_t *)ml_io_map(ELBI_PHYS, 0x1000);
	phy = (volatile uint8_t *)ml_io_map(PHY_PHYS, 0x1000);
	pcs = (volatile uint8_t *)ml_io_map(PCS_PHYS, 0x1000);
	dbi = (volatile uint8_t *)ml_io_map(DBI_PHYS, 0x1000);
	sysreg = (volatile uint8_t *)ml_io_map(SYSREG_PHYS, 0x10);
	pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x1000);
	cmu_top = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x3000);
	cmu_hsi1 = (volatile uint8_t *)ml_io_map(CMU_HSI1_PHYS, 0x4000);
	cfg = (volatile uint8_t *)ml_io_map(CFG_PHYS, 0x1000);
	gpb0 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, BANK_GPB0);
	gpa0 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, BANK_GPA0);
	gpa3 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, BANK_GPA3);
	gpf0 = redwood_gpio_bank(PINCTRL_HSI1, 0);

	wlog("wifi_pcie=%u: PCIe ch0 (BCM4375): %s%s", mode, mode == 1 ? "W2a, the look (read-only)" :
	    mode == 2 ? "W2b, link up" : mode == 3 ? "W2b + W3, link up and DMA grants" :
	    "W2b + W3 + W4/W5, link up, DMA grants, the firmware and a scan", force ? ", wifi_pcie_force=1" : "");
	{
		uint64_t phys;
		vm_offset_t virt;
		size_t size;

		if (redwood_wifi_dma_pool(&phys, &virt, &size) < 0) {
			wlog("dma pool: none from the loader");
		} else {
			wlog("dma pool: %llx size %zx, mapped at %lx", phys, size, (unsigned long)virt);
		}
	}
	look();
	if (mode >= 2) {
		/*
		 * The link once the system is up (boot-arg wifi_pcie_wait=S, default 20 s): HSI1 also carries the
		 * UFS the system is read from, and should the link stall it, the run can still be ended and its
		 * log kept (Vol-Down + Power) rather than the boot stopping without a word.
		 */
		wlog("link up in %u s (wifi_pcie_wait)", wait);
		delay_for_interval(wait, NSEC_PER_SEC);
		if (link_up() && mode >= 3) {
			grant_pool();
			if (mode >= 4 && pool_granted) {
				bcm_bus.cfg = cfg;
				bcm_bus.dbi = dbi;
				bcm_bus.mem_phys = MEM_PHYS;
				bcm_bus.mem_size = MEM_SIZE;
				bcm_bus.pool_phys = pool_phys;
				bcm_bus.pool_virt = pool_virt;
				bcm_bus.pool_mapped = pool_mapped;
				redwood_bcm4375_run(&bcm_bus);
			} else if (mode >= 4) {
				wlog("W4 not tried: the DMA pool is not granted");
			}
		}
	}
	wlog("done");
}

void
redwood_exynos_pcie_init(void)
{
	uint32_t mode = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi_pcie", &mode, sizeof(mode)) || mode < 1 || mode > 4) {
		return;
	}
	if (kernel_thread_start(pcie_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
