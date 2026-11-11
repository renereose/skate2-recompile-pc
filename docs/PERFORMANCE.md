# Performance notes

This page records where frame time goes in Skate 2 PC, the runtime changes that address it, and how to measure a change. The figures were collected on one Windows PC with an NVIDIA GeForce RTX 4080 using the frame-time CSV (`--perf_log_csv`), Windows Performance Recorder CPU sampling with stacks, and RenderDoc frame captures. Other hardware has not been measured.

## Where the time goes

Skate 2's dense city views issue 5,000–9,000 draws per frame. Each guest draw is translated by the GPU command processor thread into Direct3D 12 commands, and that thread limits the frame rate in these views. The game's main thread spends most of its time in a spin-wait for the command processor rather than in game logic.

| Cost | Cause | Change |
| --- | --- | --- |
| ROV render target path | ROV performs blending and depth/stencil in pixel shaders; slow at 2× on the tested NVIDIA GPU | RTV profile, with the draw-extent fix below |
| CPU then GPU, not overlapped | A frame was submitted to the GPU only when complete, and the idle-clean synchronization then waited for all of it | `d3d12_submit_draw_interval`; `d3d12_present_idleclean_sync_lag`, see [Idle-clean wait overlap](#idle-clean-wait-overlap) |
| Driver command-list replay on the command processor thread | `DeferredCommandList::Execute` plus `ExecuteCommandLists`, about 22% of the thread | `d3d12_async_submission` |
| Per-register and per-texture overhead | Register-table lookup on every register write, guest texture layout recomputed on every scaled-resolve check, locks and atomic counters in per-draw paths | `command-processor-overhead` patch |
| New-texture hitches | Every texture was a separate committed resource; areas loading 400–600 new textures in one frame stalled for 150–190 ms | `d3d12_texture_placed_resources` |
| Occlusion query stalls | Each query end submitted and waited for the GPU before writing the result | `d3d12_async_occlusion_queries` |
| Host presents at monitor refresh rate | The always-registered achievement toast requested a continuous UI repaint, so the presenter presented repeated frames at the monitor refresh rate | `idle-ui-repaint` patch |
| Timer thread polling | The runtime's timer thread, which updates the guest's millisecond timestamp, polled between ticks with thread yields and 1 ms sleeps: about 7% of one core, measured on the front end | `timer-queue-blocking-wait` patch: waits on a condition variable until the next tick, about 0.5% of one core |
| Per-draw `state` and `tex` work repeated with the same inputs | Every draw re-derived its sampler parameters and system constants and re-checked texture descriptors and states; every texture rebinding decoded the fetch constant and looked the texture up again; small helpers were DLL imports or virtual calls | `draw-state-caches` patch, see [Draw state caches](#draw-state-caches) |
| Per-draw `vb`, `prim` and `rt` work | Every vertex buffer, index buffer and texture memory request entered the global critical region, which guest threads also hold, even when the memory was already uploaded; every draw rebuilt its render target keys, looked the render targets up and walked the EDRAM ownership map, although consecutive draws mostly use the same render targets | `buffer-and-render-target-fast-paths` patch, see [Buffer and render target fast paths](#buffer-and-render-target-fast-paths) |
| Guest memory uploaded again every frame | With `clear_memory_page_state` on, every frame end marks guest memory as changed, so data the CPU had not touched was uploaded and processed again: about 15 MB per frame in Free Skate, with GPU work to match | The game application defaults `clear_memory_page_state` to false, see [Memory page state refresh](#memory-page-state-refresh) |
| Render target transfers before full-screen clears | On the RTV path, a draw that takes over EDRAM from another host render target first gets the old contents copied in, even when the draw replaces all of them, as a full-screen clear does. Transfers were 34% of the Steam Deck's GPU time in Free Skate (10.3 of 30.5 ms, 35 per frame) and 40% on the test PC's title screen at 2× | `gpu_render_target_transfer_skip_overwritten`, see [Render target transfer skipping](#render-target-transfer-skipping) |

## RTV black screen while bailing

With `render_target_path_d3d12=rtv`, the bail camera displayed a black screen. A RenderDoc capture of the bail frame showed the final 1280×720 resolve reading the depth render target at EDRAM tile 720 instead of the color target at tile 0. Unclipped full-screen passes used in the bail effect had their EDRAM extent estimated as the whole EDRAM. The depth target therefore wrapped around and took ownership of the color target's range before the resolve.

The draw-extent estimate executes the vertex shader of such draws on the CPU only when `execute_unclipped_draw_vs_on_cpu` is enabled for the host render target path. ReXGlue defaults it to false, while the Xenia source it is derived from documents true. `rexglue-0.10.0-unclipped-draw-extent.patch` restores true. The ROV path is unaffected because it uses `execute_unclipped_draw_vs_on_cpu_for_psi_render_backend`.

## Runtime settings

| Setting | Default | Effect |
| --- | --- | --- |
| `execute_unclipped_draw_vs_on_cpu` | true (was false) | Correct EDRAM extents for unclipped draws on the RTV path |
| `d3d12_submit_draw_interval` | 0 | Submit recorded work every N draws within a frame so the GPU starts while the frame is still being translated; 1024 measured best |
| `d3d12_async_submission` | false | Replay and execute submissions on a separate thread; frame-ending submissions still complete before the presenter is notified |
| `d3d12_texture_placed_resources` | true | Place textures up to 16 MB in shared 64 MB heaps; textures are only destroyed after their last GPU use, so ranges are reused immediately |
| `d3d12_async_occlusion_queries` | true | Leave the guest's pending markers in place and write query results when the GPU has completed them; pending results are delivered before the command processor waits for the guest |
| `gpu_texture_binding_cache` | true | Remember texture bindings by fetch constant; see [Draw state caches](#draw-state-caches) |
| `d3d12_draw_state_cache` | true | Skip per-draw sampler, system constant, descriptor and texture state work whose inputs are unchanged |
| `gpu_shared_memory_valid_range_check` | true | Answer requests for memory that is already uploaded without the global lock; see [Buffer and render target fast paths](#buffer-and-render-target-fast-paths) |
| `gpu_render_target_update_cache` | true | Skip the render target update for a draw that uses the same render targets as the previous one while their EDRAM ownership is unchanged |
| `gpu_render_target_transfer_skip_overwritten` | true | Don't copy the part of a render target transfer that the draw needing it replaces at every sample; see [Render target transfer skipping](#render-target-transfer-skipping) |
| `gpu_render_target_transfer_skip_overwritten_validate` | false | Debug: fill the skipped areas with magenta and depth 0 before the draw, so areas the draw doesn't replace show up |
| `gpu_render_target_transfer_log` | 0 (off) | Every this many frames, log transfers per frame and one frame's transfers in detail |
| `d3d12_present_idleclean_sync_lag` | 1 (Skate 2 profile; SDK default 0) | How many recently closed frames may still run on the GPU when the idle-clean wait returns; see [Idle-clean wait overlap](#idle-clean-wait-overlap) |
| `d3d12_present_idleclean_log` | 0 (off) | Every this many frames, log the idle-clean waits: how many blocked, time waited, and frames still on the GPU |
| `gpu_tiled_scene_single_pass` | true (Skate 2 profile; SDK default false) | Draw a tiled scene's first strip to the full screen height and skip the scene's replays for the other strips; see [Single-pass tiled scenes](#single-pass-tiled-scenes) |
| `gpu_tiled_scene_log` | 0 (off) | Every this many frames, log tiled scenes, extended, skipped and kept draws, shifted resolves and fallback reasons per frame |
| `gpu_draw_state_cache_validate` | false | Debug: compute everything the caches and fast paths above skip and log mismatches |
| `clear_memory_page_state` | false (ReXGlue: true) | Refresh the page-valid state of GPU-written memory at every frame end. The game application sets the default to false; see [Memory page state refresh](#memory-page-state-refresh) |

The game-side frame limiter, `skate2_frame_limit` (`off`, `auto` or a rate from 20 to 1000), paces each guest swap on an absolute schedule: deadlines advance by exactly one frame interval, the thread sleeps on a high-resolution waitable timer and spins for the final millisecond, and a frame more than one interval late restarts the schedule instead of rushing to catch up. `auto` targets the refresh rate of the window's monitor minus 3 frames or 3%, whichever is larger. See [Display and performance](DISPLAY.md#frame-limiter).

On NVIDIA GPUs, `skate2_nvidia_max_performance=on` sets the driver's Power management mode to Prefer maximum performance for `skate2.exe`, which can help because the CPU-side command processor limits the frame rate in dense views, leaving the GPU partly idle so the driver may drop it to lower clocks; see [NVIDIA power management](DISPLAY.md#nvidia-power-management).

`skate2_draw_distance_scale` and `skate2_lod_distance_scale` (0.5 to 8, default 1) push out the distance at which world objects stop being drawn and the distances at which characters and vehicles switch to lower-detail models. Values above 1 cost CPU and GPU time: a longer draw distance adds draws, which the command processor thread must translate in the dense views that already limit the frame rate, and detailed models add GPU work. Other passes, such as shadows, probably draw more too. The cost has not been measured; see [Draw distance and detail](DISPLAY.md#draw-distance-and-detail).

`Launch Skate 2 - RTV Performance.cmd` selects RTV, 2×, unlimited frame rate, a submit interval of 1024 and the submission thread. The default launchers pass no render path: they use the one saved from the settings panel or F4, or ROV when none is saved. The RTV profile requires a runtime built with these patches; older runtimes reject the new settings.

## Measurements

Average frame rate of frames grouped by draw calls per frame, RTV unless stated, 2× rendering. Each row is a separate play session through dense areas, so the routes differ; compare the draw-call groups rather than overall averages.

| Configuration | 5k draws | 7k draws | 99th percentile frame time |
| --- | --- | --- | --- |
| ROV, 60 FPS mode | 33 FPS | 22 FPS | 51 ms |
| RTV with draw-extent fix, 60 FPS mode | 54 FPS | 30 FPS | — |
| RTV, unlimited | 70 FPS | 51 FPS | 21 ms |
| + command processor overhead patch¹ | 81 FPS | 58 FPS | — |
| + submit interval 1024 | 91 FPS | 69 FPS | 15.6 ms |
| + submission thread | 99 FPS | 78 FPS | 13.3 ms |

¹ Measured during a CPU profiling session and grouped by one-second windows rather than by individual frames.

Command processor CPU time per draw fell from about 2.5–2.7 µs to about 1.9 µs with the overhead patch. In 60 FPS mode, frames that miss the 16.7 ms guest refresh wait for the next one, so moderate overruns appear as a drop to 30 FPS; unlimited mode does not quantize frame times this way.

New-texture bursts with placed textures:

| Textures created in one frame | Committed resources | Placed resources |
| --- | --- | --- |
| ~430 | 155 ms | 15–21 ms |
| ~600 | 189 ms | 44 ms |

Submit intervals of 256, 512 and 1024 were compared; 1024 was fastest and showed no visual problems. Disabling idle-clean synchronization entirely is faster still but produced visible corruption, so it remains enabled; the Skate 2 profile narrows it, see [Idle-clean wait overlap](#idle-clean-wait-overlap).

## Not yet verified

- Lens-flare visibility with asynchronous occlusion queries has not been checked against the original behavior.
- The frame limiter's effect on frame-time consistency has not been measured in gameplay yet.
- The effect of `skate2_nvidia_max_performance` on frame rate and frame times has not been measured.
- Draw-distance and LOD-distance scaling have only been checked on the front end. Their in-game visuals and performance cost have not been checked.
- The GPU trace recorder has not made an in-game capture yet, and no trace has been replayed or opened in Xenia's tools.
- The settings panel's controller shortcut and controller navigation, the Steam Deck preset on a Deck, and saving from the panel have not been tested.
- One isolated 159 ms frame with few new textures remains unexplained.
- The frame profile has only run on the test PC, on the front end. It has not run under Proton (where vkd3d-proton implements the timestamp queries) or in gameplay.
- The timer-thread change was measured on Windows only; its effect under Proton has not been measured.
- The draw-translation probe has only run on the test PC, on the front end. Gameplay, where occlusion queries get the fixed sample count, and the Steam Deck have not been measured; the controller has no button for the probe.
- The draw state caches were validated and measured on the front end only (RTV 1× and 2×, ROV 2×). Gameplay, and the Steam Deck under Proton, have not been measured or validated with `gpu_draw_state_cache_validate`.
- The buffer and render target fast paths were validated and measured on the front end only (RTV 1× and 2×, ROV 2×). Gameplay, the Steam Deck under Proton, the Vulkan backend (which shares the render target update) and the bindful descriptor path have not been measured or validated.
- `gpu_tiled_scene_single_pass` was checked in one Free Skate session each on the test PC and the Steam Deck, with no visual difference seen against strip-by-strip drawing. Other game modes, longer play, the bail camera, replays and menus over gameplay have not been checked separately.
- `clear_memory_page_state=false` was checked in one Free Skate session on the test PC, with no visual difference seen. Other game modes, longer play, the bail camera, replays and the Steam Deck have not been checked.
- The GPU pass profile has only run on the test PC, on the title screen's attract scene. It has not run under Proton, where vkd3d-proton implements the timestamp queries, or in gameplay. Whether gameplay scenes are also drawn in three tiles has not been checked.
- Render target transfer skipping was checked in Free Skate on the test PC and the Steam Deck, with no visual difference seen, but not with `gpu_render_target_transfer_skip_overwritten_validate`.
- `d3d12_present_idleclean_sync_lag=1` was checked in gameplay and menus on the test PC, with no colored stripes or black surfaces seen. The visual check on the Steam Deck is not finished. Other game modes, longer play, the bail camera and replays have not been checked separately.
- AMD and Intel GPUs, other NVIDIA GPUs, and lower-core-count CPUs have not been measured. ROV remains the default for this reason.

## GPU trace capture

The runtime can record the GPU commands of one or more frames, together with the guest memory they use, to a trace file for offline analysis.

Press **F9** in game to record the next frame. The key can be changed as `bind_gpu_trace` in the **Input/Keybinds/System** section of the F4 settings. The trace is written to `logs\gpu_traces\<TITLEID>_<YYYYMMDD-HHMMSS>_frame<N>.xtr` beside `skate2.exe`, where `<N>` is the number of the first recorded frame since startup. The log shows `GPU trace: capture requested`, then `GPU trace: recording 1 frame(s) to <path>` and, once the capture is complete, `GPU trace: wrote <frames> frame(s), <size> MB, to <path>`.

The settings are in the **GPU/Trace** section of the F4 settings, or can be given as `--name=value` options on the `skate2.exe` command line; the launchers do not pass them.

| Setting | Default | Effect |
| --- | --- | --- |
| `trace_gpu_frame_count` | 1 | Frames recorded per capture, from 1 to 1000 |
| `trace_gpu_capture_at_frame` | 0 (off) | Start a capture automatically after this many guest frames since startup. If saved to the config, it triggers on every launch |
| `trace_gpu_stream` | false | Record everything from startup until exit into one `..._stream.xtr` file; takes effect after a restart |
| `trace_gpu_prefix` | empty | Output folder; empty uses `logs\gpu_traces` beside the executable |

- The recorded frames run slowly, and the game stalls briefly when a capture starts.
- Files are large: about 10 MB of EDRAM contents, plus the memory written by the GPU and all data read during the recorded frames.
- For complete traces, use render scale 1. With resolution scaling the EDRAM snapshot is skipped, and render-to-texture data in the trace may be stale.
- Complete traces require the Direct3D 12 backend.
- The files use upstream Xenia's trace format, version 1, so Xenia's trace viewer and trace dump tools built from Xenia source should open them. This has not been tested.
- No replay tool is included.

## Frame profile

Two settings log where each frame's time goes, averaged over 120 guest frames, to the normal log (`logs\skate2_NNN.log` beside `skate2.exe`). Both are off by default and apply live. They can be given as `--name=value` options on the `skate2.exe` command line, which also works in the Steam launch options on a Steam Deck; the launchers do not pass them.

| Setting | Measures |
| --- | --- |
| `gpu_cpu_profile` | CPU time of the GPU command thread per frame: waiting for the game, each stage of draw translation, resolves, submission and waits for the GPU, plus the submission thread |
| `d3d12_gpu_timestamp_buckets` | GPU execution time per frame, with Direct3D 12 timestamp queries at the start and end of every submission and around resolves and the frame-end pass |
| `d3d12_gpu_pass_profile` | GPU execution time per render pass and per kind of work; see [GPU pass profile](#gpu-pass-profile) |

For a diagnosis, use `skate2_frame_rate=unlimited` and no frame limiter, so that a frame cap does not show up as waiting. Example from the front end on the test PC (1×, RTV, unlimited, submit interval 1024, submission thread), with both settings on:

```
GPU profile (avg per frame over 120 frames, us): frame=8073 fps=123.9 limit=command_thread | command thread: busy=7336 work=6292 idle=737 guest_wait=0 fence_wait=0 (0.0x) present_sync=750 submit_thread_wait=294
GPU profile stages (avg per frame, us): draws=5011x draw=4782 [begin=61 prim=359 rt=856 pipeline=482 tex=813 state=1311 vb=527 record=278 memexport=0 submit=6 other=89] copies=40.0x copy=121 swap=330 | submissions=11.0x submit=320 [record=23] | upload=8172KiB | submission thread: replay=1591 execute=362
GPU profile timestamps (avg per frame over 120 frames, us): busy=3454 span=7276 idle_in_span=3822 draws_and_other=3131 resolve=317 (40.0x) swap=7 submissions=11.0x utilization=43% of frame=8073 skipped_submissions=0
```

All times are microseconds per frame; `(2.0x)` and `5011x` are occurrences per frame.

- **First line.** `frame` is the time between guest swaps. The command thread is either `busy` executing guest commands or `idle` waiting for the game to write more, so the two add up to about `frame`. `work` is `busy` minus the waits listed after it: `guest_wait` (the guest's own wait-for-memory commands), `fence_wait` (waiting for the GPU to finish submitted work), `present_sync` (idle-clean synchronization, `d3d12_present_idleclean_sync`) and `submit_thread_wait` (frame end waiting for the submission thread).
- **Stages.** `draws` counts draw commands, including ones that turn out to draw nothing. Their time is split into consecutive stages:
  - `begin`: opening a submission, including waiting for frames still in flight;
  - `prim`: index processing;
  - `rt`: render target cache, including EDRAM ownership transfers;
  - `pipeline`: shader analysis, translation lookup and pipeline setup;
  - `tex`: texture requests and loading;
  - `state`: viewport, constants and descriptors;
  - `vb`: vertex and memexport buffers, including uploading changed guest memory;
  - `record`: recording the draw;
  - `memexport`: memexport invalidation and readback;
  - `submit`: mid-frame submissions;
  - `other`: time after the last stage, for example draws that end early.

  `copy` covers EDRAM resolves and `swap` the frame end, including the frame's last submission. `submit` covers every submission on the command thread; `record` is the replay and `ExecuteCommandLists` without the submission thread, or the handover to it with the thread. `upload` is the guest memory copied to the GPU per frame. `submission thread` shows the replay and execution done by `d3d12_async_submission`.
- **Timestamps.** `busy` is the sum of the command lists' execution times and `span` the time from the first one's start to the last one's end, so `idle_in_span` is time the GPU waited for work within the frame. `resolve` and `swap` (gamma and FXAA) are timed separately; `draws_and_other` is the rest of `busy`. `utilization` is `busy` as a share of the frame time measured on the CPU. The presenter's own work is not included.
- **`limit`** is a first hint, checked in this order:
  - `gpu`: GPU `busy` is at least 85% of the frame (needs timestamps);
  - `submission_thread`: the submission thread is busy for at least 85% of the frame, or the frame end waits for it for at least 20%;
  - `gpu_wait`: waits for the GPU are at least 20% of the frame; turn on timestamps to confirm;
  - `command_thread`: `work` is at least 75% of the frame, or `command_thread_submission` when the synchronous replay is at least 30% of that work;
  - `guest`: the command thread waits for the game for at least 25% of the frame, meaning the game's own threads, a frame cap or VSync;
  - `mixed` otherwise.

In the example, the command thread is busy for 91% of the frame while the GPU executes for 43% of it, so translation on the command thread limits the frame rate. The largest stages are `state`, `rt` and `tex`.

`gpu_cpu_profile` takes about a dozen CPU timestamps per draw while it is on. When both settings are off, each draw only checks a flag. The timestamp queries use one block of 64 queries per submission, and `skipped_submissions` counts submissions that found no free block.

For GPU time per render pass, see [GPU pass profile](#gpu-pass-profile).

## GPU pass profile

`d3d12_gpu_pass_profile` (`rexglue-0.10.0-gpu-pass-profile.patch`, off by default, applies live) splits the GPU's execution time into render passes and kinds of work. It writes a timestamp query wherever the kind of recorded work changes: when draws start writing different render targets, and around resolves, render target transfers, texture loads, guest memory uploads, memexport readbacks and the frame end. The time between two timestamps belongs to the work recorded between them, so the parts add up to the GPU's busy time. It logs to the normal log every 120 guest frames and can be combined with the [frame profile](#frame-profile) settings. Use `skate2_frame_rate=unlimited` and no frame limiter, as for the frame profile.

Example from the title screen's attract scene on the test PC (2×, RTV, unlimited, submit interval 1024, submission thread), with 3 of the 10 `top` lines:

```
GPU pass profile (avg per frame over 120 frames, us): busy=4458 span=6521 | draws=1210 [full=815 (22.0 passes, 3291 draws) half=190 (7.0 passes, 7 draws) small=108 (10.0 passes, 10 draws) depth_only=96 (37.0 passes, 1713 draws)] | resolve=884 [dump=0 (0.0x) copy=853 (65.0x) clear=32 (7.0x)] transfer=1795 (38.0x) texture_load=447 (28.0x) upload=89 (8.3x) memexport=0 (0.0x) swap=31 (1.0x) other=2 (8.0x) | submissions=11.0x timestamps=276x skipped_submissions=0 unsplit_marks=0
GPU pass profile draw cost (avg per frame): passes with 1 draws: 47.0 passes 47 draws 635 us (13.51 us/draw), 2-9 draws: 11.0 passes 47 draws 117 us (2.49 us/draw), 10-99 draws: 12.0 passes 331 draws 14 us (0.04 us/draw), 100-999 draws: 3.0 passes 1344 draws 47 us (0.03 us/draw), 1000+ draws: 3.0 passes 3252 draws 397 us (0.12 us/draw) | fit over passes: 9.4 us per pass + 0.10 us per draw (r2=0.53)
GPU pass profile top 1: 220 us (4.9% of busy) = draws 0 + transfers before them 220, 1.0 draws (0.20 us/draw), depth_only, pitch 1200 1xMSAA, drawn to 1200x8192, depth kD24S8@1024, pass 3 with these targets, in 100% of frames
GPU pass profile top 2: 218 us (4.9% of busy) = draws 1 + transfers before them 218, 1.0 draws (0.51 us/draw), depth_only, pitch 1200 1xMSAA, drawn to 1200x8192, depth kD24S8@1024, pass 2 with these targets, in 100% of frames
GPU pass profile top 6: 149 us (3.3% of busy) = draws 149 + transfers before them 0, 1084.0 draws (0.14 us/draw), full, pitch 1160 4xMSAA, drawn to 1152x256, rt0 k_8_8_8_8@0 depth kD24S8@1024, pass 5 with these targets, in 100% of frames
```

All times are microseconds per frame; `(37.0x)` counts the timed segments of that kind of work per frame, where consecutive operations of the same kind share one segment.

- **Passes.** A pass is a run of consecutive draws writing the same render targets, as the guest registers describe them: surface pitch and MSAA, the format and EDRAM base of each color target the draws write, and the depth buffer if they use it. A resolve ends a pass. Passes are classed by their surface pitch compared with the game's output width at the latest swap: `full` (at least 75%), `half` (at least 37.5%), `small`, and `depth_only` for passes that write no color target, such as shadow maps and depth prepasses, and draws that write nothing at all.
- **First line.** `busy` and `span` are as in the frame profile's timestamp line. `draws` is the GPU time of all guest draws, then per class the time, passes and draws per frame. `resolve` is split into `dump` (RTV path: host render targets copied into the EDRAM buffer before resolving; 0 while resolves go directly from the render targets), `copy` (the resolve's copy to guest memory) and `clear`. `transfer` is render target ownership transfers on the RTV path: when draws need EDRAM contents that another host render target holds, for example one with a different format, MSAA or pitch over the same EDRAM range, the contents are copied before the draws. `texture_load` is loading and untiling textures from guest memory, `upload` copying guest memory to the GPU, `memexport` memexport readbacks, `swap` the frame end (gamma, FXAA and the output copy) and `other` unmarked work such as submission setup. `timestamps` counts queries per frame. `skipped_submissions` counts submissions not measured because all query blocks were in flight, and `unsplit_marks` boundaries that were not timed because a submission had used all of its 1024 timestamps, in which case the open segment also gets the work after them.
- **Draw cost line.** Passes grouped by their number of draws, with time per draw in each group, and a least-squares fit of pass time against draw count over all passes of the window. Passes with one draw are usually full-screen effects, whose time is pixel work. Passes with hundreds of draws show the cost per draw: state changes, vertex work and small triangles. A large `us per draw`, or most of the draw time in the groups with many draws, points at the number of draws; most of it in the one-draw group points at full-screen effects and resolution.
- **Top lines.** The 10 passes with the most GPU time, counting their draws and the transfers their draws needed. A pass is recognized across frames by its render targets and by how many passes with the same render targets came before it in the frame (`pass 5 with these targets`). `drawn to` is the furthest corner of the draws' scissor rectangles in guest pixels, where 8192 means an unbounded scissor. Render targets are listed as format`@`EDRAM base tile; `(read only)` means the depth buffer is tested but not written, and `none` means draws with no render target.

Timestamps record when the preceding GPU work has finished, so work in neighboring segments can overlap a little and very short segments are approximate. They are written only where the render targets or the kind of work change, where a render pass ends anyway, also under vkd3d-proton on the Steam Deck, so the profile changes the work it measures little. With `d3d12_gpu_timestamp_buckets` also on, that line's `busy` is 1–2% higher, because it starts timing slightly earlier in each submission. When the setting is off, each draw tests one member (one compare and branch in the compiled draw path), and resolves, transfers, texture loads and uploads test it once each; the queries, 1024 per submission in 96 blocks, are only created when it is first switched on. While it is on, the front end at 1× ran at 5.57–5.74 ms per frame against 5.57–5.60 ms with it off (3 alternating runs each), about 2% slower on average, from the CPU work at each draw.

On the test PC, the title screen's GPU time at 2× is 40% render target transfers (1.8 of 4.5 ms), 27% draws, 20% resolves and 10% texture loads; at 1×, 28% transfers (0.53 of 1.87 ms) and 35% draws. The most expensive transfers come before depth-only passes: three per frame into a 1× depth buffer at pitch 1200 at EDRAM base 1024, the base of the main scene's 4× MSAA depth buffer, about 0.2 ms each at 2×, and one into a 4× MSAA depth buffer at pitch 640. The main scene is drawn at pitch 1160 with 4× MSAA in three passes with the same render targets, 256, 256 and 128 rows high, each with the same about 1,090 draws. This looks like the Xbox 360's tiled rendering, in which the game submits the scene once per EDRAM-sized tile, so most of these draws are translated and drawn three times.

## Draw-translation ceiling probe

A faster renderer, such as a native renderer like Skate 3's or a cache of translated draws, can only remove the cost of turning the game's draws into Direct3D 12 work. The probe measures how fast the game runs when that cost is gone, before any such work is started. It breaks the picture on purpose: while it drops draws, the screen keeps showing the last image, and the game keeps running underneath.

Press **F10** in game (the `bind_skate2_draw_probe` key in the **Input/Keybinds/System** section of the F4 settings). Each press moves to the next step:

| Step | Readout | What is skipped |
| --- | --- | --- |
| 1 | fullscreen passes dropped | Draws and resolves into surfaces at least `d3d12_debug_drop_draws_min_pitch` (1024) pixels wide: Skate 2's main scene is drawn 1160 pixels wide, and a few screen-sized passes 1040 to 1280 |
| 2 | all draws dropped | Every draw and resolve |
| 3 | all draws dropped, game's world dispatch skipped | Also the game's own dispatch of its sorted world draw list (`skate2_skip_draw_dispatch`), so the game builds no draw commands for it |
| 4 | normal rendering | Nothing; the results stay on screen |
| 5 | (hidden) | Nothing |

A readout in the top-left corner shows the step, the current frame rate and a table with the average frame time of each step over its last 10 seconds, ignoring the first 2 seconds after a change. "vs off" compares a step with normal rendering, measured over the 10 seconds before the first press, or in step 4. The log records the same results, as `Skate 2 draw probe result` lines.

Dropped draws still have their shaders analyzed, and PM4 processing, memory-export draws, fences and queries still run. Occlusion queries report a fixed sample count, `query_occlusion_fake_sample_count`, so the game keeps drawing what it would test for visibility. The settings can also be given on the command line (`--d3d12_debug_drop_draws=off|fullscreen|all`, `--skate2_skip_draw_dispatch=true`), but they are never kept between runs: a value saved in `skate2.toml` is reset at startup with a warning. The probe needs the Direct3D 12 backend; under Proton it is the same D3D12 code.

**How to read the results**, with `skate2_frame_rate=unlimited`, no frame limiter and the same spot for every step:

- The `all` step gives the ceiling: the frame time left with no draw translation at all. The difference from `off` is what any faster renderer could save at most; a real one adds its own cost back.
- `all + skip dispatch` also removes the game's own work for building the world's draw commands. If it is much faster than `all`, a renderer must also keep the game from building those commands, as Skate 3's does for hidden objects.
- `fullscreen` shows how much of the cost is in the screen-sized passes, which a native renderer would replace first.

**Decision thresholds** for starting a native renderer, as frame-time change in the "vs off" column:

- **Go:** `all` cuts the frame time by at least 40% (-40% or lower; for example 25 ms to 15 ms).
- **Stop:** a cut of 20% or less (-20% to 0%). The frame is then limited by the game's own code or by the GPU, and months of renderer work would not pay off.
- In between: compare the frame profile (`gpu_cpu_profile`) of `off` and `all` to see where the remaining time goes before deciding.

When the probe is off, each draw tests one byte and each occlusion query two.

While a mode is active, the log also lists every 5 seconds how many draws and resolves were dropped and executed per frame, by surface width, which shows what `fullscreen` covers.

**Results on the test PC** (RTX 4080, front-end attract scenes, unlimited, no limiter, RTV, submit interval 1024 or 512, submission thread; about 5,000 draws per frame), with both frame profile settings on:

| Step | 1×, frame time | 2× (saved settings), frame time | Command thread busy | GPU busy |
| --- | --- | --- | --- | --- |
| off | 8.04 ms | 9.51 ms | 91% / 93% | 43% / 64% |
| fullscreen | 2.57 ms (-68%) | 2.73 ms (-71%) | 70% / 77% | 33% / 59% |
| all | 2.49 ms (-69%) | 2.45 ms (-74%) | 51% / 50% | 0% |
| all + skip dispatch | 1.26 ms (-84%) | 1.25 ms (-87%) | 35% / 34% | 0% |
| off again | 8.05 ms | 9.49 ms | | |

Without the frame profile, 1×: off 7.75 ms, all 2.44 ms, all + skip dispatch 1.25 ms.

On this PC, removing draw translation would cut the frame time by about 70%, well past the go threshold. With all draws dropped, the frame is limited by the game's own threads (`limit=guest`). Skipping the game's world dispatch (about 580 list entries, 3,300 of the 5,000 draws per frame) halves what is left, so building and parsing those draw commands is about half of the remaining frame time. Gameplay in the city, and the Steam Deck, have not been measured with the probe.

## Draw state caches

`rexglue-0.10.0-draw-state-caches.patch` makes the frame profile's `state` and `tex` stages cheaper. A sampling profile of the command thread and per-frame counters on the front-end attract scene (about 5,000 draws per frame) showed that most of that work was repeated with the same inputs:

- About 20,000 sampler parameter derivations per frame, four per draw, although the textures change in only about 1,700 draws.
- System constants recomputed in every draw, although they changed in fewer than 100 draws per frame.
- About 5,600 texture rebindings per frame. Each one decoded the fetch constant, looked the texture up by its key (with resolution scaling, also checked whether its memory holds scaled resolve data), and looked up its descriptor, mostly for textures bound many times before.
- The texture descriptor and state checks repeated in every draw.
- Small helpers called in per-draw loops through DLL imports (`rex::bit_scan_forward`, `lzcnt`, `tzcnt` from the runtime) or virtual calls (the Direct3D 12 provider and the render target path).

Changes, all in effect by default:

| Setting | Change |
| --- | --- |
| `gpu_texture_binding_cache` | A table of 2,048 entries, indexed by the raw texture fetch constant, remembers the decoded binding and the texture objects. Entries become invalid when any texture is destroyed or the scaled resolve state of any memory page changes. Loading of changed texture data is still checked on every rebinding. About 91% of rebindings hit the table on the attract scene. |
| `d3d12_draw_state_cache` | Sampler parameters are recomputed only when the fetch constant, the shader's sampler binding or `anisotropic_override` differ from what they were computed from. System constants are recomputed only when one of their about 80 inputs (registers, draw arguments, signedness and scaling of the used textures) differs from the previous draw; 98% of draws are skipped on the attract scene. The texture descriptor index check, and marking the textures as used and transitioning them, are skipped while the texture bindings, the submission and the states of all textures are unchanged. |
| (always) | The bit helpers are inline, the provider pointer is cached, `GetPath` is final, a texture remembers its last descriptor, and texture requests return early when no used binding has changed. |

`gpu_draw_state_cache_validate=true` computes everything the two caches skip and logs `gpu_texture_binding_cache mismatch` or `d3d12_draw_state_cache` warnings (the first 16) if a skipped result would have differed. It reported no mismatches on the attract scene with RTV at 1× and 2× and with ROV at 2×. Either cache can be turned off live to look for a regression; with both off, the remaining changes still apply.

**Results on the test PC** (front-end attract scene, about 5,000 draws per frame, RTV, unlimited, submit interval 1024, submission thread), frame profile on:

| | 1× before | 1× after | 2× before | 2× after |
| --- | --- | --- | --- | --- |
| frame | 8.03 ms | 7.03–7.11 ms | 9.86 ms | 8.64 ms |
| command thread busy | 7.32 ms | 6.32–6.39 ms | 9.15 ms | 7.92 ms |
| draw translation | 4.75 ms | 3.83–3.90 ms | 5.04 ms | 3.94 ms |
| `state` | 1.37 ms | 0.99–1.05 ms | 1.45 ms | 1.04 ms |
| `tex` | 0.80 ms | 0.47–0.48 ms | 0.95 ms | 0.50 ms |
| `rt` | 0.86 ms | 0.78 ms | 0.88 ms | 0.79 ms |
| `pipeline` | 0.52 ms | 0.47 ms | 0.53 ms | 0.47 ms |
| `vb` | 0.47 ms | 0.40–0.41 ms | 0.49 ms | 0.42 ms |
| GPU busy | 3.47 ms | 3.47 ms | 6.42 ms | 6.33 ms |

Frame time with the frame profile off, 1×, three alternating runs of each build over the same 30 seconds of the attract scene: before 7.85, 8.06 and 7.89 ms (126 FPS on average); after 6.84, 6.98 and 7.06 ms (144 FPS). That is 12% less frame time, and every run of the new build was faster than every run of the old one. The remaining frame is still limited by the command thread; `rt`, `vb` (mostly locking and uploading in shared memory requests) and `prim` are now about as large as `state`.

To check the change in gameplay, compare the `state` and `tex` stages of the frame profile at the same spot with both caches on and with `--gpu_texture_binding_cache=false --d3d12_draw_state_cache=false`.

## Buffer and render target fast paths

`rexglue-0.10.0-buffer-and-render-target-fast-paths.patch` makes the frame profile's `vb`, `prim` and `rt` stages cheaper. A sampling profile of the command thread on the attract scene, after the draw state caches, showed:

- **Shared memory requests: about 32% of the command thread.** Every vertex buffer (`vb`), index buffer (`prim`) and texture request went through `SharedMemory::RequestRanges`, which takes the global critical region to read the page validity bits. Guest threads hold the same lock while handling writes to protected pages and in kernel functions, so the command thread also waited for them and woke them on unlock. Locking alone (`Mtx_lock`, `Mtx_unlock`, the lock lookup and wake-ups) was about 16% of the thread. Almost all requests find their memory already uploaded.
- **Render target update: about 10% of the command thread.** Every draw rebuilt the render target keys, looked each render target up in a hash map, sorted them by EDRAM base, walked the EDRAM ownership map, and called the transfer function even when there was nothing to transfer. Consecutive draws nearly always use the same render targets over a range they already own.

Changes, all in effect by default:

| Setting | Change |
| --- | --- |
| `gpu_shared_memory_valid_range_check` | A request for one range first reads the validity bits of its pages without the lock, with acquire loads, and returns at once if all of them are valid and their GPU memory is allocated. Otherwise the request takes the existing locked path, which uploads. Only the command thread sets validity bits; guest threads only clear them, under the lock, before the write that caused it. A range seen as valid is therefore the result a locked check would have given just before a concurrent invalidation, and the data a draw was submitted with was invalidated before its submission. |
| `gpu_render_target_update_cache` | The update remembers the bindings it derived from the registers (surface info, used render targets, EDRAM bases, formats and the estimated height). If the next draw derives the same bindings, with a height no larger, and nothing has changed EDRAM ownership, the render target set or the accumulated bindings since (ownership changes, resolves, render target creation and destruction, interlock barriers, a new frame), the update returns without changing anything: running it again would find the ranges already owned, with no transfers. |
| (always) | The transfer function is not called when there are no transfers (it did nothing then), and the render target path is read without a virtual call in the common render target code. |

`gpu_draw_state_cache_validate=true` also checks both fast paths: after an unlocked "already uploaded" answer it runs the locked request and counts it as a mismatch if that uploads, unless a guest invalidation happened between the two checks; after a render target cache hit it runs the full update and compares the transfers, the used and accumulated render targets and the interlock barrier. It logs totals regularly, as `gpu_shared_memory_valid_range_check validation:` and `gpu_render_target_update_cache validation:` lines.

With validation on, the attract scene reported no mismatches with RTV at 1× and 2× and with ROV at 2×. At RTV 1×, about 100 million unlocked answers were checked, and about 2,000 of them overlapped a guest invalidation. Over 50 million render target cache hits were checked. Either fast path can be turned off live to look for a regression.

**Results on the test PC** (front-end attract scene, about 5,000 draws per frame, RTV, unlimited, submit interval 1024, submission thread, both draw state caches on), frame profile on:

| | 1× before | 1× after | 2× before | 2× after |
| --- | --- | --- | --- | --- |
| frame | 7.10 ms | 6.03–6.19 ms | 8.51 ms | 7.61 ms |
| command thread busy | 6.38 ms | 5.34–5.46 ms | 7.80 ms | 6.90 ms |
| draw translation | 3.86 ms | 2.82–2.95 ms | 3.91 ms | 2.95 ms |
| `rt` | 0.78 ms | 0.30 ms | 0.78 ms | 0.31 ms |
| `vb` | 0.42 ms | 0.28–0.30 ms | 0.41 ms | 0.30 ms |
| `prim` | 0.33 ms | 0.22–0.25 ms | 0.33 ms | 0.23 ms |
| `record` | 0.29 ms | 0.10 ms | 0.29 ms | 0.10 ms |
| GPU busy | 3.55 ms | 3.48–3.54 ms | 6.21 ms | 6.34 ms |

`record` was not targeted. It covers the index buffer setup, barriers and recording of the draw, and also fell; this was not investigated further.

Frame time with the frame profile off, 1×, three alternating runs of each build over the same 30 seconds of the attract scene: before 6.86, 7.13 and 6.99 ms (143 FPS on average); after 5.85, 5.88 and 5.96 ms (170 FPS). That is 16% less frame time, and every run of the new build was faster than every run of the old one.

Shared memory requests fell from about 32% to about 14% of the command thread in the sampling profile. Most of what remains is uploading: about 10% of the thread, still done with the lock. These runs had `clear_memory_page_state` on (ReXGlue's default), so every page uploaded from the CPU was uploaded again in the next frame. The game application now turns it off; see [Memory page state refresh](#memory-page-state-refresh).

To check the change in gameplay, compare the `vb`, `prim` and `rt` stages of the frame profile at the same spot with both fast paths on and with `--gpu_shared_memory_valid_range_check=false --gpu_render_target_update_cache=false`.

## Memory page state refresh

ReXGlue's `clear_memory_page_state` (default true) refreshes the page-valid state of GPU-written memory at the end of every frame. In Skate 2 this marks guest memory as changed each frame, so data the CPU has not touched is uploaded and processed again. The game application registers false as the default, like its other [profile defaults](DISPLAY.md#launch-profiles); a value saved in `skate2.toml` or passed at launch still applies.

Free Skate, the same busy spot, 2×, RTV, submission thread, interval 1024, all patches above, frame profile on, about 8,000 draws per frame:

| | `clear_memory_page_state=true` | `clear_memory_page_state=false` |
| --- | --- | --- |
| frame | 11.4 ms (88 FPS) | 10.0 ms (100 FPS) |
| frame limited by | GPU | command thread |
| guest memory uploaded per frame | 15.5 MB | 0.2 MB |
| GPU busy | 10.2 ms | 3.7 ms |
| draw translation | 6.1 ms | 5.4 ms |
| `vb` / `prim` | 0.66 / 0.66 ms | 0.17 / 0.15 ms |
| submission thread replay | 2.8 ms | 1.1 ms |

No visual difference was seen in this run. The GPU time fell by far more than uploading 15 MB costs, so the GPU was probably repeating work on unchanged data, such as textures; this was not traced.

ReXGlue's description of the setting warns that turning it off may break memory coherency. If geometry, textures or effects look stale, flicker or go missing, launch with `--clear_memory_page_state=true` (or set `clear_memory_page_state = true` in `skate2.toml`) to restore the refresh, and report where it happened.

## Render target transfer skipping

On the RTV path, the runtime keeps one host render target per EDRAM layout (base, pitch, MSAA, format, color or depth). It records which of them holds the latest contents of each EDRAM tile. When a draw uses EDRAM tiles that another render target owns, the render target update takes them over and copies the old contents into the draw's render target. This copy is a transfer: `ChangeOwnership` in `RenderTargetCache::Update`, `src/graphics/pipeline/render_target/cache.cpp`, then `PerformTransfersAndResolveClears` in `src/graphics/d3d12/render_target_cache.cpp`.

The range taken over is whole rows of EDRAM tiles from the render target's base, across the full surface pitch, as many rows as the draw's estimated height needs. The height comes from the scissor, the viewport, or for unclipped draws the vertex positions computed on the CPU. Later draws only transfer the rows they add. A draw with an unbounded scissor that covers the whole surface, such as a full-screen clear, therefore takes over and copies everything up to the next render target's base, only to replace all of it.

`gpu_render_target_transfer_skip_overwritten` (`rexglue-0.10.0-render-target-transfer-skip.patch`, on by default, applies live) cuts out of the transfers the rectangle that the draw replaces at every sample. Transfers that lie entirely inside it are dropped. The rest is still copied, so no tile ends up with older contents than before. A draw qualifies only if all of these hold:

- **Shape.** It is one rectangle (rectangle list), or one or two triangles, with vertex positions computed on the CPU by the shader interpreter, as the draw extent estimate already does for unclipped draws.
  - Two triangles must form one axis-aligned rectangle, as the halves on both sides of one diagonal. Otherwise one triangle must contain the whole area needed.
  - Triangles must not be face-culled or drawn as wireframe.
  - With clipping on, every vertex must be inside the clip volume, so nothing is clipped.
  - Vertex kill, primitive reset and user clip planes disqualify the draw.
- **Coverage.** A pixel counts as covered when the rectangle includes it to within 1/128 pixel. That margin is smaller than the distance from any sample position to the pixel edge, also with resolution scaling. The covered pixels are clipped to the scissor.
- **No discarding.** The pixel shader doesn't kill pixels, alpha test is off or set to always pass, and alpha to coverage is off.
- **Depth buffer.** The depth test always passes and writes depth. The stencil test always passes and replaces the stencil on pass, also for back faces when two-sided stencil is on, with a write mask of 0xFF.
- **Color render target.** All channels are written, and blending uses source factor one, destination factor zero and addition, for both color and alpha.

The check runs only for draws that have transfers, about 35 per frame in Free Skate. Render targets that fail it keep their full transfers. Only the Direct3D 12 backend passes the pixel shader to the check, so the Vulkan backend never skips.

`gpu_render_target_transfer_skip_overwritten_validate` fills each skipped area just before the draw, with magenta color (the same bit pattern in every format) and depth and stencil 0. If the draws replace those areas as the check concluded, nothing changes on screen. Magenta, or geometry missing behind depth 0, means a draw did not replace an area it was trusted to replace: report where.

`gpu_render_target_transfer_log=N` logs every N frames:

- **Summary line.** The transfers per frame since the previous summary: draws that needed transfers, the number of transfers, how many were dropped or cut, and the samples copied against those needed.
- **Detail for the next frame.** One line per draw with transfers: its primitive, scissor, depth, stencil, color mask and blending, and the reason it doesn't replace the area. Under it, one line per transfer: the destination and source render targets, the EDRAM tiles and rows, and what happened to the transfer.

With the skip off, the log still shows what would be skipped, for A/B comparisons.

Measured in Free Skate, toggled in the same spot, with no visual difference seen:

| | Transfer GPU time | GPU busy | Frame time |
| --- | --- | --- | --- |
| Test PC (about 3,990 draws), skip off | 1.77 ms | 3.63 ms | about 4.8 ms |
| Test PC, skip on | 1.46 ms | 3.32 ms | about 4.8 ms |
| Steam Deck, skip off | 4.5 ms | — | — |
| Steam Deck, skip on | 4.0 ms | — | — |

On the Steam Deck, 12 of about 47 transfers per frame were cut, and 92–95% of the samples were still copied. Frame time did not change on either machine: the saved GPU time is early in the frame, while the CPU is still translating draws. Most of the remaining samples come from depth clears that leave the stencil alone (about 39%), from full-screen quads with clipping on (about 23%), and from the game clearing depth buffers through a 4× MSAA alias and the 1× view in turn, which moves ownership back and forth.

The GPU pass profile now attributes each transfer to the pass of the draw that needed it. Before, when a pass directly followed another without a resolve in between, its transfers were counted for the previous pass. That is why passes that write no render target (`none`) showed transfer time on the Steam Deck.

Approaches not taken:

| Approach | Why not |
| --- | --- |
| Copy only the columns a draw touches | EDRAM ownership is tracked in tile ranges that are whole rows wide, so a narrower copy would need ownership tracked per rectangle. The height is already bounded. |
| Turn copies of cleared areas into clears | A copy whose source was last written by a resolve clear could be done as a clear of the destination. But on the Steam Deck, Skate 2 does 2 resolve clears per frame against 35 transfers, and a full-screen clear draw after a resolve clear is already handled by the skip. |
| Skip transfers into areas a later draw replaces | When the transfer happens, only the current draw is known. |

## Single-pass tiled scenes

The main scene doesn't fit the 10 MB EDRAM: in Free Skate it is 1152×640 with 2× MSAA, color plus depth. Skate 2 draws it with the Xbox 360 Direct3D runtime's predicated tiling, which splits the screen into horizontal strips. On the Steam Deck the pass profile shows two strips, 544 and 96 rows, each with the same 2,739 color draws and about 735 depth-prepass draws. Two front-end GPU traces (`frame14135`, `frame17204`; 4× MSAA, three strips of 256 rows) show how the runtime does it:

- The scene is recorded once into command buffers. They are executed once per strip with `PM4_INDIRECT_BUFFER`, after a `PM4_SET_BIN_SELECT` for the strip: `0x80000003`, `0xC`, `0x30`, then `0xFFFFFFFF` at the end. Bit 31 marks the first strip.
- Every draw is predicated (bit 0 of the type-3 header) and preceded by a `PM4_SET_BIN_MASK_LO`. After it, a predicated `PM4_EVENT_WRITE_EXT` writes the draw's screen extent to memory during the first strip only. The guest code emitting this is `sub_82361150` and its siblings, which keep a 16-byte record per draw: a pointer to the mask in the command buffer and the extent.
- At the end of the first strip, a predicated interrupt (`sub_82334170`) has the guest's GPU interrupt handler turn the extents into bin masks and write them into the command buffer, while the GPU waits on a `PM4_WAIT_REG_MEM`. The masks were `0xFFFFFFFF` in the first strip and `0x8000003F` in the replays.
- The runtime writes a fixed full-screen extent for every `PM4_EVENT_WRITE_EXT` (`ExecutePacketType3_EVENT_WRITE_EXT`, `src/graphics/command_processor.cpp`), so every draw gets every strip's bins and runs in every strip. On the console, draws outside a strip are skipped by the predication.
- The strips' window offsets and scissors are set by predicated `PM4_SET_CONSTANT` packets for all strips at the start of each strip's first command buffer. Each strip ends with a depth resolve and a color resolve that clears color and depth for the next strip.

A Free Skate trace on the test PC (`frame21909`, 2x render scale) has the same structure with two strips: bin selects `0x80000003` and `0xC`, window offsets 0 and -544, scissors from row 0 to 544 and from 544 to 640, both from x 0 to 1152. Each strip has 180 depth-prepass and 1,690 color draws, ending with a depth resolve (`RB_COPY_CONTROL` 0x4) and a color resolve that clears color 0 and depth (0x00100340). Each strip resolves to its own destinations, set by predicated `PM4_SET_CONSTANT` packets for `RB_COPY_DEST_BASE`, 544 rows apart. Nothing is resolved in the middle of the scene. The clear values are 0xFFFFFF00 (depth 1, stencil 0) and 0 (black), written before each strip's first draw and before each resolve.

`gpu_tiled_scene_single_pass` (`rexglue-0.10.0-tiled-scene-single-pass.patch`, off by default in the SDK, on in the Skate 2 profile, applies from the next tiled scene) emulates the whole scene in the first strip instead:

1. **Capture.** In the first strip, the strips' rectangles are read from the predicated constants, both executed and skipped. They must stack from the top with one x range, and each window offset must move its strip to the top of the EDRAM. The host render targets must be tall enough for the full height: they are as tall as the EDRAM wraps at their pitch, 1,096 rows at 2× and pitch 1200, but only 568 rows at 4× and pitch 1160, so the 4× front-end scene is not eligible.
2. **First strip.** Draws replayed in all strips whose scissor ends at the first strip's bottom get the host scissor extended to the last strip's bottom. EDRAM ownership is still tracked for the first strip only; the extra rows are host-only storage below it. Before the first extended draw to the depth buffer and to color render target 0, their extra rows are cleared to `RB_DEPTH_CLEAR` and `RB_COLOR_CLEAR`: on the console, the later strips start with the EDRAM as the first strip's resolve clears it. Other color render targets, or a second depth buffer or color target in the scene, make the scene fall back.
3. **Later strips.** Only if every such draw was extended: predicated draws whose mask includes the first strip's bins are skipped. Resolves in a later strip's window read their source that many rows lower in the host render targets: the EDRAM dump moves the source base back by those tile rows (`DumpRenderTargets`, `src/graphics/d3d12/render_target_cache.cpp`). Direct host resolves are not used for them. The clears of those resolves still apply to the EDRAM rows.

If anything doesn't fit, the later strips run as before. The first strip's extra rows are then unused, so the output is the same, at the GPU cost of drawing them.

`gpu_tiled_scene_log=N` logs every N frames: tiled scenes and single-pass ones per frame, draws extended and from which row to which, replayed draws skipped, resolves shifted, draws kept in a later strip's window, and the fallback reasons. The fallback reasons are bits: 0x1 more than 8 strips, 0x2 fewer than 2 strips captured, 0x4 strips not stacked from the top, 0x8 render targets too short, 0x10 no draw extended, 0x20 a draw limited by the first strip couldn't be extended (including when its render targets couldn't be pre-cleared), 0x40 the first strip's resolves didn't clear the depth buffer or color target 0 that were pre-cleared, 0x80 they cleared to other values than the pre-clear used.

Risks:

- **Draws kept in a later strip.** A predicated draw only for a later strip, or an unpredicated draw in its window, still renders into that strip's EDRAM rows, which the shifted resolve doesn't read. The front-end traces have none; the log counts them as kept draws.
- **Scissored draws.** A scene draw with its own scissor ending between the first strip's bottom and the last strip's bottom is extended to the last strip's bottom. A scissor entirely below the first strip makes the scene fall back.
- **Clears.** The first version didn't clear the extra rows. In Free Skate the scene's initial clear only covers the first strip: its two draws (one through a 4× MSAA alias at pitch 600) use a 1152×544 viewport, and the later strip relies on the first strip's resolve clear (`RB_COPY_CONTROL` 0x00100340: color 0 and depth). The extra rows kept old depth, so the bottom strip failed the depth test and showed old contents: the bottom of the screen was cut off. They are now pre-cleared, and the first strip's resolve clears are checked against the pre-clear.
- **Effects reading the scene mid-scene.** A resolve in the middle of the scene, sampled by later draws, would have only the first strip's rows when the extra rows are drawn. The front-end scene resolves only at the end of each strip.

Measured in Free Skate, toggled in the same spot, with no visual difference seen:

| | Draws per frame | Command thread work | Frame time |
| --- | --- | --- | --- |
| Test PC (2×, RTV), strips | about 5,290 | 4.0 ms | 5.68 ms (176 FPS) |
| Test PC, single pass | about 2,870 | 2.8 ms | 4.45 ms (224 FPS) |
| Steam Deck (1×, RTV), strips | about 6,820 | 21.3 ms | 28.3 ms (35 FPS) |
| Steam Deck, single pass | about 3,645 | 15.3 ms | 21.8 ms (46 FPS) |

Every gameplay frame was drawn in a single pass, with no kept draws and no fallback. The front end's 4× scene still falls back (0x8), as expected. The command processor still parses the replayed command buffers, and the first strip draws 96 more rows; GPU busy fell slightly (Steam Deck 13.7 to 13.1 ms). The Skate 2 profile turns the setting on; `--gpu_tiled_scene_single_pass=false` restores strip-by-strip drawing.

Approaches not taken:

| Approach | Why not |
| --- | --- |
| Write real screen extents, as the console does | Needs every draw's screen bounds before the second strip. Running the vertex shaders on the CPU costs more than translating the draw. Computing them on the GPU needs shader changes and a mid-frame wait for the first strip. |
| Replay the first strip's translated host commands | Saves only the translation, needs fix-ups for uploads, barriers and system constants, and textures written by the first strip's resolve would be stale. |
| Turn off 2× MSAA for the main scene so it fits in one strip | A visible quality loss, and finding where the game chooses the MSAA mode needs more reverse engineering. |

## Idle-clean wait overlap

With `d3d12_present_idleclean_sync` (on in every Skate 2 profile), the command processor waits for the GPU when the game writes `WAIT_UNTIL` with the 3D idle-clean bit (0x00020000) between closed frames (`MaybeSynchronizePresentIdleclean`, `src/graphics/d3d12/command_processor.cpp`). The game writes it about 30 times per frame. Two per frame are honored, after the swap and before the next frame's first command; the others fall inside an open frame. With `d3d12_present_idleclean_sync_lag=0` (see below), the first of the two lasts until all submitted work has finished on the GPU, and the second finds that work already done. The next frame is therefore not translated while the GPU finishes the last one.

That costs the overlap at the end of every frame. The GPU's tail is mostly the work after the main scene: post-processing passes of a few draws each, resolves and the frame end, which the CPU records in far less time than the GPU runs it. In Free Skate on the Steam Deck with single-pass scenes, the command thread works 15.3 ms and the GPU 13.1 ms per frame, but a frame takes 21.8 ms, about 5 ms of it in this wait (`present_sync` in the frame profile, `limit=gpu_wait`). The test PC shows the same pattern at 4.45 ms per frame with 2.8 ms of CPU work and 3.3 ms of GPU time. Submitting more often doesn't shorten the tail: intervals of 512 and 256 draws didn't help on the Steam Deck.

What the wait protects is not known. It was added when unlimited presentation showed colored stripes and black surfaces that the paced 60 FPS mode didn't; the corrupted resource wasn't traced, and turning the wait off on the RTV profile later still showed corruption. A review of this runtime found no path where translating a frame depends on the previous frame having finished on the GPU: shared memory uploads, texture loads, resolves and render target transfers are ordered on one queue; upload, constant and descriptor pools and command allocators are reclaimed by fences; `readback_resolve` is `none`, gameplay uses no memory export, and occlusion query results are written once their submission has completed. Without the wait, up to three frames can be in flight (`kQueueFrames`).

`d3d12_present_idleclean_sync_lag` (`rexglue-0.10.0-present-idleclean-lag.patch`, off by default in the SDK, 1 in the Skate 2 profile, applies live) narrows the wait to an older frame instead, using the last submission of each closed frame (`closed_frame_submissions_`):

| Value | The idle-clean wait returns when |
| --- | --- |
| 0 | All submitted work has finished, as before |
| 1 | The frame before the last one has finished: the GPU finishes the last frame while the next one is translated, at most one frame of overlap |
| 2 | The frame two before the last one has finished, which opening the next frame already waits for: the same as turning the wait off |

Measured on the test PC in the same gameplay spot:

| | Draws per frame | `present_sync` | Frame time |
| --- | --- | --- | --- |
| Lag 0 | about 4,100 | 1.35 ms | 7.27 ms (137 FPS) |
| Lag 1 | about 4,100 | 0 | 5.93 ms (169 FPS) |

Command thread work (5.6 ms) and GPU busy time (3.35 ms) were unchanged, and the frame profile showed `limit=command_thread` in both. A lighter view (about 2,570 draws) ran at 3.74 ms (267 FPS) with lag 1, and menus went from about 4.4 to 3.65 ms. In gameplay with lag 1 the idle-clean waits never blocked. No colored stripes or black surfaces were seen on the test PC. On the Steam Deck the frame rate looked good with lag 1, but the visual check there is not finished.

The Skate 2 profile sets lag 1. `--d3d12_present_idleclean_sync_lag=0` (or `d3d12_present_idleclean_sync_lag = 0` in `skate2.toml`) restores the full wait. If lag 1 shows the corruption, the hazard needs the last frame finished before the next one is translated, which narrows the search.

`d3d12_present_idleclean_log=N` logs every N frames: honored waits per frame, how many of them blocked, the time waited on average and at most, how many closed frames were still on the GPU when waiting, and how many waits used an older frame because of the lag.

## Measuring a change

Pass `--perf_log_csv=<path>` (or `-PerformanceCapture` to `scripts/Launch-Skate2.ps1`) and compare frames grouped by `draw_calls` between runs over the same route. For a per-stage breakdown, see [Frame profile](#frame-profile). For CPU attribution, record with `wpr -start CPU -filemode` / `wpr -stop trace.etl` while the game runs, using a runtime built with debug information.
