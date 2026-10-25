<h1 align="center">Ghost of Yōtei on AnyPS5: Linux / AMD (RADV)</h1>

<p align="center"><b>Celegans12's <a href="https://github.com/Celegans12/AnyPS5/tree/yotei/best"><code>yotei/best</code></a> ported to Linux and AMD, running Ghost of Yōtei (PPSA26344 v02.002) into the open world.</b><br>
<sub>Branch <code>yotei/linux-best-u2</code> · progress thread: <a href="https://github.com/boykopovar/AnyPS5/discussions/675">discussion #675</a> · <a href="#run-it-on-linux">run it</a> · <a href="#the-world-load-gpu-fault-was-a-mesa-aco-bug">the world-load crash</a></sub></p>

> [!NOTE]
> A work-in-progress test branch, not upstream code: `yotei/best` (Windows) plus the fixes it needs on Linux/RADV,
> a Mesa patch, and diagnostics. Nothing here has been reviewed upstream.

## Where it stands (2026-10-25)

| | |
|---|---|
| **Furthest point** | Open world (loads from the title screen's Continue) |
| **Frame rate** | open world about **1.6 fps** at the village gate: 16.2 presents per 10 s without the driver's profiling (run best191), 15.7 with it over 20 minutes (best189), on `da11e77de` with the environment below and the 1080p game patch; was about 0.7 fps on 2026-10-20, 1.13 fps on 2026-10-23 and 1.3 fps on 2026-10-24 |
| **Base** | Celegans12's `yotei/best` 233fff3f6 (upstream `main` 2026-10-21, draw-ahead #6188 off by default) |
| **Needs** | Mesa 26.2.4 with [our ACO patch](docs/yotei-linux/mesa-26.2.4-aco-spill-fixes.patch); without it the GPU faults while the world loads |
| **Tested on** | CachyOS, kernel 7.2.9, AMD RX 9070 XT (RDNA4, gfx1201), RADV, 32 GB RAM + swap, GCC, Release |
| **First boot** | long: every shader compiles cold (AnyPS5 translation and Mesa); later boots use both caches. The recompiler changes of 2026-10-24 start a new AnyPS5 shader cache (about 11 minutes to the menu here, cold) |

## The world-load GPU fault was a Mesa ACO bug

Loading the world hung the GPU with a page fault at a guest address (`0x80808de000`) in compute shader `0x800028a800`.
The shader is huge (580k SPIR-V words, 256 VGPRs with about 170k VGPR spills), and ACO spills part of it to LDS.

- ACO's pass order is spill → pre-RA scheduler → register allocation. Scratch spills carry
  `memory_sync_info(storage_vgpr_spill)`, but the LDS spills (`ds_write/read_addtid_b32` in `aco_spill.cpp`) carried
  none, so the scheduler saw no memory dependency between them and moved a reload above the store to the same LDS slot.
- In our page-table lookup (`probe_bda_pointer`), the result `base + (address - begin)` went through slot 512/8448, which
  held the untranslated guest address a moment earlier. The hoisted reload returned the guest address, and the load
  through it faulted.
- How it was pinned down: the page table read back from GPU memory at the hang (umr) maps the address correctly
  ([`bda-table-lookup.py`](docs/yotei-linux/bda-table-lookup.py) replays the shader's lookup on the dump); ACO's IR
  before spilling (`RADV_DEBUG=preoptir`) is correct; after spilling and scheduling the select reads the slot before
  the result is stored.
- Fix: give the LDS spill and reload instructions the same `memory_sync_info(storage_vgpr_spill, semantic_private)`
  as the scratch ones. Upstream Mesa main has the same code. With it, the world loads.

[The patch](docs/yotei-linux/mesa-26.2.4-aco-spill-fixes.patch) applies to Mesa 26.2.4 and also holds the fix that
lets this shader compile at all (ACO's spill interference graph as vectors: 1.6 billion edges in hash sets ran out of
memory), plus two env-gated debug aids (`RADV_PRINT_SHADER_VA`, `RADV_DUMP_SHADER_BIN`).

## What this branch adds on top of `yotei/best`

- Linux fixes: struct sizes in the shader disk cache asserts; `HostImportFor` re-entered from an import's unwatch hook
  no longer self-deadlocks (Linux fd import path).
- Game fixes: unclamped blending on normalized color targets clamped; draws whose LS slot holds a merged LS+HS hull are
  skipped instead of aborting (tessellation of merged hulls isn't supported yet); ATRAC9 vibration channels decoded
  (PR #6721 by JJenkx).
- #6416 BDA fault masks for wave-operation programs, with the older fix kept for the giant programs that blow up memory
  (`APS5_BDA_STOP_WORD_LIMIT`, `APS5_BDA_CONTINUE`).
- Performance: storage refreshes flush only images whose elements overlap; `APS5_DEFER_REFRESH_FLUSH=1`.
- Performance, 2026-10-23 (each measured with an in-run A/B, most also with a confirmation run; zero faults, rendering
  unchanged; each has a runtime switch, so `APS5_AB` can toggle it):
  - BDA address tables merged incrementally: a build changes ~10 of the cached space's ~9,400 ranges, and the whole
    list was rebuilt ~3,400 times every 10 s (~375 us each): **+6–7 %** (`APS5_NO_INCREMENTAL_BDA_RANGES=1` off).
  - The reflection probe the game compresses to BC6H on the GPU each frame is copied into the cached texture on the
    GPU instead of written back and re-read (`APS5_NO_SLICE_COPY=1` off); with `APS5_DEFER_REFRESH_FLUSH=1`
    (now a runtime switch) the per-frame waits for the GPU fall by ~30 %: **+3.3 %**. Both are needed: either alone
    only moves the wait.
  - Depth surfaces indexed by address (~270 live surfaces were scanned ~40,000 times a second):
    **+2 %** (`APS5_NO_DEPTH_INDEX=1` off).
  - Recorded draws hand the resource sets their cache insert evicts to the release thread, as dispatches already
    did: **+2 %** (`APS5_NO_DEFERRED_DRAW_EVICTION=1` off).
  - BDA table misses share, recycle and hash their range lists instead of copying them (~3,500 misses every 10 s of a
    ~300 KiB list): **+9 %** (`APS5_NO_LEAN_BDA_TABLES=1` off).
  - The buffer pool's small tiers keep 4x their budget: under `APS5_STAGING_POOL_MIB=512` the small device tier
    (64 MiB) missed and evicted ~5,700 times every 10 s, each a new `VkDeviceMemory` made inside the async compute
    queues' dispatches, under the GPU mutex: **+4.6 %** (`APS5_NO_LARGER_SMALL_POOLS=1` off). The device tier stays at
    1x: 4x there took 1.5–2 GiB from the texture caches' VRAM share and made the sampled cache thrash.
  - Host-imported storage images without 64 KiB block units store without a CPU wait for their writer (their store is
    recorded on the GPU behind it anyway); the flip waited ~110 ms a present for ~20 such images: **+2.1 %, +2.6 %**
    (`APS5_NO_DEFER_IMPORTED_STORES=1` off).
  - Depth-surface lookups skip addresses no surface starts at (`APS5_NO_DEPTH_LOOKUP_FAST=1` off), pending-write
    scans walk the ~5 distinct ranges instead of ~210 (`APS5_NO_DISTINCT_WRITE_SCANS=1` off), refused hull draws are
    skipped from a memo instead of serializing a 54–64 KB request each (`APS5_NO_REFUSAL_MEMO=1` off), and
    `ShaderMemory::collect` merges through a heap (`APS5_NO_COLLECT_HEAP=1` off): **+6.2 %** together (each alone
    +2.6, +2.5, +2.4 and +1.2 %; the last not significant, its output verified identical).
- Performance, 2026-10-24/25 (each measured with an in-run A/B unless noted; zero faults, rendering unchanged in frame
  dumps of both phases):
  - Runtime-format buffer loads (the generic decoder the six giant compute programs call 112–128 times each, which
    RADV inlines) convert components in one straight line and read their element in one span: the giants run up to
    60 % fewer GPU instructions, GPU time per frame 482 -> 431 ms, world 12.0 -> 13.0 presents per 10 s
    (`d8a50e449`, `05b662722`; recompiler, so a new shader cache; `APS5_NO_COMPACT_FORMAT_CONVERT=1`,
    `APS5_NO_SINGLE_FORMAT_READ=1` off).
  - Ring 0x51's single-workgroup programs (0x80001c0300 and family, one CU per dispatch, ~76 ms a frame) overlap as the
    title allows: a dispatch joining a run reads in place what the run already accesses instead of a private staged
    copy, which made the staged-read guard refuse every join (`8106f78f5`): **+2.0 %**, confirmed **+2.6 %**;
    0x80001c0300's GPU time -58 % (`APS5_NO_RUN_IN_PLACE=1` off).
  - Queue 0's dispatches go to the draw committer as items instead of draining it (MT1a); the two switches are runtime
    switches now (`da11e77de`): **+2.4 %** with `APS5_PIPELINE_DISPATCH=1 APS5_PIPELINE_DISPATCH_MISSES=1` (opt-in).
  - BDA programs' dispatch items count their descriptor writes as their writes, as draws do and as the inline path
    notes them, so most cached dispatches no longer drain (`da11e77de`): **+1.7 %** with
    `APS5_DISPATCH_ITEM_BDA_WRITES=1` (opt-in).
  - jemalloc instead of glibc malloc (`LD_PRELOAD=/usr/lib/libjemalloc.so.2`): **+14.5 %** (15.00 vs 13.10 presents per
    10 s, restart legs best179/best181 vs best180). The comparison was against glibc with
    `GLIBC_TUNABLES=glibc.malloc.arena_max=4:glibc.malloc.mmap_threshold=131072`, under which ~10 % of queue 0's worker
    sat in malloc's slow path; against glibc's defaults the gain may be smaller (not measured).
- The second Vulkan queue (`APS5_ASYNC_LANE=51`) runs on RADV's compute family (RADV's graphics family has one queue).
- Texture-cache frame age counts seconds only once presents stand still for 10 s (`APS5_NO_PRESENT_FRAME_AGE=1` off).
- Driver threads are named on Linux (`agc q0x0`, `agc commit`, …) for `top -H` and `perf`.
- Controllers without a touchpad: hold View and move the right stick to swipe the touchpad.
- Diagnostics: `[import-retire]`, `[storage-refresh]`, `[flush-wait]`, `[bad-bda]`, `APS5_BUFFER_VA_HISTORY=1`,
  `APS5_SKIP_SHADER=<address,...>`; under `APS5_PROFILE_DRAW` also `[invocation-for]`, `[depth-surfaces]`, the
  `[bufferpool]` evictions by cause and the `[draw] pipelined` write-scan sample; `APS5_VERIFY_COLLECT_HEAP=1` and
  `APS5_VERIFY_BDA_INCREMENTAL=1` check the new paths against the old ones; `APS5_DUMP_QUEUE=<queue>` with
  `APS5_DUMP_QUEUE_SKIP=<n>` dumps a queue's packets after its first n submissions.

## Run it on Linux

1. Build Mesa 26.2.4 with the patch (RADV only is enough):
   ```
   git apply docs/yotei-linux/mesa-26.2.4-aco-spill-fixes.patch   # in a Mesa 26.2.4 checkout
   meson setup build -Dbuildtype=release -Db_ndebug=true -Dvulkan-drivers=amd -Dgallium-drivers=softpipe -Dllvm=disabled
   ninja -C build
   ```
2. Build AnyPS5 as usual ([build instructions](docs/dev/BUILD.md)).
3. Run the game with the patched driver and this environment:
   ```
   VK_ICD_FILENAMES=<mesa>/build/src/amd/vulkan/radeon_devenv_icd.x86_64.json
   APS5_SPANNED_WAVE=1 APS5_BINDLESS_IMAGES=1 APS5_BINDLESS_LAZY=1 APS5_NO_RESULT_MEMO=1 APS5_STAGING_DIFF=1
   APS5_PIPELINE_INDIRECT=1 APS5_NO_ADDRESS_DRAW_REUSE=1 APS5_ASYNC_LANE=51
   APS5_DEFER_REFRESH_FLUSH=1 APS5_BDA_CONTINUE=800029c200 APS5_NO_PRECISE_SMALL_OVERLAP=1
   APS5_RESOURCE_CACHE_ENTRIES=1024 APS5_STAGING_POOL_MIB=512
   APS5_PIPELINE_DISPATCH=1 APS5_PIPELINE_DISPATCH_MISSES=1 APS5_DISPATCH_ITEM_BDA_WRITES=1
   LD_PRELOAD=/usr/lib/libjemalloc.so.2
   ```
   `APS5_RESOURCE_CACHE_ENTRIES=1024` matters: without it the resource cache's adaptive capacity often grows to 4096 in
   the world load, keeps ~2.3 GiB more staging in VRAM and the world runs at about a third of the speed.
   `APS5_NO_PRECISE_SMALL_OVERLAP=1` keeps the world's colours right (white hair and bleached wood without it).
   jemalloc comes with most distributions (`jemalloc` package); the path may differ. Over a 27-minute run with it the
   process memory in the world stayed flat (about 3 GB resident; the peak, ~23 GB, is during the world load).
4. Optional, +13 % in the world here: shahulmubeen-a's 1080p relinker patch for 02.002.000 (output 3840x2160 ->
   1920x1080 and the dynamic-resolution table pinned to 1080p), from
   [#675](https://github.com/boykopovar/AnyPS5/discussions/675#discussioncomment-19114360).
- Choose **Performance** in the graphics settings; memory is tight with 32 GB, so have swap.
- For menu benchmarks, force the GPU clock: `echo high | sudo tee /sys/class/drm/card1/device/power_dpm_force_performance_level`.

## Known issues

- About 1.6 fps in the open world: per frame (~640 ms) about 330 ms of GPU work; the rest waits on the CPU side. The draw
  committer now records both the draws and queue 0's dispatches and is ~72 % busy; it also waits ~1.1 s per 10 s for the
  GPU mutex behind the compute queues' dispatches. Address-based (BDA) draws, ~27 % of its work, are built on it because
  the preparer thread can't map address tables yet; that is the next step.
- One run crashed at boot on a zero dword in the title's command buffer ("unsupported PM4 packet type"); the cause is
  unknown and it hasn't happened again in the runs since.
- Merged LS+HS hull draws (tessellation) are skipped, so some geometry is missing.
- Shader `0x80001df900` (clustered-lighting compute pass, 747,518 SPIR-V words) used to hang or fault and was skipped;
  with the ACO LDS spill fix it compiles (about 39 s cold) and runs in the world without faults, so it is no longer skipped.

## Credits

Celegans12 (`yotei/best`, the base of this branch), shahulmubeen-a (1080p patch, #9600), mitul72 (#6188), Pipodi,
ee6aba54, pushkarreddyy, teepean, JJenkx (#6721), danielcadev,
and everyone in [discussion #675](https://github.com/boykopovar/AnyPS5/discussions/675).

---

## Base branch: Celegans12's `yotei/best` (Windows)

The notes below are Celegans12's, unchanged, for the Windows / NVIDIA build this branch starts from.

### Where it stands (2026-10-19)

| | |
|---|---|
| **Furthest point** | Open world, the Nupur river by the Ainu village, with HUD and quest |
| **Time to the world** | about 10 minutes from a save near Teshio Ridge (warm shader cache) |
| **Frame rate in the world** | about **0.5 fps** (5.0 presents per 10 s) |
| **Stability** | 25 minutes in the world without a crash on earlier builds of this line |
| **Tested on** | Windows 11, RTX 3080 Laptop GPU 16 GB (driver 610.62), 32 GB RAM, MinGW-w64 GCC 15.2.0, Release |
| **Build** | `8ba98278a` + docs, shader-cache SourceVersion `63a364723a2373f6` |

<table>
<tr>
<td width="50%"><img src="https://raw.githubusercontent.com/Celegans12/AnyPS5/bringup-media/yotei/2026-10-18/open-world-nupur-river.png" alt="The Nupur river in daylight, 636 s after launch"></td>
<td width="50%"><img src="https://raw.githubusercontent.com/Celegans12/AnyPS5/bringup-media/yotei/2026-10-18/open-world-rain.png" alt="The same spot with rain coming in, 1,986 s after launch"></td>
</tr>
<tr>
<td align="center"><sub>Daylight at the river, 636 s after launch</sub></td>
<td align="center"><sub>The weather turns: rain, 33 minutes in</sub></td>
</tr>
<tr>
<td colspan="2"><img src="https://raw.githubusercontent.com/Celegans12/AnyPS5/bringup-media/yotei/2026-10-15/opening-cinematic-on-main.png" alt="The burning tree from the opening cinematic"></td>
</tr>
<tr>
<td colspan="2" align="center"><sub>The opening cinematic, the first world scene that rendered on current main (2026-10-15)</sub></td>
</tr>
</table>

### How the frame rate got here

| Date | Presents per 10 s in the world | What changed |
|---|---|---|
| 10-17 | 2.35 | first time in the open world |
| 10-18 | 2.97 | cheaper draw-cache keys |
| 10-18 | 3.9 | texture revalidation walks only the readable parts of partly committed memory |
| 10-18 | 4.2 | hand-off PRs from upstream, copy-backs of changed dwords only |
| 10-18 | 4.6 | draw preparer thread, device-local buffers for the cloth compute programs |
| 10-19 | **5.0** | the cloth compute ring on its own Vulkan queue |

### Run it

Build as usual ([build instructions](docs/dev/BUILD.md)), then run the game with:

```
APS5_SPANNED_WAVE=1 APS5_BINDLESS_IMAGES=1 APS5_BINDLESS_LAZY=1 APS5_NO_RESULT_MEMO=1
APS5_STAGING_DIFF=1 APS5_PIPELINE_INDIRECT=1 APS5_NO_ADDRESS_DRAW_REUSE=1
APS5_NO_PIPELINE_JOURNAL=1 APS5_RESOURCE_CACHE_ENTRIES=1024
```

- `APS5_ASYNC_LANE=51` records the cloth compute ring (queue 0x51) on a second Vulkan queue.
- The first run on a new shader-cache version compiles shaders cold; it can take 30-60 minutes to reach the world.
- Leave `APS5_PIPELINE_REQUESTS` unset when you measure: it costs about 12 % of a frame here.

### Known issues

- **GPU hangs with `APS5_ASYNC_LANE=51`.** With the second queue on, every world run with dispatch overlap hung or hit a GPU write fault within 5-14 minutes: a queue waits 40 s for the GPU, then the device is lost. That held on this build and on the previous one. Three fixes for real cross-queue ordering bugs (each with a test that fails without it) didn't stop it, and neither did serializing the two queues on the GPU. Leave the second queue off until this is found.
- With the second queue on and dispatch overlap off, we saw one GPU write fault in about 80 minutes of world time.
- A GPU hang can reset NVIDIA's shader store for the game. After that, the saved pipelines no longer load and every run compiles again (long stalls on the last loading screen). If that happens after a hang, expect one slow run.
- Black shapes on some rocks and terrain (the depth pre-pass and the G-buffer pass disagree on depth). Cause not found yet.
- Process memory is about 30 GB early in the world, so 32 GB of RAM is tight.
- The overlap fixes keep conflicting dispatches out of overlapped runs, which costs GPU time (about +14 % per frame on the previous build). Frames are CPU-bound, so presents barely move.
- One compute program (0x8000361f00) is still refused by the recompiler at registration (non-fatal so far).
- About 0.5 fps is far from playable. What limits a frame now, and what's next, is in [discussion #675](https://github.com/boykopovar/AnyPS5/discussions/675).

### What it carries on top of `main` 2026-10-15

- Upstream PRs adopted before they merged, among them #4943, #7183, #4288, #3557, #6771, #7044, #6617, #6566, #1896,
  #6763, #6632, #6631, #6501, #7134, #6187, #6192, #6636, #7212 and #7218.
- Local work:
  - image tables resolved on the GPU through a bindless descriptor heap, including T#s loaded from run-time addresses;
  - a cheaper draw-miss path (resolve memo, faster table snapshots);
  - texture revalidation that walks only the readable parts of partly committed memory;
  - the HTILE mask-clear kernel's buffer-descriptor form recognized;
  - a draw preparer thread with a two-phase draw build;
  - device-local buffers for address-based compute builds (the cloth programs);
  - copy-backs that store only the changed dwords;
  - dispatch overlap;
  - the async compute queue.

### Updates

- 2026-10-19: branch created at `8ba98278a` (async compute queue), 5.0 presents per 10 s in the world.

---

# About AnyPS5

Tool for automatic executables porting to Linux and Windows.

The relinker-only build runs on macOS, including Apple silicon.

[**Web converter**](https://boykopovar.github.io/AnyPS5/) - files stay on your device.

![anyps5_collage1](https://github.com/user-attachments/assets/ed2f0b2d-33d3-46d7-ab88-767ca872fc99)

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

[**Discord**](https://discord.gg/BHFztBPUe), [Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Architecture](docs/dev/ARCHITECTURE.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Status

[![libraries](https://boykopovar.github.io/AnyPS5/badge-libraries.svg)](https://boykopovar.github.io/AnyPS5/progress.html) [![shaders](https://boykopovar.github.io/AnyPS5/badge-shaders.svg)](https://boykopovar.github.io/AnyPS5/progress.html)

[![progress map](https://boykopovar.github.io/AnyPS5/progress.svg)](https://boykopovar.github.io/AnyPS5/progress.html)

[Progress history](https://boykopovar.github.io/AnyPS5/history.html)

<sub>* System libraries: percentage of the functions known to the project so far (declared in [core/libs/prx](core/libs/prx)), not of every PS5 system function. The total grows as more functions are declared.</sub>

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

## Compatibility

Per-game progress (stage reached, open blockers, libraries and GPU coverage) is tracked on the [blocker map](https://alxcbtw.github.io/anyps5-roadmap/), a community project built from public GitHub data ([source](https://github.com/AlxcBTW/anyps5-roadmap)). Test reports go in the [Game bring-up](https://github.com/boykopovar/AnyPS5/discussions/categories/game-bring-up) discussions, one thread per title.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
