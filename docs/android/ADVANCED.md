# Advanced configuration

Every setting of the port lives in plain text files in the app's data folder:

```
Android/data/com.libertyrecomp/files/
├── args.txt         launch settings (cvars), read at every start
├── live_cvars.txt   settings changed while the game runs (optional)
├── env.txt          environment variables for the Vulkan driver (optional)
├── driver.txt       which Vulkan driver to load (optional)
├── drivers/NAME/    custom driver packages (optional)
├── install/         installation sources (see INSTALL.md)
├── LibertyRecomp/   installed game, saves, shader cache, profiling output
└── Liberty Recompiled/logs/   runtime log files
```

To edit them, connect the device to a PC (MTP or `adb pull` / `adb push`), or use an on-device
file manager that can write to `Android/data`. Changes to `args.txt`, `env.txt` and `driver.txt`
take effect the next time the app starts. Fully close the app: swipe it away from recents, or
`adb shell am force-stop com.libertyrecomp`.

## Launch settings: `args.txt`

One setting per line in the form `--name=value`. Lines starting with `#` are comments. On the
first launch the file is created from the defaults bundled in the APK. **Delete it to go back to
the defaults.** It is recreated on the next start.

### The default profile

These are the defaults, tuned on a Retroid Pocket 5 (Snapdragon 865):

| Setting | Default | Meaning |
|---|---|---|
| `--gta4_output_height_cap` | `720` | Output height. The game renders at 720p and the display scales it to the panel. `0` uses the panel resolution. *Restart.* |
| `--gta4_native_upscaler` | `native` | `native` renders at the output resolution. `fsr1` renders below it and upscales with AMD FSR 1. *Restart.* |
| `--gta4_fsr1_quality` | `quality` | FSR 1 render scale: `ultra_quality`, `quality` (1/1.5), `balanced` (1/1.7), `performance` (1/2). Only with `fsr1`. *Restart.* |
| `--gta4_native_anti_aliasing` | `fxaa` | `off`, `fxaa` or `smaa`. The MSAA/SSAA modes exist but are far too heavy for a phone GPU. |
| `--gta4_frame_limit` | `30` | `0` (unlocked), `30`, `40`, `60`, `120`. On a 60 Hz panel, 30 divides evenly and gives the smoothest result. |
| `--gta4_present_mode` | `vsync` | `vsync` or `immediate` (tearing, lower latency). |
| `--gta4_draw_distance_scale` | `1.0` | World draw distance. `1.0` is the console's. |
| `--gta4_dynamic_draw_distance` | `true` | Lowers the draw distance while frames miss the 30 FPS budget and restores it once there is headroom. |
| `--gta4_dynamic_draw_distance_min` | `0.80` | The lowest fraction of the draw distance the dynamic controller may use. Lower values can break dynamic shadows. |
| `--gta4_shadow_map_base_size` | `256` | Shadow map base size. `256` is the console's. `512` is sharper and costs about 4 FPS in heavy scenes. *Restart.* |
| `--gta4_shadow_distance_scale` | `1.0` | Range of the sun shadows. Values below 1.0 are raised to 1.0, because they make vehicle and pedestrian shadows sink into the ground. Above 1.0 costs GPU time. *Restart.* |
| `--gta4_reflection_resolution` | `original` | Reflection resolution: `original`, `1080p`, `full`. *Restart.* |
| `--gta4_reflection_capture_distance` | `near` | Distance of the environment reflection capture: `near`, `original`, `extended`, `far`. |
| `--gta4_traffic_density_scale` | `0.4` | Moving traffic. The PC default is 1.25, the console's is 1.0. *Restart.* |
| `--gta4_parked_car_density_scale` | `0.4` | Parked cars. *Restart.* |
| `--gta4_ped_density_scale` | `0.4` | Pedestrians. *Restart.* |
| `--gta4_scenario_ped_density_scale` | `0.4` | Scenario pedestrians (people sitting, working and so on). *Restart.* |
| `--gta4_motion_blur` | `false` | The title's motion blur. |
| `--gta4_native_max_queued_frames` | `3` | How many frames the game may run ahead of the renderer (1–4). Lower values cut latency and cost throughput. |
| `--gta4_fps_guard` | `0` | `1`–`3`: update the environment reflection only every other frame while the game misses 30 FPS. Shadows are never reduced. |
| `--gta4_profile_native_detailed_cpu` / `_gpu` | `false` | Renderer profiling scopes, for development only. |
| `--log_level` | `warn` | Log verbosity, see [Logs](#logs). |

*Restart* means the value is only read at startup. The other settings can also be changed while the
game is running (see [live tuning](#live-tuning-live_cvarstxt)).

Population density and draw distance mostly cost CPU time. Resolution, anti-aliasing, shadows and
reflections mostly cost GPU time. On the Snapdragon 865 the default profile is GPU-bound in the
heaviest places.

### Ready-made profiles

Paste one of these over the matching lines in `args.txt`. The frame rates are from a Retroid
Pocket 5 in the heaviest spot found (a bridge with a far view of the city at night).

**Default: native 720p.** About 25–29 FPS in the heaviest spot and a steady 30 elsewhere.

```
--gta4_output_height_cap=720
--gta4_native_upscaler=native
```

**Sharper: 1080p output, FSR 1 from 720p.** Similar frame rate, slightly sharper on a 1080p
panel, with FSR's sharpening look.

```
--gta4_output_height_cap=1080
--gta4_native_upscaler=fsr1
--gta4_fsr1_quality=quality
```

**Native 1080p.** About 20–25 FPS. Feels like the console in heavy scenes.

```
--gta4_output_height_cap=0
--gta4_native_upscaler=native
```

**Faster: lower internal resolution.** For weaker Adreno chips.

```
--gta4_output_height_cap=720
--gta4_native_upscaler=fsr1
--gta4_fsr1_quality=performance
--gta4_native_anti_aliasing=off
--gta4_dynamic_draw_distance_min=0.55
--gta4_reflection_aa=off
```

**Console-like world.** More traffic and people, at a CPU cost. Fine on faster chips than the 865.

```
--gta4_traffic_density_scale=1.0
--gta4_parked_car_density_scale=1.0
--gta4_ped_density_scale=1.0
--gta4_scenario_ped_density_scale=1.0
```

### Other useful settings

| Setting | Meaning |
|---|---|
| `--android_surface=WxH` | Fixes the size of the Android surface, for example `1280x720`. The display hardware scales it to the panel. Read by the app before the game starts. Currently broken together with `--gta4_output_height_cap`: the picture is drawn into a corner of the screen. |
| `--gta4_reflection_aa=off` | No MSAA on the low-resolution reflection captures. Saves some GPU time at no visible cost. *Restart.* |
| `--gta4_stale_object_guard=true` | Default on. Prevents a crash when the game frees an object another thread still uses (see [PORTING.md](PORTING.md#5-stability)). |
| `--gta4_native_skip_pixel_shaders=2673E2AF` | Default. Draws using these pixel shader hashes are skipped. `2673E2AF` is a deferred light-volume shader that hangs the GPU under Turnip. Set it to an empty value to test it on another driver. |
| `--gta4_native_water_reflection_min_indices=0` | Default 0 (off). Skips draws with fewer indices than this in the water reflection, for example `300`. Small objects disappear from the reflection; at the bridge the reflection is ~500 mostly small draws. Live. |
| `--gta4_native_shadow_min_indices=0` | Default 0 (off). Skips draws with fewer indices than this in the shadow map passes. Small objects lose their shadows. Live. |
| `--gta4_native_vertex_cache_order=true` | Default on. Reorders triangles of opaque draws for the GPU vertex cache on a background thread. Same image, slightly less GPU time. Live. |
| `--android_frame_rate_hint=true` | Default on. Asks Android for a display refresh rate that is a multiple of the frame limit (`Surface.setFrameRate`, Android 11+), so adaptive 90/120 Hz panels (Samsung "Adaptive" motion smoothness, LTPO phones) show every frame for the same number of refreshes instead of juddering. Follows the in-game frame limit live; an unlocked limit clears the request. Live. |
| `--android_perf_hint=true` | Default on. Reports each frame's CPU work to Android's performance hint API (Android 13+) so clocks follow the frame budget instead of running at the maximum. The budget follows the frame limit: 90% of its period, at most `--android_perf_hint_target_ms` (default 30). On Android 14+ the session moves to the busiest threads instead of being recreated. Live. |
| `--android_thermal_guard` | Set by the launcher's **Thermal guard** check box: lowers the draw distance when Android forecasts throttling and restores it after cooling down. On by default for GPUs other than Adreno (Samsung Xclipse, Mali). |
| `--android_pin_hot_threads=false` | Default off. Keeps the busiest threads on the big CPU cores. It made those threads faster on the Retroid Pocket 5 but did not raise the frame rate. Live. |
| `--install_game_source=PATH` / `--install_update_source=PATH` | Installation sources, instead of the `install/` folder. |
| `--audio_maxqframes=32` | Audio blocks queued ahead of playback (1-64, each ~5.3 ms), which is also the audio delay. Default 32 (~170 ms) on Android since 0.5.7.7; it was 64 (~340 ms behind the picture). 16 (~85 ms) is tighter but can stutter during frame hitches on slower devices; raise it if the sound crackles. *Restart.* |
| `--touch_controls=auto` | On-screen controls. `auto` shows them whenever no gamepad is connected, `on` always shows them, `off` never does. |
| `--gta4_touch_layout=gamepad` | `gamepad` (default): a fixed on-screen Xbox 360 controller with floating sticks. `context`: upstream's context-sensitive touch layout. |
| `--touch_controls_controller_only=true` | Default on Android. In `auto` mode, only a game controller hides the touch controls. Keyboards and mice are ignored, because handhelds report their built-in buttons as both. |

> [!WARNING]
> Do **not** set `--gta4_shadow_cascade_count`. The quality-table field it patches is not really
> the cascade count. Any value below the stock one removes pedestrian and vehicle shadows and makes
> tree shadows flicker.

Every renderer feature added for Android sits behind its own setting, so a regression can be
switched off without a rebuild. [`os/android/android_args/rewrite_off.txt`](../../os/android/android_args/rewrite_off.txt)
turns off all of the renderer CPU optimizations from 0.5. If a new version misbehaves where an old
one did not, append those lines to `args.txt`.

## Live tuning: `live_cvars.txt`

While the game runs, it checks `files/live_cvars.txt` once a second. Whenever the file changes,
every `--name=value` line in it is applied immediately. This is the fastest way to compare
settings in the same spot:

```
adb shell "echo --gta4_draw_distance_scale=0.8 > /sdcard/Android/data/com.libertyrecomp/files/live_cvars.txt"
```

Every applied or rejected line is logged as `live-cvar name=value applied|rejected` (tag
`LibertyRecomp`). Settings marked *Restart* above are accepted but do nothing until the next launch.

> [!IMPORTANT]
> `live_cvars.txt` is **not** deleted when the game exits, and it is applied again on the next
> launch. Delete it when you are done experimenting, and copy the values you liked into
> `args.txt`.

From a PC with the repository checked out, `os/android/scripts/live_cvar.sh --name=value ...` does
the same thing and prints the result.

## Driver selection: `driver.txt`

The game needs Vulkan 1.2. The APK bundles a Mesa Turnip build (StevenMXZ 26.3.0-R6, for
Adreno 6xx/7xx) and loads it through libadrenotools. Without root, this replaces the system driver
for this app only.

Since 0.5.6.3 the driver is chosen on the [launcher](INSTALL.md#2-open-the-app-the-launcher)
screen, which also imports driver zips. The launcher writes your choice to `driver.txt`, and the
file can still be edited by hand. It holds a single line:

| Content | Driver |
|---|---|
| *(no file)* or `turnip` | The bundled Turnip build (default). |
| `system` | The device's own Vulkan driver. The stock Adreno 650 driver only offers Vulkan 1.1, so on the Snapdragon 865 the game will not start this way. It may work on newer Adreno drivers that offer Vulkan 1.3. |
| `custom:NAME` | A driver package you provide in `files/drivers/NAME/`. |

A custom driver package is a folder with the driver library (`.so`) and, optionally, the
`meta.json` of an AdrenoTools/Turnip release zip. **Import driver (.zip)** on the launcher does the
unpacking. By hand, unpack the zip into `files/drivers/NAME/` and put `custom:NAME` in
`driver.txt`. If `meta.json` names a `libraryName`, that library is used.
Otherwise the folder must contain exactly one `.so`.

A custom driver is native code that runs inside the game, so the app only loads a package the
player has accepted: a zip imported on the launcher is accepted as it is imported. A package unpacked
by hand, or one whose files changed afterwards (replaced with a file manager or over USB), makes the
launcher ask **Trust and use** or **Use bundled Turnip** when it is selected or before Play. The
SHA-256 of every accepted file is kept in the app's private storage, and the game checks the copy it
loads against it; a package that does not match is not loaded (the game falls back to the bundled
Turnip and logs why under `LibertyDriver`). `libraryName` in `meta.json` must be a plain file name.

If the bundled Turnip or a custom driver fails to load, the app falls back to the system driver
(since 0.5.6.2) and records both in the log (tag `LibertyDriver`) and in `files/last_launch.txt`.
If the system driver offers less than Vulkan 1.2, the app shows a dialog with the reason and
exits. An explicit `system` has nothing to fall back to.

The file may be saved with any editor: upper and lower case, surrounding spaces and the invisible
byte-order mark that Windows Notepad adds are all ignored. The same applies to `args.txt` and
`env.txt`.

Drivers tried on the Adreno 650:

- Turnip 26.3.0-R6 (bundled): the fastest.
- Turnip T30 / 26.3.0 (Mr_Purple): about 10% slower.
- ETK 26.1.3: about the same speed as R6.
- 26.3.0-r2 (Banners): hung the GPU.

## Driver environment: `env.txt`

`NAME=value` lines are exported before the Vulkan driver loads. `#` starts a comment. By default
the app sets `TU_DEBUG=sysmem`, which makes Turnip render directly to system memory instead of the
tiled GMEM path. That is clearly faster for this renderer's many small passes on the Adreno 650.
To try the tiled path:

```
TU_DEBUG=
```

`LIBERTY_FORCE_MEMFD=1` makes the game allocate its emulated Xbox 360 memory with a memfd instead
of Android's shared-memory API. The game already falls back to it by itself when the shared-memory
call fails (0.5.7.4 and newer), so this is only a workaround to try if the game fails with
"Unable to reserve the 4gb guest address space".

Any other Mesa/Turnip variable works the same way, for example `MESA_SHADER_CACHE_DISABLE=true` or
`TU_DEBUG=sysmem,noubwc`.

## Logs

The game logs to Android's logcat:

```
adb logcat -s LibertyRecomp LibertyDriver SDL DEBUG
```

By default only warnings and errors from the Android layer appear. The runtime's own log lines
need **both** of these lines in `args.txt`:

```
--diagnostics=true
--diagnostics_categories=logging
--log_level=info
```

> [!WARNING]
> `--diagnostics_categories` without `--diagnostics=true` makes the runtime exit immediately.
> Add or remove these two lines together.

Crashes are logged by Android's `DEBUG` tag with a native backtrace. Since 0.5.7.3 the launcher
also turns every crash of the game process into `files/crash_reports/crash-DATE.txt`, from what
Android keeps about it (ApplicationExitInfo and the tombstone): the signal, the crashing thread's
backtrace, the last log lines of the process, the device and driver details, and the end of the
runtime log. The raw tombstone is saved next to it as `.tombstone.pb`. The runtime writes its own
log files to `files/Liberty Recompiled/logs/`. Since 0.5.7.3 this log is on by default, at the
`--log_level` from `args.txt`, unless `args.txt` sets `--diagnostics` itself. Every start also
writes the device, GPU and driver details to `files/last_launch.txt`. Without a PC, a full logcat can be captured with
*Developer options > Take bug report*. Include these when you report a problem on the
[Issues](https://github.com/vaduur/LibertyRecompAndroid/issues) page, along with the
device model, the Android version and your `args.txt`.

## Measuring performance

`os/android/scripts/fps.sh [seconds]` reads the game's real presentation rate from SurfaceFlinger.
It needs a PC with the repository and adb. `os/android/scripts/gpu_counters.sh` and
`os/android/tools/kgslperf.c` read the Adreno GPU busy counters.

When comparing settings, measure in the same place at the same time of day. Night scenes and day
scenes load the GPU very differently, and shadows are only visible in daylight.
