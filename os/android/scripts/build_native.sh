#!/bin/bash
# Configure (once), build and stage the native side for the APK:
#   jniLibs/arm64-v8a  libmain.so, librexruntime*.so, librexgpu-*.so,
#                      libSDL3*.so, libc++_shared.so
#   assets/Resources   fonts, button prompts and the RPF key
#
# usage: build_native.sh [jobs]      (default: number of CPUs - 2)
set -euo pipefail
. "$(dirname "$0")/env.sh"
JOBS="${1:-$(( $(nproc) > 3 ? $(nproc) - 2 : 1 ))}"

[ -d "$ANDROID_NDK" ] || { echo "no NDK at $ANDROID_NDK"; exit 1; }

python "$APP_DIR/scripts/materialize_symlinks.py"
"$APP_DIR/scripts/setup_host_tools.sh"

# Snapdragon 865 and every Android 10+ arm64 device we target are ARMv8.2:
# inline LSE atomics instead of outline-atomic calls, and native ELF TLS
# (API 29+) instead of emulated TLS. Both are hot in the guest thread.
#
# LIBERTY_CPU picks the CPU target of the native code:
#   compatible (default)  ARMv8.2 instructions, so it runs on every Android 10+
#                         arm64 phone, with instruction scheduling tuned for
#                         Cortex-A720-class cores (the Dimensity 8400's A725).
#   armv9                 The Cortex-A720/A725 instruction set (ARMv9.2). Faster
#                         on the Dimensity 8400 and newer, but the APK crashes
#                         on phones with older cores (e.g. Cortex-A78, A77).
case "${LIBERTY_CPU:-compatible}" in
  armv9) CPU_TARGET="-mcpu=cortex-a720" ;;
  compatible) CPU_TARGET="-march=armv8.2-a -mtune=cortex-a720" ;;
  *) echo "unknown LIBERTY_CPU '$LIBERTY_CPU' (use compatible or armv9)"; exit 1 ;;
esac
CPU_FLAGS="$CPU_TARGET -fno-emulated-tls"
echo "== CPU target: ${LIBERTY_CPU:-compatible} ($CPU_FLAGS)"
# Reconfigure whenever the flags differ from the cached configuration, so a
# new CPU target really recompiles everything instead of reusing old objects.
if [ ! -f "$BUILD_DIR/build.ninja" ] || \
   ! grep -qxF -- "CMAKE_CXX_FLAGS:STRING=$CPU_FLAGS" "$BUILD_DIR/CMakeCache.txt"; then
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
    -DGTA4_NATIVE_SPIRV_VAL="$(cygpath -m "$ANDROID_NDK/shader-tools/windows-x86_64/spirv-val.exe")" \
    -DGTA4_NATIVE_GLSLANG_VALIDATOR="$(cygpath -m "$REPO/out/host-tools/glslang/bin/glslangValidator.exe")" \
    -DGTA4_NATIVE_DXC_LIBRARY_PATH="$(cygpath -m "$REPO/out/host-tools/dxc/bin/x64")"
fi

# Calls between unhooked recompiled functions bypass the weak sub_X aliases so
# the compiler can inline them (idempotent; a new codegen loses the rewrite).
# Off by default: no measurable gain at the bridge, neither before registers
# became C++ locals nor after (2026-10-10, with functions named by address in
# the sources kept indirect as nfsmw-android does), and the first build with
# them crashed in game code. DIRECT_CALLS_ENABLE=1 turns them on.
DIRECT_CALLS="$REPO/glue/rexglue-sdk-main/gta4-recomp/tools/direct_calls.py"
if [ "${DIRECT_CALLS_ENABLE:-0}" = "1" ]; then
  python "$(cygpath -m "$DIRECT_CALLS")" apply
else
  python "$(cygpath -m "$DIRECT_CALLS")" undo
fi
echo "== building with $JOBS jobs"
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
python "$(cygpath -m "$DIRECT_CALLS")" check "$(cygpath -m "$NM_OUT")" || {
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
