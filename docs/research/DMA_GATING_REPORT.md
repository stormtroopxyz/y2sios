# Why IDMA/DPP SFRs read as 0, and how to un-gate them (Exynos 9830 / Galaxy S20 DPU)

Scope: `drivers/video/fbdev/exynos/dpu30/**` and `drivers/soc/samsung/{cal-if,exynos-pd.c,exynos-el3_mon.c}`
in `ExtremeXT/android_kernel_samsung_exynos990` (commit `69515fbb7a4395898c05a8624f76a12afbac11c5`,
exynos9830 variant), cross-checked against `Bentlybro/z3s-mainline-linux` (commit
`572e7892ff651a50e84b4078f460bc8d6bb20210`) and this project's own GPU bring-up
(`PLANS/04-gpu.md`), which already proved the exact fix mechanism on this same phone for a
sibling IP block.

## 0. Address-map correction (read this first)

The dtsi snippet in the question labels `0x19041000..0x19046000` "IDMA" and `0x19091000` "DPP".
**Samsung's own source has it the other way round.** From the real node and the driver's
`platform_get_resource()` order:

- `drivers/video/fbdev/exynos/dpu30/cal_9830/regs-dpp.h:16-27` (header comment) and
  `:67-73` ("`1.1 - IDMA Register`"):
  `DPU_DMA SFR base: 0x19090000` (GLOBAL/common), **IDMA GF0(L0): `0x19091000`** … VGRFS(L5):
  `0x19096000`, ODMA: `0x19097000`.
- `regs-dpp.h:516-519` + the comment right above `DPP_ENABLE` (`regs-dpp.h:619-632`):
  `DPU WB MUX base: 0x19080000`; **DPP SFR base: `0x19040000`**, GF0(L0): `0x19041000` …
  VGRFS(L5): `0x19046000`.
- `drivers/video/fbdev/exynos/dpu30/dpp_drv.c:1177-1243` (`dpp_init_resources`):
  `IORESOURCE_MEM 0` → `dpp->res.dma_regs` ("`failed to remap DPU_DMA SFR region`"),
  `IORESOURCE_MEM 2` → `dpp->res.dma_com_regs` ("`DPU_DMA COMMON SFR region`", only mapped for
  `id==0`), `IORESOURCE_MEM 1` → `dpp->res.regs` ("`DPP SFR region`").
- The real board `.dts` (`arch/arm64/boot/dts/exynos/exynos9830.dts:10326-10351`) confirms the
  3‑tuple for `dpp@0x19091000`: `reg = <0x19091000 0x1000>, <0x19041000 0x1000>, <0x19090000 0x110>;`
  i.e. **reg[0]=0x19091000 is the IDMA (`dma_regs`)**, **reg[1]=0x19041000 is the DPP
  color‑pipe (`regs`)**, **reg[2]=0x19090000 is `DPU_DMA_COM` (`dma_com_regs`, 0x110 bytes,
  only wired to channel 0)**. Saved as `exynos9830.dts-pd_dpu-excerpt.dts` in this folder.

So: **`0x19041000‑0x19046000` = DPP (color pipe: CSC/scaler/HDR)**, **`0x19091000‑0x19096000` =
IDMA (the actual DMA engine)**, **`0x19090000` (0x110B) = `DPU_DMA_COM`** (global/shared: version,
SW‑reset, per‑group SFR clock‑gate‑enable), **`0x19097000`+`0x19080000` = ODMA + `DPU_WB_MUX`**
(writeback). The node is named `dpp@0x19091000` because the v4l2-subdev "dpp" object owns both
register windows — this almost certainly explains why z3s's doc lumps "DPU_DMA and DPP" together
under `0x19091000`. All of these windows are reported dead by you and by z3s; the fix below is
the same for every one of them.

## 1. Root-cause hypotheses, ranked

### #1 (very high confidence): D‑TZPC (DPU's local TrustZone Protection Controller) was never
"restored" to a non‑secure‑friendly state, because S‑Boot never ran Linux's power‑domain‑on
sequence for `pd‑dpu`

Samsung's generic Exynos power‑domain driver ties a **TZPC restore SMC** directly to power‑domain
power‑on, independent of and in addition to any clock gating:

- `arch/arm64/boot/dts/exynos/exynos9830.dts:5341-5350`:
  ```dts
  pd-dpu@15861E80 {
      compatible = "samsung,exynos-pd";
      reg = <0x00 0x15861e80 0x20>;
      cal_id = <0xb1380003>;
      need_smc = <0x19010204>;
      status = "okay";
      #power-domain-cells = <0x00>;
      skip-idle-ip;
  };
  ```
- `drivers/soc/samsung/cal-if/pmucal_local.c:18-50` (`pmucal_local_enable`, called by the genpd
  "power on" path): after running the PMU on-sequence, **if `need_smc` is non‑zero** it calls
  `exynos_pd_tz_restore(pmucal_pd_list[pd_id].need_smc)` and logs `"DTZPC restore smc error"` on
  failure — "DTZPC" = **D**ecode-**TZPC**, the same `D_TZPC_DPU` IP we see as a clock consumer in
  CMU_DPU (`CLK_CON_GAT_GOUT_BLK_DPU_UID_D_TZPC_DPU_IPCLKPORT_PCLK`, offset `0x201c`,
  `cmucal-sfr.c:6248`) and as its own Q‑channel (`QCH_CON_D_TZPC_DPU_QCH`, offset `0x304c`,
  `cmucal-sfr.c:2422`).
- `drivers/soc/samsung/exynos-el3_mon.c:214-221` + `include/soc/samsung/exynos-el3_mon.h:21-26`:
  ```c
  int exynos_pd_tz_restore(unsigned int addr)
  {
      return exynos_smc(SMC_CMD_PREAPRE_PD_ONOFF, EXYNOS_WAKEUP_PD_DOWN, addr,
                         RUNTIME_PM_TZPC_GROUP);
  }
  /* SMC_CMD_PREAPRE_PD_ONOFF = 0x82000410, EXYNOS_WAKEUP_PD_DOWN = 1, RUNTIME_PM_TZPC_GROUP = 2 */
  ```
  (`include/linux/smc.h:41-42`: `#define SMC_CMD_PREAPRE_PD_ONOFF (0x82000410)` is filed under
  `/* For D-GPIO/D-TZPC */` — i.e. Samsung's own header literally says this SMC ID exists to
  reprogram D‑TZPC as part of power sequencing.)

So the concrete call for `pd-dpu` is:

```
exynos_smc(0x82000410, 1, 0x19010204, 2)
```

**This is not a new guess** — it is the exact call pattern this project already used
successfully for a sibling power domain. `PLANS/04-gpu.md` (lines ~56‑64, status: **done**)
records, for `pd-embedded_g3d` (`need_smc = 0x18410204` at `exynos9830.dts:5399`):

> `TZPC | after power-on: smc(0x82000410, 1, 0x18410204, 2) (returns 0)`

i.e. the identical SMC, with only the per‑domain `need_smc` constant swapped, is already proven
on this exact phone/firmware to return `0` (success). `pd-dpu`'s constant is `0x19010204`.

**Why this explains every symptom:** powering S‑Boot's splash through requires DECON + DSIM +
the already‑programmed IDMA descriptor to be running, but S‑Boot (or whatever secure/non‑secure
hand‑off happened before your kernel got control) never executed the Linux `pmucal_local_enable()`
path for `pd-dpu`, so the "DTZPC restore" SMC was never issued. D‑TZPC's hardware default (and
its behavior on `pd-dpu` power‑up before restore) is apparently to keep the DPP/IDMA/DMA‑COM/WB
windows marked secure, while DECON's own window is either in a different (always-NS) TZPC group
or is covered separately — consistent with `DECON_F` (`0x19050000`) working today while every
`IDMA`/`DPP`/`DMA_COM` register is RAZ. TZPC enforcement on this family returns **OKAY with 0
data** on a disallowed non‑secure transaction (not `SLVERR`/abort), which is exactly "(no fault)"
as you observed. z3s's "fault on register access" for the same windows is their informal wording
for the identical symptom — they never run `pmucal`/genpd either (mainline 6.12 has no Exynos
DPU driver at all), so they never issue this SMC either.

**Caution / known limitation of this exact mechanism:** z3s hit the *same* SMC family (TZPC
restore) failing silently for a different, more strictly-protected IP: `docs/DEVLOG.md:526-527`
(z3s‑mainline‑linux) — on the secure ABOX SysMMU, *"the DTZPC unlock SMC is accepted but does not
unlock"*. That block guards DSP firmware authentication and may be deliberately pinned secure by
the LDFW regardless of the restore call. DPU has no equivalent "must stay secure" reason (it's a
plain display DMA engine, and Samsung's own driver unprotects it unconditionally at probe via
`decon_set_protected_content`, see §2), so we rank this risk low for DPU specifically, but you
should check the SMC's return value (`x0`) and re‑read a register immediately after, rather than
assume success.

### #2 (lower confidence, same symptom family): per‑channel DRM/"content path protection" bit
left set from a previous secure‑video session

Independent of D‑TZPC, each DPP/IDMA *channel* has a separate secure/non‑secure **content
protection** bit, toggled over SMC, that the Samsung driver clears unconditionally during
DECON enable/disable:

- `drivers/video/fbdev/exynos/dpu30/helper.c:289-325` (`decon_get_protect_id`,
  `decon_control_protection`):
  ```c
  static int decon_get_protect_id(int dma_id) {
      switch (dma_id) {
      case 0: return PROT_L0;   /* GF0  */
      case 1: return PROT_L1;   /* GF1  */
      case 2: return PROT_L2;   /* VG   */
      case 3: return PROT_L3;   /* VGS  */
      case 4: return PROT_L4;   /* VGF  */
      case 5: return PROT_L5;   /* VGRFS*/
      case 6: return PROT_WB1;  /* WB   */
      }
  }
  static int decon_control_protection(int ch, bool en) {
      int prot_id = decon_get_protect_id(ch);
      return exynos_smc(SMC_PROTECTION_SET, 0, prot_id,
                         en ? SMC_PROTECTION_ENABLE : SMC_PROTECTION_DISABLE);
  }
  ```
- `helper.c:344-382` (`decon_set_protected_content`), called from `decon_core.c:1036,1281,2550,3237`
  on every enable/disable with `regs==NULL` ⇒ `cur_protect_bits=0` ⇒ unconditionally unprotects
  every channel whose previous bitmask bit was set.
- IDs, from `include/linux/smc.h:63-80`: `SMC_PROTECTION_SET=0x82002010`,
  `PROT_L0=3, PROT_L1=4, PROT_L2=5, PROT_L4=6, PROT_L3=9, PROT_L5=10, PROT_WB1=11`,
  `SMC_PROTECTION_ENABLE=1, SMC_PROTECTION_DISABLE=0` (note the non‑linear L3/L4/L5 numbering).

This governs the IDMA's **AXI master NS‑bit for its DRAM read transactions** (DRM/secure‑video
use), not CPU register‑bus access — so it does *not* explain your register reads, since your
framebuffer DMA from non‑secure `0xf1000000` is clearly already working (if this bit were set,
the DMA's own memory reads would fault/produce garbage, not just the CPU's register reads). We
rank this low‑probability as the *primary* cause, but it's cheap, harmless, and exactly what
Samsung's own driver always does at `decon` enable, so issue it anyway for every channel you plan
to touch (window 5 / channel 0 ⇒ `prot_id = PROT_L0 = 3`):

```
exynos_smc(0x82002010, 0, 3, 0)   /* SMC_PROTECTION_SET, ch0/GF0, DISABLE */
```

### #3 (unlikely, kept for completeness): CMU_DPU clock/Q‑channel gating

`CMU_DPU` base = **`0x19000000`**, block size `0x8000`
(`drivers/soc/samsung/cal-if/exynos9830/cmucal-sfr.c:26`: `SFR_BLOCK(CMU_DPU, 0x19000000, 0x8000)`).
Relevant gates/QCH (all `CMU_DPU`‑relative; absolute = `0x19000000 + offset`):

| Name | Offset | Notes |
|---|---|---|
| `CLK_CON_GAT_CLK_BLK_DPU_UID_DPU_CMU_DPU_IPCLKPORT_PCLK` | `0x2000` | root PCLK for CMU_DPU's *own* register bank |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_AD_APB_DECON0_IPCLKPORT_PCLKM` | `0x2008` | DECON0's dedicated APB register‑bus bridge clock |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_DPU_IPCLKPORT_ACLK_DECON` | `0x200c` | shared AXI bus clock (data **and** reg‑bus) for DECON |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_DPU_IPCLKPORT_ACLK_DMA` | `0x2010` | shared AXI clock for **IDMA** (data + its own reg bus) |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_DPU_IPCLKPORT_ACLK_DPP` | `0x2014` | shared AXI clock for **DPP** |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_DPU_IPCLKPORT_ACLK_DPU_WB_MUX` | `0x2018` | WB mux |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_D_TZPC_DPU_IPCLKPORT_PCLK` | `0x201c` | **D‑TZPC's own clock** |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_LHM_AXI_P_DPU_IPCLKPORT_I_CLK` | `0x2020` | AXI‑P (register/peripheral bus) link master — gates **all** SFR access into the block |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_LHS_AXI_D0/D1/D2_...` | `0x2024/0x2028/0x202c` | AXI‑D (data) link slaves, one per SysMMU port |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_SYSMMU_DPUD{0,1,2}_IPCLKPORT_CLK_S1/S2` | `0x2050/0x2054/0x2058/0x205c/0x2060/0x2064` | SysMMU clocks |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_SYSREG_DPU_IPCLKPORT_PCLK` | `0x2068` | SYSREG_DPU |
| `CLK_CON_GAT_GOUT_BLK_DPU_UID_RSTNSYNC_CLK_DPU_BUSD/BUSP_...` | `0x2048/0x204c` | reset synchronizers |
| `CLK_CON_DIV_DIV_CLK_DPU_BUSP` | `0x1800` | BUSP divider |
| `DPU_CMU_DPU_CONTROLLER_OPTION` | `0x0800` | bit29 `ENABLE_POWER_MANAGEMENT`, bit28 `ENABLE_AUTOMATIC_CLKGATING` |

Every `CLK_CON_GAT_*` register uses the same bitfield layout (seen consistently, e.g.
`cmucal-sfr.c:6239-6247`): **bit21 = `CG_VAL`** (current gate state, 1=clock on),
**bit20 = `MANUAL`** (1 = SW override `CG_VAL`, 0 = HW auto request/grant via Q‑channel),
**bit28 = `ENABLE_AUTOMATIC_CLKGATING`**.

Q‑Channel registers, same base, from `cmucal-sfr.c:2417-2436` / `cmucal-qch.c:429-448`:

| Name | Offset |
|---|---|
| `QCH_CON_DPU_CMU_DPU_QCH` | `0x3038` |
| `QCH_CON_DPU_QCH_DPU` | `0x303c` |
| **`QCH_CON_DPU_QCH_DPU_DMA`** | **`0x3040`** |
| **`QCH_CON_DPU_QCH_DPU_DPP`** | **`0x3044`** |
| `QCH_CON_DPU_QCH_DPU_WB_MUX` | `0x3048` |
| `QCH_CON_D_TZPC_DPU_QCH` | `0x304c` |
| `QCH_CON_LHM_AXI_P_DPU_QCH` | `0x3050` |
| `QCH_CON_LHS_AXI_D0/D1/D2_DPU_QCH` | `0x3054/0x3058/0x305c` |
| `QCH_CON_PPMU_DPUD0/1/2_QCH` | `0x3060/0x3064/0x3068` |
| `QCH_CON_SYSMMU_DPUD{0,1,2}_QCH_S1/S2` | `0x306c..0x3080` |
| `QCH_CON_SYSREG_DPU_QCH` | `0x3084` |

Bitfields (`cmucal-sfr.c:8966-10243`): **bit0 `ENABLE`**, **bit1 `CLOCK_REQ`** (force‑request the
clock, ignoring the IP's own idle signal), **bit2 `IGNORE_FORCE_PM_EN`**,
**bits[25:16] `EXPIRE_VAL`** (idle timeout before auto‑gate‑off).

**Why this is probably *not* your blocker:** `drivers/video/fbdev/exynos/dpu30/decon_dsi.c:231-237`
— `decon_get_clocks()`/`decon_set_clocks()` for the **DSI** output path are literal no‑ops
(`return 0; }` / `{ }`); the only `devm_clk_get(dev, "aclk")` calls in the whole driver are in
`dpp_drv.c:1320` (**write‑back only**, inside `dpp_set_output_device()`, gated on `IS_WB(attr)`)
and `dsim_drv.c`/`displayport_drv.c`. The devicetree confirms it: `dpp_0..dpp_5` and `decon_f`
have **no `clocks`/`clock-names`/`power-domains` properties at all** (only `dpp_6` (WB), `dsim_0`
and `displayport` request `clock-names = "aclk"; clocks = <&clock GATE_DPU_DPU>;` —
`exynos9830-dpu.dtsi:252-253,277-278,302-303` already in this folder, and `GATE_DPU_DPU = CLK_DPU_BASE+1`
per `include/dt-bindings/clock/exynos9830.h`). That means for the normal DSI/command‑mode path
Samsung relies entirely on the Q‑channel's automatic "HW request/grant" (bit20 `MANUAL=0`): the
IDMA/DPP/DECON ACLK gates turn themselves on whenever the IP's internal logic is active, with no
software clock enable at all — which is exactly consistent with your DMA engine clearly still
running (continuously fetching/pushing pixels) while its *register* reads are 0. If the clock
feeding the register path were truly off, the datapath would be dead too. Still worth a **read‑only
sanity check** before anything else (see Verification Order below): if `0x19002010`/`0x19002014`
(`ACLK_DMA`/`ACLK_DPP` gates, bit21) read `CG_VAL=0`, that would upend this ranking.

## 2. IDMA shadow registers (Q3)

Confirmed: `drivers/video/fbdev/exynos/dpu30/cal_9830/regs-dpp.h:497`:
```c
#define DMA_SHD_OFFSET 0x800
```
used e.g. in `dpp_reg.c:1180-1188` (`shd_addr[0] = dma_read(id, IDMA_IN_BASE_ADDR_Y8 + DMA_SHD_OFFSET)`).
So yes — every IDMA (and ODMA, same macro) live register at offset `R` has its shadow/latched
copy at `R + 0x800`, inside the same per‑channel 0x1000 window (`0x19091000+0x800` for GF0, etc.).
DPP's registers do **not** use this offset in the code we found (DPP's "shadow" registers are a
separate fixed set of named offsets, e.g. `DPP_ENABLE_SHD`‑style addresses in `regs-dpp.h`, not a
blanket `+0x800`), and `DPU_WB_MUX` has explicit named shadow registers too
(`DPU_WB_ENABLE_SHD = 0x0B00`, `DPU_WB_CSC_CON_SHD = 0x0B08`, `DPU_WB_IMG_SIZE_SHD = 0x0B14`,
`regs-dpp.h:583-585`) rather than a uniform `+0x800`.

Per‑IP TZPC flag bits actually *documented* in the headers (read‑only mirrors, not writable from
the window itself — see §1#1):
- `DPP_ENABLE` (`0x19041000+n*0x1000`, offset `0x0000`) **bit0 = `DPP_TZPC_FLAG`** (`regs-dpp.h:632`).
- `DPU_WB_ENABLE` (`0x19080000`, offset `0x0000`) **bit0 = `DPU_WB_TZPC_FLAG`** (`regs-dpp.h:530`).
- `IDMA_ENABLE` (`0x19091000+n*0x1000`, offset `0x0000`) has **no documented bit0 name** in this
  header (`regs-dpp.h:76-89`: bits 1/2/4/8/24 are named — `INSTANT_OFF_PENDING`, `OP_STATUS`,
  `SFR_UPDATE_FORCE`, `SRESET`, `ASSIGNED_MO` — bit0 is unlabeled/reserved). Don't assume it
  mirrors `DPP_TZPC_FLAG`; test empirically once the block is reachable.

`DPU_DMA_COM` (`0x19090000`, 0x110B, shared/global) extra registers worth probing first (cheap,
side‑effect‑free reads): `DPU_DMA_VERSION` (`0x0000`, expect `0x05000000`), `DPU_DMA_QCH_EN`
(`0x000C`, bit0), `DPU_DMA_GLB_CGEN_CH0/1/2` (`0x0040/0x0044/0x0048`, **bit31 = `DMA_SFR_CGEN`** —
an IP‑internal, non‑CMU "disable automatic SFR clock gating" bit, separate from the CMU Q‑channel
gating in §1#3) (`regs-dpp.h:30-61`).

## 3. Verification order (read‑only, safe, run this first)

1. `0x19002010` (`ACLK_DMA` gate) and `0x19002014` (`ACLK_DPP` gate) — check bit21 `CG_VAL`.
   Expect `1` (clocks already running); if `0`, re‑rank §1#3 upward.
2. `0x19003040` / `0x19003044` (`QCH_CON_DPU_QCH_DPU_DMA` / `..._DPP`) — check bits 0/1.
3. `0x19090000` (`DPU_DMA_VERSION`) — if this (the *common*/global DMA block) reads non‑zero
   while per‑channel `0x19091000` (`IDMA_ENABLE`) reads 0, the protection is per‑channel, not
   block‑wide — informs whether §1#1 (whole‑block D‑TZPC) or §1#2 (per‑channel content
   protection) is the better first target.
4. `0x19041000` (`DPP_ENABLE`, channel 0/GF0) and `0x19091000` (`IDMA_ENABLE`, channel 0) —
   the two you already know read 0; re‑read after each step below.

## 4. Fix order (what to actually call)

**Step A — TZPC restore SMC (top candidate, proven call‑pattern on this phone for a sibling domain):**
```
x0 = 0x82000410   // SMC_CMD_PREAPRE_PD_ONOFF
x1 = 1            // EXYNOS_WAKEUP_PD_DOWN
x2 = 0x19010204   // pd-dpu's "need_smc" (exynos9830.dts:5345)
x3 = 2            // RUNTIME_PM_TZPC_GROUP
smc #0
```
Check `x0 == 0`. Re-read `0x19041000` / `0x19091000` / `0x19090000`. Analogous GPU call already
proven working on this exact phone: `smc(0x82000410, 1, 0x18410204, 2)` → returns 0
(`PLANS/04-gpu.md`, `pd-embedded_g3d`'s `need_smc`, `exynos9830.dts:5399`).

**Step B — per‑channel content‑protection disable (cheap, matches what Samsung's driver always
does, do regardless of whether Step A alone fixes it):**
```
x0 = 0x82002010   // SMC_PROTECTION_SET
x1 = 0
x2 = 3            // PROT_L0 (channel/window-5 source = DMA channel 0 = GF0)
x3 = 0            // SMC_PROTECTION_DISABLE
smc #0
```
Repeat per channel you need, with `prot_id` from §1#2's table (`3,4,5,9,6,10` for
`ch0..ch5`/`GF0,GF1,VG,VGS,VGF,VGRFS`, `11` for WB).

**Step C — generic secure‑SFR *read* via the EL3 monitor (fallback if A/B don't flip the
protection bit itself, or just to probe without side effects at any time)**, from
`arch/arm64/kernel/exynos-smc.S:57-70` (`exynos_smc_readsfr`, the exact assembly, not a
reconstruction):
```
w1 = (addr >> 2) | 0xC0000000   // SMC_REG_CLASS_SFR_R = (0x3<<30)
w0 = 0xFFFFFF9B                 // SMC_CMD_REG = -101, sign-extended
smc #0
// x0 == 0 on success; value is in x2
```
This has the **secure monitor itself perform the read** (it runs at EL3 so TZPC does not apply
to it) and hand the value back to NS — it does not require D‑TZPC to actually be reprogrammed,
so it should work even if Step A is "accepted but does not unlock" (see the ABOX caveat in §1#1).
`include/linux/smc.h:135-140` additionally defines an SFR‑**write** class
(`SMC_REG_CLASS_SFR_W = 0x1<<30`) using the same `SMC_CMD_REG`, but no in‑tree Linux caller
exercises it (only `exynos_smc_readsfr` ships as assembly) — treat a write attempt
(`smc(SMC_CMD_REG, (addr>>2)|0x40000000, value, 0)`) as experimental/unverified, not as a proven
path. This mechanism is independently corroborated on this exact SoC family by
`z3s-mainline-linux/docs/DEVLOG.md:544-547`, which references "the EL3 `SMC_CMD_REG` SFR path"
explicitly while debugging a different (ABOX DSP) secure-register problem — i.e. a second,
independent reverse‑engineering team on the same chip already relies on this exact primitive.

**Step D — last resort, higher risk (only if A/B/C all fail):** mirrors the one real gotcha this
project hit on the GPU side (`PLANS/04-gpu.md`: S‑Boot left G3D "on but with the *off*-sequence's
`PLL_CON1`"; fixed only by a full Samsung `..._off()` → `..._on()` **PMU power‑cycle**, not the
TZPC SMC alone). If the DPU block has an analogous "half‑configured" state, you may need an
actual `pd-dpu` PMU power‑domain off→on cycle, *then* the Step A SMC. `pd-dpu`'s PMU control
register is at `0x15861E80` (`exynos9830.dts:5342`, reg size `0x20`) — by direct analogy with
`pd-embedded_g3d` (`PLANS/04-gpu.md`: PMU `0x15860000`, `CONFIGURATION +0x2200` bit0,
`STATUS +0x2204` bit0, `OUT +0x2220`), `pd-dpu`'s `CONFIGURATION`/`STATUS` are most likely
`0x15861E80`/`0x15861E84` (**not yet independently verified for `pd-dpu`** — this is inference by
analogy from the proven G3D case, not a direct citation). **This is the risky step**: `pd-dpu`
also powers DECON/DSIM, so an actual power‑off will blank the already‑live panel, and Samsung's
exact PMU "on" sequence (`pmucal_pd_list[pd_id].on`, a table of register pokes we have not
extracted) is what would need replaying to safely bring it back — attempt only with a way to
recover (e.g. a watchdog reset) in hand, and only after A–C have been tried and read back.

## 5. Other homebrew/mainline efforts on this exact problem (Q4)

- **`Bentlybro/z3s-mainline-linux`** (Galaxy S20 Ultra, same SoC; mainline 6.12): confirms the
  exact same wall, in almost the same words as this question —
  `docs/DISPLAY.md` ("What is left"): *"the DPU_DMA (`0x19100000`) and DPP (`0x19091000`) blocks
  fault on register access (gated), so their size registers cannot be poked without ungating
  those clocks first."* (Saved locally would be redundant — see `docs_DISPLAY.md` note already in
  `PLANS/07-display.md`; full doc fetched from
  `https://raw.githubusercontent.com/Bentlybro/z3s-mainline-linux/572e7892ff651a50e84b4078f460bc8d6bb20210/docs/DISPLAY.md`.)
  Their approach is identical to yours: draw into the bootloader‑lit framebuffer via `simpledrm`,
  reprogram only DECON/DSIM/DSC (ordinary AP MMIO, not secure), and treat the DPU_DMA/DPP
  ungating as a separate, unsolved prerequisite for a true mode‑set (their 120 Hz work needs to
  shrink the IDMA source size, which needs the same ungating).
  Their README explicitly plans to finish the panel‑resolution switch in **uniLoader** (their
  bootloader project) instead of Linux, specifically because the bootloader already owns the
  panel/DECON/DSIM state cleanly — i.e. their eventual answer to "where do you reprogram DPU" is
  "in the bootloader stage, before Linux", the same stage you're already operating at.
  `docs/DEVLOG.md:526-527` (same repo) separately documents that the identical
  "DTZPC unlock SMC" family (used for the ABOX secure SysMMU, a different IP) was *"accepted but
  does not unlock"` — the one caution against assuming Step A is automatically sufficient
  everywhere on this chip (see §1#1).
- **`ben443/samsung-s20-pmos`** (postmarketOS port, same device): per web search, boots with
  display already lit by the bootloader (same simpledrm-on-S-Boot-framebuffer strategy); no
  DPU/DPP driver work found beyond that in the time available — treat as unconfirmed/needs a
  direct repo read if more detail is required.
- **This project's own GPU work** (`PLANS/04-gpu.md`) is the strongest data point of all: it
  already proved, on this exact phone, that `smc(0x82000410, 1, <pd's need_smc>, 2)` is a real,
  working, non‑destructive call that un-gates a sibling power domain's protected register/SysReg
  state (`G2 | Power cycle + TZPC SMC + first job | done`). Re‑using it for `pd-dpu`
  (`need_smc=0x19010204`) is the direct, lowest‑risk next experiment.
- No reference to **uniLoader**'s own source (as opposed to z3s's mention of it) or to
  **Mu‑Silicium's `SimpleFbDxe`** reprogramming DPU DMA was found within the research budget;
  this workspace's own `PLANS/07-display.md` (step **D7**, status "research") is the only
  existing first‑party note on this exact problem, and it is what this report is written to
  unblock.

## 6. Files saved in this folder (session `files/dpu-ref/`)

New in this pass (not already present from earlier sessions):
- `cmucal-sfr.DPU-excerpt.c`, `cmucal-node.DPU-excerpt.c`, `cmucal-qch.DPU-excerpt.c` — every
  `DPU`‑matching line from the full `exynos9830/cmucal-{sfr,node,qch}.c` (12349/~8700/1010 lines
  each; full files are at
  `drivers/soc/samsung/cal-if/exynos9830/cmucal-{sfr,node,qch}.c` in the kernel repo at the commit
  above), each line tagged `//@LINE:n` with its line number in the original file for citation.
- `helper.c` — `decon_set_protected_content` / `decon_control_protection` / `decon_get_protect_id`
  (content‑protection SMC path, §1#2).
- `include_linux_smc.h`, `drivers_soc_samsung_exynos-el3_mon.c`,
  `include_soc_samsung_exynos-el3_mon.h`, `drivers_soc_samsung_cal-if_pmucal_local.c`,
  `arch_arm64_kernel_exynos-smc.S` — the full SMC/TZPC‑restore mechanism (§1#1, §4 Step A/C).
  (Identical copies already existed in `files/gpu-ref/` from the earlier GPU bring-up — same
  SoC‑wide files — duplicated here so `dpu-ref/` is self‑contained.)
- `exynos9830.dts-pd_dpu-excerpt.dts` — the real board `.dts` node excerpts: `pd-dpu` power
  domain (with `need_smc`), the `dpp@0x19091000` 3‑window `reg`, `decon_f@0x19050000`, and the
  alias/pd‑name lists, each tagged with original line numbers.

Already present from earlier work in this session (not re‑fetched): `decon.h`, `decon_cal.h`,
`decon_core.c`, `decon_dsi.c`, `decon_reg.c`, `dpp.h`, `dpp_cal.h`, `dpp_drv.c`, `dpp_reg.c`,
`dsim_drv.c`, `dsim_reg.c`, `exynos9830-dpu.dtsi`, `regs-decon.h`, `regs-dpp.h`, `regs-dsim.h`,
panel files.
