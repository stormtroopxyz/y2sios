# 10 — mali2metal: Metal on the Mali‑G77, worked from evidence

mali2metal is the Metal implementation of this port: `RedwoodMetal.bundle` (`userland/src/redwoodmetal.m`,
`redwooddrawable.m`) over the compiler library `libredwoodair` (`tools/mesa/air/`: AIR → NIR → Panfrost) and
the kernel's GPU driver (`bsd/dev/redwood_mali.c`, `iokit/Kernel/RedwoodMali.cpp`). 05‑metal.md is its history
up to 25 October; this file is how it is worked on from now on.

**Goal:** CoreAnimation, SpringBoard and apps render on the Mali as they do on an iPhone, through a clean
implementation in which every change is backed by evidence from the phone.

## Rules

1. **No change without a record that asks for it.** A change starts from a record in the channel (or a
   measurement in a report) and says which one in its results-table row. "It might be X" is not a reason; if
   the evidence is missing, the first change adds the record that will show it.
2. **One hypothesis per run, measured before and after.** A run's report is compared with the previous one
   (`tools/m2m.py report` does this by default). A change that does not move its own record is not done.
3. **Fix the cause, not the symptom.** No per-label or per-process special cases, no "if this pipeline then
   skip". When the cause is in Mesa/Panfrost, the fix goes into its patch in `tools/mesa/` with the reason.
4. **No silent paths.** Anything declined, not implemented, or failed writes a record (W/E). Anything that
   cannot be done honestly is refused by name, never pretended.
5. **Guessing only when nothing can be measured**, and then said so in the results table, with the
   measurement that would settle it.

## The channel (m2m)

One channel for Metal and only Metal, apart from the console: `/dev/m2m` (written by user space, one record
per `write`), kernel records from the GPU driver, a 1 MB ring read through `sysctl kern.m2m_log`. Code:
`bsd/dev/redwood_m2m.c`, `bsd/sys/redwood_m2m.h`, `userland/src/m2m.h`.

```
<seq> <seconds.millis> <sev> <CAT> <process>[<pid>] <message>
42 81.233 E SURF  backboardd[16] no drawable: surface 9 (1136 bytes a row) could not be copied: ...
```

The kernel stamps the time, process and pid. Severities: **I** information, **W** declined or degraded,
**E** error, **F** fatal (a client died, the GPU is lost). A writer is limited to 400 records/s; drops are
reported as `W M2M flood`. On any return to the loader (panics included) the ring's last 128 KB are appended
to the saved log, so a run without a network keeps them.

### Categories

| CAT | Written by | What |
|---|---|---|
| `M2M` | kernel | the channel itself (start, floods, full client table) |
| `DEV` | driver, kernel | device and compiler library; a client registering (`client: metal`) and exiting |
| `PSO` | driver | render/compute pipeline states: created (`pipeline N: ...`), refused, blend set-up |
| `FUNC` | driver | function lookups, specialisation, function constants |
| `RES` | driver, kernel | buffers, textures, samplers, depth/stencil states; GPU memory refused |
| `SURF` | driver | drawables and IOSurface imports |
| `ENC` | driver | encoder state or a draw/dispatch refused or not implemented |
| `CMD` | driver | command buffers and the GPU work in them (failures, GPU did not finish) |
| `GPU` | kernel | job faults (with the submitting process), resets, fence waits that gave up |
| `CRASH` | kernel, `inject.c` | a Metal client killed by a signal (kernel), and where (`pc`, callers) |
| `PERF` | driver, kernel | 5 s windows: command buffers, commit and GPU wait (driver); GPU busy/idle/clock (kernel) |
| `SUM` | driver | a process's totals, on `com.redwood.metal.summary` |
| `TRACE` | driver | detail asked for by notification: `trace`, `dump`, `snap` |
| `MARK` | `m2m_mark` | a step of a scripted run |
| `LOG` | — | a line written without a category: a writer bug, counted by the report |

A process becomes a **client** when the driver opens the GPU (`m2m_client("metal")`); the kernel then reports
how it ends (`F CRASH ... ended by signal N` or `I DEV ... exited`).

## The loop

```
./y2s.sh kernel                                     # when the kernel changed
cd userland && QUIET=1 RC=rc.launchd.metal RC_JOBS=rc.jobs.manual ./build-gpu-y2s.sh   # when userland changed
./y2s.sh loop <label>                               # deploy, boot, tour, fetch, report
```

`loop` with iOS up on Wi‑Fi and the USB cable in:

1. payload from `m2m.cmdline` (the standard Metal boot line; changing it is a change like any other) + pack;
2. `GET /menu?to=1` → `kern.redwood_menu` → the loader opens Log access; `uflash`; cold restart (`ureboot`);
3. wait for `rwlogd`, 45 s to settle, `GET /tour` → `/redwood/etc/m2m.tour` (`userland/m2m.tour`): the same
   gestures every run, each step a `MARK`, the processes' `SUM` at the end, `tour done`;
4. `GET /m2m` saved as `logs/m2m/<time>-<label>.m2m`; `tools/m2m.py report` against the previous run:
   crashes, GPU faults, problems grouped by signature (with counts, processes, first step), per-step counts,
   performance per process and for the GPU, and what is new, gone or changed since the last run.

Other: `./y2s.sh m2m tail` (live), `./y2s.sh m2m tour` (the tour on the running system), `./y2s.sh m2m report
[RUN] [--prev RUN]`. `M2M_VIA=download ./y2s.sh loop` goes through Download Mode instead of Log access.

**Reading Apple's code** where a record points: `tools/dsc.py 0x1d32b7b9c` disassembles the iOS 16.5 shared
cache (`ios165/20F66__iPhone10,3_6`, as built: unslid, which is what CRASH records print in `[cache ...]`),
names the selector of each `objc_msgSend` stub call and the image of other branches (needs `pip3 install
--user capstone`). `ipsw dyld` cannot open this cache: its `.symbols` sub-cache was not extracted.
`tools/dsc_xref.py Metal.framework/Metal TEXT` finds a C string in one image and the `adrp`+`add` pairs
that load it (where to point `dsc.py`); `tools/dsc_exports.py libdispatch.dylib ADDRESS...` names addresses
from that image's own export trie (what a stub calls).

`kern.redwood_menu` values: 1 Log access (loader reason 3), 2 the menu (0), 3 Download Mode (4); anything else
is refused. (Until 27 October the value was passed through unmapped, so "Log access" arrived as the loader's
"early exception" and the phone stopped in the menu.)

## Results

| Run | Date | Change (and the record that asked for it) | Result (from the report) |
|---|---|---|---|
| 0 | 27 Oct | — (before the channel): `logs/m2m/run0-baseline-console.log` (the console log; no channel yet) | backboardd and SpringBoard are the Metal clients; idle home screen: 10 command buffers/s, ~22 draws each, commit 0.9 ms, GPU wait 2.2 ms |
| 1 | 27 Oct | the channel, the loop, the tour (no driver behaviour changed); first run bootstrapped with `M2M_VIA=download` | `20261027-094153-baseline`: **24 F**: mediaserverd (a Metal client through `mediamtl=1`) killed by SIGSEGV every ~5.8 s; **8 E**: every MapKit render pipeline refused (`interface: a buffer in address space 0`: Maps is black); chronod waits on the GPU 1 s on average (max 5 s) with no GPU fault; backboardd commits 12-34 ms with 0 GPU wait (CPU-bound); a pass drawing into mip level 1 is drawn into level 0 (Search); GPU 96 % idle at 662 MHz |
| 2 | 27 Oct | crash recorder in every Metal client (`m2m_watch_crashes`, the same code as `inject.c`'s); `tools/dsc.py` reads the shared cache at a CRASH record's `[cache ...]` address | `crashrec`: the fault is `MPSAutoCache::MPSAutoCache` (MPSCore) reading +0x18 of a nil `MPSCommandBufferImageCache`. Read in the cache: `-[MPSCommandBufferImageCache initWithCommandBuffer:]` returns nil if the buffer does not conform to `MTLCommandBuffer` **or** `MPSDevice::GetMPSDevice` finds no device (ladder of `supportsFamily:` Apple8..Apple2 with GPU-name checks); which of the two is not decidable from the binary |
| 3 | 27 Oct | record what MPS checks (`rw_mps_facts`, once per process when MPS is loaded) | `mpsfacts`: `MPSSupportsMTLDevice YES` (families Apple1 Apple2 are enough), userDictionary present, **command buffer conforms to MTLCommandBuffer NO** in every process |
| 4 | 27 Oct | one table of each class's Metal protocol (`rw_declare_protocols`), `MTLCommandBuffer` declared on the command buffer; the scattered `+load` declarations folded into it | `mtlcbproto`: **F 48 → 0**, mediaserverd runs once and goes further: a compute kernel (`kernelHomographySolver`) refused by the translator and a sampler with min/mag filter 2 refused. Not yet conformed to (looked at only): CommandQueue, ComputeEncoder, Render/ComputePipeline, Sampler, DepthStencilState |
| 5 | 27 Oct | repeat (only the protocols record corrected) | `repeat`: same problems, no new ones. **Noise floor** for identical runs: backboardd ±10 % (commit 2.1-2.4 ms, 11-12 cb/s, GPU wait ~3.0 ms), GPU busy ~4 %; chronod's GPU wait 218-1140 ms is bimodal: an event, not noise |

| 6 | 27 Oct | `batch1prof` (`M2M_PROF=1`; backboardd's timings not comparable): profiler with inclusive stacks; completion split; mip level/slice of attachments in the contract; pointer offsets passed as callee parameters (translator); cubic filter served as linear; the compiler library now rebuilt whenever `tools/mesa/air/` changes (it never was on the Mali build) | profile: `rwa_render_draw` 39 % of the library's samples, of which Panfrost's draw 46 %, our `check_bindings` 18 % (a `snprintf` per buffer per draw); BO allocation 18 % (pool BOs of each new batch: a `WAIT_BO` and an alloc ioctl each); chronod: a 7 s completion = 5.1 s queued + 0.1 ms GPU + 1.9 s handlers -- **not the GPU**; Search: 48 draws refused, attachments 704x128 at level 0 beside 352x64 at level 1; mediaserverd: `kernelHomographySolver` translates now, next refusals visible (texture view, `air.is_null_texture_2d`, a texture count) |
| 7 | 27 Oct | `batch2`: item 2 step A (refusal text only when refusing); item 3 one serial completion queue per command queue; item 4 render area = smallest attachment | backboardd a draw 32.9 → **30.1 us** (library 27.9 → **25.1**), beyond the ±10 % floor only for the library; Search: **0 refused draws** (48 before), no `W ENC`; chronod: completions no longer queue behind other queues' (0 ahead), but single waits of seconds remain. *Correction:* 57 draws per command buffer here is **not** "recovered blur draws": identical builds give 42-57 (runs 5, 9, 10), it is noise |
| 8 | 27 Oct | `batch3`: item 2 step B (kernel status page + submit seqno; shim answers own-BO `WAIT_BO` idle queries) | **invalid**: the run used a stale compiler library: the rebuild hash covered only `air/`, not `redwood-drm/` nor the patches. Fixed: the hash covers `air/*`, `redwood-drm/*`, `*.patch` in `mk-mesa.sh`, `build-gpu-y2s.sh` and `build.sh` |
| 9 | 27 Oct | `batch4`: run 8 again on the correct library; completion records gain QoS and queue depth | backboardd a draw 30.1 → **25.1 us** (library 25.1 → **20.7**): step B together with A: 32.9 → 25.1 (−24 %), library 27.9 → 20.7 (−26 %) |
| 10 | 27 Oct | `batch5`: repeat of 9 plus item 5's next layer recorded | a draw **26.6 us** (library 21.8): run 9 holds within the floor. chronod: a 5130 ms completion with **0 completions ahead** on its queue, committed at QoS 0 (unspecified), run at 0x15 → it waits for a *thread*, not for work. mediaserverd: `kernelHomographySolver` translates; new refusals: texture view not implemented, intrinsic `air.is_null_texture_2d`, a dispatch `kGP7x1_4cf_1` reads texture 0 with 2 textures bound |
| 11 | 27 Oct | `batch6`: kernel records a Metal client's suspend/resume (`task_pidsuspend`/`pidresume`, `task_suspend_internal`/`resume_internal` → `DEV client suspended/resumed`) | chronod is **not suspended**: a 3063 ms completion and no chronod suspend record (the only one in the run is CollectionsPoster). Found while reading the record's code: its "queued" interval also held the commit's bookkeeping and any scheduled handlers run inline, so it did not yet say *where* the time went → split |
| 12 | 27 Oct | `batch7`: the slow-completion record split (bookkeeping / scheduled handlers / time on the fence queue); **texture views** (`rwa_texture_view`: a second texture over the same resource, Apple's view-cast table and same-bit-length rule, refusals by name; level/slice/format windows applied in `texture_box`, sampler/image views, surfaces, copy, mipmaps) | chronod: 1994 ms **on the fence queue** with 0 ahead, 0 ms bookkeeping, 0 scheduled handlers → GCD did not run the block for 2 s. mediaserverd: views of RGBA8 levels (0+1, 1+1) made; `kGP7x1_4cf_1`/`kGP1x7_4cf_1` dispatch (6 each), the `E CMD` of run 10 is gone. backboardd a draw 28.9 us (library 23.5), above runs 9-11 (24.5-26.6) with no per-draw code changed: re-measured in run 13 before any claim |
| 13 | 27 Oct | `batch8`: `air.is_null_texture_*` (translator: bit n of word 0 of a constant buffer the shader leaves free, `air_shader_info.null_query_ubo`/`null_query_textures`; library: the word filled per bind from the slots bound, an empty asked-about slot bound to a 1x1 zero placeholder of its declared shape, `check_bindings` lets it be empty); scheduling record on a slow completion (thread priorities, task role, darwinbg) | the `is_null_texture_2d` refusal is **gone**, 0 new problems. backboardd a draw 26.9 us (library 22.2): run 12's 28.9 was noise (range over runs 9-13: 24.5-28.9). chronod: 7732 ms on the fence queue; committing thread priority 47, completing thread **31**; role 0, darwinbg 0 → not throttled, not background |
| 14 | 27 Oct | `batch9`: processor ticks (HOST_CPU_LOAD_INFO) at enqueue and at the block's start; `I PSO compute pipeline made` once per function | chronod: 2066 ms on the fence queue while the processors were **0 % idle** (73 % user, 27 % system; 619 ticks in 2.07 s = 3 processors' worth at hz 100, `cpumask=0x3f` asks for 6: the count is recorded from run 16). So the block at priority 31 lost to busier work. Read in Metal (`tools/dsc_xref.py`, `tools/dsc_exports.py`): `_MTLCommandQueue` makes `com.Metal.CompletionQueueDispatch` with `dispatch_queue_attr_make_with_qos_class(SERIAL, QOS_CLASS_USER_INTERACTIVE, 0)`; ours had no QoS. guidedFilterWeight was not asked for in runs 13-14 (the PSO records name every kernel made), so its dispatch is still unseen |
| 15 | 27 Oct | `batch10`: the completion queue made as Apple's (name, USER_INTERACTIVE) | **0 slow completions** (runs 11-14 had one each, 2.0-7.7 s); chronod's longest completion per 5 s window 2.0 ms (run 14: 2066 ms). Repeated in run 16 before it is called fixed |
| 16 | 27 Oct | `batch11`: repeat of 15 (and the processor count in the scheduling record) | **0 slow completions** again; chronod's longest completion 15 ms. Item 3 closed: runs 11-14 one stall each (2.0-7.7 s), runs 15-16 none. The processor count was not recorded (it is printed only with a slow completion) |
| 17 | 27 Oct | `batch12prof` (`M2M_PROF=1`, timings not comparable): item 2 step C's profile after A and B; `tools/prof-stacks.py RUN FUNCTION` sums the sampled stacks through one function | 729 library samples: `rwa_render_draw` 42.8 % (Panfrost's draw 30.9 %, of it `panfrost_update_shader_state` 21.3 %, mostly the render-to-texture flush it triggers: `panfrost_batch_submit` 94). **GPU allocations, ~14 %**: `pan_kmod_bo_alloc` 61 (50 from `resource_create`, callers past the 12-frame depth), mmap 22, and frees 41 (`drmCloseBufferHandle`: 24 from the batch pool's cleanup, 13 from `rwa_texture_destroy`). Fence export per flush (`drmSyncobjExportSyncFile`) 27. Next: count what is allocated (run 18) before changing anything |
| 18 | 27 Oct | `batch13`: `perf: allocations` per 5 s window (buffers and textures made and freed, bytes, the library's time); `m2m.py` compares `allocs_per_s`, `alloc_ms_per_s` | backboardd makes and frees **25-141 textures a window** in the tour's transitions at 0.3-0.4 ms each (up to 46 ms a window), almost no buffers; MapKit 120 buffers (24.9 MB) in one window, chronod 36 buffers (28.5 MB) in one, mediaserverd 102 textures + 51 buffers in one. So step C's allocation cost is CoreAnimation's texture churn meeting a Panfrost BO allocation (+ mmap) per texture when the cache has nothing that fits. Next: the shapes and sizes churned, then whether a texture-memory reuse keyed on them (what Apple's heap does) removes the kernel calls |
| 19 | 27 Oct | `batch14`: per window, the five commonest texture shapes made and the average lifetime of those freed | backboardd's churn is CoreAnimation's offscreen passes: BGRA8 render targets (usage ShaderRead+RenderTarget) of 64x64, 128x64 and many odd blur/shadow sizes; 256x1 RGBA16Float gradient tables; 180x180 RGBA8 icons. They live **1-4 s on average** (not per-frame), and are made in bursts 5-25 s apart, up to 203 in a window (82.7 ms; 0.2-0.5 ms each). Panfrost's BO cache drops a BO idle for more than 2 s (`panfrost_bo_cache_evict_stale_bos`), and skips one the GPU still reads (`dontwait` fetch), so each burst meets an empty cache: a kernel allocation and a map per texture. The fix is a cache policy (keep idle BOs by a byte budget rather than 2 s), which trades memory for it: to be decided with hit/miss counters in the record |
| 20 | 27 Oct | `batch15m5` (`M2M_SET="cpumask=0xff dvfs_cl2=2304000"`; the loop's `M2M_SET` puts key=value pairs into `m2m.cmdline` for one run): kernel `E CRASH fault:` record for every SIGILL/SIGSEGV/SIGBUS in any process (pc, lr, far, esr, instruction word, `[cache]` address, the core and its MIDR: `bsd/uxkern/ux_exception.c`); each core's `cpu-ident` and the processor count into the channel; `dvfs:` lines into the channel; Safari step in the tour (`com.redwood.open` in SpringBoard: `-[LSApplicationWorkspace openURL:]`) | **WebContent faults are stale instructions, not missing ones**: 25 faults, 24 on the M5s (cores 6-7, MIDR 0x531f0040), 1 on an A76; SIGILL with ESR class 0 (unknown) at JIT addresses whose memory holds ordinary code (`stp x29, x30`, `mov`, `cmp`, `ldr`), the rest SIGSEGV at pc 0 from JIT code. Read in the shared cache: `sys_cache_control(PrepareForExecution)` = `sys_icache_invalidate` = IC IVAU per 64 B line only, `sys_dcache_flush` = DSB only -- no DC CVAU, which Apple's cores do not need and these do (CTR_EL0.IDC 0 on A55, A76 and M5). Also: the phone at 87-102 C, every cluster at thermal step 1-2 (A55 ~806 MHz average); ACPM does not answer FREQ_GET (every clock read 0) |
| 21 | 27 Oct | `batch16m5uci`: SCTLR_EL1.UCI cleared in `SCTLR_EL1_DEFAULT` (user DC/IC by VA trap; `sleh.c` `redwood_user_cache_op` does DC CVAU + IC IVAU on the line through the kernel's physical alias); Panfrost BO cache kept 30 s within 64 MB (`tools/mesa/panfrost-retain.patch`) with hit/miss counters (`rwa_gpu_cache_stats`, `perf: gpu buffer cache`) | **the phone reset itself as SpringBoard came up, again and again, and left no log** (the loader keeps a log only when the kernel returns). Not a trap storm: the whole shared cache and dyld hold one user cache-maintenance instruction, `sys_icache_invalidate`'s IC IVAU (`tools/dsc_cacheops.py`). Cause not established |
| 22 | 27 Oct | `batch17notrap`: the same build with the trap made opt-in (`jit_flush=1`) and not asked for | boots and completes the tour: the trap was the reset, the cache change is not. **GPU buffer cache 90-100 % hits** (37 % in its first window); backboardd a texture 0.37 -> 0.23 ms (not 0: what remains is not the kernel allocation). All 8 cores up (the A55s join 28 s after boot by design: `small_cores_after`; run 20's "5 of 8" was read before that) |
| 23 | 27 Oct | `batch18jit`: `jit_flush=1` arms the trap 60 s after boot (`jit_flush_after=`), on every core by cross-call and on any core that starts later; the first 20 trapped operations recorded before they are done; the channel followed live from the Mac (`tools/m2m_follow.py`) | **Safari's faults 38 -> 0, crashes 3 -> 0, with all 8 cores**; the first trap only when Safari opened (WebKit, IC IVAU per line, managed pages, on M5 core 7); 578 operations in that 10 s window, 1 in the next; no reset. Standard line from here: `cpumask=0xff dvfs_cl2=2304000 jit_flush=1`. Open: why arming at boot reset the phone |
| 24 | 27 Oct | (the user, on run 23's build) Safari "a problem repeatedly occurred" after a few pages | the channel, read live: WebContent faults resumed **on every kind of core** (A55 cpu 3, A76 cpu 4-5, M5 cpu 6-7) while the per-line trap ran (up to 3489 a 10 s): the missing DC CVAU was not the whole of it. With only the A55s and A76s (one DynamIQ cluster) Safari works: **an IC IVAU does not reach the other cluster's instruction caches** |
| 25 | 27 Oct | `batch19range`: at sys_icache_invalidate's IC IVAU (checked: unslid pc 0x1d0c3dc8c, `ic ivau, x9` = 0xd50b7529), its whole remaining range at once (x9 the line, x10 minus the lines left): DC CVAU on every line, then a cross-call in which every core empties its own instruction cache (IC IALLU), and the loop taken to its last iteration; the tour's Safari step opens three pages | **0 faults** over three pages (WebKit drawing 16 command buffers/s instead of 0.6), 0 crashes. Cost: up to 16774 flushes a 10 s, each emptying all 8 instruction caches |
| 26 | 27 Oct | `batch20calog`: a range of up to 256 lines invalidated line by line on every core (IC IVAU by the kernel's alias, locally) instead of IC IALLU; `CA_LOG_TRIPLE_BUFFERING=1` for backboardd (`JOB_ENV`) | **0 faults** still. CoreAnimation logged nothing about triple buffering |
| 27 | 27 Oct | `batch21tl` (`touch_lat=1`): per-frame lines from the display driver; `tools/frame_split.py` | animation frames (3242): median 59.9 fps but only **48 % at 60 Hz**, 32 % at 40 fps, 20 % at 30 fps or worse; window server's own time from the frame before on the panel to this swap: median 9.3, p90 27 ms; GPU done to on the panel 6.9 ms. Found: CoreAnimation calls `SwapWait` for the swaps before each one (`tl: w N-2`, `w N-1`, `s N`) -- Apple's double buffering -- and ours answered at once while SwapEnd blocked |
| 28 | 27 Oct | `batch22swapwait`: SwapEnd returns once queued; `SwapWait(id)` blocks until that swap is on the panel; a would-tear counter | correct (1 would-tear in 11064 waits) but **no gain**: 45 % at 60 Hz. The frames are spaced by the window server's own CPU time (p90 21 ms; QuartzCore most of it, our driver ~13 % of backboardd's samples). Back to `fb_swapwait=1` by default. The phone rebooted once ~60-90 s after boot in this build (cause not read yet: the loader's log needs Log access). **Idle, every cluster at its lowest clock, the SoC stays at 81-82 C; 20 s of use takes it to 95-99 C** |
| 29 | 27 Oct | `batch23snd` (sound, `snd_tone=1500`): CMU_AUD's gates and Q-channels on the speaker path opened as cal-if does | every gate was already passing (CG_VAL set, manual) and the Q-channels requested: no change, RDMA3 still reads 0 frames, UAIF2 status 0. (The user heard no beep.) |
| 30 | 27 Oct | `batch24qch`: the ABOX's own Q-channel for UAIF2's bit clock and the bus clock disabled (`ABOX_QCHANNEL_DISABLE`, as abox_if.c's BCLK widget does) | run interrupted before its log was read; sound set aside by the user's order (power and CPU first) |
| 31 | 27 Oct | `batch25texbuf`: textures over a buffer's memory (`rwa_texture_from_buffer`: the buffer object handed to a linear 2D import inside the process by the DRM shim's tagged `drmPrimeHandleToFD`; import refusals now name the buffer or the IOSurface) | chronod's widget gets its texture (2D 987x465 BGRA8, 3968 bytes a row, offset 0) where it got nil; 0 new problems |
| 32 | 27 Oct | `batch26gpugov` (`touch_lat=1`): the GPU's clock governed like the CPU clusters' (`dvfs_gov_gpu=1`): every 200 ms its busy time (`redwood_mali_busy_pct`, never-reset totals per kind of job), Samsung's table 156-702 MHz, aiming at 40 % busy and the top at 60 % (the swap waits for the GPU, so a busier GPU is a longer frame); thermal steps cap it | **GPU 220-330 MHz on average through the tour instead of 702; idle settles at 71-73 C (81-82 C before), the tour at 75-83 C (88-99 C), most windows unthrottled. Frames unchanged**: 49.5 % of animation frames at 60 Hz (48 %), GPU done the same. Next in power: the CPU cores' idle power-down -- needs the MCT as the per-core timer (the ARM timer is `use-clocksource-only` in the device tree: it does not wake a powered-off core), PSCI CPU_SUSPEND 0x10000 (the dtb's c2), and a loader trampoline that finds a waking core by an exact MPIDR table (A55 #1's MPIDR masks to M5 #0's id) |
| 33 | 27 Oct | `cpuidle1`, `cpuidle2wakes`, `fbirq1`, `wakes3`, `idle2`, `idle3` (`cpuidle=1`): idle cores powered down (PSCI CPU_SUSPEND, MCT wake, loader resume trampoline); wake record per core | every core powers down without a fault (A55s ~99 % asleep, big cores 60-75 %); wakers found and cut: sound feeding silence (~1000/s -> ~220/s), Bluetooth 2 ms polls (-> interrupt, ~7/s), framebuffer latch polls (-> DECON frame-start interrupt), short delay()s (-> spin, `powergate_latency`). Idle 74-78 C |
| 34 | 27 Oct | `idleprof` (profiler on the still lock screen) | 48 % of backboardd's samples in memmove under `panfrost_emit_const_buf`: slot 0 always a user buffer, uploaded whole (64 KB a stage) every draw. Fix: slot 0 at an aligned offset bound as the buffer itself plus its mapping for pushed words (`perf: constant buffers` line) |
| 35 | 27 Oct | `batch27`-`batch28`: touch, battery, Wi-Fi, quick bugs | touch and fuel gauge dead after `powergate_latency`: the bit-banged I2C's clock-stretch wait was 1000 x 1 us delays, now 1 ms real -> 25 ms by the clock. Wi-Fi joined by name reached iOS as 0 dBm/0 MHz/no RSN: now `WLC_GET_BSS_INFO` after the join and `WLC_GET_RSSI` every 2 s. Charger present = CHGIN_OK and CHGIN_DTLS 3 (Samsung's `max77705_get_vbus_state`). Lock screen at boot (`autounlock=0`). Amplifier quiet timer fixed |
| 36 | 27 Oct | `cold1` (cold, no reflash), `batch29`: GPU governor and touch | cold: 65-80 C, clocks at the top, still 80 % of animation frames on time (heat is not the cause). Blur screens (Search, Settings, Control/Notification Centre) at ~30 fps; single fragment jobs 17-23 ms at 156-260 MHz with the GPU 85-93 % idle. Added: a GPU job > 8 ms wakes the governor and takes the GPU to the top; Samsung's touch booster floors (dts `input_booster` touch: head 130 ms, tail 500 ms) applied the moment a finger lands; GPU 572 MHz floor on touch; brightness capped at 70 % from 90 C, 50 % from 96 C. Result: median longest job 12.4 -> 7.8 ms but on-time frames 80 -> 78 %: the GPU is not the limit either |
| 37 | 27 Oct | `batch30tl`-`batch33tl` (`touch_lat=1`), `batch31prof` (SpringBoard profiled too) | per frame: previous frame on the panel -> swap 9 ms median / 22 p90, GPU 0.5-2.5 ms, GPU done -> panel 7 ms; 30 % of intervals 25 ms (not a multiple of the 60 Hz refresh). backboardd 0.11 and SpringBoard 0.06 threads running on average: waiting, not working. **The window server never registers for vsync notifications (selector 72): 0 requests** (`fb_vsync:` line). It reads GetBlock 0x4d (1580 bytes, answered with zeros), GetBlock 0x40 and SupportedFrameInfo (selector 74) -- the next lead: how iOS 16's CoreAnimation learns the display's timing |
| 38 | 27 Oct | `batch34`: storage, Wi-Fi quality | `disk0` (RedwoodStorage: BSD Name + Size) -> TotalDiskCapacity 127968215040 (was -1, Settings showed 2 GB). Wi-Fi noise floor from `WLC_GET_PHY_NOISE` (-91 dBm) into CURRENT_NETWORK / NOISE / link events with SNR, as itlwm fills `asr_noise` (iOS showed one bar at -42 dBm with noise 0) |
| 39 | 27 Oct | `batch35vsync` (`touch_lat=1`): `CA_FORCE_TIMER=1` in backboardd's job (userland/mkjobs.py) | Why there was no vsync: QuartzCore 0x188364614 sets the display's flag only for ChipID 0x7000-0x8004 or `CA_FORCE_TIMER` != 0, and 0x188373218 then calls IOMobileFramebufferEnableVSyncNotifications (framework 0x1af1c4afc: selector 72 with the type-5 handler from the table at 0x1e2289100) instead of its own timer. Now: 799 requests, 6304 vsyncs sent. **No gain**: next refresh 76 % (74-80 % in runs 36-38) -- the drift was not what misses refreshes. Kept: it is Apple's path for a display that sends vsync |
| 40 | 27 Oct | `batch36nobeat` (`fb_beat=0`) | the presenter's beat off: 78 % on the next refresh -- no gain either. Ruled out so far: heat, GPU clock, vsync source, beat. Left: the tails -- vsync -> swap p90 7-16 ms (the window server's own frame), GPU p90 8 ms; 99 % of backboardd's render passes load their attachments (a full framebuffer read per pass on a tiler) -- the next measurement |

## Plans (27 October)

**Rule clarified:** one hypothesis per *record*. Changes whose records are independent (different processes or
categories) may share a run; a performance claim is only made against the noise floor (run 5).

| # | Item | Evidence | Plan | State |
|---|---|---|---|---|
| 2 | backboardd commits CPU-bound | run 6 profile | A: refusal text built only when refusing (`check_bindings`, attachments) → measure per-draw library time. B: answer Panfrost's timeout-0 `WAIT_BO` idle queries in the shim from a kernel-shared completed-job counter and each submit's seqno (own buffers only; shared ones still ask the kernel). C: then Panfrost's own draw (`update_shader_state`, batch flushes from render-to-texture) | A run 7, B run 9 (−24 % a draw, −26 % library). C: profiled (17): allocations ~14 % of the library, fence export ~4 %; allocation counts per window (18) |
| 3 | chronod completion latency | run 6 split: queued 5.1 s, handlers 1.9 s, GPU 0.1 ms | one serial completion queue per command queue (Apple's model) instead of one per process; the record names the queue; then what the 1.9 s handler does (profile chronod) | queue fix run 7; the remaining multi-second waits have nothing ahead (run 10). not suspended (11); the time is on the fence queue (12) at priority 31 on 0 %-idle processors (13, 14); Apple's queue is USER_INTERACTIVE, ours now too: **0 stalls in runs 15 and 16 -- closed**. Left: chronod ~0.8-1.1 ms a draw (first-use variants?), and why the processors are saturated (3 or 6 online) |
| 4 | Search blur pyramid in the wrong level | run 1 `W ENC`; run 6 48 refused draws | level/slice per attachment (done); render area = smallest attachment (Metal's rule, `renderTargetWidth` 0) | run 7: 0 refusals, 0 `W ENC`; checked on the phone after run 18: the Search blur is correct -- **closed** |
| 5 | mediaserverd (AppleCVA/MPS photo pipeline) | run 4/6 refusals | done: pointer offsets as parameters, cubic → linear. Next: texture views, `air.is_null_texture_2d`, the "reads texture 0 and 2 were bound" dispatch | texture views (12; the binding mismatch was the nil view) and `is_null_texture` (13) done, 0 refusals left in mediaserverd; guidedFilterWeight's dispatch not yet requested in a run since |
| 1 | Maps black | 8 PSO refusals | last, as asked | — |

## Open problems, by evidence (next runs)

| # | Record | What is known | Next step |
|---|---|---|---|
| 1 | `E PSO` MapKit: 8 render pipelines refused, `interface: a buffer in address space 0` | Maps draws nothing on Metal | read the translator's refusal and MapKit's AIR (the functions are named in the PSO records) |
| 2 | `W PERF` backboardd: commits of 100-260 draws take 12-34 ms with no GPU wait | the frame budget is 16.7 ms; the GPU is 96 % idle | per-draw CPU split (encoder vs Gallium vs Panfrost) from the `perf: per draw` records, then the profiler |
| 3 | chronod GPU wait 0.2-1.1 s average, up to 5 s, no GPU fault | widgets are slow to appear | record each long fence wait with the job it waits for (kernel) |
| 4 | `W ENC` a pass draws into mip level 1, the driver draws into level 0 (Search) | wrong pixels in the Search blur pyramid | render into the attachment's level/slice (Gallium surface `u.tex.level`) |
| 5 | `E PSO` mediaserverd compute `kernelHomographySolver` refused (pointer at a computed offset) | photo processing | translator support for that call shape |
| 6 | `E RES` mediaserverd sampler min/mag filter 2 | | read what MPS passes; Metal has only 0 and 1 for min/mag |
| 7 | `W DEV protocols` not conformed to: queue, compute encoder, pipelines, sampler, depth/stencil | nothing has failed for it yet | declare when a record shows a test of one (as MPS's was) |
