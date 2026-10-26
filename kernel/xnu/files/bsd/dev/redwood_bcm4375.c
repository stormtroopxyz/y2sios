/*
 * Wi-Fi of the Galaxy S20+ (y2s), steps W4 and W5 of PLANS/06-wireless.md, boot-arg wifi_pcie=4: the
 * Broadcom BCM4375 B1's firmware started and spoken to, as far as a scan. redwood_exynos_pcie.c has
 * the link up and the loader's DMA pool granted to the PCIe block when this runs; this is everything
 * after: the BARs, the chip's cores, the firmware and NVRAM into its RAM, the shared area, the five
 * common message rings, the firmware's version and address, the CLM blob, and an escan whose results
 * go to the log.
 *
 * It follows Linux's brcmfmac step for step -- the driver that runs this very chip on the same SoC
 * (z3s, the S20 Ultra) -- with the repository's three BCM4375 patches (wifi/lx/patches/brcmfmac):
 * [pcie] pcie.c, [chip] chip.c, [msgbuf] msgbuf.c, [ring] commonring.c, [cfg] cfg80211.c, [fw]
 * firmware.c, [common] common.c, [types] fwil_types.h, [fweh] fweh.h, all of Linux 7.0.14
 * (xnu-deps/build/brcmfmac/src/brcmfmac). Nothing is driven by interrupts: one thread does every step
 * and looks at the completion rings itself.
 *
 * Files, from /fw on the root (userland/build.sh, from ../fw-wifi): bcm4375-sta.bin (the vendor's
 * bcmdhd_sta.bin_b1, 18.41.117), bcm4375-clm.blob, and the NVRAM: bcm4375-nvram-semco.txt (SEMCO's
 * "nvram_190424_FW18.35.346_Y2_EU", Y2 being this phone) unless boot-arg wifi_nvram=murata.
 *
 * Every line goes to the loader's log, "wifi: ...". Every wait is bounded.
 *
 * W6: then it stays, as the station (sys/redwood_sta.h: scan, join through the firmware's own WPA2
 * supplicant, leave) and as the link behind en0 (sys/redwood_net.h: one TX flow ring, RX completions to
 * the stack). From then on several threads use the rings -- en0's (its poll() and transmit()), the
 * station's, and anyone waiting for an ioctl -- so the rings take ring_lock, ioctls one at a time
 * ioctl_lock, and one thread at a time looks at the completion rings (poll_lock).
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/redwood_net.h>
#include <sys/redwood_sta.h>
#include <sys/systm.h>
#include <vm/memory_types.h>

#include "lx_wpa.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void loader_log_line(const char *line);
extern const uint8_t *redwood_adsp_fw_file(const char *name, uint64_t *size);
extern void redwood_adsp_fw_free(const uint8_t *data, uint64_t size);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern bool redwood_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);

/* what redwood_exynos_pcie.c hands over */
struct redwood_bcm_bus {
	volatile uint8_t *cfg;          /* the endpoint's configuration space (the iATU's CFG0 window) */
	volatile uint8_t *dbi;          /* the root port's */
	uint32_t mem_phys, mem_size;    /* the outbound memory window, PCI address = CPU address */
	uint64_t bar0_size, bar2_size;
	bool bar0_wide, bar2_wide;
	uint64_t pool_phys;             /* the DMA pool, granted to the PCIe block */
	vm_offset_t pool_virt;          /* ... its first pool_mapped bytes, write-combined */
	size_t pool_mapped;
};
void redwood_bcm4375_run(const struct redwood_bcm_bus *bus);

/* ------------------------------------------------------------------------ */
/* Constants                                                                */

#define CFG_BAR0_WINDOW         0x80            /* [pcie]124 */
#define BAR0_REG_SIZE           0x1000
#define BAR0_MAP                0x8000          /* BRCMF_PCIE_REG_MAP_SIZE */

#define ENUM_BASE               0x18000000U     /* SI_ENUM_BASE_DEFAULT */
#define CC_EROMPTR              0xfc
#define CID_ID(x)               ((x) & 0xffff)
#define CID_REV(x)              (((x) >> 16) & 0xf)

#define CORE_CC                 0x800           /* BCMA_CORE_* */
#define CORE_80211              0x812
#define CORE_PCIE2              0x83c
#define CORE_ARM_CR4            0x83e

#define BCMA_IOCTL              0x0408
#define  BCMA_IOCTL_CLK         0x0001
#define  BCMA_IOCTL_FGC         0x0002
#define BCMA_RESET_CTL          0x0800
#define  BCMA_RESET_CTL_RESET   0x0001
#define ARMCR4_CPUHALT          0x0020          /* [chip]78 */
#define D11_PHYCLOCKEN          0x0004
#define D11_PHYRESET            0x0008
#define ARMCR4_CAP              0x04
#define ARMCR4_BANKIDX          0x40
#define ARMCR4_BANKINFO         0x44

#define RAMBASE_4375            0x170000        /* [chip]741 */
#define RAMSIZE_MAGIC           0x534d4152      /* "SMAR" at 0x6c of the image: the RAM it was built for [pcie]288 */
#define RAMSIZE_OFFSET          0x6c

/* the PCIe2 core's registers, through the BAR0 window on that core [pcie]103..152 */
#define PCIE2_CONFIGADDR        0x120
#define PCIE2_CONFIGDATA        0x124

/* shared area [pcie]139..249 */
#define SH_MIN_VERSION          5
#define SH_MAX_VERSION          7
#define SH_DMA_INDEX            0x10000
#define SH_DMA_2B_IDX           0x100000
#define SH_HOSTRDY_DB1          0x10000000
#define SH_CONSOLE              20
#define SH_HTOD_MB_DATA         40
#define SH_DTOH_MB_DATA         44
#define SH_MAX_RXBUFPOST        34
#define SH_RX_DATAOFFSET        36
#define SH_RING_INFO            48
#define SH_SCRATCH_LEN          52
#define SH_SCRATCH_ADDR         56
#define SH_RINGUPD_LEN          64
#define SH_RINGUPD_ADDR         68
#define SH_HOST_CAP             84              /* 0003-bcm4375-host-cap.patch */
#define SH_ASSERT               0x10            /* PCIE_SHARED_ASSERT (bcmdhd) */
#define SH_TRAP                 0x400           /* PCIE_SHARED_TRAP */
#define CFG_SBMBX               0x98            /* BRCMF_PCIE_REG_SBMBX */
#define D2H_DEV_DS_ENTER_REQ    0x2
#define D2H_DEV_FWHALT          0x10000000
#define H2D_HOST_DS_ACK         0x2
#define SEED_BYTES              128             /* dhdpcie_wrt_rnd(): BCM_ENTROPY_HOST_NBYTES */
#define SEED_MAGIC              0xfeedc0deU
#define CONSOLE_BUFADDR         8
#define CONSOLE_BUFSIZE         12
#define CONSOLE_WRITEIDX        16

/* rings [pcie]410..433, msgbuf.h */
#define RING_CTRL_SUBMIT        0
#define RING_RXPOST_SUBMIT      1
#define RING_CTRL_COMPLETE      2
#define RING_TX_COMPLETE        3
#define RING_RX_COMPLETE        4
#define RINGS                   5
#define RING_MEM_SZ             16
static const uint16_t ring_items[RINGS] = { 64, 1024, 64, 1024, 1024 };
static const uint16_t ring_item_len[RINGS] = { 40, 32, 24, 24, 40 };            /* shared version 7 */
static const uint16_t ring_item_len_pre7[RINGS] = { 40, 32, 24, 16, 32 };

/* message types [msgbuf] */
#define MSG_GEN_STATUS          0x1
#define MSG_RING_STATUS         0x2
#define MSG_FLOW_CREATE         0x3
#define MSG_FLOW_CREATE_CMPLT   0x4
#define MSG_FLOW_DELETE         0x5
#define MSG_FLOW_DELETE_CMPLT   0x6
#define MSG_FLOW_FLUSH_CMPLT    0x8
#define MSG_IOCTLPTR_REQ        0x9
#define MSG_IOCTLPTR_REQ_ACK    0xa
#define MSG_IOCTLRESP_BUF_POST  0xb
#define MSG_IOCTL_CMPLT         0xc
#define MSG_EVENT_BUF_POST      0xd
#define MSG_WL_EVENT            0xe
#define MSG_TX_POST             0xf
#define MSG_TX_STATUS           0x10
#define MSG_RXBUF_POST          0x11
#define MSG_RX_CMPLT            0x12
#define IOCTL_REQ_PKTID         0xfffe
#define IOCTL_MAX_IN            1518            /* BRCMF_TX_IOCTL_MAX_MSG_SIZE */

/* firmware commands [fwil.h] and events [fweh.h] */
#define WLC_GET_VERSION         1
#define WLC_UP                  2
#define WLC_DOWN                3
#define WLC_SET_INFRA           20
#define WLC_SET_AUTH            22
#define WLC_GET_BSSID           23
#define WLC_SET_SSID            26
#define WLC_DISASSOC            52
#define WLC_GET_VAR             262
#define WLC_SET_VAR             263
#define WLC_SET_WSEC_PMK        268
#define E_SET_SSID              0
#define E_AUTH                  3
#define E_DEAUTH                5
#define E_DEAUTH_IND            6
#define E_ASSOC                 7
#define E_REASSOC               9
#define E_DISASSOC              11
#define E_DISASSOC_IND          12
#define E_LINK                  16
#define E_PSK_SUP               46
#define E_IF                    54
#define E_ESCAN_RESULT          69
#define E_STATUS_SUCCESS        0
#define E_STATUS_PARTIAL        8
#define E_FLAG_LINK             0x01            /* BRCMF_EVENT_MSG_LINK */
#define FWSUP_COMPLETED         6               /* BRCMF_E_STATUS_FWSUP_COMPLETED */
#define FWSUP_TIMEOUT           7
#define AES_ENABLED             0x0004          /* wlioctl_defs.h */
#define WPA2_AUTH_PSK           0x0080
#define WPA2_AUTH_PSK_SHA256    0x8000
#define WPA3_AUTH_SAE_PSK       0x40000
#define AUTH_OPEN               0               /* the "auth" iovar: brcmf_set_auth_type() [cfg]2000 */
#define AUTH_SAE                3
#define MFP_NONE                0               /* BRCMF_MFP_* */
#define MFP_CAPABLE             1
#define MFP_REQUIRED            2
#define AKM_PSK                 (1u << 2)       /* RSN AKM suites 00-0f-ac:N, a bit each */
#define AKM_PSK_SHA256          (1u << 6)
#define AKM_SAE                 (1u << 8)
#define AKM_FT_SAE              (1u << 9)
#define RSN_CAP_MFPR            0x0040
#define RSN_CAP_MFPC            0x0080
#define WSEC_PASSPHRASE         1
#define PMK_STRUCT_SIZE         264
#define WSEC_KEY_SIZE           164             /* struct brcmf_wsec_key_le */
#define CRYPTO_ALGO_AES_CCM     4
#define KEY_PRIMARY             0x2             /* BRCMF_PRIMARY_KEY */
#define ETHERTYPE_EAPOL_HI      0x88
#define ETHERTYPE_EAPOL_LO      0x8e
#define EAPOL_QUEUE             4
#define EAPOL_MOST              512             /* wsec_pmk_t of bcmdhd 101.16 (256-byte key): the larger of the two */

/* buffers in the pool */
#define CTL_BUF_SIZE            8192            /* BRCMF_MSGBUF_MAX_CTL_PKT_SIZE */
#define IOCTLRESP_BUFS          8
#define EVENT_BUFS              8
#define RX_BUF_SIZE             2048
#define RX_BUFS                 256             /* brcmfmac fills the ring up to max_rxbufpost (511) */
#define CTL_BUFS                (IOCTLRESP_BUFS + EVENT_BUFS)
#define PKTID_CTL(i)            (1 + (i))       /* request ids: control buffers, then rx buffers */
#define PKTID_RX(i)             (1 + CTL_BUFS + (i))
#define TX_BUFS                 64
#define TX_BUF_SIZE             2048
#define PKTID_TX(i)             (1 + CTL_BUFS + RX_BUFS + (i))
#define FLOW_ITEMS              512             /* BRCMF_H2D_TXFLOWRING_MAX_ITEM */
#define FLOW_ITEM_LEN           48              /* BRCMF_H2D_TXFLOWRING_ITEMSIZE */
#define FLOW_RING_ID            2               /* flow ring 0: BRCMF_H2D_MSGRING_FLOWRING_IDSTART */
#define FOUND_MOST              64

/* ------------------------------------------------------------------------ */
/* State                                                                    */

struct core {
	uint16_t id;
	uint8_t rev;
	uint32_t base, wrap;
};

struct ring {
	uint8_t *buf;                   /* in the pool */
	uint64_t phys;
	uint16_t depth, item_len;
	uint16_t w_ptr, r_ptr;
	uint32_t w_idx_addr, r_idx_addr;
};

static const struct redwood_bcm_bus *bus;
static volatile uint8_t *regs, *tcm, *cfg;
static uint32_t tcm_size;               /* how much of BAR2 is mapped */
static uint32_t win = ~0U;
static struct core cores[48];
static unsigned ncores;
static struct core *cc, *pcie2, *cr4;
static uint32_t ramsize;
static uint32_t mb_int, mb_mask, h2d_mb0, h2d_mb1;
static uint8_t *pool;                   /* bump allocator over the mapped part of the pool */
static size_t pool_used;
static uint32_t shared_addr, shared_flags, shared_version, rx_dataoffset, max_rxbufpost, console_addr;
static uint32_t htod_mb_addr, dtoh_mb_addr;
static uint32_t console_buf, console_size, console_read;
static unsigned dma_idx_sz;
static uint8_t *idxbuf;
static uint64_t idxbuf_phys;
static struct ring rings[RINGS];
static uint8_t *ioctl_in;
static uint64_t ioctl_in_phys;
static uint8_t *ctl_buf[CTL_BUFS], *rx_buf[RX_BUFS];
static uint64_t ctl_phys[CTL_BUFS], rx_phys[RX_BUFS];
static uint16_t ioctl_trans;
static volatile bool ioctl_done;
static int ioctl_status;
static uint32_t ioctl_resp_len, ioctl_resp_id;
static uint32_t rx_frames, events_seen, tx_frames, tx_dropped;
static volatile bool escan_done;
static int escan_status;
static bool scan_v2;
static struct redwood_sta_bss *found;   /* what the last escan heard (FOUND_MOST of them) */
static unsigned nfound;
static uint8_t mac[6];

static LCK_GRP_DECLARE(bcm_lck_grp, "redwood-bcm4375");
static LCK_MTX_DECLARE(ring_lock, &bcm_lck_grp);       /* the rings, the buffers, what the completions say */
static LCK_MTX_DECLARE(ioctl_lock, &bcm_lck_grp);      /* one ioctl at a time */
static LCK_MTX_DECLARE(poll_lock, &bcm_lck_grp);       /* one thread at a time at the completion rings */
static LCK_MTX_DECLARE(scan_lock, &bcm_lck_grp);       /* one escan at a time (the boot's and wifid's) */
static LCK_SPIN_DECLARE(state_lock, &bcm_lck_grp);     /* sta_state, for state() (which may not sleep) */

/* the indices of the H2D rings (the flow ring's follow the two common ones) */
static uint32_t h2d_w_base, h2d_r_base, idx_step;

/* the TX flow ring and its buffers */
static struct ring flow;
static uint8_t *tx_buf[TX_BUFS];
static uint64_t tx_phys[TX_BUFS];
static uint64_t tx_busy;                /* a bit a buffer the firmware has */
static volatile bool flow_up, flow_answered;
static int flow_status;

/* received frames, handed to en0 once the ring lock is let go */
static struct { uint16_t buf, offset, length; } rxq[RX_BUFS];
static unsigned rxq_n;

/* what the events said of a join */
static volatile uint32_t ev_ssid_status, ev_psk_status, ev_down_reason;
static volatile bool ev_ssid, ev_link_up, ev_link_down, ev_psk;
static volatile int lost_pending;       /* the network joined is gone: the errno, for poll() to pass on */

static bool link_attached;
static bool mb_enabled;
static unsigned rx_logged, tx_logged;

/*
 * The host's WPA2 supplicant (this firmware has none: "sup_wpa" is unsupported, as on Samsung's Android,
 * where wpa_supplicant does it): the four-way handshake's arithmetic is lx_wpa.c's; the EAPOL frames come
 * in with the received ones (bcm_poll() keeps them here) and go out on the flow ring; the keys go to the
 * firmware with "wsec_key".
 */
static struct lx_wpa wpa;
static uint8_t wpa_rsn[64];
static volatile unsigned wpa_rsn_length;
static volatile bool wpa_on, wpa_done, wpa_failed;
static const char *wpa_why;
static struct { uint16_t length; uint8_t frame[EAPOL_MOST]; } eapol_q[EAPOL_QUEUE];
static unsigned eapol_n;
static thread_t ioctl_owner, eapol_owner;
static LCK_MTX_DECLARE(eapol_lock, &bcm_lck_grp);      /* one thread at a time answers the handshake */
static bool tx_raw(const uint8_t *frame, size_t length, unsigned *slot_out);
static int key_install(const uint8_t *key, unsigned length, unsigned index, const uint8_t *peer, const uint8_t *rsc);
static struct redwood_sta_state sta_state;

/* ------------------------------------------------------------------------ */
/* Log, registers, the backplane                                            */

static void blog(const char *format, ...) __printflike(1, 2);

static void
blog(const char *format, ...)
{
	char line[300];
	va_list ap;
	size_t len;

	strlcpy(line, "wifi: ", sizeof(line));
	va_start(ap, format);
	vsnprintf(line + 6, sizeof(line) - 7, format, ap);
	va_end(ap);
	len = strlen(line);
	line[len] = '\n';
	line[len + 1] = '\0';
	loader_log_line(line);
}

static inline uint32_t
r32(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
w32(volatile uint8_t *base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint16_t
r16(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint16_t *)(base + off);
}

static inline void
w16(volatile uint8_t *base, uint32_t off, uint16_t value)
{
	*(volatile uint16_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* BAR0's 4 KB window onto the backplane [pcie]1869 */
static uint32_t
window(uint32_t addr)
{
	uint32_t base = addr & ~(BAR0_REG_SIZE - 1);

	if (base != win) {
		w32(cfg, CFG_BAR0_WINDOW, base);
		if (r32(cfg, CFG_BAR0_WINDOW) != base) {
			w32(cfg, CFG_BAR0_WINDOW, base);
		}
		win = base;
	}
	return addr & (BAR0_REG_SIZE - 1);
}

static uint32_t
bp_read(uint32_t addr)
{
	uint32_t off = window(addr);

	return r32(regs, off);
}

static void
bp_write(uint32_t addr, uint32_t value)
{
	uint32_t off = window(addr);

	w32(regs, off, value);
}

/* a register of the PCIe2 core: the window on it [pcie]632 */
static uint32_t
pcie2_read(uint32_t off)
{
	return bp_read(pcie2->base + off);
}

static void
pcie2_write(uint32_t off, uint32_t value)
{
	bp_write(pcie2->base + off, value);
}

/* the chip's memory through BAR2, from its address 0 (the TCM: RAM from RAMBASE_4375) */
static inline uint32_t
tcm_r32(uint32_t addr)
{
	return r32(tcm, addr);
}

static inline void
tcm_w32(uint32_t addr, uint32_t value)
{
	w32(tcm, addr, value);
}

/* an address in the chip's RAM, within what is mapped of BAR2 (anything else would fault the kernel) */
static bool
in_ram(uint32_t addr, uint32_t len)
{
	return addr >= RAMBASE_4375 && addr + len >= addr && addr + len <= RAMBASE_4375 + ramsize && addr + len <= tcm_size;
}

static void *
pool_alloc(size_t size, uint64_t *phys)
{
	size_t at = (pool_used + 63) & ~(size_t)63;

	if (at + size > bus->pool_mapped) {
		return NULL;
	}
	pool_used = at + size;
	bzero(pool + at, size);
	*phys = bus->pool_phys + at;
	return pool + at;
}

/* ------------------------------------------------------------------------ */
/* The chip: its cores (EROM), the ARM, its RAM                             */

#define DMP_DESC_TYPE_MSK       0xf
#define DMP_DESC_VALID          0x1
#define DMP_DESC_COMPONENT      0x1
#define DMP_DESC_MASTER_PORT    0x3
#define DMP_DESC_ADDRESS        0x5
#define DMP_DESC_ADDRSIZE_GT32  0x8
#define DMP_DESC_EOT            0xf

static uint32_t
erom_desc(uint32_t *at, uint8_t *type)
{
	uint32_t v = bp_read(*at);

	*at += 4;
	if (type != NULL) {
		*type = v & DMP_DESC_TYPE_MSK;
		if ((*type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS) {
			*type = DMP_DESC_ADDRESS;
		}
	}
	return v;
}

/* brcmf_chip_dmp_get_regaddr() [chip]834 */
static bool
erom_regaddr(uint32_t *at, uint32_t *base, uint32_t *wrap)
{
	uint8_t desc, wraptype;
	uint32_t v, guard;

	*base = *wrap = 0;
	v = erom_desc(at, &desc);
	if (desc == DMP_DESC_MASTER_PORT) {
		wraptype = 3;                   /* MWRAP */
	} else if (desc == DMP_DESC_ADDRESS) {
		*at -= 4;
		wraptype = 2;                   /* SWRAP */
	} else {
		*at -= 4;
		return false;
	}
	for (guard = 0; guard < 64 && (*base == 0 || *wrap == 0); guard++) {
		uint32_t g2 = 0;

		do {
			v = erom_desc(at, &desc);
			if (desc == DMP_DESC_EOT) {
				*at -= 4;
				return false;
			}
		} while (desc != DMP_DESC_ADDRESS && desc != DMP_DESC_COMPONENT && ++g2 < 64);
		if (desc == DMP_DESC_COMPONENT) {
			*at -= 4;
			return true;
		}
		if (v & DMP_DESC_ADDRSIZE_GT32) {
			erom_desc(at, NULL);
		}
		uint32_t sztype = (v >> 4) & 3;

		if (sztype == 3) {
			uint32_t sz = erom_desc(at, NULL);

			if (sz & DMP_DESC_ADDRSIZE_GT32) {
				erom_desc(at, NULL);
			}
		}
		if (sztype != 0 && sztype != 1) {       /* 4 KB and 8 KB regions only */
			continue;
		}
		uint32_t stype = (v >> 6) & 3;

		if (*base == 0 && stype == 0) {
			*base = v & 0xfffff000U;
		}
		if (*wrap == 0 && stype == wraptype) {
			*wrap = v & 0xfffff000U;
		}
	}
	return *base != 0 && *wrap != 0;
}

/* brcmf_chip_dmp_erom_scan() [chip]906 */
static void
erom_scan(void)
{
	uint32_t at = bp_read(ENUM_BASE + CC_EROMPTR), v, base, wrap;
	uint8_t type = 0;
	unsigned guard;

	for (guard = 0; guard < 1024 && type != DMP_DESC_EOT && ncores < sizeof(cores) / sizeof(cores[0]); guard++) {
		v = erom_desc(&at, &type);
		if (!(v & DMP_DESC_VALID) || type != DMP_DESC_COMPONENT) {
			continue;
		}
		uint16_t id = (uint16_t)((v >> 8) & 0xfff);

		v = erom_desc(&at, &type);
		if ((v & DMP_DESC_TYPE_MSK) != DMP_DESC_COMPONENT) {
			break;
		}
		uint32_t nmw = (v >> 14) & 0x1f, nsw = (v >> 19) & 0x1f;
		uint8_t rev = (uint8_t)(v >> 24);

		if (nmw + nsw == 0 && id != 0x827 /* PMU */ && id != 0x840 /* GCI */) {
			continue;
		}
		if (!erom_regaddr(&at, &base, &wrap)) {
			continue;
		}
		cores[ncores].id = id;
		cores[ncores].rev = rev;
		cores[ncores].base = base;
		cores[ncores].wrap = wrap;
		ncores++;
	}
}

static struct core *
core_find(uint16_t id, unsigned unit)
{
	for (unsigned i = 0; i < ncores; i++) {
		if (cores[i].id == id && unit-- == 0) {
			return &cores[i];
		}
	}
	return NULL;
}

/* brcmf_chip_ai_coredisable() [chip]350 */
static void
core_disable(struct core *c, uint32_t prereset, uint32_t reset)
{
	if ((bp_read(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET) == 0) {
		bp_write(c->wrap + BCMA_IOCTL, prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
		(void)bp_read(c->wrap + BCMA_IOCTL);
		bp_write(c->wrap + BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);
		delay(20);
		for (unsigned i = 0; i < 300 && bp_read(c->wrap + BCMA_RESET_CTL) != BCMA_RESET_CTL_RESET; i++) {
			delay(1);
		}
	}
	bp_write(c->wrap + BCMA_IOCTL, reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
	(void)bp_read(c->wrap + BCMA_IOCTL);
}

/* brcmf_chip_ai_resetcore() [chip]434 (a core that is not a D11 one) */
static void
core_reset(struct core *c, uint32_t prereset, uint32_t reset, uint32_t postreset)
{
	core_disable(c, prereset, reset);
	for (unsigned i = 0; i < 50 && (bp_read(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET); i++) {
		bp_write(c->wrap + BCMA_RESET_CTL, 0);
		delay(50);
	}
	bp_write(c->wrap + BCMA_IOCTL, postreset | BCMA_IOCTL_CLK);
	(void)bp_read(c->wrap + BCMA_IOCTL);
}

/* brcmf_chip_tcm_ramsize() [chip]680 */
static uint32_t
cr4_ramsize(void)
{
	uint32_t cap = bp_read(cr4->base + ARMCR4_CAP), size = 0;
	uint32_t banks = (cap & 0xf) + ((cap >> 4) & 0xf);

	for (uint32_t i = 0; i < banks; i++) {
		bp_write(cr4->base + ARMCR4_BANKIDX, i);
		uint32_t info = bp_read(cr4->base + ARMCR4_BANKINFO), block = 8192;

		if (info & 0x200) {
			block >>= 3;
		}
		size += ((info & 0x7f) + 1) * block;
	}
	return size;
}

/* brcmf_chip_cr4_set_passive() [chip]1323: the ARM halted, the D11 cores disabled */
static void
cr4_passive(void)
{
	uint32_t keep = bp_read(cr4->wrap + BCMA_IOCTL) & ARMCR4_CPUHALT;
	struct core *d11;

	core_reset(cr4, keep, ARMCR4_CPUHALT, ARMCR4_CPUHALT);
	for (unsigned i = 0; (d11 = core_find(CORE_80211, i)) != NULL; i++) {
		core_disable(d11, D11_PHYRESET | D11_PHYCLOCKEN, D11_PHYCLOCKEN);
	}
}

/* brcmf_chip_cr4_set_active() [chip]1340, brcmf_pcie_buscore_activate() [pcie]1929 */
static void
cr4_active(uint32_t rstvec)
{
	tcm_w32(0, rstvec);
	core_reset(cr4, ARMCR4_CPUHALT, 0, 0);
}

/* ------------------------------------------------------------------------ */
/* The firmware console                                                     */

static void
console_dump(const char *when)
{
	static char line[200];
	static size_t at;
	uint32_t write_idx, n = 0;

	if (console_addr == 0 || console_size == 0 || !in_ram(console_buf, console_size) ||
	    !in_ram(console_addr, CONSOLE_WRITEIDX + 4)) {
		return;
	}
	write_idx = tcm_r32(console_addr + CONSOLE_WRITEIDX);
	while (console_read != write_idx && n++ < 16384) {
		uint8_t ch = tcm[console_buf + console_read];

		console_read = console_read + 1 == console_size ? 0 : console_read + 1;
		if (ch == '\r') {
			continue;
		}
		if (ch != '\n' && at < sizeof(line) - 1) {
			line[at++] = (char)ch;
			continue;
		}
		line[at] = '\0';
		if (at != 0) {
			blog("console%s%s: %s", when ? " " : "", when ? when : "", line);
		}
		at = 0;
	}
}

/* ------------------------------------------------------------------------ */
/* Rings [ring], [pcie]1028..1110                                           */

static uint16_t
idx_read(uint32_t addr)
{
	if (dma_idx_sz == 0) {
		return r16(tcm, addr);
	}
	return dma_idx_sz == 2 ? *(volatile uint16_t *)(idxbuf + addr) : (uint16_t)*(volatile uint32_t *)(idxbuf + addr);
}

static void
idx_write(uint32_t addr, uint16_t value)
{
	if (dma_idx_sz == 0) {
		w16(tcm, addr, value);
	} else if (dma_idx_sz == 2) {
		*(volatile uint16_t *)(idxbuf + addr) = value;
	} else {
		*(volatile uint32_t *)(idxbuf + addr) = value;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
ring_bell(void)
{
	pcie2_write(h2d_mb0, 1);        /* "any arbitrary value will do" [pcie]1074 */
}

static void *
ring_reserve(struct ring *r)
{
	for (int retry = 0; retry < 2; retry++) {
		uint16_t avail = r->r_ptr <= r->w_ptr ? r->depth - r->w_ptr + r->r_ptr : r->r_ptr - r->w_ptr;

		if (avail > 1) {
			uint8_t *at = r->buf + (size_t)r->w_ptr * r->item_len;

			r->w_ptr = r->w_ptr + 1 == r->depth ? 0 : r->w_ptr + 1;
			bzero(at, r->item_len);
			return at;
		}
		r->r_ptr = idx_read(r->r_idx_addr);
	}
	return NULL;
}

static void
ring_commit(struct ring *r)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	idx_write(r->w_idx_addr, r->w_ptr);
	ring_bell();
}

/* brcmf_pcie_init_ringbuffers() [pcie]1226, brcmf_pcie_alloc_dma_and_ring() [pcie]1140 */
static bool
rings_init(void)
{
	uint32_t ri = tcm_r32(shared_addr + SH_RING_INFO);

	if (!in_ram(ri, 60)) {
		blog("W4 FAIL: ring info address %#x is outside RAM", ri);
		return false;
	}
	uint32_t ringmem = tcm_r32(ri + 0);
	uint32_t h2d_w = tcm_r32(ri + 4), h2d_r = tcm_r32(ri + 8), d2h_w = tcm_r32(ri + 12), d2h_r = tcm_r32(ri + 16);
	uint16_t max_flow = r16(tcm, ri + 52), max_sub = r16(tcm, ri + 54), max_cmpl = r16(tcm, ri + 56);
	uint32_t step = 4;

	if (shared_version < 6) {
		max_sub = max_flow;
		max_flow = (uint16_t)(max_sub - 2);
		max_cmpl = 3;
	}
	blog("ring info at %#x: ring memory %#x, indices h2d w %#x r %#x d2h w %#x r %#x; %u flow, %u submission, %u completion rings",
	    ri, ringmem, h2d_w, h2d_r, d2h_w, d2h_r, max_flow, max_sub, max_cmpl);
	if (max_flow > 512 || max_sub < 2 || max_cmpl < 3) {
		blog("W4 FAIL: the ring counts make no sense");
		return false;
	}
	if (!in_ram(ringmem, RINGS * RING_MEM_SZ) ||
	    (dma_idx_sz == 0 && (!in_ram(h2d_w, 3 * 4) || !in_ram(h2d_r, 3 * 4) || !in_ram(d2h_w, 3 * 4) || !in_ram(d2h_r, 3 * 4)))) {
		blog("W4 FAIL: ring memory or TCM indices outside RAM");
		return false;
	}
	if (dma_idx_sz != 0) {
		/* the indices in our memory, the firmware told where [pcie]1280 */
		size_t size = ((size_t)max_sub + max_cmpl) * dma_idx_sz * 2;
		uint64_t at;

		idxbuf = pool_alloc(size, &idxbuf_phys);
		if (idxbuf == NULL) {
			blog("W4 FAIL: no pool for the index buffer");
			return false;
		}
		step = dma_idx_sz;
		h2d_w = 0;
		h2d_r = h2d_w + max_sub * dma_idx_sz;
		d2h_w = h2d_r + max_sub * dma_idx_sz;
		d2h_r = d2h_w + max_cmpl * dma_idx_sz;
		at = idxbuf_phys + h2d_w;
		tcm_w32(ri + 20, (uint32_t)at);
		tcm_w32(ri + 24, (uint32_t)(at >> 32));
		at = idxbuf_phys + h2d_r;
		tcm_w32(ri + 28, (uint32_t)at);
		tcm_w32(ri + 32, (uint32_t)(at >> 32));
		at = idxbuf_phys + d2h_w;
		tcm_w32(ri + 36, (uint32_t)at);
		tcm_w32(ri + 40, (uint32_t)(at >> 32));
		at = idxbuf_phys + d2h_r;
		tcm_w32(ri + 44, (uint32_t)at);
		tcm_w32(ri + 48, (uint32_t)(at >> 32));
	}
	h2d_w_base = h2d_w;
	h2d_r_base = h2d_r;
	idx_step = step;
	for (unsigned i = 0; i < RINGS; i++) {
		struct ring *r = &rings[i];
		uint32_t mem = ringmem + i * RING_MEM_SZ;

		r->depth = ring_items[i];
		r->item_len = shared_version < 7 ? ring_item_len_pre7[i] : ring_item_len[i];
		r->buf = pool_alloc((size_t)r->depth * r->item_len, &r->phys);
		if (r->buf == NULL) {
			blog("W4 FAIL: no pool for ring %u", i);
			return false;
		}
		tcm_w32(mem + 8, (uint32_t)r->phys);
		tcm_w32(mem + 12, (uint32_t)(r->phys >> 32));
		w16(tcm, mem + 4, r->depth);
		w16(tcm, mem + 6, r->item_len);
		if (i < 2) {
			r->w_idx_addr = h2d_w + i * step;
			r->r_idx_addr = h2d_r + i * step;
		} else {
			r->w_idx_addr = d2h_w + (i - 2) * step;
			r->r_idx_addr = d2h_r + (i - 2) * step;
		}
	}
	return true;
}

/* brcmf_pcie_init_scratchbuffers() [pcie]1406 */
static bool
scratch_init(void)
{
	uint64_t phys;

	if (pool_alloc(8, &phys) == NULL) {
		return false;
	}
	tcm_w32(shared_addr + SH_SCRATCH_ADDR, (uint32_t)phys);
	tcm_w32(shared_addr + SH_SCRATCH_ADDR + 4, (uint32_t)(phys >> 32));
	tcm_w32(shared_addr + SH_SCRATCH_LEN, 8);
	if (pool_alloc(1024, &phys) == NULL) {
		return false;
	}
	tcm_w32(shared_addr + SH_RINGUPD_ADDR, (uint32_t)phys);
	tcm_w32(shared_addr + SH_RINGUPD_ADDR + 4, (uint32_t)(phys >> 32));
	tcm_w32(shared_addr + SH_RINGUPD_LEN, 1024);
	return true;
}

/* ------------------------------------------------------------------------ */
/* Messages [msgbuf]                                                        */

static void
put_hdr(uint8_t *item, uint8_t type, uint32_t request_id)
{
	item[0] = type;
	item[1] = 0;            /* ifidx */
	item[2] = 0;
	item[3] = 0;
	memcpy(item + 4, &request_id, 4);
}

static void
put_addr(uint8_t *at, uint64_t phys)
{
	uint32_t lo = (uint32_t)phys, hi = (uint32_t)(phys >> 32);

	memcpy(at, &lo, 4);
	memcpy(at + 4, &hi, 4);
}

/* brcmf_msgbuf_rxbuf_ctrl_post() [msgbuf]1010: one ioctl response or event buffer */
static bool
post_ctl(unsigned i)
{
	uint8_t *item = ring_reserve(&rings[RING_CTRL_SUBMIT]);
	uint16_t len = CTL_BUF_SIZE;

	if (item == NULL) {
		return false;
	}
	put_hdr(item, i < IOCTLRESP_BUFS ? MSG_IOCTLRESP_BUF_POST : MSG_EVENT_BUF_POST, PKTID_CTL(i));
	memcpy(item + 8, &len, 2);
	put_addr(item + 16, ctl_phys[i]);
	ring_commit(&rings[RING_CTRL_SUBMIT]);
	return true;
}

/* brcmf_msgbuf_rxbuf_data_post(): one receive buffer, struct msgbuf_rx_bufpost */
static bool
post_rx(unsigned i)
{
	uint8_t *item = ring_reserve(&rings[RING_RXPOST_SUBMIT]);
	uint16_t len = RX_BUF_SIZE;

	if (item == NULL) {
		return false;
	}
	put_hdr(item, MSG_RXBUF_POST, PKTID_RX(i));
	memcpy(item + 10, &len, 2);
	put_addr(item + 24, rx_phys[i]);
	ring_commit(&rings[RING_RXPOST_SUBMIT]);
	return true;
}

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* MHz of a BSS's control channel, by the band its chanspec is in (d11ac chanspecs: 2G 0, 6G 0x8000, 5G 0xc000) */
static uint16_t
chan_mhz(uint8_t channel, uint16_t chanspec)
{
	switch (chanspec & 0xc000) {
	case 0x0000:
		return channel == 14 ? 2484 : (uint16_t)(2407 + 5 * channel);
	case 0x8000:
		return (uint16_t)(5950 + 5 * channel);
	default:
		return (uint16_t)(5000 + 5 * channel);
	}
}

/* ... and a 20 MHz chanspec of a frequency (0: not one of ours) */
static uint16_t
mhz_chanspec(uint16_t mhz)
{
	if (mhz == 2484) {
		return 0x1000 | 14;
	}
	if (mhz >= 2412 && mhz <= 2472) {
		return (uint16_t)(0x1000 | ((mhz - 2407) / 5));
	}
	if (mhz >= 5160 && mhz <= 5885) {
		return (uint16_t)(0xd000 | ((mhz - 5000) / 5));
	}
	return 0;
}

/*
 * One BSS of an escan result: brcmf_bss_info_le [types]314, natural alignment: version 0, length 4,
 * BSSID 8, beacon_period 14, capability 16, SSID_len 18, SSID 19, chanspec 72, RSSI 78, ctl_ch 88,
 * ie_offset 116, ie_length 120. Kept with its information elements (Apple80211 reads the network's
 * protection out of them).
 */
static void
heard_one(const uint8_t *bi, uint32_t avail)
{
	uint32_t length = le32(bi + 4), ie_offset, ie_length;
	struct redwood_sta_bss *b = NULL;
	int16_t rssi;

	if (found == NULL || length < 126 || length > avail) {
		return;
	}
	rssi = (int16_t)le16(bi + 78);
	for (unsigned i = 0; i < nfound; i++) {
		if (memcmp(found[i].bssid, bi + 8, 6) == 0) {
			if (rssi <= found[i].rssi) {
				return;
			}
			b = &found[i];
			break;
		}
	}
	if (b == NULL) {
		if (nfound >= FOUND_MOST) {
			return;
		}
		b = &found[nfound++];
	}
	bzero(b, __builtin_offsetof(struct redwood_sta_bss, elements));
	memcpy(b->bssid, bi + 8, 6);
	b->beacon_interval = le16(bi + 14);
	b->capability = le16(bi + 16);
	b->ssid_length = bi[18] > 32 ? 32 : bi[18];
	memcpy(b->ssid, bi + 19, b->ssid_length);
	b->rssi = rssi;
	b->frequency = chan_mhz(bi[88] != 0 ? bi[88] : (uint8_t)(le16(bi + 72) & 0xff), le16(bi + 72));
	ie_offset = le16(bi + 116);
	ie_length = le32(bi + 120);
	if (ie_offset >= 126 && ie_offset <= length && ie_length <= length - ie_offset) {
		b->elements_length = (uint16_t)(ie_length > REDWOOD_STA_ELEMENTS_MOST ? REDWOOD_STA_ELEMENTS_MOST : ie_length);
		memcpy(b->elements, bi + ie_offset, b->elements_length);
	}
}

/* brcmf_fweh_process_event(): an Ethernet frame of type 0x886c, Broadcom's event header (big-endian) [fweh]215..248 */
static void
event(const uint8_t *frame, uint32_t len)
{
	if (len < 72 || frame[12] != 0x88 || frame[13] != 0x6c) {
		return;
	}
	uint16_t flags = (uint16_t)(frame[26] << 8 | frame[27]);
	uint32_t type = be32(frame + 28), status = be32(frame + 32), reason = be32(frame + 36), datalen = be32(frame + 44);
	const uint8_t *data = frame + 72;

	events_seen++;
	if (datalen > len - 72) {
		datalen = len - 72;
	}
	if (type == E_ESCAN_RESULT) {
		if (status == E_STATUS_PARTIAL && datalen >= 12 + 126) {
			/* brcmf_escan_result_le: buflen, version, sync_id, bss_count, then the BSSes */
			uint16_t count = le16(data + 10);
			const uint8_t *bi = data + 12;
			uint32_t left = datalen - 12;

			for (unsigned i = 0; i < count && left >= 126; i++) {
				uint32_t l = le32(bi + 4);

				heard_one(bi, left);
				if (l == 0 || l > left) {
					break;
				}
				bi += l;
				left -= l;
			}
		} else if (status != E_STATUS_PARTIAL) {
			escan_status = (int)status;
			escan_done = true;
		}
		return;
	}
	if (type == 87 && wpa_on) {                     /* WLC_E_ASSOC_REQ_IE: the RSN element we associated with */
		for (uint32_t at = 0; at + 2 <= datalen && at + 2 + data[at + 1] <= datalen; at += 2 + data[at + 1]) {
			if (data[at] == 48 && 2u + data[at + 1] <= sizeof(wpa_rsn)) {
				memcpy(wpa_rsn, data + at, 2u + data[at + 1]);
				wpa_rsn_length = 2u + data[at + 1];
				break;
			}
		}
	}
	if (type == E_IF && datalen >= 5) {
		blog("event IF: ifidx %u action %u flags %u bsscfg %u role %u", data[0], data[1], data[2], data[3], data[4]);
	}
	switch (type) {
	case E_SET_SSID:
		ev_ssid_status = status;
		ev_ssid = true;
		break;
	case E_LINK:
		if (flags & E_FLAG_LINK) {
			ev_link_up = true;
		} else {
			ev_down_reason = reason;
			ev_link_down = true;
		}
		break;
	case E_PSK_SUP:
		ev_psk_status = status;
		ev_psk = true;
		break;
	case E_DEAUTH:
	case E_DEAUTH_IND:
	case E_DISASSOC:
	case E_DISASSOC_IND:
		ev_down_reason = reason;
		ev_link_down = true;
		break;
	}
	if ((type == E_LINK && !(flags & E_FLAG_LINK)) || type == E_DEAUTH || type == E_DEAUTH_IND || type == E_DISASSOC ||
	    type == E_DISASSOC_IND) {
		if (sta_state.phase == REDWOOD_STA_JOINED && lost_pending == 0) {
			lost_pending = ECONNRESET;
		}
	}
	if (events_seen <= 200) {
		blog("event %u status %u reason %u flags %#x, %u bytes", type, status, reason, flags, datalen);
	}
}

/* brcmf_msgbuf_process_msgtype() [msgbuf]1288. With ring_lock held. */
static void
process(const uint8_t *item)
{
	static unsigned odd;
	uint32_t id = le32(item + 4);

	switch (item[0]) {
	case MSG_IOCTL_CMPLT:
		/* struct msgbuf_ioctl_resp_hdr: status 8, ring 10, resp_len 12, trans_id 14, cmd 16 */
		if (le16(item + 14) != ioctl_trans || ioctl_done) {
			/* the answer to an ioctl that was given up on: its buffer back to the firmware */
			if (id >= 1 && id <= IOCTLRESP_BUFS) {
				post_ctl(id - 1);
			}
			break;
		}
		ioctl_status = (int16_t)le16(item + 8);
		ioctl_resp_len = le16(item + 12);
		ioctl_resp_id = id;
		ioctl_done = true;
		break;
	case MSG_WL_EVENT:
		if (id >= PKTID_CTL(IOCTLRESP_BUFS) && id < PKTID_CTL(CTL_BUFS)) {
			unsigned i = id - 1;
			uint16_t len = le16(item + 12);

			if (rx_dataoffset + len <= CTL_BUF_SIZE) {
				event(ctl_buf[i] + rx_dataoffset, len);
			}
			post_ctl(i);
		}
		break;
	case MSG_RX_CMPLT:
		/* struct msgbuf_rx_complete: metadata_len 12, data_len 14, data_offset 16, flags 18 */
		if (id >= PKTID_RX(0) && id < PKTID_RX(RX_BUFS)) {
			unsigned i = id - PKTID_RX(0);
			uint16_t len = le16(item + 14), off = le16(item + 16);

			if (off == 0) {
				off = (uint16_t)rx_dataoffset;
			}
			rx_frames++;
			if (rx_logged < 24) {
				const uint8_t *f = rx_buf[i] + off;

				rx_logged++;
				blog("rx %u: %u bytes at %u, flags %#x, status %d, ifidx %u, type %02x%02x", rx_frames, len, off, le16(item + 18),
				    (int16_t)le16(item + 8), item[1], (uint32_t)off + 14 <= RX_BUF_SIZE ? f[12] : 0,
				    (uint32_t)off + 14 <= RX_BUF_SIZE ? f[13] : 0);
			}
			if (link_attached && len >= 14 && (uint32_t)off + len <= RX_BUF_SIZE && rxq_n < RX_BUFS &&
			    (item[18] & 0x07) != 0x02) {
				rxq[rxq_n].buf = (uint16_t)i;
				rxq[rxq_n].offset = off;
				rxq[rxq_n].length = len;
				rxq_n++;
			} else {
				post_rx(i);
			}
		}
		break;
	case MSG_TX_STATUS:
		if (tx_logged < 16) {
			tx_logged++;
			blog("tx status: id %u, status %d", id, (int16_t)le16(item + 14));
		}
		if (id >= PKTID_TX(0) && id < PKTID_TX(TX_BUFS)) {
			tx_busy &= ~(1ULL << (id - PKTID_TX(0)));
		}
		break;
	case MSG_FLOW_CREATE_CMPLT:
	case MSG_FLOW_DELETE_CMPLT:
		/* struct msgbuf_flowring_create_resp: status 8, flow_ring_id 10 */
		flow_status = (int16_t)le16(item + 8);
		flow_answered = true;
		break;
	case MSG_GEN_STATUS:
	case MSG_RING_STATUS:
		if (le16(item + 8) != 0) {
			blog("firmware %s status %d (ring %u)", item[0] == MSG_GEN_STATUS ? "general" : "ring",
			    (int16_t)le16(item + 8), le16(item + 10));
		}
		break;
	case MSG_IOCTLPTR_REQ_ACK:
	case MSG_FLOW_FLUSH_CMPLT:
		break;
	default:
		if (odd++ < 16) {
			blog("message type %#x on a completion ring", item[0]);
		}
		break;
	}
}

/*
 * brcmf_pcie_handle_mb_data() [pcie]: the device's mailbox word. brcmfmac only looks at it for PCIe2 cores
 * before rev 64 (int_fn0 is 0 after), so deep sleep is acknowledged only there; a firmware halt is reported.
 */
static void
mb_poll(void)
{
	static unsigned logged;
	uint32_t v;

	if (mb_enabled) {
		uint32_t pending = pcie2_read(mb_int);

		if (pending != 0) {
			pcie2_write(mb_int, pending);           /* (as brcmf_pcie_isr_thread() acknowledges them) */
		}
	}
	if (dtoh_mb_addr == 0 || (v = tcm_r32(dtoh_mb_addr)) == 0) {
		return;
	}
	if (pcie2->rev < 64) {
		tcm_w32(dtoh_mb_addr, 0);
		if (v & D2H_DEV_DS_ENTER_REQ) {
			tcm_w32(htod_mb_addr, H2D_HOST_DS_ACK);
			w32(cfg, CFG_SBMBX, 1);
		}
	}
	if (logged < 4 || (v & D2H_DEV_FWHALT)) {
		logged++;
		blog("device mailbox data %#x%s", v, (v & D2H_DEV_FWHALT) ? ": the firmware halted" : "");
	}
}

/* every completion ring, once: brcmf_commonring_get_read_ptr()/read_complete() [ring]207..236 */
static unsigned
poll_rings(void)
{
	unsigned done = 0;

	mb_poll();

	for (unsigned i = RING_CTRL_COMPLETE; i < RINGS; i++) {
		struct ring *r = &rings[i];

		for (int pass = 0; pass < 2; pass++) {
			r->w_ptr = idx_read(r->w_idx_addr);
			if (r->w_ptr >= r->depth) {
				break;
			}
			uint16_t n = r->w_ptr >= r->r_ptr ? r->w_ptr - r->r_ptr : r->depth - r->r_ptr;

			if (n == 0) {
				break;
			}
			for (uint16_t k = 0; k < n; k++) {
				process(r->buf + (size_t)(r->r_ptr + k) * r->item_len);
			}
			r->r_ptr = (uint16_t)(r->r_ptr + n == r->depth ? 0 : r->r_ptr + n);
			idx_write(r->r_idx_addr, r->r_ptr);
			done += n;
		}
	}
	return done;
}

static unsigned bcm_poll(void);

/*
 * The EAPOL frames kept: each answered (lx_wpa_message()); after message 3 the reply (message 4) is let
 * go out before the keys are set (else the firmware would cipher it), then the pairwise key and the group
 * key go to the firmware. A group key handshake later sets the new group key.
 */
static void
eapol_handle(void)
{
	uint8_t frame[EAPOL_MOST], reply[14 + LX_WPA_REPLY_MOST];
	unsigned length, reply_length = 0, slot = 0;
	const char *why = NULL;
	int what;
	uint64_t deadline;

	if (eapol_owner == current_thread() || !lck_mtx_try_lock(&eapol_lock)) {
		return;
	}
	eapol_owner = current_thread();
	for (;;) {
		lck_spin_lock(&state_lock);
		if (eapol_n == 0) {
			lck_spin_unlock(&state_lock);
			break;
		}
		length = eapol_q[0].length;
		memcpy(frame, eapol_q[0].frame, length);
		memmove(&eapol_q[0], &eapol_q[1], sizeof(eapol_q[0]) * (EAPOL_QUEUE - 1));
		eapol_n--;
		lck_spin_unlock(&state_lock);
		if (!wpa_on || length <= 14) {
			continue;
		}
		what = lx_wpa_message(&wpa, frame + 14, length - 14, reply + 14, &reply_length, &why);
		blog("wpa: a key message of %u bytes: %s%s%s", length - 14, what == LX_WPA_ANSWER ? "message 1, answered" :
		    what == LX_WPA_KEYS ? "message 3, answered: the keys" : what == LX_WPA_GROUP ? "a new group key" :
		    what == LX_WPA_NOTHING ? "nothing to answer" : "REFUSED: ", what == LX_WPA_BAD && why ? why : "", "");
		if (what == LX_WPA_BAD) {
			if (!wpa.have_keys || (why != NULL && strncmp(why, "its signature", 13) == 0)) {
				wpa_why = why;
				wpa_failed = true;
			}
			continue;
		}
		if (what == LX_WPA_NOTHING || reply_length == 0) {
			continue;
		}
		memcpy(reply, frame + 6, 6);            /* to the access point */
		memcpy(reply + 6, mac, 6);
		reply[12] = ETHERTYPE_EAPOL_HI;
		reply[13] = ETHERTYPE_EAPOL_LO;
		if (!tx_raw(reply, 14 + reply_length, &slot)) {
			blog("wpa: the reply could not be sent");
			continue;
		}
		if (what == LX_WPA_ANSWER) {
			continue;
		}
		/* brcmf_netdev_wait_pend8021x(): the reply out before the keys are in */
		clock_interval_to_deadline(300, NSEC_PER_MSEC, &deadline);
		while ((tx_busy & (1ULL << slot)) && mach_absolute_time() < deadline) {
			if (bcm_poll() == 0) {
				delay(1000);
			}
		}
		if (what == LX_WPA_KEYS) {
			int e1 = key_install(wpa.pairwise, 16, 0, frame + 6, NULL);
			int e2 = key_install(wpa.group, wpa.group_length, wpa.group_index, NULL, wpa.group_counter);

			blog("wpa: pairwise key %d, group key %u (%u bytes) %d", e1, wpa.group_index, wpa.group_length, e2);
			if (e1 == 0 && e2 == 0) {
				wpa_done = true;
			} else {
				wpa_why = "the firmware refused a key";
				wpa_failed = true;
			}
		} else {
			blog("wpa: new group key %u: %d", wpa.group_index,
			    key_install(wpa.group, wpa.group_length, wpa.group_index, NULL, wpa.group_counter));
		}
	}
	bzero(frame, sizeof(frame));
	bzero(reply, sizeof(reply));
	eapol_owner = THREAD_NULL;
	lck_mtx_unlock(&eapol_lock);
}

/*
 * The completion rings looked at once, by one thread at a time (another that finds them taken goes
 * on: what it waits for is being looked at). Received frames go to en0 after ring_lock is let go,
 * and their buffers back to the firmware; a network lost is said to the station boundary.
 */
static unsigned
bcm_poll(void)
{
	struct { uint16_t buf, offset, length; } q[RX_BUFS];
	unsigned n, done;
	int lost;

	if (!lck_mtx_try_lock(&poll_lock)) {
		return 0;
	}
	lck_mtx_lock(&ring_lock);
	done = poll_rings();
	n = rxq_n;
	memcpy(q, rxq, n * sizeof(q[0]));
	rxq_n = 0;
	lck_mtx_unlock(&ring_lock);
	for (unsigned i = 0; i < n; i++) {
		const uint8_t *frame = rx_buf[q[i].buf] + q[i].offset;

		if (frame[12] == ETHERTYPE_EAPOL_HI && frame[13] == ETHERTYPE_EAPOL_LO && wpa_on) {
			lck_spin_lock(&state_lock);
			if (eapol_n == EAPOL_QUEUE) {
				memmove(&eapol_q[0], &eapol_q[1], sizeof(eapol_q[0]) * (EAPOL_QUEUE - 1));
				eapol_n--;
			}
			if (q[i].length <= EAPOL_MOST) {
				eapol_q[eapol_n].length = q[i].length;
				memcpy(eapol_q[eapol_n].frame, frame, q[i].length);
				eapol_n++;
			}
			lck_spin_unlock(&state_lock);
		} else if (!(frame[12] == 0x88 && frame[13] == 0x6c) && q[i].length <= REDWOOD_NET_FRAME_MAX) {
			redwood_net_input(frame, q[i].length);
		}
	}
	if (n != 0) {
		lck_mtx_lock(&ring_lock);
		for (unsigned i = 0; i < n; i++) {
			post_rx(q[i].buf);
		}
		lck_mtx_unlock(&ring_lock);
	}
	lost = lost_pending;
	if (lost != 0 && sta_state.phase == REDWOOD_STA_JOINED) {
		lck_spin_lock(&state_lock);
		sta_state.phase = REDWOOD_STA_IDLE;
		sta_state.last_error = lost;
		lck_spin_unlock(&state_lock);
		flow_up = false;
		blog("the network is lost (reason %u)", ev_down_reason);
		redwood_net_link_state(false);
		redwood_sta_lost(lost);
	}
	lost_pending = 0;
	lck_mtx_unlock(&poll_lock);
	if (eapol_n != 0 && flow_up && ioctl_owner != current_thread()) {
		eapol_handle();
	}
	return done + n;
}

/* brcmf_msgbuf_tx_ioctl() [msgbuf]431 and the wait for its completion: 0, or the firmware's error (negative) */
static int
ioctl(uint32_t cmd, void *buf, uint32_t len, uint32_t in_len)
{
	uint8_t *item;
	uint16_t trans, out16 = (uint16_t)len, in16 = (uint16_t)(in_len > IOCTL_MAX_IN ? IOCTL_MAX_IN : in_len);
	uint64_t deadline;
	int status;

	lck_mtx_lock(&ioctl_lock);
	ioctl_owner = current_thread();
	lck_mtx_lock(&ring_lock);
	item = ring_reserve(&rings[RING_CTRL_SUBMIT]);
	if (item == NULL) {
		lck_mtx_unlock(&ring_lock);
		ioctl_owner = THREAD_NULL;
		lck_mtx_unlock(&ioctl_lock);
		return -1000;
	}
	trans = ++ioctl_trans;
	put_hdr(item, MSG_IOCTLPTR_REQ, IOCTL_REQ_PKTID);
	memcpy(item + 8, &cmd, 4);
	memcpy(item + 12, &trans, 2);
	memcpy(item + 14, &in16, 2);
	memcpy(item + 16, &out16, 2);
	put_addr(item + 24, ioctl_in_phys);
	bzero(ioctl_in, CTL_BUF_SIZE);
	memcpy(ioctl_in, buf, in16);
	ioctl_done = false;
	ring_commit(&rings[RING_CTRL_SUBMIT]);
	lck_mtx_unlock(&ring_lock);

	clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
	while (!ioctl_done && mach_absolute_time() < deadline) {
		if (bcm_poll() == 0) {
			delay(200);
		}
	}
	lck_mtx_lock(&ring_lock);
	if (!ioctl_done) {
		uint32_t flags = tcm_r32(shared_addr);

		ioctl_done = true;              /* (a late answer is now one given up on: process() gives its buffer back) */
		lck_mtx_unlock(&ring_lock);
		blog("ioctl %u timed out: shared flags %#x%s%s, control ring w %u r %u (device r %u)", cmd, flags,
		    (flags & SH_ASSERT) ? " ASSERT" : "", (flags & SH_TRAP) ? " TRAP" : "", rings[RING_CTRL_SUBMIT].w_ptr,
		    rings[RING_CTRL_SUBMIT].r_ptr, idx_read(rings[RING_CTRL_SUBMIT].r_idx_addr));
		console_dump("(an ioctl timed out)");
		ioctl_owner = THREAD_NULL;
		lck_mtx_unlock(&ioctl_lock);
		return -1001;
	}
	if (ioctl_resp_id >= 1 && ioctl_resp_id <= IOCTLRESP_BUFS) {
		unsigned i = ioctl_resp_id - 1;
		uint32_t n = ioctl_resp_len < len ? ioctl_resp_len : len;

		if (n > CTL_BUF_SIZE) {
			n = CTL_BUF_SIZE;
		}
		memcpy(buf, ctl_buf[i], n);
		post_ctl(i);
	}
	status = ioctl_status;
	lck_mtx_unlock(&ring_lock);
	ioctl_owner = THREAD_NULL;
	lck_mtx_unlock(&ioctl_lock);
	return status;
}

/* an iovar: "name\0" then the data [fwil.c brcmf_fil_iovar_data_get/set] */
static int
iovar(bool set, const char *name, void *data, uint32_t len)
{
	uint8_t *buf;
	size_t nl = strlen(name) + 1, size = nl + len + 8;
	int err;

	if (size < 256) {
		size = 256;
	}
	buf = kalloc_data(size, Z_WAITOK | Z_ZERO);
	if (buf == NULL) {
		return -1002;
	}
	memcpy(buf, name, nl);
	memcpy(buf + nl, data, len);
	err = ioctl(set ? WLC_SET_VAR : WLC_GET_VAR, buf, (uint32_t)(set ? nl + len : (nl + len > len ? nl + len : len)),
	    (uint32_t)(nl + len));
	if (!set && err == 0) {
		memcpy(data, buf, len);
	}
	kfree_data(buf, size);
	return err;
}

static int
iovar_int(const char *name, int32_t value)
{
	return iovar(true, name, &value, sizeof(value));
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */

/* brcmf_fw_nvram_strip() [fw]395: "key=value" lines, NUL-separated, boardrev=0xff if none, padded, the length token last */
static uint8_t *
nvram_make(const uint8_t *text, uint64_t size, uint32_t *out_len)
{
	uint8_t *out = kalloc_data(size + 64, Z_WAITOK | Z_ZERO);
	uint32_t n = 0, token;
	bool boardrev = false;
	uint64_t i = 0;

	if (out == NULL) {
		return NULL;
	}
	while (i < size) {
		uint64_t start = i, end;

		while (i < size && text[i] != '\n') {
			i++;
		}
		end = i++;
		while (start < end && (text[start] == ' ' || text[start] == '\t')) {
			start++;
		}
		while (end > start && (text[end - 1] == '\r' || text[end - 1] == ' ' || text[end - 1] == '\t')) {
			end--;
		}
		if (end == start || text[start] == '#' || memchr(text + start, '=', end - start) == NULL) {
			continue;
		}
		if (end - start >= 8 && memcmp(text + start, "boardrev", 8) == 0) {
			boardrev = true;
		}
		memcpy(out + n, text + start, end - start);
		n += (uint32_t)(end - start);
		out[n++] = '\0';
	}
	if (!boardrev && n + 16 < size + 64) {
		memcpy(out + n, "boardrev=0xff", 14);
		n += 14;
	}
	n = (n + 1 + 3) & ~3U;
	token = n / 4;
	token = (~token << 16) | (token & 0xffff);
	memcpy(out + n, &token, 4);
	*out_len = n + 4;
	return out;
}

/*
 * fw/NAME of the small image, straight from the RAM disk (redwood_image.c: never given back), else /fw on the
 * root, read into memory of ours. The small image is mounted on /redwood, so a path lookup of /fw finds nothing.
 */
static const uint8_t *
fw_file(const char *name, uint64_t *size, bool *ours)
{
	const uint8_t *data = redwood_image_file("fw", name, size, NULL);

	*ours = false;
	if (data == NULL) {
		data = redwood_adsp_fw_file(name, size);
		*ours = data != NULL;
	}
	return data;
}

static void
fw_release(const uint8_t *data, uint64_t size, bool ours)
{
	if (ours && data != NULL) {
		redwood_adsp_fw_free(data, size);
	}
}

/* brcmf_c_download_clm() [common]102..160: "clmload" in chunks of 1400, struct brcmf_dload_data_le */
static int
clm_load(void)
{
	uint64_t size = 0;
	bool ours;
	const uint8_t *clm = fw_file("bcm4375-clm.blob", &size, &ours);
	uint8_t *chunk;
	int err = 0;
	uint32_t status = ~0U;

	if (clm == NULL) {
		blog("no /fw/bcm4375-clm.blob: no CLM (the firmware's own regulatory defaults)");
		return -1;
	}
	chunk = kalloc_data(12 + 1400, Z_WAITOK | Z_ZERO);
	if (chunk == NULL) {
		fw_release(clm, size, ours);
		return -1;
	}
	for (uint64_t at = 0; at < size && err == 0;) {
		uint32_t len = size - at > 1400 ? 1400 : (uint32_t)(size - at);
		uint16_t flag = (uint16_t)((1u << 12) | (at == 0 ? 0x2 : 0) | (at + len == size ? 0x4 : 0)), type = 2;
		uint32_t crc = 0;

		memcpy(chunk, &flag, 2);
		memcpy(chunk + 2, &type, 2);
		memcpy(chunk + 4, &len, 4);
		memcpy(chunk + 8, &crc, 4);
		memcpy(chunk + 12, clm + at, len);
		err = iovar(true, "clmload", chunk, 12 + len);
		at += len;
	}
	kfree_data(chunk, 12 + 1400);
	fw_release(clm, size, ours);
	(void)iovar(false, "clmload_status", &status, sizeof(status));
	blog("CLM: %llu bytes loaded, %s %d; clmload_status %d", size, err ? "error" : "answer", err, (int)status);
	return err;
}

/* ------------------------------------------------------------------------ */
/* The steps                                                                */

static bool
bars(void)
{
	uint32_t bar0 = bus->mem_phys, bar2;
	uint64_t s0 = bus->bar0_size, s2 = bus->bar2_size;

	if (s0 == 0 || s2 == 0 || s2 > bus->mem_size) {
		blog("W4 FAIL: BAR sizes %llu and %llu do not fit the window", s0, s2);
		return false;
	}
	bar2 = (uint32_t)((bar0 + s0 + s2 - 1) & ~(s2 - 1));
	if (bar2 + s2 > (uint64_t)bus->mem_phys + bus->mem_size) {
		blog("W4 FAIL: BAR2 (%llu KB) does not fit after BAR0 in the window", s2 >> 10);
		return false;
	}
	/* the root port forwards the window (bridge memory base/limit, 1 MB granules) */
	w32(bus->dbi, 0x20, (((bus->mem_phys + bus->mem_size - 1) >> 16) & 0xfff0) << 16 | ((bus->mem_phys >> 16) & 0xfff0));
	w32(cfg, 0x10, bar0);
	if (bus->bar0_wide) {
		w32(cfg, 0x14, 0);
	}
	w32(cfg, 0x18, bar2);
	if (bus->bar2_wide) {
		w32(cfg, 0x1c, 0);
	}
	w32(cfg, 0x04, r32(cfg, 0x04) | 0x6);   /* memory space, bus master */
	blog("BARs: BAR0 %#x (%llu KB), BAR2 %#x (%llu KB); endpoint command %#x, BAR0 reads %#x, BAR2 %#x; bridge window %#x",
	    bar0, s0 >> 10, bar2, s2 >> 10, r32(cfg, 0x04) & 0xffff, r32(cfg, 0x10), r32(cfg, 0x18), r32(bus->dbi, 0x20));
	/*
	 * From kernel_map, not ml_io_map()'s 8 MB io_submap: the DMA pool window and the other drivers
	 * already fill that, and a 4 MB BAR2 on top panicked (kmem_alloc ... failed with 3 @vm_kern.c:178).
	 */
	regs = (volatile uint8_t *)ml_io_map_unmappable(bar0, s0 < BAR0_MAP ? (vm_size_t)s0 : BAR0_MAP, VM_WIMG_IO);
	tcm_size = (uint32_t)(s2 > 0x400000 ? 0x400000 : s2);
	tcm = (volatile uint8_t *)ml_io_map_unmappable(bar2, tcm_size, VM_WIMG_IO);
	if (regs == NULL || tcm == NULL) {
		blog("W4 FAIL: BAR0 or BAR2 not mapped");
		return false;
	}
	return true;
}

static bool
chip(void)
{
	uint32_t id = bp_read(ENUM_BASE);
	char line[220];
	size_t at = 0;

	blog("chip id %#x: BCM%x rev %u, package %u (want 4375)", id, CID_ID(id), CID_REV(id), (id >> 20) & 0xf);
	if (CID_ID(id) != 0x4375) {
		blog("W4 FAIL: not the BCM4375 (%#x)", id);
		return false;
	}
	erom_scan();
	for (unsigned i = 0; i < ncores && at < sizeof(line) - 24; i++) {
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %x/%u@%x", cores[i].id, cores[i].rev, cores[i].base);
	}
	blog("cores (%u):%s", ncores, line);
	cc = core_find(CORE_CC, 0);
	pcie2 = core_find(CORE_PCIE2, 0);
	cr4 = core_find(CORE_ARM_CR4, 0);
	if (cc == NULL || pcie2 == NULL || cr4 == NULL) {
		blog("W4 FAIL: chipcommon, PCIe2 or ARM CR4 missing from the EROM");
		return false;
	}
	/* brcmf_pcie_buscore_reset() [pcie]1904: the mailbox registers by the PCIe2 core's revision */
	if (pcie2->rev >= 64) {
		mb_int = 0xc30, mb_mask = 0xc34, h2d_mb0 = 0xa20, h2d_mb1 = 0xa24;
	} else {
		mb_int = 0x48, mb_mask = 0x4c, h2d_mb0 = 0x140, h2d_mb1 = 0x144;
	}
	ramsize = cr4_ramsize();
	blog("PCIe2 core rev %u at %#x (wrapper %#x), ARM CR4 rev %u at %#x (wrapper %#x): RAM %#x + %#x",
	    pcie2->rev, pcie2->base, pcie2->wrap, cr4->rev, cr4->base, cr4->wrap, RAMBASE_4375, ramsize);
	/* brcmf_pcie_attach() [pcie]717: BAR1 window sized */
	pcie2_write(PCIE2_CONFIGADDR, 0x4e0);
	pcie2_write(PCIE2_CONFIGDATA, pcie2_read(PCIE2_CONFIGDATA));
	pcie2_write(mb_mask, 0);
	pcie2_write(mb_int, pcie2_read(mb_int));
	if (ramsize == 0 || RAMBASE_4375 + ramsize > tcm_size) {
		blog("W4 FAIL: RAM %#x + %#x is not within BAR2's %#x", RAMBASE_4375, ramsize, tcm_size);
		return false;
	}
	return true;
}

/* brcmf_pcie_download_fw_nvram() [pcie]1718 */
static bool
firmware(void)
{
	uint32_t which = 0, nvram_len = 0, resetintr, written, now;
	const char *nvname;
	uint64_t fw_size = 0, nv_size = 0;
	const uint8_t *fw, *nvtext;
	uint8_t *nvram;
	uint64_t deadline;
	bool fw_ours = false, nv_ours = false;

	PE_parse_boot_argn("wifi_nvram", &which, sizeof(which));
	nvname = which == 1 ? "bcm4375-nvram-murata.txt" : "bcm4375-nvram-semco.txt";
	fw = fw_file("bcm4375-sta.bin", &fw_size, &fw_ours);
	nvtext = fw_file(nvname, &nv_size, &nv_ours);
	if (fw == NULL || nvtext == NULL || fw_size < 0x100) {
		blog("W4 FAIL: fw/bcm4375-sta.bin (%llu bytes) or fw/%s (%llu bytes) missing", fw_size, nvname, nv_size);
		fw_release(fw, fw_size, fw_ours);
		fw_release(nvtext, nv_size, nv_ours);
		return false;
	}
	blog("fw/bcm4375-sta.bin %llu bytes, fw/%s %llu bytes (%s)", fw_size, nvname, nv_size,
	    fw_ours ? "read from /fw" : "in the RAM disk");
	nvram = nvram_make(nvtext, nv_size, &nvram_len);
	fw_release(nvtext, nv_size, nv_ours);
	if (le32(fw + RAMSIZE_OFFSET) == RAMSIZE_MAGIC) {
		uint32_t own = le32(fw + RAMSIZE_OFFSET + 4);

		blog("the image says RAM %#x (the cores %#x)", own, ramsize);
		if (own != 0 && own <= ramsize) {
			ramsize = own;
		}
	}
	if (nvram == NULL || fw_size + 8 + SEED_BYTES + nvram_len > ramsize) {
		blog("W4 FAIL: firmware %llu + NVRAM %u bytes do not fit RAM %#x", fw_size, nvram_len, ramsize);
		fw_release(fw, fw_size, fw_ours);
		if (nvram != NULL) {
			kfree_data(nvram, nv_size + 64);
		}
		return false;
	}
	resetintr = le32(fw);

	cr4_passive();
	for (uint64_t at = 0; at < fw_size; at += 4) {
		uint32_t word = 0;

		memcpy(&word, fw + at, fw_size - at >= 4 ? 4 : (size_t)(fw_size - at));
		*(volatile uint32_t *)(tcm + RAMBASE_4375 + at) = word;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	fw_release(fw, fw_size, fw_ours);
	tcm_w32(RAMBASE_4375 + ramsize - 4, 0);
	for (uint32_t at = 0; at < nvram_len; at += 4) {
		uint32_t word;

		memcpy(&word, nvram + at, 4);
		*(volatile uint32_t *)(tcm + RAMBASE_4375 + ramsize - nvram_len + at) = word;
	}
	/*
	 * Entropy below the NVRAM: the bytes, then {count, 0xfeedc0de}. Samsung's dhdpcie_wrt_rnd() writes 128 bytes
	 * for this chip's ARM CR4 path, brcmfmac's WCC_SEED (the 4375's entry) 256; the firmware randomises its heap with it.
	 */
	{
		uint32_t seed[SEED_BYTES / 4], top = RAMBASE_4375 + ramsize - nvram_len;

		read_random(seed, sizeof(seed));
		tcm_w32(top - 8, SEED_BYTES);
		tcm_w32(top - 4, SEED_MAGIC);
		for (uint32_t i = 0; i < SEED_BYTES / 4; i++) {
			tcm_w32(top - 8 - SEED_BYTES + i * 4, seed[i]);
		}
	}
	kfree_data(nvram, nv_size + 64);
	written = tcm_r32(RAMBASE_4375 + ramsize - 4);
	blog("firmware %llu bytes at %#x (reset vector %#x), %s %u bytes at the top; first word reads back %#x",
	    fw_size, RAMBASE_4375, resetintr, nvname, nvram_len, tcm_r32(RAMBASE_4375));

	cr4_active(resetintr);
	clock_interval_to_deadline(5000, NSEC_PER_MSEC, &deadline);
	do {
		delay(50000);
		now = tcm_r32(RAMBASE_4375 + ramsize - 4);
	} while (now == written && mach_absolute_time() < deadline);
	if (now == written || !in_ram(now, SH_HOST_CAP + 4)) {
		blog("W4 FAIL: the firmware did not start: last word %#x (was %#x)", now, written);
		return false;
	}
	shared_addr = now;
	blog("W4 PASS: the firmware is up, shared area at %#x", shared_addr);
	return true;
}

/* brcmf_pcie_init_share_ram_info() [pcie]1630 + host_cap (0003-bcm4375-host-cap.patch) */
static bool
shared(void)
{
	shared_flags = tcm_r32(shared_addr);
	shared_version = shared_flags & 0xff;
	console_addr = tcm_r32(shared_addr + SH_CONSOLE);
	if (in_ram(console_addr, CONSOLE_WRITEIDX + 4)) {
		console_buf = tcm_r32(console_addr + CONSOLE_BUFADDR);
		console_size = tcm_r32(console_addr + CONSOLE_BUFSIZE);
		console_dump(NULL);
	}
	if (shared_version < SH_MIN_VERSION || shared_version > SH_MAX_VERSION) {
		blog("W5 FAIL: shared area version %u (flags %#x): not 5..7", shared_version, shared_flags);
		return false;
	}
	if (shared_flags & SH_DMA_INDEX) {
		dma_idx_sz = (shared_flags & SH_DMA_2B_IDX) ? 2 : 4;
	}
	max_rxbufpost = r16(tcm, shared_addr + SH_MAX_RXBUFPOST);
	htod_mb_addr = tcm_r32(shared_addr + SH_HTOD_MB_DATA);
	dtoh_mb_addr = tcm_r32(shared_addr + SH_DTOH_MB_DATA);
	if (!in_ram(htod_mb_addr, 4) || !in_ram(dtoh_mb_addr, 4)) {
		htod_mb_addr = dtoh_mb_addr = 0;
	}
	rx_dataoffset = tcm_r32(shared_addr + SH_RX_DATAOFFSET);
	if (shared_version >= 6) {
		tcm_w32(shared_addr + SH_HOST_CAP, shared_version | 0x400 | 0x1000 | 0x20000 | 0x800000);
	}
	blog("shared area: flags %#x (version %u, %s indices), max rx buffers %u, rx data offset %u, console %#x (%u bytes), host_cap %#x",
	    shared_flags, shared_version, dma_idx_sz ? (dma_idx_sz == 2 ? "2-byte DMA" : "4-byte DMA") : "TCM", max_rxbufpost,
	    rx_dataoffset, console_addr, console_size, tcm_r32(shared_addr + SH_HOST_CAP));
	return rx_dataoffset < 512;
}

static bool
buffers(void)
{
	ioctl_in = pool_alloc(CTL_BUF_SIZE, &ioctl_in_phys);
	for (unsigned i = 0; i < CTL_BUFS; i++) {
		ctl_buf[i] = pool_alloc(CTL_BUF_SIZE, &ctl_phys[i]);
	}
	for (unsigned i = 0; i < RX_BUFS; i++) {
		rx_buf[i] = pool_alloc(RX_BUF_SIZE, &rx_phys[i]);
	}
	if (ioctl_in == NULL || ctl_buf[CTL_BUFS - 1] == NULL || rx_buf[RX_BUFS - 1] == NULL) {
		blog("W5 FAIL: no pool for the buffers (%zu bytes used)", pool_used);
		return false;
	}
	/* brcmf_msgbuf attach [msgbuf]1622..1630: receive buffers, event buffers, ioctl response buffers */
	for (unsigned i = 0; i < RX_BUFS; i++) {
		post_rx(i);
	}
	for (unsigned i = IOCTLRESP_BUFS; i < CTL_BUFS; i++) {
		post_ctl(i);
	}
	for (unsigned i = 0; i < IOCTLRESP_BUFS; i++) {
		post_ctl(i);
	}
	return true;
}

/* an iovar's text, to the log in lines of 200 (newlines made spaces) */
static void
log_text(const char *name, uint32_t size)
{
	char *text = kalloc_data(size, Z_WAITOK | Z_ZERO);
	size_t n;
	int err;

	if (text == NULL) {
		return;
	}
	err = iovar(false, name, text, size - 1);
	text[size - 1] = '\0';
	for (char *p = text; *p; p++) {
		if (*p == '\n' || *p == '\r') {
			*p = ' ';
		}
	}
	n = strlen(text);
	blog("%s: %d \"%.200s\"", name, err, text);
	for (size_t at = 200; at < n; at += 200) {
		blog("%s (more): \"%.200s\"", name, text + at);
	}
	kfree_data(text, size);
}

/* What brcmf_c_preinit_dcmds() [common] and brcmf_cfg80211 attach ask, then the radio up. */
static bool
setup(void)
{
	uint32_t version = 0, one = 1, scan_ver[2] = { 0, 0 };
	static const uint8_t wanted[] = { E_SET_SSID, E_AUTH, E_DEAUTH, E_DEAUTH_IND, E_ASSOC, E_REASSOC, E_DISASSOC,
		                          E_DISASSOC_IND, E_LINK, E_PSK_SUP, E_IF, E_ESCAN_RESULT };
	uint8_t mask[16];
	int err;

	err = ioctl(WLC_GET_VERSION, &version, sizeof(version), sizeof(version));
	blog("WLC_GET_VERSION: %d, version %u", err, version);
	if (err == -1001) {
		blog("W5 FAIL: the firmware answers no ioctl");
		return false;
	}
	log_text("ver", 256);
	/* the firmware's features: "idsup" (its own WPA2 supplicant), "sae" (WPA3-Personal in the firmware), "mfp" */
	log_text("cap", 1024);
	err = iovar(false, "cur_etheraddr", mac, sizeof(mac));
	blog("cur_etheraddr: %d %02x:%02x:%02x:%02x:%02x:%02x", err, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	clm_load();
	log_text("clmver", 256);
	blog("mpc 0: %d", iovar_int("mpc", 0));
	blog("WLC_SET_INFRA 1: %d", ioctl(WLC_SET_INFRA, &one, sizeof(one), sizeof(one)));
	blog("allmulti 1: %d", iovar_int("allmulti", 1));
	bzero(mask, sizeof(mask));
	err = iovar(false, "event_msgs", mask, sizeof(mask));
	for (unsigned i = 0; i < sizeof(wanted); i++) {
		mask[wanted[i] / 8] |= (uint8_t)(1 << (wanted[i] % 8));
	}
	blog("event_msgs: get %d, set %d", err, iovar(true, "event_msgs", mask, sizeof(mask)));
	err = ioctl(WLC_UP, &one, sizeof(one), sizeof(one));
	blog("WLC_UP: %d", err);
	err = iovar(false, "scan_ver", scan_ver, sizeof(scan_ver));
	scan_v2 = err == 0;                     /* BRCMF_FEAT_SCAN_V2: "scan_ver" answers (feature.c brcmf_feat_iovar_int_get) */
	blog("scan_ver: %d (%#x %#x): escan v%u", err, scan_ver[0], scan_ver[1], scan_v2 ? 2 : 1);
	return true;
}

/*
 * brcmf_run_escan() [cfg]1442 + brcmf_escan_prep() [cfg]1100: what is asked for (a name, channels,
 * listening only, a dwell time), else every channel the firmware's own way. Returns when the scan is over.
 */
static int escan_once(const struct redwood_sta_scan *what);

static int
escan_run(const struct redwood_sta_scan *what)
{
	int err;

	lck_mtx_lock(&scan_lock);
	err = escan_once(what);
	lck_mtx_unlock(&scan_lock);
	return err;
}

static int
escan_once(const struct redwood_sta_scan *what)
{
	uint8_t params[8 + 76 + 2 * 64];
	uint8_t *p = params + 8, *chans;
	uint32_t ver = scan_v2 ? 2 : 1, len, nch = 0, sl = 0, scan_type = 0, fixed, whole;
	uint16_t action = 1, sync = 0x1234;
	int32_t minus1 = -1, active = -1, passive = -1;
	uint64_t deadline;
	int err;

	bzero(params, sizeof(params));
	if (what != NULL) {
		sl = what->ssid_length > 32 ? 32 : what->ssid_length;
		scan_type = what->passive ? 1 : 0;      /* BRCMF_SCANTYPE_PASSIVE */
		if (what->dwell_ms != 0) {
			active = passive = what->dwell_ms;
		}
	}
	memcpy(params, &ver, 4);
	memcpy(params + 4, &action, 2);
	memcpy(params + 6, &sync, 2);
	if (scan_v2) {
		/* brcmf_scan_params_v2_le: version 0, length 2, ssid 4, bssid 40, bss_type 46, scan_type 48, nprobes 52,
		 * active 56, passive 60, home 64, channel_num 68, channels 72 */
		uint16_t pv = 2, plen = 76;

		memcpy(p, &pv, 2);
		memcpy(p + 2, &plen, 2);
		memcpy(p + 4, &sl, 4);
		if (sl != 0) {
			memcpy(p + 8, what->ssid, sl);
		}
		memset(p + 40, 0xff, 6);
		p[46] = 2;                      /* DOT11_BSSTYPE_ANY */
		memcpy(p + 48, &scan_type, 4);
		memcpy(p + 52, &minus1, 4);
		memcpy(p + 56, &active, 4);
		memcpy(p + 60, &passive, 4);
		memcpy(p + 64, &minus1, 4);
		chans = p + 72;
		fixed = 72;
		whole = 76;
	} else {
		/* brcmf_scan_params_le: ssid 0, bssid 36, bss_type 42, scan_type 43, nprobes 44, active 48, passive 52,
		 * home 56, channel_num 60, channels 64 */
		memcpy(p, &sl, 4);
		if (sl != 0) {
			memcpy(p + 4, what->ssid, sl);
		}
		memset(p + 36, 0xff, 6);
		p[42] = 2;
		p[43] = (uint8_t)scan_type;
		memcpy(p + 44, &minus1, 4);
		memcpy(p + 48, &active, 4);
		memcpy(p + 52, &passive, 4);
		memcpy(p + 56, &minus1, 4);
		chans = p + 64;
		fixed = 64;
		whole = 68;
	}
	for (unsigned i = 0; what != NULL && i < what->frequency_count && i < 64 && nch < 64; i++) {
		uint16_t cs = mhz_chanspec(what->frequencies[i]);

		if (cs != 0) {
			memcpy(chans + 2 * nch, &cs, 2);
			nch++;
		}
	}
	memcpy(chans - 4, &nch, 4);
	len = 8 + (fixed + 2 * nch > whole ? fixed + 2 * nch : whole);

	lck_mtx_lock(&ring_lock);
	nfound = 0;
	escan_done = false;
	escan_status = 0;
	lck_mtx_unlock(&ring_lock);
	err = iovar(true, "escan", params, len);
	if (err != 0) {
		blog("escan (v%u, %u channels): %d", ver, nch, err);
		return EIO;
	}
	clock_interval_to_deadline(20000, NSEC_PER_MSEC, &deadline);
	while (!escan_done && mach_absolute_time() < deadline) {
		if (bcm_poll() == 0) {
			delay(2000);
		}
	}
	if (!escan_done) {
		return ETIMEDOUT;
	}
	return escan_status == 0 ? 0 : EIO;
}

/*
 * The RSN element (48) of a BSS's elements: its AKM suites (00-0f-ac:N as bit N) and its capabilities.
 * False if there is none (an open or WPA1 network).
 */
static bool
rsn_parse(const uint8_t *ie, unsigned len, uint32_t *akms, uint16_t *caps)
{
	*akms = 0;
	*caps = 0;
	for (unsigned at = 0; at + 2 <= len && at + 2 + ie[at + 1] <= len; at += 2 + ie[at + 1]) {
		const uint8_t *e = ie + at + 2, *end = e + ie[at + 1];
		unsigned n;

		if (ie[at] != 48) {
			continue;
		}
		e += 2 + 4;                     /* version, group cipher */
		if (e + 2 > end) {
			return true;
		}
		n = (unsigned)(e[0] | e[1] << 8);
		e += 2 + 4 * n;                 /* pairwise ciphers */
		if (e + 2 > end) {
			return true;
		}
		n = (unsigned)(e[0] | e[1] << 8);
		e += 2;
		for (unsigned i = 0; i < n && e + 4 <= end; i++, e += 4) {
			if (e[0] == 0x00 && e[1] == 0x0f && e[2] == 0xac && e[3] < 32) {
				*akms |= 1u << e[3];
			}
		}
		if (e + 2 <= end) {
			*caps = (uint16_t)(e[0] | e[1] << 8);
		}
		return true;
	}
	return false;
}

static const char *
security_name(const struct redwood_sta_bss *b)
{
	uint32_t akms;
	uint16_t caps;

	if (!rsn_parse(b->elements, b->elements_length, &akms, &caps)) {
		return (b->capability & 0x10) ? "WEP/WPA1" : "open";
	}
	if ((akms & (AKM_SAE | AKM_FT_SAE)) && (akms & (AKM_PSK | AKM_PSK_SHA256))) {
		return "WPA2/WPA3 transition";
	}
	if (akms & (AKM_SAE | AKM_FT_SAE)) {
		return "WPA3-only (SAE)";
	}
	if (akms & (AKM_PSK | AKM_PSK_SHA256)) {
		return (caps & RSN_CAP_MFPR) ? "WPA2 (MFP required)" : "WPA2";
	}
	return "RSN, not a passphrase (802.1X?)";
}

/* ------------------------------------------------------------------------ */
/* W6: the station (sys/redwood_sta.h) and the link behind en0              */

static struct redwood_sta_bss join_bss;

static void
state_set(enum redwood_sta_phase phase, const struct redwood_sta_bss *bss, int error)
{
	lck_spin_lock(&state_lock);
	sta_state.phase = phase;
	if (bss != NULL) {
		sta_state.bss = *bss;
	}
	sta_state.last_error = error;
	lck_spin_unlock(&state_lock);
}

/* brcmf_msgbuf_flowring_create_worker() [msgbuf]588: flow ring 0 (ring 2), towards the access point */
static bool
flow_create(const uint8_t *da)
{
	static bool made;
	uint16_t id = FLOW_RING_ID, items = FLOW_ITEMS, item_len = FLOW_ITEM_LEN;
	uint8_t *item;
	uint64_t deadline;

	if (!made) {
		flow.depth = FLOW_ITEMS;
		flow.item_len = FLOW_ITEM_LEN;
		flow.buf = pool_alloc((size_t)FLOW_ITEMS * FLOW_ITEM_LEN, &flow.phys);
		for (unsigned i = 0; i < TX_BUFS; i++) {
			tx_buf[i] = pool_alloc(TX_BUF_SIZE, &tx_phys[i]);
		}
		if (flow.buf == NULL || tx_buf[TX_BUFS - 1] == NULL) {
			blog("W6 FAIL: no pool for the flow ring and its buffers (%zu bytes used)", pool_used);
			return false;
		}
		flow.w_idx_addr = h2d_w_base + FLOW_RING_ID * idx_step;
		flow.r_idx_addr = h2d_r_base + FLOW_RING_ID * idx_step;
		made = true;
	}
	lck_mtx_lock(&ring_lock);
	flow.w_ptr = flow.r_ptr = 0;
	bzero(flow.buf, (size_t)FLOW_ITEMS * FLOW_ITEM_LEN);
	idx_write(flow.w_idx_addr, 0);
	tx_busy = 0;
	item = ring_reserve(&rings[RING_CTRL_SUBMIT]);
	if (item == NULL) {
		lck_mtx_unlock(&ring_lock);
		return false;
	}
	/* struct msgbuf_tx_flowring_create_req: da 8, sa 14, tid 20, if_flags 21, id 22, tc 24, priority 25,
	 * int_vector 26, max_items 28, len_item 30, ring 32 */
	put_hdr(item, MSG_FLOW_CREATE, 0);
	memcpy(item + 8, da, 6);
	memcpy(item + 14, mac, 6);
	memcpy(item + 22, &id, 2);
	memcpy(item + 28, &items, 2);
	memcpy(item + 30, &item_len, 2);
	put_addr(item + 32, flow.phys);
	flow_answered = false;
	ring_commit(&rings[RING_CTRL_SUBMIT]);
	lck_mtx_unlock(&ring_lock);
	clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
	while (!flow_answered && mach_absolute_time() < deadline) {
		if (bcm_poll() == 0) {
			delay(1000);
		}
	}
	blog("flow ring %u: %s (status %d)", id, flow_answered ? "made" : "NO ANSWER IN 2 S", flow_status);
	flow_up = flow_answered && flow_status == 0;
	return flow_up;
}

static void
flow_delete(void)
{
	uint16_t id = FLOW_RING_ID;
	uint8_t *item;
	uint64_t deadline;

	if (flow.buf == NULL) {
		return;
	}
	flow_up = false;
	lck_mtx_lock(&ring_lock);
	item = ring_reserve(&rings[RING_CTRL_SUBMIT]);
	if (item == NULL) {
		lck_mtx_unlock(&ring_lock);
		return;
	}
	put_hdr(item, MSG_FLOW_DELETE, 0);
	memcpy(item + 8, &id, 2);
	flow_answered = false;
	ring_commit(&rings[RING_CTRL_SUBMIT]);
	lck_mtx_unlock(&ring_lock);
	clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
	while (!flow_answered && mach_absolute_time() < deadline) {
		if (bcm_poll() == 0) {
			delay(1000);
		}
	}
	blog("flow ring %u deleted: %s (status %d)", id, flow_answered ? "answered" : "no answer", flow_status);
}

static int
sta_leave(void)
{
	uint8_t scb[12];
	bool joined = sta_state.phase == REDWOOD_STA_JOINED;
	int err;

	state_set(REDWOOD_STA_IDLE, NULL, 0);
	flow_up = false;
	if (joined) {
		redwood_net_link_state(false);
	}
	bzero(scb, sizeof(scb));
	err = ioctl(WLC_DISASSOC, scb, sizeof(scb), sizeof(scb));
	flow_delete();
	lost_pending = 0;
	blog("leave: WLC_DISASSOC %d", err);
	return 0;
}

static int
sta_power(bool on)
{
	uint32_t one = 1;
	int err;

	if (!on) {
		if (sta_state.phase == REDWOOD_STA_JOINED || sta_state.phase == REDWOOD_STA_JOINING) {
			sta_leave();
		}
		err = ioctl(WLC_DOWN, &one, sizeof(one), sizeof(one));
		state_set(REDWOOD_STA_OFF, NULL, 0);
	} else {
		err = ioctl(WLC_UP, &one, sizeof(one), sizeof(one));
		if (err == 0) {
			state_set(REDWOOD_STA_IDLE, NULL, 0);
		}
	}
	blog("power %s: %d", on ? "on" : "off", err);
	return err == 0 ? 0 : EIO;
}

static int
sta_scan(const struct redwood_sta_scan *what)
{
	enum redwood_sta_phase was = sta_state.phase;
	int err;

	if (was == REDWOOD_STA_OFF) {
		return ENETDOWN;
	}
	if (was == REDWOOD_STA_IDLE) {
		state_set(REDWOOD_STA_SCANNING, NULL, sta_state.last_error);
	}
	err = escan_run(what);
	if (was == REDWOOD_STA_IDLE && sta_state.phase == REDWOOD_STA_SCANNING) {
		state_set(REDWOOD_STA_IDLE, NULL, sta_state.last_error);
	}
	blog("scan (%s, %u channels%s): %d, %u heard", what != NULL && what->ssid_length ? "a name" : "any name",
	    what != NULL ? what->frequency_count : 0, what != NULL && what->passive ? ", listening" : "", err, nfound);
	return err;
}

static void
sta_heard(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context)
{
	lck_mtx_lock(&ring_lock);
	for (unsigned i = 0; i < nfound; i++) {
		each(&found[i], context);
	}
	lck_mtx_unlock(&ring_lock);
}

/*
 * brcmf_cfg80211_connect() [cfg]2377 with the firmware's supplicant (BRCMF_PROFILE_FWSUP_PSK): wsec AES,
 * wpa_auth WPA2-PSK, open authentication, sup_wpa 1, the passphrase (or the PMK) by WLC_SET_WSEC_PMK, then
 * "join" (WLC_SET_SSID where that is refused). Joined: the link up (E_LINK) and, with WPA2, the keys
 * (E_PSK_SUP FWSUP_COMPLETED); then the flow ring to carry frames, and en0's link up.
 */
static int
sta_join(const struct redwood_sta_join *what)
{
	bool secure = what->security == REDWOOD_STA_WPA2_PSK, any_bssid = true;
	uint8_t jp[68], sp[52], scb[12];
	uint32_t zero = 0, sl = what->ssid_length > 32 ? 32 : what->ssid_length;
	int32_t minus1 = -1;
	int err, e_wsec, e_auth, e_mode, e_sup, e_mfp, result;
	uint32_t wpa_auth = 0, mfp = MFP_NONE;
	bool sae = false, host_sup = false;
	uint64_t deadline;

	if (sta_state.phase == REDWOOD_STA_OFF) {
		return ENETDOWN;
	}
	if (sta_state.phase == REDWOOD_STA_JOINED || sta_state.phase == REDWOOD_STA_JOINING) {
		sta_leave();
	}
	for (unsigned i = 0; i < 6; i++) {
		any_bssid = any_bssid && what->bssid[i] == 0;
	}
	/* the access point it will be, as the last scan heard it: the one asked for, else the loudest of the name */
	lck_mtx_lock(&ring_lock);
	bzero(&join_bss, sizeof(join_bss));
	for (unsigned i = 0, best = ~0U; i < nfound; i++) {
		bool match = any_bssid ? found[i].ssid_length == sl && memcmp(found[i].ssid, what->ssid, sl) == 0 :
		    memcmp(found[i].bssid, what->bssid, 6) == 0;

		if (match && (best == ~0U || found[i].rssi > found[best].rssi)) {
			best = i;
			join_bss = found[i];
		}
	}
	lck_mtx_unlock(&ring_lock);
	if (join_bss.ssid_length == 0) {
		memcpy(join_bss.ssid, what->ssid, sl);
		join_bss.ssid_length = (uint8_t)sl;
		memcpy(join_bss.bssid, what->bssid, 6);
	}
	state_set(REDWOOD_STA_JOINING, &join_bss, 0);

	/*
	 * WPA2 or WPA3, by what the access point offers (iOS hands over the passphrase as typed either way):
	 * WPA2-PSK where it is offered (a WPA2/WPA3 transition network too), SAE where it is all there is
	 * (boot-arg wifi_sae=1: SAE wherever offered). MFP as its RSN capabilities ask; SAE requires it.
	 */
	{
		uint32_t akms = 0, force_sae = 0;
		uint16_t caps = 0;
		bool rsn = rsn_parse(join_bss.elements, join_bss.elements_length, &akms, &caps);

		PE_parse_boot_argn("wifi_sae", &force_sae, sizeof(force_sae));
		sae = secure && (akms & (AKM_SAE | AKM_FT_SAE)) && (force_sae || !(akms & (AKM_PSK | AKM_PSK_SHA256)));
		if (sae && what->passphrase_length == 0) {
			blog("join: WPA3 (SAE) needs the passphrase, and only a PMK was given");
			result = EINVAL;
			goto fail;
		}
		wpa_auth = !secure ? 0 : sae ? WPA3_AUTH_SAE_PSK :
		    ((akms & AKM_PSK_SHA256) && !(akms & AKM_PSK)) ? WPA2_AUTH_PSK_SHA256 : WPA2_AUTH_PSK;
		mfp = sae || (caps & RSN_CAP_MFPR) ? MFP_REQUIRED : (rsn && (caps & RSN_CAP_MFPC)) ? MFP_CAPABLE : MFP_NONE;
		if (!secure) {
			mfp = MFP_NONE;
		}
		blog("join: the access point offers %s (AKMs %#x, RSN capabilities %#x): joining with %s, MFP %u",
		    join_bss.elements_length ? security_name(&join_bss) : "(not heard by the last scan)", akms, caps,
		    !secure ? "no security" : sae ? "WPA3-SAE" : "WPA2-PSK", mfp);
	}
	e_wsec = iovar_int("wsec", secure ? AES_ENABLED : 0);
	e_mfp = iovar_int("mfp", (int32_t)mfp);
	e_mode = iovar_int("wpa_auth", (int32_t)wpa_auth);
	e_auth = iovar_int("auth", sae ? AUTH_SAE : AUTH_OPEN);
	if (e_auth != 0 && !sae) {
		e_auth = ioctl(WLC_SET_AUTH, &zero, sizeof(zero), sizeof(zero));
	}
	e_sup = iovar_int("sup_wpa", secure ? 1 : 0);
	if (sae) {
		uint8_t none = 0;

		(void)iovar(true, "wpaie", &none, 0);   /* brcmf_cfg80211_connect(): no RSN element of the host's */
	}
	blog("join: a name of %u bytes, %s; wsec %d, mfp %d, wpa_auth %d, auth %d, sup_wpa %d", sl, secure ?
	    (what->passphrase_length ? "a passphrase" : "a PMK") : "open", e_wsec, e_mfp, e_mode, e_auth, e_sup);
	host_sup = secure && e_sup != 0;
	if (host_sup && sae) {
		blog("join: WPA3 (SAE) without the firmware's supplicant (sup_wpa %d) is not done on the host yet", e_sup);
		result = EOPNOTSUPP;
		goto fail;
	}
	if (host_sup) {
		bzero(&wpa, sizeof(wpa));
		if (what->passphrase_length != 0) {
			lx_wpa_pmk(what->passphrase, what->passphrase_length, what->ssid, sl, wpa.pmk);
		} else {
			memcpy(wpa.pmk, what->pmk, 32);
		}
		memcpy(wpa.own, mac, 6);
		read_random(wpa.own_nonce, sizeof(wpa.own_nonce));
		lck_spin_lock(&state_lock);
		eapol_n = 0;
		lck_spin_unlock(&state_lock);
		wpa_done = wpa_failed = false;
		wpa_why = NULL;
		wpa_rsn_length = 0;
		wpa_on = true;
		blog("join: the firmware has no supplicant of its own (sup_wpa %d): the four-way handshake is the host's", e_sup);
	}
	if (sae && e_auth != 0) {
		blog("join: THE FIRMWARE REFUSES SAE AUTHENTICATION (auth 3: %d): no WPA3 in the firmware", e_auth);
		result = EOPNOTSUPP;
		goto fail;
	}
	if (secure && !host_sup) {
		/* wsec_pmk_t: key_len 0, flags 2, key 4 */
		uint8_t *pmk = kalloc_data(PMK_STRUCT_SIZE, Z_WAITOK | Z_ZERO);
		uint16_t kl, fl;

		if (pmk == NULL) {
			result = ENOMEM;
			goto fail;
		}
		if (what->passphrase_length != 0) {
			kl = what->passphrase_length > 64 ? 64 : what->passphrase_length;
			fl = WSEC_PASSPHRASE;
			memcpy(pmk + 4, what->passphrase, kl);
		} else {
			kl = sizeof(what->pmk);
			fl = 0;
			memcpy(pmk + 4, what->pmk, kl);
		}
		memcpy(pmk, &kl, 2);
		memcpy(pmk + 2, &fl, 2);
		err = ioctl(WLC_SET_WSEC_PMK, pmk, PMK_STRUCT_SIZE, PMK_STRUCT_SIZE);
		bzero(pmk, PMK_STRUCT_SIZE);
		kfree_data(pmk, PMK_STRUCT_SIZE);
		blog("join: WLC_SET_WSEC_PMK %d", err);
		if (err != 0) {
			result = EINVAL;
			goto fail;
		}
	}
	lck_mtx_lock(&ring_lock);
	ev_ssid = ev_link_up = ev_link_down = ev_psk = false;
	ev_ssid_status = ev_psk_status = ev_down_reason = 0;
	lck_mtx_unlock(&ring_lock);

	/* brcmf_ext_join_params_le: ssid 0, scan params 36 (type, nprobes 40, active 44, passive 48, home 52), bssid 56,
	 * chanspec_num 64 */
	bzero(jp, sizeof(jp));
	memcpy(jp, &sl, 4);
	memcpy(jp + 4, what->ssid, sl);
	jp[36] = 0xff;
	memcpy(jp + 40, &minus1, 4);
	memcpy(jp + 44, &minus1, 4);
	memcpy(jp + 48, &minus1, 4);
	memcpy(jp + 52, &minus1, 4);
	if (any_bssid) {
		memset(jp + 56, 0xff, 6);
	} else {
		memcpy(jp + 56, what->bssid, 6);
	}
	err = iovar(true, "join", jp, sizeof(jp));
	if (err != 0) {
		/* brcmf_join_params: ssid 0, bssid 36, chanspec_num 44 */
		bzero(sp, sizeof(sp));
		memcpy(sp, &sl, 4);
		memcpy(sp + 4, what->ssid, sl);
		memcpy(sp + 36, jp + 56, 6);
		e_mode = ioctl(WLC_SET_SSID, sp, sizeof(sp), sizeof(sp));
		blog("join: \"join\" %d, WLC_SET_SSID %d", err, e_mode);
		if (e_mode != 0) {
			result = EIO;
			goto fail;
		}
	}

	result = ETIMEDOUT;
	clock_interval_to_deadline(20000, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (bcm_poll() == 0) {
			delay(5000);
		}
		if (ev_ssid && ev_ssid_status != 0) {
			result = ev_ssid_status == 3 ? ENETUNREACH : ev_ssid_status == 2 ? ETIMEDOUT : ECONNREFUSED;
			break;
		}
		if (ev_link_down && (ev_link_up || ev_ssid)) {
			result = secure ? EAUTH : ECONNREFUSED;
			break;
		}
		if (ev_psk && ev_psk_status == FWSUP_TIMEOUT) {
			result = EAUTH;
			break;
		}
		if (ev_link_up && (!secure || (ev_psk && ev_psk_status == FWSUP_COMPLETED))) {
			result = 0;
			break;
		}
		if (ev_link_up && host_sup) {
			result = 0;
			break;
		}
	}
	blog("join: %d (SET_SSID %s %u, link %s, keys %s %u, reason %u)", result, ev_ssid ? "said" : "not said", ev_ssid_status,
	    ev_link_up ? "up" : ev_link_down ? "down" : "-", ev_psk ? "said" : "not said", ev_psk_status, ev_down_reason);
	if (result != 0) {
		goto fail;
	}
	{
		uint8_t bssid[6];

		if (ioctl(WLC_GET_BSSID, bssid, sizeof(bssid), sizeof(bssid)) == 0) {
			memcpy(join_bss.bssid, bssid, 6);
		}
	}
	if (!flow_create(join_bss.bssid)) {
		result = EIO;
		goto fail;
	}
	if (host_sup) {
		/* the RSN element the firmware put in the association request: message 2 carries it as it was */
		uint32_t lens[2] = { 0, 0 };
		uint8_t *ies = kalloc_data(1024, Z_WAITOK | Z_ZERO);
		static const uint8_t standard[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f,
			                            0xac, 2, 0, 0 };

		wpa.rsn = NULL;
		if (wpa_rsn_length != 0) {
			wpa.rsn = wpa_rsn;
			wpa.rsn_length = wpa_rsn_length;
		} else if (ies != NULL && iovar(false, "assoc_info", lens, sizeof(lens)) == 0 && lens[0] != 0 && lens[0] <= 1024 &&
		    iovar(false, "assoc_req_ies", ies, lens[0]) == 0) {
			for (unsigned at = 0; at + 2 <= lens[0] && at + 2 + ies[at + 1] <= lens[0]; at += 2 + ies[at + 1]) {
				if (ies[at] == 48 && 2 + ies[at + 1] <= sizeof(wpa_rsn)) {
					memcpy(wpa_rsn, ies + at, 2 + ies[at + 1]);
					wpa.rsn = wpa_rsn;
					wpa.rsn_length = 2 + ies[at + 1];
					break;
				}
			}
		}
		if (ies != NULL) {
			kfree_data(ies, 1024);
		}
		if (wpa.rsn == NULL) {
			memcpy(wpa_rsn, standard, sizeof(standard));
			wpa.rsn = wpa_rsn;
			wpa.rsn_length = sizeof(standard);
		}
		memcpy(wpa.peer, join_bss.bssid, 6);
		blog("wpa: the association request's RSN element: %u bytes (%s)", wpa.rsn_length, wpa_rsn_length ?
		    "from the ASSOC_REQ_IE event" : lens[0] ? "the firmware's" : "a standard one");
		lck_mtx_lock(&ring_lock);
		ev_link_down = false;
		lck_mtx_unlock(&ring_lock);
		result = ETIMEDOUT;
		clock_interval_to_deadline(10000, NSEC_PER_MSEC, &deadline);
		while (mach_absolute_time() < deadline) {
			if (bcm_poll() == 0) {
				delay(2000);
			}
			if (wpa_done) {
				result = 0;
				break;
			}
			if (wpa_failed) {
				result = EAUTH;
				break;
			}
			if (ev_link_down) {
				result = EAUTH;
				break;
			}
		}
		blog("wpa: the handshake: %d%s%s (link %s, reason %u); %u frames received in all; rx post ring w %u, device r %u; "
		    "rx complete ring device w %u, r %u", result, wpa_why ? ": " : "", wpa_why ? wpa_why : "",
		    ev_link_down ? "down" : "up", ev_down_reason, rx_frames, rings[RING_RXPOST_SUBMIT].w_ptr,
		    idx_read(rings[RING_RXPOST_SUBMIT].r_idx_addr), idx_read(rings[RING_RX_COMPLETE].w_idx_addr),
		    rings[RING_RX_COMPLETE].r_ptr);
		if (result != 0) {
			console_dump("(the handshake failed)");
			flow_delete();
			goto fail;
		}
	}
	state_set(REDWOOD_STA_JOINED, &join_bss, 0);
	blog("W6 PASS: joined (%d dBm, %u MHz): en0's link is up", join_bss.rssi, join_bss.frequency);
	redwood_net_link_state(true);
	return 0;

fail:
	wpa_on = false;
	bzero(&wpa, sizeof(wpa));
	bzero(scb, sizeof(scb));
	(void)ioctl(WLC_DISASSOC, scb, sizeof(scb), sizeof(scb));
	state_set(REDWOOD_STA_IDLE, NULL, result);
	lost_pending = 0;
	return result;
}

static void
sta_get_state(struct redwood_sta_state *state)
{
	lck_spin_lock(&state_lock);
	*state = sta_state;
	lck_spin_unlock(&state_lock);
}

static unsigned
sta_channels(struct redwood_sta_channel *channels, unsigned most)
{
	static const uint8_t ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
		                       144, 149, 153, 157, 161, 165 };
	unsigned n = 0;

	for (unsigned ch = 1; ch <= 13 && n < most; ch++, n++) {
		channels[n].frequency = (uint16_t)(2407 + 5 * ch);
		channels[n].flags = 0;
	}
	for (unsigned i = 0; i < sizeof(ch5) && n < most; i++, n++) {
		channels[n].frequency = (uint16_t)(5000 + 5 * ch5[i]);
		channels[n].flags = ch5[i] >= 52 && ch5[i] <= 144 ? REDWOOD_STA_CHANNEL_PASSIVE | REDWOOD_STA_CHANNEL_RADAR : 0;
	}
	return n;
}

/* one frame out on the flow ring, from a buffer of ours (the handshake's replies) */
static bool
tx_raw(const uint8_t *frame, size_t length, unsigned *slot_out)
{
	uint8_t *item;
	unsigned slot;
	uint16_t data_len;

	if (!flow_up || length < 14 || length > TX_BUF_SIZE) {
		return false;
	}
	lck_mtx_lock(&ring_lock);
	if (tx_busy == ~0ULL) {
		(void)poll_rings();
	}
	if (tx_busy == ~0ULL || (item = ring_reserve(&flow)) == NULL) {
		lck_mtx_unlock(&ring_lock);
		return false;
	}
	slot = (unsigned)__builtin_ctzll(~tx_busy);
	memcpy(tx_buf[slot], frame, length);
	tx_busy |= 1ULL << slot;
	put_hdr(item, MSG_TX_POST, PKTID_TX(slot));
	memcpy(item + 8, tx_buf[slot], 14);
	item[22] = 0x01 | (7 << 5);             /* 802.3, priority 7 (brcmf sends EAPOL at the highest) */
	item[23] = 1;
	put_addr(item + 32, tx_phys[slot] + 14);
	data_len = (uint16_t)(length - 14);
	memcpy(item + 42, &data_len, 2);
	ring_commit(&flow);
	lck_mtx_unlock(&ring_lock);
	*slot_out = slot;
	return true;
}

/*
 * A key to the firmware: "wsec_key", struct brcmf_wsec_key_le (index 0, len 4, data 8, algo 112, flags 116,
 * iv_initialized 132, rxiv hi 140 lo 144, ea 156), as brcmf_cfg80211_add_key() fills it: the pairwise key with
 * the access point's address, the group key primary with its receive counter.
 */
static int
key_install(const uint8_t *key, unsigned length, unsigned index, const uint8_t *peer, const uint8_t *rsc)
{
	uint8_t k[WSEC_KEY_SIZE];
	uint32_t v;
	int err;

	if (length == 0 || length > 32) {
		return -1;
	}
	bzero(k, sizeof(k));
	v = index;
	memcpy(k, &v, 4);
	v = length;
	memcpy(k + 4, &v, 4);
	memcpy(k + 8, key, length);
	v = CRYPTO_ALGO_AES_CCM;
	memcpy(k + 112, &v, 4);
	if (peer != NULL) {
		memcpy(k + 156, peer, 6);
	} else {
		v = KEY_PRIMARY;
		memcpy(k + 116, &v, 4);
	}
	if (rsc != NULL) {
		uint32_t hi = (uint32_t)rsc[5] << 24 | (uint32_t)rsc[4] << 16 | (uint32_t)rsc[3] << 8 | rsc[2];
		uint16_t lo = (uint16_t)(rsc[1] << 8 | rsc[0]);

		v = 1;
		memcpy(k + 132, &v, 4);
		memcpy(k + 140, &hi, 4);
		memcpy(k + 144, &lo, 2);
	}
	err = iovar(true, "wsec_key", k, sizeof(k));
	bzero(k, sizeof(k));
	return err;
}

/* en0's frame out: copied into a TX buffer, posted on the flow ring (brcmf_msgbuf_txflow() [msgbuf]700) */
static bool
link_transmit(mbuf_t packet, size_t length)
{
	uint8_t *item;
	unsigned slot;
	uint16_t data_len;

	if (!flow_up || length < 14 || length > TX_BUF_SIZE) {
		tx_dropped++;
		return false;
	}
	lck_mtx_lock(&ring_lock);
	if (tx_busy == ~0ULL) {
		(void)poll_rings();             /* (the TX completions give buffers back) */
	}
	if (tx_busy == ~0ULL) {
		lck_mtx_unlock(&ring_lock);
		tx_dropped++;
		return false;
	}
	slot = (unsigned)__builtin_ctzll(~tx_busy);
	if (mbuf_copydata(packet, 0, length, tx_buf[slot]) != 0 || (item = ring_reserve(&flow)) == NULL) {
		lck_mtx_unlock(&ring_lock);
		tx_dropped++;
		return false;
	}
	tx_busy |= 1ULL << slot;
	/* struct msgbuf_tx_msghdr: the Ethernet header 8, flags 22, seg_cnt 23, metadata 24, data 32, metadata_len 40,
	 * data_len 42 */
	put_hdr(item, MSG_TX_POST, PKTID_TX(slot));
	memcpy(item + 8, tx_buf[slot], 14);
	item[22] = 0x01;                        /* BRCMF_MSGBUF_PKT_FLAGS_FRAME_802_3, priority 0 */
	item[23] = 1;
	put_addr(item + 32, tx_phys[slot] + 14);
	data_len = (uint16_t)(length - 14);
	memcpy(item + 42, &data_len, 2);
	ring_commit(&flow);
	tx_frames++;
	lck_mtx_unlock(&ring_lock);
	return true;
}

static struct redwood_sta_ops bcm_ops = {
	.power = sta_power,
	.scan = sta_scan,
	.heard = sta_heard,
	.join = sta_join,
	.leave = sta_leave,
	.state = sta_get_state,
	.channels = sta_channels,
};

static struct redwood_net_link bcm_link = {
	.name = "the BCM4375 (PCIe)",
	.wireless = true,
	.transmit = link_transmit,
	.poll = bcm_poll,
};

/* en0 and the station; then the network of fw/network (two lines: its name, its passphrase), if there is one */
static void
service_start(void)
{
	static struct redwood_sta_join what;
	unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase);
	int err;

	memcpy(bcm_ops.address, mac, 6);
	memcpy(bcm_link.address, mac, 6);
	state_set(REDWOOD_STA_IDLE, NULL, 0);
	link_attached = redwood_net_attach(&bcm_link);
	if (!link_attached) {
		blog("W6 FAIL: en0 is someone else's already");
		return;
	}
	redwood_sta_attach(&bcm_ops);
	blog("W6: en0 is the BCM4375, and the station iOS's Wi-Fi side asks (scan, join, leave)");
	bzero(&what, sizeof(what));
	if (!redwood_wifi_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
		blog("W6: no fw/network: nothing joined by the kernel; Settings > Wi-Fi joins");
		return;
	}
	what.ssid_length = (uint8_t)name_length;
	what.passphrase_length = (uint8_t)passphrase_length;
	what.security = REDWOOD_STA_WPA2_PSK;
	err = redwood_sta_ask_join_standing(&what);
	bzero(&what, sizeof(what));
	blog("W6: the network of fw/network (a name of %u bytes) asked to be joined: %d", name_length, err);
}

void
redwood_bcm4375_run(const struct redwood_bcm_bus *b)
{
	int err;

	bus = b;
	cfg = b->cfg;
	pool = (uint8_t *)b->pool_virt;
	pool_used = 0;
	blog("W4: the BCM4375's firmware (pool %#llx, %zu KB mapped)", b->pool_phys, b->pool_mapped >> 10);
	found = kalloc_data(FOUND_MOST * sizeof(*found), Z_WAITOK | Z_ZERO);
	if (found == NULL || !bars() || !chip() || !firmware()) {
		return;
	}
	if (!shared() || !rings_init() || !scratch_init()) {
		console_dump("(after a failure)");
		return;
	}
	/* brcmf_pcie_intr_enable() then brcmf_pcie_hostready() [pcie]916..928 (the interrupt itself is not taken: the
	 * GIC's line stays off and poll_rings() acknowledges the mailbox) */
	lck_mtx_lock(&ring_lock);
	pcie2_write(mb_mask, pcie2->rev >= 64 ? 0xffff : 0x00ff0300);
	mb_enabled = true;
	if (shared_flags & SH_HOSTRDY_DB1) {
		pcie2_write(h2d_mb1, 1);
	}
	lck_mtx_unlock(&ring_lock);
	lck_mtx_lock(&ring_lock);
	if (!buffers()) {
		lck_mtx_unlock(&ring_lock);
		return;
	}
	lck_mtx_unlock(&ring_lock);
	blog("rings and buffers up: %zu KB of the pool used", pool_used >> 10);
	if (!setup()) {
		console_dump(NULL);
		return;
	}
	/*
	 * en0 now, before the scan: configd makes its "Wi-Fi" network service (and so DHCP) only of the interfaces
	 * there when it starts, about 27 s into the boot; the scan took us past that (run 9: en0 at 30 s, no service).
	 */
	service_start();
	err = escan_run(NULL);
	console_dump(NULL);
	blog("W5 %s: escan %s (%d), %u networks heard, %u events, %u frames received", nfound ? "PASS" : "FAIL",
	    escan_done ? "done" : "NOT done in 20 s", err, nfound, events_seen, rx_frames);
	for (unsigned i = 0; i < nfound; i++) {
		blog("  %02x:%02x:%02x:%02x:%02x:%02x %4u MHz %4d dBm %s \"%.*s\"", found[i].bssid[0], found[i].bssid[1],
		    found[i].bssid[2], found[i].bssid[3], found[i].bssid[4], found[i].bssid[5], found[i].frequency, found[i].rssi,
		    security_name(&found[i]), found[i].ssid_length, found[i].ssid);
	}
}
