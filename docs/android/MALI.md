# Running on ARM Mali (experimental)

Tested on a Mali-G720 MC7 (Dimensity 8400 / POCO X7 Pro, ARM driver
r49p1, Vulkan 1.3, Android 16) at ~30 FPS. There is no Turnip for Mali:
use the **System driver** on the launcher and start from
`os/android/android_args/mali-g720.txt` (FSR performance, no AA).

Full bring-up notes (in Spanish) live in the author's personal docs;
this page is the technical summary.

## What Mali needed

1. **Device selection** (`src/ui/vulkan/vulkan_device.cpp`).
   Mali exposes no `shaderClipDistance` (`maxClipDistances = 0`), which
   used to reject the whole device. New cvar
   `vulkan_require_shader_clip_distance` (default `false`); without it
   the backend degrades per draw (user clip planes off in the Vulkan
   pipeline cache, clip-plane draws rejected in `graphics_system.cpp`).
   `sampleRateShading` is still required (Mali has it).

2. **Static `ClipDistance` in precompiled shaders**
   (`src/graphics/gta4_native/native_spirv_clip_distance.h`).
   The offline stock shader cache declares `BuiltIn ClipDistance` even
   for draws that never enable a clip plane. Pipeline creation without
   the feature is invalid; the ARM driver answers with a segfault
   (`addr=0x50` in `cmpbe_v2_compile_multiple_shaders`) instead of an
   error. First victim: vs `2668E8F9BB250542` + ps `gta_im_ps2` (late),
   frame 1. On devices without the feature the loader now rewrites the
   decoration into a plain unconsumed `Location` output and drops the
   capability (same word count, validated with `spirv-val`). Correct
   values are BuiltIn 3/4, Capability 32/33. Draws that really use
   clip planes stay rejected at draw time.

3. **BC textures** (`src/graphics/pipeline/texture/conversion.cpp`,
   `graphics_system.cpp`).
   Mali has no `textureCompressionBC`, so every DXT1 (`k_DXT1 = 18`)
   and DXT4_5 (`k_DXT4_5 = 20`) texture failed `format-capabilities`
   and its draws were rejected (only lights/untextured geometry
   visible). Where BC sampling is unavailable those blocks are now
   decoded to RGBA8 on upload (`ConvertTexelBC1/BC2/BC3ToRGBA8`,
   plus `ConvertTexelDXT3AToRGBA8`), joining the existing
   block-to-pixels flow (CTX1/DXN/DXT5A). DXT2/DXT4 premultiplied
   alpha is treated as straight alpha. Cost: 4–8x texture memory and
   bandwidth. Capability is probed once from BC3 format features
   (`NativeBCCompressionSupported`); desktops keep native BC.

4. **Android layer** (`os/android/app/src/main/java/com/libertyrecomp/`).
   `TU_DEBUG=sysmem` is Turnip-only (skipped for the system driver),
   launcher/driver strings mention Mali, launch report keeps working
   without kgsl.

5. **Crash backtraces** (`src/core/exception_handler_posix.cpp`).
   Some ROMs never emit tombstones for game-process crashes. On
   Android the fault handler now logs a best-effort native backtrace
   (tag `LibertyCrash`: signal, fault address, `dladdr` frames) before
   chaining to debuggerd. Fatal path only.

## Debugging notes

- Texture/pipeline `reject()` logs are sampled through
  `NextNativeTraceDiagnosticCount`, which returns `UINT64_MAX` unless
  the `native-trace` diagnostics category is on — with it off, zero
  reject lines appear. Enable briefly with `--diagnostics=true` plus
  `--diagnostics_categories=logging,native-trace` (very verbose).
- `gta4_native_dump_shaders=true` writes every registered stock
  shader's post-transform SPIR-V to `spv/` in the user folder
  (early `{hash}_{vs,ps}.spv` plus `{hash}_{vs,ps}_late_final.spv`).
- `bash os/android/scripts/fps.sh [seconds]` measures the real
  presentation rate from SurfaceFlinger.
- `simpleperf --app` is broken on HyperOS and `record -p` is denied;
  the in-process backtrace above replaces both.

## Linux builds

`os/android/scripts/{env,build_native,setup_host_tools}.sh` work on
Linux now (`npath`/`$PYTHON` helpers, `JAVA_HOME` prefers JDK 21 —
AGP 8.7's Kotlin cannot parse Java 25). Linux needs DXC 1.9
(`setup_host_tools.sh` fetches `linux_dxc_2026_09_28`): the bundled
DXC 1.8 segfaults on `ps_bink.hlsl`. Requires NDK `29.0.14206865`,
`platforms;android-35`, `build-tools;35.0.0`. Install without
`adb install -g` on HyperOS (`INSTALL_GRANT...` denial); a signature
mismatch with the GitHub APK forces `adb uninstall` first (wipes the
installed game and `args.txt`).

## Known Mali gaps

Cold shader/texture cache stutters on first sight (pipeline cache
persists under `files/LibertyRecomp/shader_cache/`), per-draw GPU
cost and resolves untuned for tile GPUs, skipped light-volume shader
(`2673E2AF`, a Turnip workaround — tested harmless on Mali),
thermals and long-session stability unmeasured.
