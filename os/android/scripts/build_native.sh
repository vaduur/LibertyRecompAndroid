#!/bin/bash
# Configure (once), build and stage the native side for the APK:
#   jniLibs/arm64-v8a  libmain.so, librexruntime*.so, librexgpu-*.so,
#                      libSDL3*.so, libc++_shared.so
#   assets/Resources   fonts, button prompts and the RPF key
#
# usage: build_native.sh [jobs]      (default: number of CPUs - 2)
set -euo pipefail
. "$(dirname "$0")/env.sh"
# The generated gta4-recomp sources are around 2 MB each and clang wants
# several GB apiece, so scaling the job count with the core count alone
# exhausts memory and takes the machine down with it. Cap the default and
# pass an explicit count to go faster on a machine with RAM to spare.
_cores=$(nproc)
[ "$_cores" -gt 6 ] && _cores=6
JOBS="${1:-$(( _cores / 2 > 0 ? _cores / 2 : 1 ))}"

[ -d "$ANDROID_NDK" ] || { echo "no NDK at $ANDROID_NDK"; exit 1; }

"$PYTHON" "$APP_DIR/scripts/materialize_symlinks.py"
"$APP_DIR/scripts/setup_host_tools.sh"

# Snapdragon 865 and every Android 10+ arm64 device we target are ARMv8.2:
# inline LSE atomics instead of outline-atomic calls, and native ELF TLS
# (API 29+) instead of emulated TLS. Both are hot in the guest thread.
CPU_FLAGS="-march=armv8.2-a -fno-emulated-tls"
if [ ! -f "$BUILD_DIR/build.ninja" ] || ! grep -q -- "-fno-emulated-tls" "$BUILD_DIR/CMakeCache.txt"; then
  echo "== configuring $BUILD_TYPE in $BUILD_DIR"
  cmake -S "$REPO" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_FLAGS="$CPU_FLAGS" \
    -DCMAKE_CXX_FLAGS="$CPU_FLAGS" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/toolchains/android.cmake" \
    -DANDROID_NDK="$ANDROID_NDK" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-28 \
    -DANDROID_STL=c++_shared \
    -DLIBERTY_RECOMP_TARGET_PLATFORM=android \
    -DLIBERTY_RECOMP_ANDROID_RUNTIME_ASSETS=ON \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DREXGLUE_ENABLE_TRACY=OFF \
    -DGTA4_NATIVE_SPIRV_VAL="$(winpath "$ANDROID_NDK/shader-tools/$_host/spirv-val$_exe")" \
    -DGTA4_NATIVE_GLSLANG_VALIDATOR="$(winpath "$REPO/out/host-tools/glslang/bin/glslangValidator.exe")" \
    -DGTA4_NATIVE_DXC_LIBRARY_PATH="$(winpath "$REPO/out/host-tools/dxc/bin/x64")"
fi

# Calls between unhooked recompiled functions bypass the weak sub_X aliases so
# the compiler can inline them (idempotent; a new codegen loses the rewrite).
# Off by default: no measurable gain at the bridge, neither before registers
# became C++ locals nor after (2026-10-10, with functions named by address in
# the sources kept indirect as nfsmw-android does), and the first build with
# them crashed in game code. DIRECT_CALLS_ENABLE=1 turns them on.
DIRECT_CALLS="$REPO/glue/rexglue-sdk-main/gta4-recomp/tools/direct_calls.py"
# cygpath only exists under MSYS; elsewhere the path is already native.
DIRECT_CALLS_PATH="$(cygpath -m "$DIRECT_CALLS" 2>/dev/null || echo "$DIRECT_CALLS")"
if [ "${DIRECT_CALLS_ENABLE:-0}" = "1" ]; then
  "$PYTHON" "$DIRECT_CALLS_PATH" apply
else
  "$PYTHON" "$DIRECT_CALLS_PATH" undo
fi
echo "== building with $JOBS jobs"
# Every behaviour-changing cvar must be in the hack registry (safe mode).
HACK_REGISTRY_CHECK="$REPO/glue/rexglue-sdk-main/gta4-recomp/tools/check_hack_registry.py"
if [ -f "$HACK_REGISTRY_CHECK" ]; then
  "$PYTHON" "$HACK_REGISTRY_CHECK" || { echo "!! hack registry check failed"; exit 1; }
else
  echo "-- skipping hack registry check: $HACK_REGISTRY_CHECK is not in the repository"
fi
cmake --build "$BUILD_DIR" --target LibertyRecomp -- -j"$JOBS" || { echo "!! native build failed"; exit 1; }

echo "== staging libraries"
mkdir -p "$JNILIBS"
# Only the engine's libraries; the driver proxy's come from build_driver_proxy.sh.
rm -f "$JNILIBS"/libmain.so "$JNILIBS"/libliberty_install.so "$JNILIBS"/librexruntime.so "$JNILIBS"/librexgpu-*.so       "$JNILIBS"/libSDL3.so "$JNILIBS"/libc++_shared.so
for so in "$BUILD_DIR"/glue/gta4-recomp/libmain.so \
          "$BUILD_DIR"/glue/gta4-recomp/libliberty_install.so "$REX_OUT_DIR"/librexruntime.so \
          "$REX_OUT_DIR"/librexgpu-*.so "$REX_OUT_DIR"/libSDL3.so; do
  [ -f "$so" ] || continue
  # Packaged unstripped by default so ndk-stack and simpleperf can resolve
  # frames. STRIP=1 makes a smaller APK.
  if [ "${STRIP:-0}" = "1" ]; then
    "$NDK_BIN/llvm-strip" --strip-unneeded -o "$JNILIBS/$(basename "$so")" "$so"
  else
    cp "$so" "$JNILIBS/"
  fi
done
cp "$NDK_SYSROOT_LIB/libc++_shared.so" "$JNILIBS/"

echo "== checks"
# A hook added after the hooked list was taken must not be bypassed.
NM_OUT="$(mktemp)"
"$NDK_BIN/llvm-nm" --defined-only "$BUILD_DIR/glue/gta4-recomp/libmain.so" > "$NM_OUT"
"$PYTHON" "$DIRECT_CALLS_PATH" check "$(winpath "$NM_OUT")" || {
  echo "!! run: direct_calls.py hooks <llvm-nm libmain.so> && direct_calls.py apply, then rebuild"; exit 1; }
rm -f "$NM_OUT"
"$NDK_BIN/llvm-nm" -D --defined-only "$JNILIBS/libmain.so" | grep -E ' SDL_main$' >/dev/null || {
  echo "!! SDL_main is not exported from libmain.so"; exit 1; }
# Every DT_NEEDED entry must be either staged here or a system library.
for so in "$JNILIBS"/*.so; do
  for need in $("$NDK_BIN/llvm-readelf" -d "$so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
    case "$need" in
      libc.so|libm.so|libdl.so|liblog.so|libandroid.so|libvulkan.so|libEGL.so|libGLESv2.so|libGLESv3.so|libOpenSLES.so|libaaudio.so|libz.so|libmediandk.so|libnativewindow.so|libjnigraphics.so) ;;
      *) [ -f "$JNILIBS/$need" ] || { echo "!! $(basename "$so") needs $need, which is not staged"; exit 1; } ;;
    esac
  done
done
ls -la "$JNILIBS"

echo "== staging resources"
RES="$ASSETS/Resources"
rm -rf "$RES"
mkdir -p "$RES/button_prompts"
cp -r "$REPO/LibertyRecompLib/font_atlases" "$RES/"
cp -r "$REPO/LibertyRecompLib/private/button_prompts/." "$RES/button_prompts/"
[ -f "$REPO/LibertyRecompLib/aes_key.bin" ] && cp "$REPO/LibertyRecompLib/aes_key.bin" "$RES/"
du -sh "$RES"
echo "== done"
