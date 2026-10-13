# Mali build (Dimensity 8400 and other Mali / Immortalis GPUs)

> [!WARNING]
> **Untested.** This build was prepared without access to a Mali device. The upstream port has
> only ever been run on Adreno. Everything below describes what the Mali build changes and what to
> check first; nothing here is a confirmed result. Please report what you see in
> [Issues](https://github.com/vaduur/LibertyRecompAndroid/issues).

Target: MediaTek **Dimensity 8400** (Mali-G720 MC7), Android 14 or newer. Other Mali and Immortalis
parts with a Vulkan 1.2+ driver should behave the same way.

## Why the normal build does not work

The default APK loads a bundled **Mesa Turnip** Vulkan driver through libadrenotools. Turnip exists
only for Adreno GPUs, so on a Mali phone it cannot load. The game also needs **Vulkan 1.2** (plus
dynamic rendering), which on Mali has to come from the phone's own driver.

The app already had a *system driver* mode and a fallback to it (0.5.6.2). The Mali build makes
that the default instead of a fallback.

## What the Mali build changes

| | Adreno build (default) | Mali build |
|---|---|---|
| Default driver (no `driver.txt`) | bundled Turnip | the device's own Vulkan driver |
| `TU_DEBUG=sysmem` | set | not set (Turnip-only variable) |
| Version name | `0.5.8` | `0.5.8-mali` |
| Launcher driver list | Turnip marked recommended | System driver marked recommended |
| Error text on an unusable GPU | "Supported GPUs: Adreno 6xx and 7xx" | Mali-specific text |

Both builds can still select any driver on the launcher, and an existing `driver.txt` always wins.

The driver proxy also gained a **capability report** in both builds. For every Vulkan device it now
records whether the driver offers the features the renderer refuses to start without
(`shaderInt64`, `depthClamp`, `shaderClipDistance`, `sampleRateShading`, `independentBlend`,
`runtimeDescriptorArray`, `descriptorBindingPartiallyBound`, `bufferDeviceAddress`). A driver that
explicitly lacks one now gets a dialog naming it, instead of a renderer that fails inside the game.
The values are in `files/last_launch.txt` under `driver status:` (`1` present, `0` missing, `-1`
could not be queried).

## Building

Same steps as [BUILDING.md](BUILDING.md); only the packaging step differs:

```bash
LIBERTY_GPU=mali bash os/android/scripts/build_apk.sh
```

or, directly: `./gradlew assembleDebug -PlibertyGpu=mali`. The native libraries
(`build_native.sh`) are identical for both builds.

### Building on GitHub (no PC)

`.github/workflows/build-android-apk.yml` builds the APK on a GitHub Windows runner. It runs by
itself on a push to the `mali-dimensity-8400` branch, or from the **Actions** tab with **Run
workflow** (choose `mali` or `adreno`). The APK appears under **Artifacts** on the finished run.
On a fork, Actions must first be enabled once from the Actions tab. The native build is very
large, so the first run takes hours; the build folder is cached, so later runs are faster. This
workflow has not been run yet, so expect to fix a first failure.

The package name is unchanged (`com.libertyrecomp`), so the Mali APK replaces an Adreno one and
shares the installed game and saves. Add `applicationIdSuffix = ".mali"` in
`os/android/app/build.gradle.kts` if you want both side by side (each would then need its own
game install, about 7 GB).

## First launch: what to check

1. Open the launcher. **System driver** should be selected.
2. Press Play. If the game does not start, read `Android/data/com.libertyrecomp/files/last_launch.txt`.
   It records the SoC, the Vulkan device and driver, the capability report and the result.
3. For a live log: `adb logcat -s LibertyRecomp LibertyDriver SDL DEBUG`. The renderer logs why it
   rejects a device (for example `doesn't support shaderInt64`) at warning level.

## Things that may still go wrong

These are open questions, not known bugs:

- **The skipped light-volume shader.** Pixel shader `2673E2AF` is skipped by default because it
  hangs the GPU under Turnip on Adreno. Whether it hangs a Mali driver is unknown. To test, put
  `--gta4_native_skip_pixel_shaders=` (empty) in `args.txt`. A few light volumes are missing while
  it is skipped.
- **Performance tuning is Adreno-specific.** Defaults (720p, 30 FPS, draw distance controller)
  were measured on an Adreno 650. Mali is also tile-based, and the renderer's many small passes and
  resolves may cost differently there.
- **Driver bugs.** The renderer works around two Turnip bugs (pipelines with no fragment shader,
  GPU hangs). Mali drivers will have their own; expect visual glitches to need new workarounds.
- If shaders misrender, these existing settings are worth a try one at a time (they change
  shader generation, so they are guesses, not fixes): `--gta4_native_ieee_mul=0`,
  `--gta4_native_early_fragment_tests=false`, `--gta4_native_loop_watchdog=false`.
  See [ADVANCED.md](ADVANCED.md).

When reporting a problem, attach `last_launch.txt` and, if the game started, a debug log from
**Play (Debug Log)** on the launcher.
