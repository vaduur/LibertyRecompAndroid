# Building the Android APK

The build runs on **Windows with Git Bash** and on **Linux** (and macOS with the
host-tool paths adjusted). The scripts pick the host tools and the interpreter
themselves: `env.sh` exports `PYTHON` (`python3` on Linux, `python` on Git Bash)
and `setup_host_tools.sh` downloads the matching DXC and glslang builds.

The generated C++ for the game is checked in, as it is upstream, so building needs no game files.

## Prerequisites

| Tool | Version used |
|---|---|
| Git for Windows (Git Bash) *or* Linux | any recent |
| Python | 3.12 |
| CMake | 3.31 (3.29 or newer is required) |
| Ninja | 1.11 |
| Android Studio (Windows) *or* the SDK command-line tools (Linux) | for the SDK, the platform tools and a JDK 17+ |
| Android SDK platform | 35 |
| **Android NDK** | **r29, `29.0.14206865`** (older NDKs lack `std::jthread` in libc++) |

`os/android/scripts/env.sh` finds the SDK at `%LOCALAPPDATA%\Android\Sdk` on
Windows and at `$HOME/Android/Sdk` elsewhere, and the JDK in Android Studio's
`jbr` on Windows or in `/usr/lib/jvm` on Linux. Override `ANDROID_HOME`,
`ANDROID_NDK`, `NDK_VER`, `JAVA_HOME` or `PYTHON` if yours are elsewhere.

On Linux the SDK command-line tools are enough:

```bash
sdkmanager "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;29.0.14206865"
```

## Steps

All commands run from the repository root, in Git Bash or a plain Linux shell.

```bash
# 1. Fetch the pinned dependencies and apply the source patches (as upstream)
python3 tools/setup_repo.py

# 2. Build the Vulkan driver proxy and the libadrenotools hooks (once)
bash os/android/scripts/build_driver_proxy.sh

# 3. Build the game, the runtime and the renderer, and stage them into jniLibs.
#    The first run also downloads DXC and glslang into out/host-tools.
#    STRIP=1 strips the libraries (about a 50 MB APK instead of about 170 MB).
STRIP=1 bash os/android/scripts/build_native.sh

# 4. Package the APK (debug-signed)
rm -f os/android/app/build/outputs/apk/debug/app-debug.apk
bash os/android/scripts/build_apk.sh

# 5. Install on the connected device and start the game
#    (through the launcher with "--ez play true", which presses Play at once)
bash os/android/scripts/install.sh
```

The output is `os/android/app/build/outputs/apk/debug/app-debug.apk`.

Notes:

- The first native build compiles about 90 large generated source files and takes a long time.
  Later builds are incremental.
- **Delete the old APK before step 4.** Gradle's incremental packaging otherwise leaves tens of
  megabytes of dead space in it.
- Leave `STRIP` unset for development builds. Unstripped libraries let `ndk-stack`,
  `os/android/scripts/symbolize.sh` and simpleperf resolve symbols.
- The build type defaults to `RelWithDebInfo` (`BUILD_TYPE=Release` builds into a separate
  folder).
- `-DLIBERTY_ANDROID_PGO=generate` / `=<profile.profdata>` in `build_native.sh`'s configure step
  enables profile-guided optimization (see [PORTING.md](PORTING.md#1-building-for-android)).
- APKs built on different machines are signed with different debug keys. A device then has to
  uninstall the old build before installing the new one, which deletes the game and saves (see
  [INSTALL.md](INSTALL.md#upgrading-to-a-newer-version)).

## Development helpers

| Script | Purpose |
|---|---|
| `logs.sh` | Follow the game's logcat output (`--dump` saves it, `--pull` also fetches the runtime's log files) |
| `push_args.sh <file>` | Push a settings profile as `args.txt` (`--clear` removes it) |
| `push_install_sources.sh <iso> [<tu>]` | Copy the installation sources to the device and preselect them |
| `live_cvar.sh --name=value ...` | Change settings in the running game |
| `fps.sh [seconds]` | Measure the presented frame rate through SurfaceFlinger |
| `cpu_frame.sh`, `gpu_counters.sh` | CPU and Adreno GPU profiling helpers |
| `symbolize.sh` | Symbolize a native crash backtrace |

`os/android/android_args/` holds the settings profiles used during development:
`perf.txt` (older handheld profile), `debug.txt` (verbose logging) and `rewrite_off.txt`
(disables the 0.5 renderer CPU changes).
