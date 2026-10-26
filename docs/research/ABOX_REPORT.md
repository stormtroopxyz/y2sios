# Exynos 9830 ABOX Speaker Path — Register Reference & DSP-Bypass Feasibility Report

Target: Galaxy S20+ "y2s" (Exynos 990 / ABOX IP **v3**, `SND_SOC_SAMSUNG_ABOX_VERSION = 0x30001`), goal: drive
`DRAM ring buffer -> RDMA -> SPUS -> SIFS0 -> UAIF2 (TDM/dsp_a, ABOX = BCLK master) -> 2x CS35L41` entirely from a
custom AP-side (non-secure EL1) kernel, **without booting the Cortex-A32 "Calliope" DSP**.

Primary source: `ExtremeXT/android_kernel_samsung_exynos990`, commit `69515fbb7a4395898c05a8624f76a12afbac11c5`
(this is a generic Exynos990 GPL kernel drop, **not** Samsung's y2s board files specifically — no y2s/z3s board
`.dts` or `cs35l41` board instantiation exists in this tree, so device-tree *board* facts below are cross-checked
against your local stock `mixer_paths.xml`/`abox_tplg.conf`, which **are** y2s-specific). All register-level C/H
driver code, however, is the literal ABOX v3 / CS35L41 driver used across Exynos990 devices including y2s.

All saved source files referenced below are copied next to this report in this same folder.

---

## 0. Executive summary / answer to the core question

**Routing/format registers = plain AP-writable silicon.** The RDMA↔SPUS↔SIFS↔UAIF *crossbar* (which SPUS output
feeds which SIFS, which SIFS feeds which UAIF, UAIF TDM format/master-slave/slot-count/enable) lives in the ABOX
"System/SPUS/ROUTE" SFR block (`0x18C50000`–`0x18C50300`) and is programmed by plain `regmap`/MMIO
`snd_soc_component_update_bits()` calls in `abox_cmpnt.c` / `abox_if.c`. **None of this requires Calliope.**
Confirmed: `ABOX_ROUTE_UAIF_SPK`, `ABOX_FUNC_CHAIN_SRC_OUT` (SPUS OUT mux), `ABOX_SIFS_OUT_SEL`, all of
`UAIF_CTRL0/CTRL1` — all plain register fields, all written directly from the AP driver (see §2).

**RDMA buffer/enable = IPC-only in the stock driver, but that's a software choice, not a documented hardware
wall.** `abox_rdma_hw_params()` / `_prepare()` / `_trigger()` **never** touch `RDMA_BUF_STR/BUF_END/BUF_OFFSET`,
the `CTRL0.ENABLE` bit, or the `CTRL1` width/channel/format bits. All of that is sent as an `IPC_PCMPLAYBACK`
message (`PCM_SET_BUFFER`, `PCM_PLTDAI_HW_PARAMS`, `PCM_PLTDAI_PREPARE`, `PCM_PLTDAI_TRIGGER`) to Calliope, which —
somewhere in its closed firmware — performs the actual register writes. The only two RDMA registers the AP pokes
directly are `CTRL0.BURST_LEN` and `CTRL0.AUTO_FADE_IN` (cosmetic/perf bits). **The register map itself gives no
indication these fields are hardware-gated on the DSP** (no `need_smc`/secure marker anywhere in the ABOX SFR
region, unlike the `pd-aud` power domain — see §1.3), and the exact same generic "xDMA" IP (`BUF_STR/BUF_END/
BUF_OFFSET/STR_POINT/ENABLE`, see `abox_soc_3.h:437-481`) is reused by the WDMA/DDMA blocks and by other Samsung
SoC subsystems that **are** driven directly by the AP with no embedded DSP at all (e.g. VTS capture DMA on several
Exynos SoCs). So: **bypass is very plausible, but unproven by source alone** — the exact bit-patterns Calliope
writes into `BUF_STR/BUF_END/BUF_OFFSET/CTRL1` are not visible anywhere in the open driver; they live in the
closed `calliope_dram.bin`/`calliope_sram.bin` firmware. See §2.4 for a concrete recommended bring-up strategy
(dump live register state from a running stock Android boot, then replicate without booting Calliope).

**SIFS0 itself needs no DSP**: it is a pure hardware **mixer** (sums whatever `SPUS OUTn` are routed into it), not
a mux requiring a DSP-managed selection — it just passively sums audio from any RDMA chain that is actually
producing samples. **BD_MIXER is the one genuinely DSP-resident block**: it is a topology-defined (`abox_tplg.conf`)
software mixing node that only exists inside Calliope's firmware graph, used by stock Android to combine
simultaneous streams (primary/deep/fast/compress) before RDMA3. **You don't need it** — for a single continuous OS
audio stream you simply leave `RDMA3_A = None` (the mixer_paths.xml init default, §4) so RDMA3 pulls straight from
its own DRAM ring buffer, exactly matching your intended design.

**CS35L41 needs no DSP either for basic audio.** Setting "PCM Source = ASP" (`DAC_PCM1_SRC = ASPRX1`, i.e. the
same config used for the earpiece/`dev-handset` path) completely bypasses the amp's own internal Halo-core
protection DSP — no CS35L41 firmware patch/preload needed for a first audible test. There is **no dedicated
sine/tone generator** in this chip (see §3.4) — the simplest "make noise" path is still feeding real TDM data.

---

## 1. ABOX v3 register map (Exynos 9830) relevant to playback

Source: `sound/soc/samsung/abox/abox_soc_3.h` (saved here as `abox_soc_3.h`), selected via
`Kconfig: SND_SOC_SAMSUNG_ABOX_VERSION = 0x30001 if SOC_EXYNOS9830` (not `_EVT0`).
GitHub: `ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/sound/soc/samsung/abox/Kconfig#L10-L23`

All offsets below are **relative to ABOX SFR base `0x18C50000`** (dts `abox@18c50000 { reg = <0x18c50000 0x10000 ...>; reg-names = "sfr","sysreg","sram","timer"; }`,
`ExtremeXT/.../arch/arm64/boot/dts/exynos/exynos9830.dts#L6051-L6052`).

### 1.1 Global / routing registers
`abox_soc_3.h:14-115` (saved file, same lines)

| Name | Offset | Abs addr | Notes |
|---|---|---|---|
| `ABOX_IP_INDEX` | 0x0000 | 0x18C50000 | |
| `ABOX_VERSION` | 0x0004 | 0x18C50004 | bits[31:8] = IP version (3.0.1 on y2s) |
| `ABOX_SYSPOWER_CTRL` | 0x0010 | 0x18C50010 | bit0: request DRAM access power (see `abox_request_dram_on()`, `abox.c:1000-1036`) |
| `ABOX_SYSPOWER_STATUS` | 0x0014 | 0x18C50014 | bit0: DRAM-power-acked status, polled by `abox_check_dram_status()` |
| `ABOX_SYSTEM_CONFIG0` | 0x0020 | 0x18C50020 | |
| `ABOX_REMAP_MASK` / `ABOX_REMAP_ADDR` | 0x0024/0x0028 | 0x18C50024/28 | Calliope boot-vector remap — **leave untouched to help keep DSP from fetching valid code even if accidentally un-reset** |
| `ABOX_DYN_CLOCK_OFF`/`_OFF1` | 0x0030/0x0034 | | per-block dynamic clock gating disable bits |
| `ABOX_QCHANNEL_DISABLE` | 0x0038 | | |
| **`ABOX_ROUTE_CTRL0`** | **0x0040** | **0x18C50040** | bits[31:28] `ROUTE_DSIF`; **bits[3:0],[7:4],...,[27:24] = `ROUTE_UAIF_SPK(0..6)`** — i.e. UAIF*n* "SPK" (TX/speaker direction) source-select mux, 4 bits/UAIF (`ABOX_ROUTE_UAIF_SPK_L(x)=x*4`). **UAIF2's field is bits[11:8].** |
| **`ABOX_ROUTE_CTRL1`** | **0x0044** | **0x18C50044** | bits[29:25] `ROUTE_SPUSM` (SPUS capture-mux select); bits[24:0] = 5×`ROUTE_NSRC(0..4)` (5 bits each) |
| `ABOX_ROUTE_CTRL2` | 0x0048 | | `RSRC_CONNECTION_TYPE`/`NSRC_CONNECTION_TYPE` bits, `ROUTE_RSRC(0..?)`, `ROUTE_SPUST` |
| `ABOX_TICK_DIV_RATIO`/`ABOX_TICK_GEN`(`=ABOX_MO_CTRL`) | 0x0050/0x0054 | | |
| `ABOX_ROUTE_CTRL3`/`_CTRL4` | 0x0080/0x0084 | | extra routing (DSIF-era SoCs mostly) |
| `ABOX_SW_PDI_CTRL0..3` | 0x00A0-0x00AC | | serial-PDI (bus-interface) format fields, sidetone related |

### 1.2 SPUS (playback-side processing) / RDMA / SIFS registers
`abox_soc_3.h:117-213` and `:437-481` (saved file)

| Name | Offset | Notes |
|---|---|---|
| `ABOX_SPUS_CTRL_FC0`(`=ABOX_SPUS_CTRL0`) | 0x0200 | holds `FUNC_CHAIN_SRC_OUT` mux for **SPUS-OUT 0-3** (8 bits each: `IN`[2 bits],`OUT`[4 bits],`ASRC`[1 bit]) — **RDMA3's output mux (`SPUS OUT3`) lives here**, confirmed by `abox_cmpnt.c:4111-4115` (`spus_out3_enum` bound to `ABOX_SPUS_CTRL_FC0`). Destination codes (`spus_outx_texts[]`, `abox_cmpnt.c:4092-4095`): `0=RESERVED,1=SIFS0,2=SIFS1,3=RESERVED,4=SIFS2,...` |
| `ABOX_SPUS_CTRL1` | 0x0204 | `SIFSM_IN_SEL`[11:8], **`SIFS_OUT_SEL(x)`[one 4-bit field per SIFS 1..5]** — this is the real "mux" register for SIFS1-5 (SIFS0 needs none, it's a mixer — see below), `MIXP_FORMAT`[4:0] |
| `ABOX_SPUS_CTRL2` | 0x0208 | `SIFST_IN_SEL`, `MIXP_LD_FLUSH`, `MIXP_FLUSH` |
| `ABOX_SPUS_CTRL3` | 0x020C | `SIFS_FLUSH(x)` per-SIFS flush bits |
| `ABOX_SPUS_CTRL4`/`CTRL5` | 0x0210/0214 | `SRC_ASRC_ID` |
| `ABOX_SPUS_CTRL_FC1` | 0x0218 | holds OUT4-7 mux fields (OUT7 confirmed at `abox_cmpnt.c:4116-4120` for OUT4, similar decl for OUT7) |
| `ABOX_SPUS_CTRL_FC2` | 0x021C | holds OUT8-11 mux fields |
| `ABOX_SPUS_SBANK_RDMA(x)` | 0x0220+4x | per-RDMA on-chip staging-SRAM bank allocation (`abox_cmpnt_adjust_sbank()`, `abox_cmpnt.c:5524`, called from `abox_rdma_hw_params()`) |
| `ABOX_SPUS_SBANK_ASRC(x)` | 0x0250+4x | |
| `ABOX_SPUS_SBANK_MIXP` / `_SIDETONE` | 0x0280/0x0284 | |
| `ABOX_SPUS_CTRL_SIFS_CNT(x)` | 0x0290 (+4x, with wraparound for x>3/4) | per-SIFS sample-count register — reset by AP via `abox_cmpnt_reset_cnt_val()` (`abox_cmpnt.c:5600-5638`), a **plain register write**, called at the tail of every UAIF playback `hw_params` |
| `ABOX_SPUS_LATENCY_CTRL0..3` | 0x02A0-0x02B8 | |
| **`ABOX_RDMA_BASE`** | **0x1000**, `ITV=0x100` | → **RDMA*n* at `0x18C51000 + 0x100*n`**, matches your stated formula exactly, and matches dts `abox-rdma@18c51300` = RDMA3 (`exynos9830.dts:6162-6174`) |
| `ABOX_RDMA_CTRL0(x)` | +0x00 | `BURST_LEN`[22:19], `ENABLE`[0] — **ENABLE is a real bit here**, but see §2.1: stock driver never sets it directly |
| `ABOX_RDMA_CTRL1(x)` | +0x04 | generic "xDMA" format register, shared field layout (`abox_soc_3.h:552-591`, `DMA_REG_CTRL1` in `abox_dma.h:22`): `WIDTH`[12:11], `CHANNELS`[10:8] (or combined `FORMAT`[12:8] = `abox_get_format()` encoding, see §2.2), `BUF_UPDATE`[5], `BUF_TYPE`[4], `DUMMY_START`[1], `ENABLE`[0] (dup. with CTRL0's bit0 depending on generation) |
| `ABOX_RDMA_BUF_STR(x)` | +0x08 | ring-buffer physical start address, bits[31:4] (16-byte aligned) |
| `ABOX_RDMA_BUF_END(x)` | +0x0C | ring-buffer physical end address, bits[31:4] |
| `ABOX_RDMA_BUF_OFFSET(x)` | +0x10 | bits[15:4] — period-size-like offset |
| `ABOX_RDMA_STR_POINT(x)` | +0x14 | bits[31:4] — current/initial read pointer |
| `ABOX_RDMA_VOL_FACTOR(x)`/`VOL_CHANGE(x)` | +0x18/+0x1C | 24-bit volume/ramp factor (DSP uses this for smooth volume — confirms why Samsung likes doing this in firmware) |
| `ABOX_RDMA_SBANK_LIMIT(x)` | +0x20 | |
| `ABOX_RDMA_BIT_CTRL0/1(x)` | +0x24/+0x28 | |
| `ABOX_RDMA_STATUS(x)` | +0x30 | bit31 `PROGRESS`; `RBUF_OFFSET`/`RBUF_CNT` (field split differs by ABOX_SOC_VERSION — on 9830 (`<3.0.0` branch doesn't apply; we're exactly `3.0.1 > 3.0.0`) uses `RBUF_OFFSET`[27:12], `RBUF_CNT`[11:0], `abox_soc_3.h:463-477`) — **read-only, polled by AP** in `abox_rdma_pointer()` (`abox_rdma.c:1587-1639`) to compute the ALSA position; this read path is plain AP MMIO, confirming the status register is live/AP-visible regardless of who programs BUF_STR etc. |
| `ABOX_WDMA_BASE` | 0x3000, ITV 0x100 | same generic layout, capture direction |
| `ABOX_UAIF_BASE` | **0x0500**, ITV 0x10 | → **UAIF*n* at `0x18C50500 + 0x10*n`** → **UAIF2 = `0x18C50520`**, matches dts `abox-uaif@18c50520` (`exynos9830.dts:6471-6485`) |
| `ABOX_UAIF_CTRL0(x)` | +0x0 | `START_FIFO_DIFF_MIC`[31:28],`START_FIFO_DIFF_SPK`[27:24],`START_FIFO_SIZE_MIC`[23:20],`START_FIFO_SIZE_SPK`[19:16], `DATA_MODE`[4], `IRQ_MODE`[3], **`MODE`[2]** (0=slave,1=master — see §2.3), `MIC_ENABLE`[1], **`SPK_ENABLE`[0]** |
| `ABOX_UAIF_CTRL1(x)` | +0x4 | **`FORMAT`[28:24]** (5-bit code = `(channels-1) | width_code<<3`, see §2.2), `BCLK_POLARITY`[23], **`WS_MODE`[22]** (0=I2S,1=**DSP_A/TDM**), `WS_POLAR`[21], `SLOT_MAX`[20:18], `SBIT_MAX`[17:12], `VALID_STR`[11:6], `VALID_END`[5:0] |
| `ABOX_UAIF_IRQ_CTRL(x)` | +0x8 | `FILTER_CNT`[6:0] |
| `ABOX_UAIF_STATUS(x)` | +0xC | `ERROR_OF_MIC`[1], etc. |
| `ABOX_CA32_CORE0_BASE` | 0x5000, ITV 4 | Calliope core0 GPR dump window (`abox_soc_3.h:772-775`) → **`0x18C55000`**, exactly matches your stated DSP-core address and dts `abox-core@18c55000` |
| `ABOX_CA32_CORE1_BASE` | 0x5080 | second core (`abox-core@18c55080`) — a second Calliope instance/companion core with its own firmware (`calliope_sram_2.bin`/`calliope_dram_2.bin`) |
| `ABOX_CA32_STATUS` | 0x5100 | |

GitHub permalink for the whole header:
`ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/sound/soc/samsung/abox/abox_soc_3.h`

### 1.3 Power domain (`pd-aud`) and CA32/Calliope reset — **outside** the ABOX SFR block

From `arch/arm64/boot/dts/exynos/exynos9830.dts` (full file downloaded from
`https://raw.githubusercontent.com/ExtremeXT/android_kernel_samsung_exynos990/69515fbb7a4395898c05a8624f76a12afbac11c5/arch/arm64/boot/dts/exynos/exynos9830.dts`,
17484 lines; this is a flattened/decompiled DTB — numeric phandles, no board overlay for y2s is present in this
repo, only the common SoC dts):

```
pd-aud@15861900 {                              // dts line 5308
    compatible = "samsung,exynos-pd";
    reg = <0x00 0x15861900 0x20>;
    cal_id = <0xb1380000>;
    need_smc = <0x18e10204>;                   // <-- power sequencing gated behind an SMC!
    power-down-ok = <0x01>;
    #power-domain-cells = <0x00>;
    skip-idle-ip;
};
```
and `aliases { pd_aud = "/pd-aud@15861900"; }` (dts line 17103).

`pmu_system_controller = "/system-controller@15860000";` (dts line 17385) → **PMU_ALIVE base = `0x15860000`**, so
`pd-aud`'s control/status pair sits at `0x15861900`..`0x1586191F` within PMU_ALIVE.

**Important nuance**: `need_smc = <0x18e10204>` means the generic `samsung,exynos-pd` driver performs this
particular power-domain's on/off sequencing via an **SMC call into existing ARM Trusted Firmware/PWRCAL**, not a
raw register poke (Exynos990-era SoCs moved PD sequencing into secure firmware/ACPM for many domains). A
non-secure-EL1 custom OS can still issue this if it reuses the *same* SMC function-ID/arg convention the stock
bootloader's TF-A already implements (it doesn't require replacing EL3) — but this needs to be validated against
your boot chain; it is **not** a plain `writel()`.

Calliope cores — reset/power, **inside PMU_ALIVE, separate from the audio-IP SFR block** (dts lines 6074-6124):
```
abox-core@18c55000 {                 // CA32 core 0 ("main" Calliope DSP)
    compatible = "samsung,abox-core";
    reg = <0x18c55000 0x80>;          // GPR dump window only — not control
    samsung,id = <0>;  samsung,type = "CA32";
    samsung,pmu_power   = <0x3180 0x01>;   // PMU_ALIVE + 0x3180 = 0x15863180
    samsung,pmu_enable  = <0x31a0 0x01>;   // PMU_ALIVE + 0x31a0 = 0x158631A0  <-- the actual reset-release bit
    samsung,pmu_standby = <0x31a4 0x11>;   // PMU_ALIVE + 0x31a4 = 0x158631A4
};
abox-core@18c55080 {                 // CA32 core 1 (second/companion Calliope core)
    samsung,pmu_power   = <0x3200 0x01>;   // 0x15863200
    samsung,pmu_enable  = <0x3220 0x01>;   // 0x15863220
    samsung,pmu_standby = <0x3224 0x01>;   // 0x15863224
};
```
These `pmu_power`/`pmu_enable`/`pmu_standby` triples are consumed verbatim by `abox_core.c`:
```c
// abox_core.c:94-104 (abox_core_power) and :107-118 (abox_core_enable)
list_for_each_entry(core, &cores, list) {
    unsigned int offset = core->pmu_power[OFFSET];
    unsigned int mask   = core->pmu_power[MASK];
    exynos_pmu_update(offset, mask, on ? mask : 0);   // plain PMU_ALIVE regmap write
}
```
`abox_cpu_power(bool on)` (`abox.c:2211-2213`) just calls `abox_core_power(on)`, and `abox_cpu_enable()`
(`abox.c:2215-2221`) only calls `abox_core_enable()` **on disable** ("reset core only if disable" — comment in
source). **Conclusion: to keep the DSP in reset, your custom kernel must simply never write bit0 of PMU_ALIVE
offsets `0x31A0`/`0x3220` (enable) nor `0x3180`/`0x3200` (power) — this is a completely separate register pair
from anything in the `0x18C5xxxx` ABOX SFR block, and `exynos_pmu_update()` here is a plain regmap/MMIO write
(no `need_smc` on these core nodes, unlike `pd-aud`).** `ABOX_REMAP_ADDR`/`ABOX_REMAP_MASK` (§1.1) additionally
let you control where Calliope's PC vector points if it were ever released — leave at reset default (0) for extra
safety.

### 1.4 CMU_AUD clocks (PLL_AUD, UAIF2 BCLK mux/divider)

Exynos9830 clocks are **not** driven by a hand-written `clk-exynos9830.c` register table directly; the driver uses
Samsung's generic "CAL" (Clock/power Abstraction Layer) indirection, but **the underlying SFR addresses are plain,
statically-known local MMIO** — confirmed by the generated lookup table
`drivers/soc/samsung/cal-if/exynos9830/cmucal-sfr.c`:

```c
SFR_BLOCK(CMU_AUD, 0x18c00000, 0x8000),                                  // cmucal-sfr.c:6  -> CMU_AUD base = 0x18C00000 (!), right below the ABOX SFR block at 0x18C50000
SFR(PLL_LOCKTIME_PLL_AUD0, 0x0,   CMU_AUD),                              // cmucal-sfr.c:53 -> 0x18C00000
SFR(PLL_CON3_PLL_AUD0,     0x10c, CMU_AUD),                              // cmucal-sfr.c:54 -> 0x18C0010C
SFR(PLL_CON5_PLL_AUD0,     0x114, CMU_AUD),                              // cmucal-sfr.c:55 -> 0x18C00114
SFR(PLL_LOCKTIME_PLL_AUD1, 0x4,   CMU_AUD),                              // cmucal-sfr.c:56
SFR(PLL_CON3_PLL_AUD1,     0x14c, CMU_AUD),                              // cmucal-sfr.c:57
SFR(PLL_CON5_PLL_AUD1,     0x154, CMU_AUD),                              // cmucal-sfr.c:58
SFR(CLK_CON_MUX_MUX_CLK_AUD_UAIF2,            0x1014, CMU_AUD),          // cmucal-sfr.c:96  -> 0x18C01014
SFR(CLK_CON_DIV_DIV_CLK_AUD_AUDIF,            0x1800, CMU_AUD),          // cmucal-sfr.c:389 -> 0x18C01800
SFR(CLK_CON_DIV_DIV_CLK_AUD_UAIF2,            0x1834, CMU_AUD),          // cmucal-sfr.c:394 -> 0x18C01834
SFR(CLK_CON_GAT_GOUT_BLK_AUD_UID_ABOX_IPCLKPORT_BCLK_UAIF2, 0x2028, CMU_AUD), // :623 -> 0x18C02028

SFR_ACCESS(PLL_CON3_PLL_AUD0_ENABLE,  31, 1, PLL_CON3_PLL_AUD0),  // cmucal-sfr.c:2999
SFR_ACCESS(PLL_CON3_PLL_AUD0_STABLE,  29, 1, PLL_CON3_PLL_AUD0),  // :3000
SFR_ACCESS(PLL_CON3_PLL_AUD0_DIV_P,    8, 6, PLL_CON3_PLL_AUD0),  // :3001  (P divider)
SFR_ACCESS(PLL_CON3_PLL_AUD0_DIV_M,   16,10, PLL_CON3_PLL_AUD0),  // :3002  (M divider/multiplier)
SFR_ACCESS(PLL_CON3_PLL_AUD0_DIV_S,    0, 3, PLL_CON3_PLL_AUD0),  // :3003  (S divider, post-scaler)
SFR_ACCESS(PLL_CON5_PLL_AUD0_DIV_K,    0,16, PLL_CON5_PLL_AUD0),  // :3004  (fractional K)
SFR_ACCESS(CLK_CON_MUX_MUX_CLK_AUD_UAIF2_SELECT, 0, 2, ...),      // :3121  mux select
SFR_ACCESS(CLK_CON_MUX_MUX_CLK_AUD_UAIF2_BUSY,  16, 1, ...),      // :3122
SFR_ACCESS(CLK_CON_DIV_DIV_CLK_AUD_UAIF2_DIVRATIO, 0, 9, ...),    // :3734  divider ratio (9-bit)
SFR_ACCESS(CLK_CON_DIV_DIV_CLK_AUD_UAIF2_BUSY,    16, 1, ...),    // :3735
```
GitHub: `ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/drivers/soc/samsung/cal-if/exynos9830/cmucal-sfr.c`
(full file is 1.3 MB — fetched live with `curl`+`grep`, not saved whole into this folder; the `.h` with the enum
IDs, `cmucal-sfr.h`, was similarly spot-checked for lines 1-110).

Clock IDs exposed to Linux's common-clock-framework names (`clk-exynos9830.c:248-260,518-536`, saved in this
folder):
```c
struct init_vclk exynos9830_aud_vclks[] = {
    VCLK(DOUT_CLK_AUD_AUDIF,  DIV_CLK_AUD_AUDIF,  "DOUT_CLK_AUD_AUDIF",  0,0,NULL),   // :519
    VCLK(DOUT_CLK_AUD_UAIF2,  DIV_CLK_AUD_UAIF2,  "DOUT_CLK_AUD_UAIF2",  0,0,NULL),   // :524
    VCLK(DOUT_CLK_AUD_BUS,    DIV_CLK_AUD_BUS,    "DOUT_CLK_AUD_BUS",    0,0,NULL),   // :530
    VCLK(DOUT_CLK_AUD_CNT,    DIV_CLK_AUD_CNT,    "DOUT_CLK_AUD_CNT",    0,0,NULL),   // :534
    VCLK(PLL_OUT_AUD0, PLL_AUD0, "PLL_OUT_AUD0", 0,0,NULL),                           // :535
    VCLK(PLL_OUT_AUD1, PLL_AUD1, "PLL_OUT_AUD1", 0,0,NULL),                           // :536
};
```
These feed the five `abox@18c50000` clock inputs named in the dts (`clocks=<...>; clock-names="pll","pll1","audif","bus","cnt";`,
`exynos9830.dts:6059-6060`), and separately `abox-uaif@18c50520`'s own `clk_bclk`/`clk_mux` (CMU_AUD
`MUX_CLK_AUD_UAIF2`→`DIV_CLK_AUD_UAIF2`→gate `BCLK_UAIF2`) are what `abox_uaif_hw_params()` programs directly via
`clk_set_rate()` when UAIF2 is bus **master** (see §2.3) — i.e. **the AP computes and sets the UAIF2 BCLK frequency
itself** (`rate * channels * width`), it is not something Calliope needs to do.

**Caveat**: these CMU registers are accessed through Linux's `clk` framework + a "CAL" abstraction layer backed by
this static table (`cmucal-sfr.c`), which is compiled-in data, not a live ACPM round-trip for *this* specific
block (no `acpm-ipc-channel` marker on the per-IP entries, only on the giant top-level
`clock-controller@0x1a330000` node that multiplexes *all* Exynos clock domains through one Linux `clk` provider —
`exynos9830.dts:14778-14784`). Treat this as "very likely a plain local MMIO write, same semantics as any other
Samsung CMU," but you should verify empirically (no secure/ACPM round-trip observed in these specific SFR_ACCESS
entries) before fully relying on it, since I could not find authoritative public documentation that CMU_AUD writes
never go through ACPM on this SoC generation.

---

## 2. AP-driven register writes vs. DSP/IPC, per playback-start step

Source files saved in this folder: `abox_rdma.c`, `abox_if.c` (UAIF — note: this *file* is `abox_if.c` but its
internal header comment still says `abox_uaif.c`, a leftover from an earlier rename — it is the real, current
UAIF driver), `abox_cmpnt.c`, `abox_core.c`, `abox.c`, `abox_ipc_uapi.h`.

### 2.1 RDMA (e.g. RDMA3) — `abox_rdma.c`

| ALSA call | What's written directly by the AP (`regmap`/`snd_soc_component_*`) | What's sent via IPC to Calliope |
|---|---|---|
| `abox_rdma_hw_params()` (`abox_rdma.c:1342-1456`) | `DMA_REG_CTRL0.BURST_LEN` only (`snd_soc_component_update_bits(..., DMA_REG_CTRL0, ABOX_RDMA_BURST_LEN_MASK, ...)`, line ~1403); also calls `abox_cmpnt_adjust_sbank()` (plain `ABOX_SPUS_SBANK_RDMA` register write, `abox_cmpnt.c:5524-5675`) | `IPC_PCMPLAYBACK` / `PCM_SET_BUFFER` (phys addr, period bytes, period count) — line ~1418; then `IPC_PCMPLAYBACK` / `PCM_PLTDAI_HW_PARAMS` (sample_rate, bit_depth, channels, packed) — line ~1425 |
| `abox_rdma_prepare()` (`:1493-1527`) | `DMA_REG_CTRL.AUTO_FADE_IN` bit only; calls `abox_cmpnt_sifsm_prepare()` (plain regmap reads/writes of `ABOX_SIDETONE_CTRL`/`ABOX_UAIF_CTRL1`/`ABOX_SW_PDI_CTRL*` — see `abox_cmpnt.c:4179-4260`) | `IPC_PCMPLAYBACK` / `PCM_PLTDAI_PREPARE` |
| `abox_rdma_trigger()` (`:1528-1586`) | *(nothing — no regmap write at all)* | `IPC_PCMPLAYBACK` / `PCM_PLTDAI_TRIGGER` (param=1 start / 0 stop); on stop for `PLATFORM_REALTIME` type also `IPC_ERAP` / `REALTIME_STOP` |
| `abox_rdma_pointer()` (`:1587-1638`) | **Read-only**: `regmap_read(ABOX_RDMA_STATUS(id))` for `PROGRESS`/`RBUF_OFFSET`/`RBUF_CNT` | — |
| `abox_rdma_hw_free()` (`:1457-1492`) | — | `IPC_PCMPLAYBACK` / `PCM_PLTDAI_HW_FREE` |

**`RDMA_CTRL0.ENABLE`, `RDMA_CTRL1` (format/width/channels), `RDMA_BUF_STR/BUF_END/BUF_OFFSET/STR_POINT`,
`VOL_FACTOR/VOL_CHANGE` are never written anywhere in `abox_rdma.c`.** Grep confirms zero hits for
`BUF_STR|BUF_END|BUF_OFFSET|STR_POINT|ENABLE_MASK|DMA_FORMAT|DMA_WIDTH|DMA_CHANNELS` outside of bit-shift/field
math used to *decode* the (read-only) STATUS register. The IPC message struct is defined in
`include/sound/samsung/abox_ipc.h` (saved as `abox_ipc_uapi.h`):
```c
enum PCMMSG {
    PCM_CPUDAI_HW_PARAMS = 4, PCM_CPUDAI_SET_FMT = 5, PCM_CPUDAI_STARTUP = 8,
    PCM_PLTDAI_HW_PARAMS = 14, PCM_PLTDAI_HW_FREE = 15, PCM_PLTDAI_PREPARE = 16,
    PCM_PLTDAI_TRIGGER = 17, PCM_PLTDAI_POINTER = 18, PCM_SET_BUFFER = 20, ...
};
struct PCMTASK_SET_BUFFER { int phyaddr; int size; int count; };
```
(`abox_ipc_uapi.h:28-52`, originally `include/sound/samsung/abox_ipc.h`)

### 2.2 SPUS / SIFS routing — `abox_cmpnt.c` (plain registers, confirmed)

```c
// abox_cmpnt.c:4111-4115  RDMA3's SPUS-OUT mux is a DAPM enum directly on a register:
static SOC_ENUM_SINGLE_DECL(spus_out3_enum, ABOX_SPUS_CTRL_FC0,
        ABOX_FUNC_CHAIN_SRC_OUT_L(3), spus_outx_texts);
static const struct snd_kcontrol_new spus_out3_controls[] = {
    SOC_DAPM_ENUM("DEMUX", spus_out3_enum),
};
```
```c
// abox_cmpnt.c:4524-4527  UAIF2's SPK-source mux is likewise a plain register enum:
static SOC_ENUM_SINGLE_DECL(uaif2_spk_enum, ABOX_ROUTE_CTRL0,
        ABOX_ROUTE_UAIF_SPK_L(2), uaif_spkx_texts);   // texts: RESERVED,SIFS0,SIFS1,SIFS2,SIFS3,SIFS4,SIFS5,...,SIFMS
```
```c
// abox_cmpnt.c:5600-5638  abox_cmpnt_reset_cnt_val(): plain read+write, called from abox_uaif_hw_params()
ret = snd_soc_component_read(cmpnt, ABOX_ROUTE_CTRL0, &val);
sifs_id = (val & ABOX_ROUTE_UAIF_SPK_MASK(idx)) >> ABOX_ROUTE_UAIF_SPK_L(idx) - 1;
snd_soc_component_write(cmpnt, ABOX_SPUS_CTRL_SIFS_CNT(sifs_id), 0);
```
`abox_get_format()` (`abox.h:474-490`, the width/channels → 5-bit field encoder shared by RDMA/UAIF/MIXP):
```c
static inline u32 abox_get_format(u32 width, u32 channels)
{
    u32 ret = (channels - 1);                 // bits [2:0]
    switch (width) {
    case 16: ret |= 1 << 3; break;             // bits [4:3] width code
    case 24: ret |= 2 << 3; break;
    case 32: ret |= 3 << 3; break;
    }
    return ret;
}
```
("ABOX RDMA3 Width=32 / Channel=2" from your stock `mixer_paths.xml` init block → `abox_get_format(32,2)=0x19`.)

### 2.3 UAIF2 (TDM/dsp_a, master) — `abox_if.c` (100% plain registers, zero IPC)

```c
// abox_if.c:302-366  abox_uaif_set_fmt() — format/clock-role/polarity, ALL direct regmap:
snd_soc_component_read(cmpnt, UAIF_REG_CTRL0, &ctrl0);
snd_soc_component_read(cmpnt, UAIF_REG_CTRL1, &ctrl1);
case SND_SOC_DAIFMT_DSP_A:  set_value_by_name(ctrl1, ABOX_WS_MODE, 1);         // dsp_a / TDM
case SND_SOC_DAIFMT_IB_NF:  set_value_by_name(ctrl1, ABOX_BCLK_POLARITY, 0);
                            set_value_by_name(ctrl1, ABOX_WS_POLAR, 0);
case SND_SOC_DAIFMT_CBS_CFS: set_value_by_name(ctrl0, ABOX_MODE, 1);          // ABOX = bus MASTER (your config)
// case SND_SOC_DAIFMT_CBM_CFM: data->slave = true; set_value_by_name(ctrl0, ABOX_MODE, 0);  // ABOX = slave
snd_soc_component_write(cmpnt, UAIF_REG_CTRL0, ctrl0);
snd_soc_component_write(cmpnt, UAIF_REG_CTRL1, ctrl1);
```
```c
// abox_if.c:408-491  abox_uaif_hw_params() — BCLK generation + TDM slot/width, ALL direct:
if (data->slave) {
    clk_set_rate(data->clk_mux,  RATE_MUX_SLAVE);
    clk_set_rate(data->clk_bclk, RATE_MUX_SLAVE);
} else {                                           // <-- ABOX-master case (your board)
    clk_set_rate(data->clk_bclk, rate * channels * width);   // AP computes & sets UAIF2 BCLK itself via CMU_AUD
}
set_value_by_name(ctrl1, ABOX_SBIT_MAX, (width - 1));
set_value_by_name(ctrl1, ABOX_VALID_STR, (width - 1));       // non-I2S (i.e. dsp_a) path
set_value_by_name(ctrl1, ABOX_VALID_END, (width - 1));
set_value_by_name(ctrl1, ABOX_SLOT_MAX, (channels - 1));
set_value_by_name(ctrl1, ABOX_FORMAT, abox_get_format(width, channels));
snd_soc_component_write(cmpnt, UAIF_REG_CTRL1, ctrl1);
if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
    abox_cmpnt_reset_cnt_val(dev, abox_data->cmpnt, dai->id);  // plain register, see §2.2
```
```c
// abox_if.c:495-520  abox_uaif_mute_stream() — SPK_ENABLE bit, direct register, with FIFO-flush delay:
if (mute) usleep_range(600, 1000);
snd_soc_component_update_bits(cmpnt, UAIF_REG_CTRL0, ABOX_SPK_ENABLE_MASK, !mute << ABOX_SPK_ENABLE_L);
```
**No `abox_request_ipc()`/`ABOX_IPC_MSG` usage anywhere in `abox_if.c`.** UAIF2's entire hardware state (format,
master/slave, TDM slot count/width, BCLK frequency, speaker-enable) is 100% AP register writes.

### 2.4 Conclusion for Q2 and a concrete bring-up plan

- **SPUS routing mux, SIFS routing, `ROUTE_UAIF_SPK`, and all of UAIF2 = pure AP register pokes.** No DSP needed,
  no IPC exists for any of it. High confidence, fully sourced above.
- **SIFS0 itself** is a passive hardware mixer — needs no configuration beyond having `SPUS OUTn = SIFS0` set
  somewhere (which is the plain register above); it will sum whatever valid audio arrives.
- **RDMA3's actual data-motion (buffer addr/size/enable, format/width/channels fields, volume ramp)** is, *in the
  stock driver*, delegated 100% to Calliope via IPC — this is the literal meaning of z3s's "SIFS0/RDMA3/UAIF2 are
  DSP-managed" finding, narrowed down precisely to RDMA's buffer-management logic (SIFS0/UAIF2 turned out to need
  no DSP at all once routing is set correctly — only RDMA truly depends on Calliope *in this driver's design*).
- Nothing in the open-source register map (`abox_soc_3.h`) marks `RDMA_BUF_STR/BUF_END/CTRL1/ENABLE` as
  secure-only or DSP-exclusive — contrast with `pd-aud`'s explicit `need_smc` marker (§1.3). The IP block
  (generic Samsung "xDMA", same register layout reused for WDMA/DDMA, and for VTS capture DMA on related SoCs
  that run with **no** embedded DSP at all) gives good circumstantial evidence this is a pure software-architecture
  choice by Samsung (to centralize buffer refill, ASRC, BD_MIXER multi-stream summing, and smooth volume ramping
  in firmware for the complex Android audio stack) rather than a hardware interlock.
- **Recommended validation**: because the *exact* bit patterns Calliope writes into `BUF_STR/BUF_END/BUF_OFFSET/
  CTRL1` are not visible in any open source (they're baked into the closed `calliope_dram.bin`), the lowest-risk
  path is: boot stock Android once, start speaker playback, and read back live register state at
  `0x18C51300`-`0x18C5133F` (RDMA3) via `/dev/mem` or a debug build with `CONFIG_SND_SOC_SAMSUNG_ABOX` debugfs
  (`abox_dbg.c`/`abox_debug.xml` expose raw SFR dumps) to learn the concrete values (ring size vs `period_bytes`,
  exact `BUF_OFFSET` semantics, whether `CTRL1.ENABLE` and `CTRL0.ENABLE` are both required). Then replicate those
  writes from your own kernel with Calliope held in reset (§1.3). This turns an "architecture plausibility
  argument" into an empirically-verified contract.

---

## 3. CS35L41 TDM/ASP-slave init sequence

Sources (saved in this folder): `cs35l41-registers.h` (`include/linux/mfd/cs35l41/registers.h`), `cs35l41.c`
(`sound/soc/codecs/cs35l41.c`), `cs35l41-tables.c` (`drivers/mfd/cs35l41-tables.c`). Also
`drivers/mfd/cs35l41-core.c` / `cs35l41-pwr.c` (MFD split, not saved — small, quoted inline below).

### 3.1 Key register addresses
`cs35l41-registers.h:40-102` and `:630-671`

| Register | Addr | Key fields |
|---|---|---|
| `CS35L41_DEVID` | 0x00000000 | chip ID, read in probe |
| **`CS35L41_PWR_CTRL1`** | **0x00002014** | **bit0 = `GLOBAL_EN`** — exactly matches your stated `0x2014` |
| `CS35L41_PWR_CTRL2` | 0x00002018 | boost/amp block-level enables |
| `CS35L41_PWR_CTRL3` | 0x0000201C | |
| `CS35L41_AMP_OUT_MUTE` | 0x00002024 | |
| `CS35L41_PLL_CLK_CTRL` | 0x00002C04 | `PLL_OPENLOOP`[11], `PLL_CLK_SEL`[2:0] (0=SCLK/BCLK,1=LRCLK,3=internal,4=PDM,5=MCLK,7=SoundWire), `REFCLK_FREQ`[10:5], `PLL_CLK_EN`[0] |
| `CS35L41_DSP_CLK_CTRL` | 0x00002C08 | |
| `CS35L41_GLOBAL_CLK_CTRL` | 0x00002C0C | `GLOBAL_FS`[4:0] sample-rate code (48kHz=0x03, 16kHz=0x12, etc.) |
| `CS35L41_DATA_FS_SEL` | 0x00002C10 | |
| `CS35L41_SP_ENABLES` | 0x00004800 | |
| `CS35L41_SP_RATE_CTRL` | 0x00004804 | low 6 bits = PLL ref-clock rate code when `PLLSRC=SCLK` |
| `CS35L41_SP_FORMAT` | 0x00004808 | `SCLK_MSTR`,`LRCLK_MSTR` (master/slave), `ASP_FMT`[2 bits: 0=DSP_A/TDM, 2=I2S], `LRCLK_INV`,`SCLK_INV`, `ASP_WIDTH_RX/TX` |
| `CS35L41_SP_HIZ_CTRL` | 0x0000480C | |
| `CS35L41_SP_FRAME_TX_SLOT` | 0x00004810 | |
| `CS35L41_SP_FRAME_RX_SLOT` | 0x00004820 | **`ASP_RX1_SLOT`[5:0]`, `ASP_RX2_SLOT`[13:8]** — TDM slot assignment, directly matches your `mixer_paths.xml` "Left/Right ASPRX1/2 Slot Position" controls |
| `CS35L41_SP_TX_WL`/`SP_RX_WL` | 0x00004830/0x00004840 | actual word length (vs. physical slot width) |
| `CS35L41_ASP_CONTROL4` | 0x00004854 | written as part of an fsync errata patch |
| `CS35L41_DAC_PCM1_SRC` | 0x00004C00 | **DAC input-mux: `ASPRX1`(0x08) = raw bypass, or internal Halo-DSP `DSPTX1` output = protected path** |
| `CS35L41_ASP_TX1..4_SRC` | 0x00004C20-2C | |
| `CS35L41_DSP1_RX1..8_SRC` | 0x00004C40-5C | protection-DSP input mux (VPMON/VBSTMON/TEMPMON monitors etc.) |

### 3.2 Power-up order (verified against driver, not guessed)

**Stage A — reset/regulator bring-up** (`drivers/mfd/cs35l41-core.c`, `cs35l41_dev_init()`):
```c
regulator_bulk_enable(VA, VP);                         // core supplies first
usleep_range(1000, 1100);
gpiod_set_value_cansleep(cs35l41->reset_gpio, 1);       // RESET deasserted (your GPB2-4/GPB2-5)
usleep_range(2000, 2100);
mfd_add_devices(..., {"cs35l41-codec","cs35l41-cal","cs35l41-bd","cs35l41-pwr"});
```

**Stage B — `cs35l41_init()`** (`cs35l41.c:2150-2233`):
```c
regmap_read(regmap, CS35L41_DEVID, &regid);             // identify chip
regmap_read(regmap, CS35L41_REVID, &reg_revid);         // A0/B0/etc, even=CS35L41 odd=CS35L41R
regmap_write(regmap, CS35L41_IRQ1_MASK1, CS35L41_INT1_MASK_DEFAULT);
regmap_write(regmap, CS35L41_DSP1_RX5_SRC, CS35L41_INPUT_SRC_VPMON);
regmap_write(regmap, CS35L41_DSP1_RX6_SRC, CS35L41_INPUT_SRC_VBSTMON);
regmap_write(regmap, CS35L41_DSP1_RX7_SRC, CS35L41_INPUT_SRC_TEMPMON);
regmap_write(regmap, CS35L41_DSP1_RX8_SRC, CS35L41_INPUT_SRC_RSVD);
if (reg_revid == CS35L41_REVID_A0)
    regmap_register_patch(regmap, cs35l41_reva0_errata_patch, ...);   // cs35l41.c:2101-2112, see below
// POLL for OTP auto-load completion (hardware auto-loads factory trims from OTP on power-up):
do { usleep_range(1000,1100); regmap_read(regmap, CS35L41_IRQ1_STATUS4, &int_status);
} while (!(int_status & CS35L41_OTP_BOOT_DONE));        // up to 100 ms
cs35l41_otp_unpack(cs35l41);                            // unpacks factory trims into boost-converter regs
regmap_read(regmap, CS35L41_OTP_TRIM_30, &spk_3_trim);
regmap_read(regmap, CS35L41_OTP_TRIM_31, &spk_4_trim);
```
Errata patch applied for A0 silicon (`cs35l41.c:2101-2112`):
```c
static const struct reg_sequence cs35l41_reva0_errata_patch[] = {
    {0x00000040, 0x00005555}, {0x00000040, 0x0000AAAA},   // test-key unlock sequence
    {0x00003854, 0x05180240}, {CS35L41_OTP_TRIM_30, 0x9091A1C8},
    {0x00003014, 0x0200EE0E}, {CS35L41_BSTCVRT_DCM_CTRL, 0x00000051},
    {0x00000054, 0x00000004}, {CS35L41_IRQ1_DB3, 0}, {CS35L41_IRQ2_DB3, 0},
    {0x00000040, 0x0000CCCC}, {0x00000040, 0x00003333},   // re-lock
};
```

**Stage C — ASP format/clock, via the normal ASoC `set_fmt`/`hw_params`/`set_sysclk` calls** (machine driver would
call these in response to your dai_link's configured format — for ABOX-master/CS35L41-slave/dsp_a this is
`SND_SOC_DAIFMT_CBS_CFS | SND_SOC_DAIFMT_DSP_A`, plus whatever BCLK/FS inversion the board needs):
```c
// cs35l41_set_dai_fmt(), cs35l41.c:1193-1270
case CBS_CFS: slave_mode = 0; break;                 // CS35L41 = slave (matches "ABOX is BCLK master")
regmap_update_bits(regmap, CS35L41_SP_FORMAT, SCLK_MSTR_MASK,  0);
regmap_update_bits(regmap, CS35L41_SP_FORMAT, LRCLK_MSTR_MASK, 0);
case DSP_A: asp_fmt = 0; break;                       // matches "dsp_a" format
regmap_update_bits(regmap, CS35L41_SP_FORMAT, ASP_FMT_MASK, 0);

// cs35l41_component_set_sysclk() / cs35l41_dai_set_sysclk(), cs35l41.c:1505-1577
cs35l41->clksrc = CS35L41_PLLSRC_SCLK;                // PLL source = BCLK, matches your spec exactly
regmap_update_bits(regmap, CS35L41_SP_RATE_CTRL, 0x3F, extclk_cfg);
regmap_update_bits(regmap, CS35L41_PLL_CLK_CTRL, PLL_OPENLOOP_MASK, 1<<11);
regmap_update_bits(regmap, CS35L41_PLL_CLK_CTRL, REFCLK_FREQ_MASK, extclk_cfg<<5);
regmap_update_bits(regmap, CS35L41_PLL_CLK_CTRL, PLL_CLK_EN_MASK, 0);
regmap_update_bits(regmap, CS35L41_PLL_CLK_CTRL, PLL_CLK_SEL_MASK, CS35L41_PLLSRC_SCLK /* =0 */);
regmap_update_bits(regmap, CS35L41_PLL_CLK_CTRL, PLL_OPENLOOP_MASK, 0);
regmap_update_bits(regmap, CS35L41_PLL_CLK_CTRL, PLL_CLK_EN_MASK, 1<<0);     // PLL_CLK_EN last

// cs35l41_pcm_hw_params(), cs35l41.c:1399-1469
regmap_update_bits(regmap, CS35L41_GLOBAL_CLK_CTRL, GLOBAL_FS_MASK, fs_cfg);  // e.g. 48000->0x03
regmap_update_bits(regmap, CS35L41_SP_FORMAT, ASP_WIDTH_RX_MASK, asp_width);
regmap_update_bits(regmap, CS35L41_SP_RX_WL,  ASP_RX_WL_MASK,    asp_wl);
// NOTE: for i2s_mode==false (i.e. your dsp_a/TDM case) this function does NOT set RX1/RX2 slot —
// slot assignment is a separate, directly-writable control (CS35L41_SP_FRAME_RX_SLOT, see 3.1),
// matching the "Left/Right ASPRX1/2 Slot Position" ALSA controls already seen in your mixer_paths.xml.
```

**Stage D — `GLOBAL_EN` last, on DAPM power-up** (`cs35l41_main_amp_event()`, `cs35l41.c:992-1049`):
```c
case SND_SOC_DAPM_POST_PMU:
    regmap_multi_reg_write_bypassed(regmap, cs35l41_pup_patch, ...);      // cs35l41.c:808-814, boost-conv unlock sequence
    regmap_update_bits(regmap, CS35L41_PWR_CTRL1, GLOBAL_EN_MASK, 1<<0);  // <-- GLOBAL_EN LAST, exactly as you stated
    if (dsp.preloaded && !halo_played) {
        cs35l41_set_csplmboxcmd(cs35l41, CSPL_MBOX_CMD_PAUSE);
        regmap_write(regmap, CS35L41_DAC_PCM1_SRC, CS35L41_INPUT_SRC_ASPRX1);  // force raw passthrough on first play
    }
    cirrus_pwr_start(mfd_suffix);
```
`cs35l41_pup_patch` / `cs35l41_pdn_patch` (`cs35l41.c:808-821`) are a test-key-unlock + single protected-register
write (`0x2084`, an undocumented boost/LBST config register) + re-lock, differing only in one bit (`...1AA0` for
power-up vs `...1AA3` for power-down).

### 3.3 Bypassing the amp's own internal (Halo-core) protection DSP for a first test

`cs35l41_pcm_source_event()` (`cs35l41.c:935-973`) shows the amp has **two independent DSPs** in play: Calliope
(ABOX side, irrelevant to the amp) and the CS35L41's **own tiny embedded "Halo core"** running a "Protection"
firmware (`"Left/Right DSP1 Firmware" = "Protection"` in your `mixer_paths.xml`). Setting
`"Left/Right PCM Source" = "ASP"` (i.e. `DAC_PCM1_SRC = CS35L41_INPUT_SRC_ASPRX1 = 0x08`) makes the amp output the
**raw TDM/ASP input directly**, with `cs35l41->halo_booted` staying false, entirely skipping the
preload/boot/mailbox-handshake dance (`cs35l41_dsp_init()`, `CSPL_MBOX_CMD_*`) needed for the protected path. This
is exactly the config your stock `dev-handset` (earpiece) path already uses (`"Left PCM Source"="ASP"`,
§4), vs. `dev-speaker`'s `"PCM Source"="DSP"`. **Recommendation: for your first audible test, configure like
`dev-handset` (ASP passthrough, no amp-DSP firmware needed) rather than `dev-speaker` (protected mode, needs the
amp's own DSP firmware blob loaded via `wm_adsp`).**

### 3.4 Built-in test tone?

No. `cs35l41-registers.h` has no `TONE`/`SINE` generator register — only `CS35L41_MIXER_NGATE_CFG` (a **noise
*gate***, not a noise/tone *generator*) and the monitor mixer-source constants
(`cs35l41-registers.h:640-648`: `ASPRX1=0x08, ASPRX2=0x09, VMON=0x18, IMON=0x19, CLASSH=0x21, VPMON=0x28,
VBSTMON=0x29, TEMPMON=0x3A`) — no "internal tone" source code exists among them. The only way to get sound out of
this chip with zero TDM input would be to write a custom Halo-core DSP patch that synthesizes a tone and routes
`DAC_PCM1_SRC = DSP1-output`, which is a non-trivial reverse-engineering project in its own right (same firmware
loading mechanism as the "Protection" patch, via `wm_adsp2_component_probe()`, `cs35l41.c:1768`). **Practical
recommendation: your first audible test still requires actually driving a real TDM frame from UAIF2** — there is
no register shortcut.

---

## 4. Stock y2s `mixer_paths.xml` / `abox_tplg.conf` speaker route (local files)

Read directly from `/path/to/y2s-workspace/fw-stock/vendor/etc/mixer_paths.xml` (166 KB) and
`/path/to/y2s-workspace/fw-stock/vendor/firmware/abox_tplg.conf` (143 KB, the human-readable
topology source compiled into `abox_tplg.bin`). Not copied into this folder (large, already in your workspace) —
line numbers below refer to `mixer_paths.xml` as it exists in that path.

### 4.1 Static init block (`mixer_paths.xml:1-115`, excerpted)
```xml
<!-- UAIF2 - Speaker/RCV AMP -->
<!-- SIFS0 : Mixer for Playback -->      <!-- SIFS1-5 : Mux for Playback -->
<ctl name="ABOX SIFS0 Width" value="24" />      <ctl name="ABOX SIFS0 Channel" value="2" />
<ctl name="ABOX UAIF2 Width" value="32" />  <ctl name="ABOX UAIF2 Channel" value="4" />
<ctl name="ABOX UAIF2 Rate" value="48000" /> <ctl name="ABOX UAIF2 Extend BCLK" value="1" />
<ctl name="ABOX SPUS OUT3" value="SIFS0" />                 <!-- RDMA3 -> SIFS0 default routing -->
<ctl name="ABOX UAIF2 SPK" value="RESERVED" />              <!-- UAIF2 input unrouted until a path activates it -->
<ctl name="ABOX RDMA3 Width" value="32" />  <ctl name="ABOX RDMA3 Channel" value="2" />
<ctl name="ABOX RDMA3 Period" value="240" />
<ctl name="ABOX RDMA3_A" value="None" />                    <!-- RDMA3 defaults to its OWN DRAM ring buffer, NOT BD_MIXER -->
<ctl name="Left ASPRX1 Slot Position" value="0" />  <ctl name="Left ASPRX2 Slot Position" value="1" />
<ctl name="Right ASPRX1 Slot Position" value="1" /> <ctl name="Right ASPRX2 Slot Position" value="0" />
```
`ABOX RDMA3_A = None` by default confirms what §2.4 argues: **the stock speaker path does *not* require
BD_MIXER** — BD_MIXER is only wired in by the explicit `route-bdmixer-to-rdma3` path fragment used by Android's
multi-stream media playback (below); a single-stream custom OS can leave RDMA3 in its default, simpler mode.

### 4.2 Path graph for speaker playback
```xml
<path name="route-bdmixer-to-rdma3">                 <!-- only used by the full Android media-playback path -->
    <ctl name="ABOX SPUS ASRC3" value="1"/>
    <ctl name="ABOX RDMA3_A" value="BD_MIXER" />
</path>
<path name="route-rdma3-to-sifs0">
    <ctl name="ABOX SPUS OUT3" value="SIFS0" />
</path>
<path name="route-sifs0-to-uaif2">
    <ctl name="ABOX UAIF2 SPK" value="SIFS0" />
</path>
<path name="route-normal-spkrcv-playback">
    <path name="route-bdmixer-to-rdma3" /> <path name="route-rdma3-to-sifs0" /> ... <path name="route-sifs0-to-uaif2" />
</path>
<path name="dev-speaker">                            <!-- CS35L41 (right/bottom) side, codec controls -->
    <ctl name="Right ASPTX1 Slot Position" value="0" /> ... <ctl name="Right ASP TX1 Source" value="DSPTX1" />
    <ctl name="Right PCM Source" value="DSP" />         <!-- protected/Halo-DSP path -->
    <ctl name="Right Boost Enable" value="Enabled" />
    <ctl name="Right AMP Enable Switch" value="1" />
</path>
<path name="dev-dual-speaker">                       <!-- both amps, left+right slot assignment for stereo -->
    ... <ctl name="Left PCM Source" value="DSP" /> ... <ctl name="Right PCM Source" value="DSP" /> ...
</path>
<path name="media-speaker">
    <ctl name="ABOX Path Change ID" value="28" />
    <path name="dev-speaker" /> <path name="route-normal-spkrcv-playback" />
</path>
<path name="media-dual-speaker">                     <!-- stereo/both-amp variant -->
    <path name="dev-dual-speaker" /> <path name="route-soundboost-spkrcv-playback" />
</path>
```
```xml
<!-- Common CODEC configurations, earpiece (bypasses amp-side protection DSP) -->
<path name="dev-handset">
    ... <ctl name="Left PCM Source" value="ASP" /> ...      <!-- raw passthrough, no Halo DSP needed -->
</path>
```
`route-normal-spkrcv-playback` is exactly the chain **RDMA3(+BD_MIXER) → SPUS OUT3=SIFS0 → UAIF2 SPK=SIFS0**
your task description outlined, confirming it end-to-end against the live config shipped on this phone. For a
from-scratch OS, drop `route-bdmixer-to-rdma3` (leave `RDMA3_A=None`) and drive RDMA3 straight from your own ring
buffer; keep `route-rdma3-to-sifs0` + `route-sifs0-to-uaif2` (plain register writes, §2.2/§1.1) and use
`dev-handset`-style `PCM Source=ASP` on the amp for your first test (§3.3).

---

## 5. File manifest (saved in this folder)

| File | Source | Purpose |
|---|---|---|
| `abox_soc_3.h` | `sound/soc/samsung/abox/abox_soc_3.h` | ABOX v3 (9830) full register/bitfield map |
| `abox_rdma.c` | `sound/soc/samsung/abox/abox_rdma.c` | RDMA hw_params/prepare/trigger — proves IPC-only buffer mgmt |
| `abox_if.c` | `sound/soc/samsung/abox/abox_if.c` | UAIF set_fmt/hw_params/mute — proves 100% direct register |
| `abox_cmpnt.c` | `sound/soc/samsung/abox/abox_cmpnt.c` | ASoC component: SPUS/SIFS/ROUTE kcontrols, all plain registers |
| `abox_cmpnt.h` | `sound/soc/samsung/abox/abox_cmpnt.h` | API surface for the above |
| `abox_core.c` | `sound/soc/samsung/abox/abox_core.c` | Calliope PMU power/enable/standby + firmware download |
| `abox.c` | `sound/soc/samsung/abox/abox.c` | top-level probe, `SYSPOWER_CTRL`, `abox_cpu_power/enable`, suspend/resume IPC |
| `abox.h` | `sound/soc/samsung/abox/abox.h` | DAI id enums (`ABOX_RDMA3`, `ABOX_UAIF2`, `ABOX_RDMA3_BE`...), `abox_get_format()` |
| `abox_dma.h` | `sound/soc/samsung/abox/abox_dma.h` | `DMA_REG_*` offset aliases (confirms = `ABOX_RDMA_*`) |
| `abox_ipc_uapi.h` | `include/sound/samsung/abox_ipc.h` | `PCMMSG`/`SIGNAL_ID` IPC enum + message structs |
| `clk-exynos9830.c` | `drivers/clk/samsung/clk-exynos9830.c` | CMU_AUD clock-ID → Linux clk name table |
| `cs35l41-registers.h` | `include/linux/mfd/cs35l41/registers.h` | Full CS35L41 register address map |
| `cs35l41.c` | `sound/soc/codecs/cs35l41.c` | CS35L41 init/format/PLL/power-up-sequence driver |
| `cs35l41-tables.c` | `drivers/mfd/cs35l41-tables.c` | regmap default/volatile/readable tables |

Not saved here (too large / fetched live via `curl`+`grep` only, cite by URL+line instead):
- `arch/arm64/boot/dts/exynos/exynos9830.dts` (436 KB, 17484 lines) — `pd-aud`, `abox-core`, `abox-rdma/uaif` node
  addresses (§1.1/§1.3).
- `drivers/soc/samsung/cal-if/exynos9830/cmucal-sfr.c` (1.3 MB) / `cmucal-sfr.h` (677 KB) — CMU_AUD SFR addresses
  (§1.4).
- `/path/to/y2s-workspace/fw-stock/vendor/etc/mixer_paths.xml` and
  `.../vendor/firmware/abox_tplg.conf` — already in your workspace, excerpted in §4.

All GitHub citations use repo `ExtremeXT/android_kernel_samsung_exynos990` at commit
`69515fbb7a4395898c05a8624f76a12afbac11c5` (fetched 2026-10-25). Example permalink pattern:
`https://github.com/ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/<path>#L<start>-L<end>`

---

## 6. Gaps / uncertainties / suggested follow-ups

1. **This kernel tree is not y2s's own board source** — no `cs35l41` I2C-address/GPIO board node, no y2s `.dts`
   fragment exists in `ExtremeXT/android_kernel_samsung_exynos990`. All board-specific facts (I2C addr 0x40/0x41,
   reset GPIOs gpb2-4/gpb2-5, slave count, dual-speaker slot scheme) come from your own stated facts plus
   cross-validation against the **locally present, genuinely-y2s** `mixer_paths.xml`/`abox_tplg.conf` — not from
   this GitHub repo directly. If you can locate the actual y2s/z3s kernel fork (the task mentions a "z3s project"
   Linux port — worth asking them directly for their board `.dts`/`cs35l41` instantiation and, more importantly,
   any debug register dumps they captured while attempting the native path), that would directly supersede the
   generic-driver inferences here.
2. **`pd-aud` power sequencing requires an SMC** (`need_smc=0x18e10204`, §1.3) — the exact SMC function ID/calling
   convention used by Samsung's `exynos-pd`/PWRCAL glue was not traced into the EL3/ACPM firmware (closed,
   outside kernel source). This is probably the single biggest unknown blocking a from-scratch bring-up before
   even reaching ABOX registers.
3. **The precise semantics of `RDMA_BUF_OFFSET`/`STR_POINT` and whether `CTRL0.ENABLE` alone is sufficient to
   start DMA** (vs. needing a CTRL1 duplicate enable bit, or sbank/latency-ctrl prerequisites) are not visible in
   any open source — only Calliope's closed firmware programs them. Treat §2.4's bring-up plan (empirical register
   dump from a live stock boot) as mandatory, not optional, before trusting a bypass implementation.
4. I did not trace `abox_tplg.bin` (binary topology blob) itself — only the human-readable `abox_tplg.conf`
   source was read; byte-for-byte confirmation of e.g. `SectionControlEnum."RDMA3_A"` numeric IDs would require
   parsing the compiled binary (ALSA `kernel/include/uapi/sound/asoc.h` TLV format), which was out of scope here.
5. CMU_AUD "no ACPM round-trip" conclusion (§1.4) is inferred from the absence of an `acpm-ipc-channel` marker on
   the per-block `SFR_BLOCK(CMU_AUD, ...)` entry, by analogy with how `cal-if` works on other Exynos generations I
   have prior knowledge of — I did not find an explicit statement in-source confirming CMU_AUD writes are never
   proxied through ACPM on *this specific* SoC revision; verify empirically.
