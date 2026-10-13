# What was done to run Liberty Recompiled on Android

This document describes the work in this fork that took Liberty Recompiled from a desktop-only
project (Windows, Linux, macOS) to a playable Android build on a Snapdragon 865 handheld. Every
change is on top of upstream `main` (`1922957d`); `git log 1922957d..` lists them all.

Upstream gives the hard part: GTA IV's PowerPC code recompiled to C++ by the ReXGlue SDK, an Xbox
360 kernel and runtime, and the `gta4_native` renderer, which replays the game's Direct3D 9-style
draw calls through Vulkan or Metal. Upstream also had a skeleton Android Gradle project with SDL's
Java glue, but nothing built or ran on Android.

Target device during development: **Retroid Pocket 5**, which has a Snapdragon 865 (1× Cortex-A77
at 2.84 GHz, 3× A77, 4× A55), an Adreno 650, 8 GB of RAM and Android 13.

## 1. Building for Android

- **Toolchain.** The project builds with **NDK r29** (29.0.14206865), arm64-v8a, API level 28+ for
  the native code and minSdk 29 for the app. Older NDKs (r27c) ship a libc++ without `std::jthread`,
  which the runtime uses, so `clock_cast` is polyfilled as well.
- **What gets built.** Android builds the same "Graine" ReXGlue consumer as macOS. It is packaged
  as `libmain.so` (the game: recompiled code, hooks, app layer), `librexruntime.so` (kernel and
  runtime), `librexgpu-gta4-native.so` (the renderer plugin), `libSDL3.so` and
  `libc++_shared.so`. A build check makes sure every `DT_NEEDED` entry is either staged into the
  APK or a system library.
- **Code generation flags.** The build uses `-march=armv8.2-a` for inline LSE atomics instead of
  outline-atomic calls, and `-fno-emulated-tls` for native ELF TLS. Both are hot on the guest
  thread, which uses thread-locals and atomics constantly.
- **Linking.** `libmain` uses ThinLTO and `-Bsymbolic-functions`, so calls between recompiled
  functions do not go through the PLT. The ReXGlue CLI emits a 64-byte aligned TLS segment, which
  Bionic requires for arm64 executables and libraries.
- **PGO.** Profile-guided optimization is wired up for the renderer library
  (`-DLIBERTY_ANDROID_PGO=generate` writes profiles on the device every 60 s;
  `=<path to .profdata>` uses them).
- **Direct calls.** An experiment, `tools/direct_calls.py`, rewrites calls between unhooked
  recompiled functions to bypass their weak aliases so the compiler can inline them. It is off by
  default: it gave no measurable gain and made the APK bigger.
- **Host tools.** The DXC bundled with XenosRecomp (1.8) crashes on the Bink video shaders, so
  `setup_host_tools.sh` fetches DXC 1.9 and glslang for the shader override cache.
  `materialize_symlinks.py` replaces git symlinks, which do not survive a Windows checkout.
- **Scripts.** Everything is in `os/android/scripts`: `build_native.sh`, `build_driver_proxy.sh`,
  `build_apk.sh`, `install.sh`, `logs.sh`, `fps.sh`, `live_cvar.sh`, `push_args.sh`,
  `push_install_sources.sh`, `symbolize.sh` and more. See [BUILDING.md](BUILDING.md).

## 2. The Android platform layer

- **`LibertyActivity`** (an `SDLActivity` subclass) prepares everything the runtime reads before
  SDL loads `libmain.so`:
  - it points `XDG_DATA_HOME` and `HOME` at the app's external files folder, so the game, saves
    and caches live in `Android/data/com.libertyrecomp/files` with no storage permission;
  - it copies fonts, button prompts and the RPF key out of the APK into `REX_RESOURCES_DIR`
    whenever the APK changes;
  - it reads `args.txt` (created from `assets/default_args.txt` on the first launch), `env.txt`,
    `driver.txt` and `--android_surface=WxH`.
- **Launcher** (`LauncherActivity`, 0.5.6.3). A plain Android screen in the main process shows
  the game status, offers the Vulkan driver choice (bundled Turnip, system or imported zips, saved
  to `driver.txt`) and starts the game. The driver must be chosen before any native code loads,
  and the driver proxy is set up once per process, so the game activity runs in its own process
  (`:game`). Play ends a game process left over from an earlier session before starting a new one.
  A launcher opened from the app icon over a running game closes at once, so the icon returns to
  the game.
- **Installer in the launcher** (0.5.7). `libliberty_install.so` contains the same inspection
  and installation code as the in-game installer (`src/install`), without the rest of the
  title, behind a small JNI layer (`src/android/android_install_jni.cpp`). The launcher checks the
  disc image as soon as it is chosen, installs with a progress bar, and uses the installer's
  `IsInstallReady` for its status. Sources are picked with the system file picker. The installer
  maps disc images into memory by path, so the picked document is turned back into a file path,
  which needs *All files access* (Android 11+) or legacy storage (Android 10). The RPF key reaches
  it through the same `REX_RESOURCES_DIR` resources the game uses.
- **Installer.** The SDL file picker returns `content://` URIs, which the installer cannot open.
  The activity therefore takes the disc image and the title update from `files/install/` and
  passes them as `--install_game_source` / `--install_update_source`. The install dialog gained
  `Preselect()`, which fills both rows and starts the installation by itself once the disc passes
  inspection. The RPF AES key is looked up in the resources folder instead of next to the
  executable.
- **Platform stubs** (`gta4-recomp/src/android/android_platform_stubs.cpp`) stand in for the
  services the macOS host implements in Objective-C++ (Game Center, user music, microphone) and
  for the community multiplayer backend. Each one reports "unavailable", so the game takes its
  existing offline paths.
- **File system.** On Android `/proc/self/exe` is `app_process`, so the runtime locates itself
  through `dladdr` on its own library instead.
- **Lifecycle.** The game survives the screen turning off and switching apps. The presenter keeps
  its own `ANativeWindow` reference, detaches when the app goes to the background and attaches the
  new window on return.
- **Crash reporting.** A SIGSEGV/SIGILL that is not a guest-memory fault now chains to the
  previous handler (debuggerd), so it produces a crash report. Before, the handler re-ran the
  faulting instruction forever, which showed up as a frozen black screen.
- **Input and UI.** Touch and gamepad drive the ImGui installer and menus, and the on-screen
  keyboard no longer pops up.
- **Idle loops.** The SDL UI event loop no longer spins a core when idle, and the kernel's
  `WaitMultiple` sleeps instead of busy-polling.
- **Audio.** Crackling under CPU load came from the guest's thread priorities never reaching
  Android, and SCHED_FIFO is denied to apps. It is fixed by giving the "Audio Worker" and
  "XMA Decoder" threads `nice -16`.
- **Display timing.** The guest's vblank interrupt is driven by the display's real vsync
  (AChoreographer) instead of a free-running timer.
- **Logs.** Native log output goes to logcat (tag `LibertyRecomp`). The diagnostics policy and its
  argument errors are reported there too, instead of failing silently.

## 3. The GPU driver

The renderer needs **Vulkan 1.2**, but the Adreno 650's stock Qualcomm driver exposes only
**Vulkan 1.1**. The fix is to ship a driver:

- `os/android/native` builds a **Vulkan proxy `libvulkan.so`** on
  [libadrenotools](https://github.com/bylaws/libadrenotools). The proxy is taken from
  skate3-android / skate3-pocket; [PROVENANCE.md](../../os/android/native/PROVENANCE.md) has the
  details. The runtime calls `dlopen("libvulkan.so")` by bare name, and the APK's native library
  folder is searched first, so the proxy always wins. It then loads either the bundled **Mesa
  Turnip** build (default) or the system driver, and it checks that the device it gets really is
  the requested driver. libadrenotools can fall back to the stock driver silently, and the check
  catches that.
- `DriverBridge.java` unpacks and verifies (SHA-256) the bundled Turnip zip, and supports
  `driver.txt` = `system` / `custom:NAME`.
- Several Turnip builds were compared. **StevenMXZ 26.3.0-R6** is the fastest on the Adreno 650.
- **`TU_DEBUG=sysmem`** is the default. Turnip's tiled GMEM mode loses about 15% on this
  renderer's many small passes and resolves.

Driver-specific problems found and fixed:

- **Wrong colors (blue instead of yellow).** The renderer is tuned for identity image-view
  swizzles, and on MoltenVK it uses them. Android now does the same.
- **White and black squares, "pixel explosions".** A Turnip bug: a pipeline with **no fragment
  shader** but bound color attachments writes garbage, even with a zero write mask. That produced
  NaN/Inf in the HDR targets. Such pipelines now get an empty fragment shader
  (`gta4_native_empty_fragment`).
- **GPU hangs near water.** These were traced to one deferred light-volume pixel shader
  (`2673E2AF`). Its draws are skipped (`gta4_native_skip_pixel_shaders`). A SPIR-V **loop
  watchdog** also bounds shader loops, so a runaway loop cannot hang the GPU.
- **Turnip's `maxBoundDescriptorSets` is 4.** Descriptor sets were merged to fit, and shader
  constants moved to **UBO constant banks**.
- **Never-signalled fences.** Fence waits are bounded, so a submission that never completes is
  detected instead of freezing the game.

## 4. Renderer performance

### The starting point

`gta4_native` runs on three threads:

1. The game's render thread calls the hooked D3D-style API. Each call is captured into a
   `NativeCommand` holding the draw state, constant snapshots and texture references.
2. A worker assembles the frame.
3. A recorder thread (`GtaRecorder`) records and submits Vulkan command buffers.

On the desktop this has plenty of headroom. On the Snapdragon 865, the first working build ran the
open world at **about 11 FPS** with about 5800 draws per frame. It was CPU-bound: every draw cost
about 24 µs spread over the three threads, and the game's own simulation also needs its share of
the big cores.

### CPU work

Each item below sits behind its own cvar, so it can be disabled without a rebuild.
[`android_args/rewrite_off.txt`](../../os/android/android_args/rewrite_off.txt) turns off the most
recent batch (0.5).

- **Pipelining.** The worker assembles frames while the recorder records the previous one. Retired
  commands are released in the background. Worker wake-ups are batched, and captured draws are
  published in batches of 32. The game may run up to three frames ahead
  (`gta4_native_max_queued_frames`).
- **Command transport.**
  - Command allocation is lock-free, with an O(1) pool refill.
  - Retired commands are **recycled in place**. The 3 KB `NativeCommand` is no longer constructed,
    destroyed and freed for every draw.
  - **State commands are coalesced:** `Set*` calls ride inside the next queued command instead of
    each taking a full command.
- **Constants.** Only the **prefix of each constant bank that the draw's shader actually reads** is
  snapshotted, hashed (lazily) and uploaded. Constant versions and snapshot handles come from
  pools.
- **Caches.**
  - The texture capture memo is keyed on the texture fetch words (85% hit rate).
  - Buffer and texture capture caches are two-way.
  - Pipelines are cached across state snapshots.
  - Prepared texture bindings are reused from any recent draw.
  - Shader range classification is memoized.
  - Vertex bindings are deduplicated.
- **Texture protection.** The guest memory protection of textures uses a flat index. Textures are
  stamped once per frame, and the protection set is collected outside the queue lock.
- **Fewer walks.** Two per-frame passes over every command were dropped from the recorder. Buffer
  shadow validation runs per frame instead of per draw. Frame statistics are gathered on the
  worker.
- **Draw merging.**
  - Runs of identical `DrawPrimitiveUp` calls are merged: the water surface is about 500 tiny draws.
  - Consecutive indexed triangle-list draws with identical state and adjacent index ranges are
    recorded as one draw.
- **Scheduling.** The busiest threads can be pinned to the big cores (`gta4_thread_pinning`). It is
  off by default, because changing the game's thread timing provokes the race described under
  [Stability](#5-stability).

Result: CPU time per thread in the heaviest far views went from about 32 ms to about 17 ms, and
the renderer is no longer the bottleneck.

### GPU work

- **Xenos multiplies.** The Xbox 360 GPU's multiply rules (0 × anything = 0) were emulated with
  extra instructions in every shader. Bit-exact cheaper forms are used where possible, plus plain
  IEEE multiplies where the result cannot differ (`gta4_native_ieee_mul`).
- **Early fragment tests** are enabled for draws that write neither depth nor stencil.
- **Lighting pass reuse.** The local lights' stencil setup runs inside the open lighting pass. This
  is what needed the empty-fragment-shader fix above.
- **Water reflection.** It is updated every frame only when water is actually drawn. The exterior
  environment capture range is shorter (`gta4_reflection_capture_distance=near`).
- **Upscaling.** `gta4_output_height_cap` renders at a lower output height and lets the presenter
  or the display scale it. FSR 1 (EASU + RCAS) is available for upscaling to a higher output.
- **A dynamic draw distance controller** lowers the draw distance (down to 75% by default) only
  while frames miss the 30 FPS budget, and restores it once there is headroom.

Today the Snapdragon 865 is **GPU-bound** at native 720p in the heaviest scenes. A frame there has
about 2100 draws at about 7 µs of fixed GPU cost each. The resolves needed for the Xbox 360's
render-to-texture model add a few milliseconds more.

### Content defaults

The defaults keep the console's look where it matters:

- draw distance 1.0;
- shadow map base size 256, which is the console's (512 is a PC upgrade);
- shadow range 1.0.

Population density is lowered to 0.4 of the PC default (1.25), because every car and pedestrian is
more draws.

**Shadow range below 1.0 is not allowed any more.** Lowering it looked like a cheap optimization,
but the range also sizes the shadow volume around the camera. Below 1.0, vehicle and pedestrian
shadows "sink" into the ground and leave only a sliver under the car. This had gone unnoticed
because benchmarking was done at night, when shadows are not visible.

## 5. Stability

- **Freed-object race.** `sub_8296E480` and `sub_8296E310` copy an object's world matrix through
  a chain of pointers. Another game thread can free that object at the same moment. The console's
  timing hid this race; the faster renderer exposes it as a crash on an unmapped page.
  `android_stale_object_guard.cpp` checks that the whole pointer chain still leads to mapped guest
  memory. It uses a per-thread page cache keyed on the heap's access epoch, so the check is cheap.
  If the chain is broken, the guard takes the game's own "no object" path (an identity matrix)
  instead of crashing (`gta4_stale_object_guard`).
- **The 30 FPS guard** (`gta4_fps_guard`, off by default) can halve the environment reflection
  rate while frames miss 30 FPS. An earlier version also cut shadows and could get stuck, which
  turned the player's shadow into a dot.
- **The frame-wait hook** (`gta4_frame_wait_sleep`, off by default) replaces the game's busy-wait
  on completed frames with a short sleep.

## 6. Version history

| Version | Highlights |
|---|---|
| 0.2 | Turnip R6, FSR 1, dynamic draw distance, batched command queue, vblank from the display |
| 0.3 | Light-volume GPU hang fixed, PGO, water draw merging |
| 0.4 | Native 720p at 30 FPS with VSync, GPU-side cuts (early tests, lighting pass reuse, cheaper multiplies) |
| 0.5 | Renderer CPU rewrite (command recycling, state coalescing, partial constants, caches): the far view holds 30 FPS at 720p |
| 0.5.5 | 1080p output with FSR 1 quality as an alternative profile, crash guard for the freed-object race |
| 0.5.6 | Shadow fix (console range and map size), indexed draw merging, second crash guard, native 720p defaults |
| 0.5.6.1 | First public release: documentation, no debug shader dumps in the data folder |
| 0.5.6.2 | System-driver fallback when Turnip cannot drive the GPU, an error dialog instead of a silent exit, `last_launch.txt` report, settings files tolerate a byte-order mark |
| 0.5.6.3 | Launcher screen with Play and a Vulkan driver picker that imports driver zips; the game runs in its own process so the chosen driver always applies |
| 0.5.6.4 | Custom and bundled Turnip drivers are accepted on Android 10 to 12, whose platform Vulkan loader reports only version 1.1 |
| 0.5.7 | The launcher installs the game, the title update and the episodes, with files picked anywhere on the device |
| 0.5.7.1 | Launcher layout fixed on 20:9 and 21:9 screens; logo and Play button in the new typeface |
| 0.5.7.2 | On-screen Xbox gamepad replaces the context touch layout; it hides while a controller is connected |
| 0.5.7.3 | Crash reports: the launcher saves and shows every crash of the game process (signal, backtrace, last log lines) with a Share button; the runtime log is on by default |
| 0.5.7.4 | Guest memory is created with ASharedMemory (the initializer was never called, so every device used the legacy /dev/ashmem that newer Android builds refuse), with a memfd fallback; a failed start shows a dialog instead of crashing on teardown |
| 0.5.7.5 | The game picks a landscape output even when its window is still portrait at startup (phones whose natural orientation is portrait got a narrow strip in the middle of the screen) |
| 0.5.7.6 | The game window stays landscape: SDL never saw the manifest's orientation hint (activity-level metadata) and allowed any orientation, so phones held upright turned the game to portrait |
| 0.5.7.7 | Audio delay cut from up to ~340 ms to ~170 ms: the guest audio queue defaults to 32 blocks on Android instead of 64 (16 stuttered during frame hitches) |
| 0.5.7.8 | Launcher: Play (Debug Log) records the game process's logcat into files/debug_logs/; GitHub link in the footer |

## 7. What is left

- **GPU cost per draw** is now the limit on the Snapdragon 865: there is a fixed cost per draw and
  per render pass, and the necessary resolves add more. Batching more draws, and moving the
  resolves into existing passes, are the next steps.
- **The skipped light-volume shader** should be fixed in the shader translation rather than
  skipped.
- **Other GPUs.** Adreno 7xx and 8xx devices have had only brief tries. Mali runs
  through the system driver since the Mali support pass (Mali-G720 tested).
