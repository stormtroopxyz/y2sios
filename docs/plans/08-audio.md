# 08 — Sound: speakers through the ABOX and the CS35L41 amplifiers

Goal: what iOS plays (ringtones, music, keyboard clicks, video) comes out of the bottom speaker
and the earpiece. Later: volume buttons, the microphones.

## What already exists (from upstream, Qualcomm phone)

The iOS side is done and does not depend on the chip:

- `userland/src/audio_plugin.c`: a CoreAudio HAL plug‑in in mediaserverd. It writes 48 kHz PCM to
  **`/dev/redwood-audio`** (`bsd/dev/redwood_audio.c`; ioctls for volume and state).
- `bsd/dev/redwood_guard.c`: speaker protection in place of the DSP's (gain, high‑pass, power
  averages, peak limiter).
- `bsd/dev/redwood_sound.c` + `redwood_speaker.c`: the **hardware back end**. It is
  Qualcomm‑only (ADSP: AFE/ADM/ASM, TDM pins, Qualcomm amps). This is what gets replaced.

So the job is a new back end, `redwood_exynos_sound.c`: PCM from `redwood_audio.c` → ABOX →
UAIF2 → the two amps.

## Hardware (y2s device tree, `files/dts`)

| | |
|---|---|
| ABOX (Calliope) | `abox@18c50000`. RDMA 0–11 at `0x18c51000 + 0x100·n`, WDMA 0–4 at `0x18c53000 + 0x100·n`, two DSP cores at `0x18c55000`/`…080`. Secure SysMMU at `0x18e50000` (**read‑only for us**, in bypass) |
| Speaker link | **UAIF2**, format `dsp_a` (TDM), codecs `cs35l41_l` + `cs35l41_r` |
| Amps | 2× **Cirrus CS35L41** (firmware named `cs35l40-*`) on **`hsi2c_31`** at 400 kHz: `0x40` = right / bottom speaker (`cs35l40-bot`, reset **gpb2‑5**), `0x41` = left / earpiece (`cs35l40-rcv`, reset **gpb2‑4**); IRQ gpm24; VA = `s_ldo4`, VP = battery |
| Firmware (`fw-stock/vendor/firmware`) | `calliope_{sram,dram}.bin` (DSP), `abox_tplg.bin` (DAPM topology), `cs35l40-{bot,rcv}.wmfw` + tuning `.bin` (amp DSP), `mixer_paths*.xml` |

z3s (Linux on the same SoC; `docs_DEVLOG.md` 518–577) got:
- the DSP booted (bypassing the SysMMU, plus an EL3 SMC for the ABOX GIC);
- the front end pumping;
- **the amps powered and clocked** (`GLOBAL_EN` 0x2014 written last).

They stopped at the back end (SIFS0/RDMA3/UAIF2), which the DSP firmware owns.

## Approach: no DSP first

The DSP is the hardest and riskiest part: secure SysMMU, EL2 traps, and firmware‑owned DAIs.
The ABOX's DMA, mixer and UAIF are ordinary AP registers. So **plan A** skips the DSP:

1. RDMA n reads our PCM ring buffer (physical address; the SysMMU is in bypass) →
2. the ABOX route/mixer (SPUS → SIFS) →
3. UAIF2 as TDM master (BCLK/LRCK from the ABOX clocks) → the amps' ASP inputs.

Plan B is z3s's route (boot Calliope with the stock firmware plus the RDMA‑trigger trick). Use it
only if the RDMA cannot be started without the DSP.

| ID | Step | Measured by |
|---|---|---|
| A1 | **Register map.** Done: `files/abox-ref/ABOX_REPORT.md` and 14 Samsung driver files. Summary below. | **done** |
| A2 | **Probe**, boot‑arg `snd=1`. Read `pd-aud` status (PMU `0x15861900`), CMU_AUD `0x18C00000` (PLL_AUD0/1, UAIF2 mux/divider), the ABOX version, UAIF2 `0x18C50520`, ROUTE_CTRL0 `0x18C50040`, SPUS_CTRL_FC0, RDMA3. Then `snd=1` with `pd-aud` power‑on + `smc(0x82000410, 1, 0x18e10204, 2)` (the GPU/DPU D‑TZPC pattern) and read them again. | `snd:` lines in the log |
| A3 | **Amps over I2C**, `snd=2`: HSI2C 31 driver (reuse the HSI2C code of `redwood_max77705.c`/touch), reset gpb2‑4/5, read the device ID (`0x0` → `0x35a40`), then the Android/z3s init order, with `GLOBAL_EN` last (no tone generator on this amp: the first sound is A5). | ID read back; amp status registers show power‑up |
| A4 | **UAIF2 clocks**, `snd=3`: ABOX power domain up, audio PLL/CMU_AUD at 48 kHz × 32 × slots; UAIF2 TDM master | BCLK/LRCK toggling (sample the GPIO data register as `redwood_sound.c` does) |
| A5 | **Tone through RDMA**, `snd=4`: RDMA → route → UAIF2 with a 1 kHz tone in our buffer (−20 dBFS); amps on | audible tone |
| A6 | **iOS audio**, `snd=5`: `redwood_audio.c` feeds the RDMA ring (period interrupts or a timer); the guard in between; amps power up on non‑silence and off after 3 s of silence (as upstream) | music and keyboard clicks play |
| A7 | Volume buttons → system volume (already HID consumer keys?); earpiece for calls later; microphones (WDMA, PDM) later | |

## Register map (A1 findings, Samsung exynos990 kernel)

- **AP registers, no DSP or IPC involved:** route `ABOX_ROUTE_UAIF_SPK` (ROUTE_CTRL0 `0x18C50040`), SPUS‑OUT mux (`SPUS_CTRL_FC0..2` `0x18C50200+`), SIFS0 (a passive mixer), and **all of UAIF2** (`0x18C50520`: format, master, TDM slots, enable). `abox_if.c` contains no IPC at all. The AP sets the UAIF2 BCLK through CMU_AUD (`0x18C00000`).
- **RDMA buffer and enable:** the stock driver only programs these through DSP IPC (`IPC_PCMPLAYBACK`: `PCM_SET_BUFFER`, `PLTDAI_HW_PARAMS/PREPARE/TRIGGER`). The registers (`BUF_STR/BUF_END/BUF_OFFSET/CTRL1/ENABLE`) carry no secure marking, so writing them directly is **plausible but unproven**. This is the plan‑A risk. A5 tests it.
- **BD_MIXER** is the only DSP‑resident block, and the stock `mixer_paths.xml` routes `RDMA3_A = None`. One stream does not need it.
- **Calliope DSP:** reset/power is in PMU_ALIVE (`0x158631A0`/`0x15863220` enable bits). We leave it unpowered.
- **`pd-aud`** (`0x15861900`, `need_smc 0x18e10204`): its D‑TZPC restore SMC is the same as the GPU's and the display's.
- **CS35L41:** `GLOBAL_EN` = `PWR_CTRL1 0x2014` bit 0, written last. PLL source SCLK/BCLK (`PLL_CLK_CTRL 0x2C04`). TDM format `SP_FORMAT 0x4808`, RX slots `0x4820`. **There is no tone generator** (drop A3's tone test; A3 proves only the I2C ID and power‑up). Use `PCM Source = ASP` to bypass the amp's Halo DSP for the first sound.

## Risks

- ABOX register writes that need the DSP. If RDMA/UAIF will not start with the DSP in reset, go to
  plan B.
- The CMU_AUD / audio PLL setup may be only in Samsung's `cal-if` tables (ACPM); z3s got the UAIF2
  BCLK running, so it is reachable.
- Amp protection: no DSP speaker protection. The guard plus a conservative amp gain (start 20 dB
  under Android's level) cover it, as upstream did.

## Results log

- 2026‑10‑25: firmware and configs extracted from the stock AP (`fw-stock/INDEX.md`); hardware
  facts from the device trees.
- Driver written: `bsd/dev/redwood_exynos_sound.c` (boot‑arg `snd=1`, plus `snd_below`, `snd_gain`
  and `snd_tone=MS`). No DSP. The AP programs ABOX itself:
  - Power: pd‑aud on, then the DTZPC restore SMC (`need_smc` 0x18e10204), then the pads out of
    retention (PMU +0x1920 bit 11).
  - Clocks: PLL_AUD0 at 1179.648 MHz; AUDIF ÷16 = 73.728 MHz; UAIF2 BCLK ÷24 = 3.072 MHz.
  - Path: RDMA3 (s16 stereo, a 32 KB ring at the tail of the loader's DMA pool below 4 GB) →
    SPUS chain 3 → SIFS0 (32‑bit stereo) → UAIF2. UAIF2 runs DSP_A, NB_NF, ABOX master, 2 × 32‑bit
    slots.
  - SBANK slices are given to RDMA3 (offset 0) and the mixer (offset 512), 512 bytes each, which
    the firmware would normally do.
  - Both CS35L41s are set up as Cirrus's driver does: reset, OTP_BOOT_DONE, OTP trim unpack,
    phone DT pdata, slave DSP_A, SCLK PLL at 3.072 MHz, ASPRX1 → DAC. Left (0x41) takes slot 0,
    right (0x40) slot 1.
  - The amps power up on sound and down after 3 s of quiet. iOS volume goes to their digital
    volume. Faults are polled twice a second.
  - The DMA position is cross-checked against the clock after 250 ms, with fallback to the clock.
    The `snd:` log lines carry every register readback needed for a one‑boot diagnosis.
