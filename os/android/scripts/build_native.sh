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

"$PYTHON" "$APP_DIR/scripts/materialize_symlinks.py"
"$APP_DIR/scripts/setup_host_tools.sh"

# Snapdragon 865 and every Android 10+ arm64 device we target are ARMv8.2:
# inline LSE atomics instead of outline-atomic calls, and native ELF TLS
# (API 29+) instead of emulated TLS. Both are hot in the guest thread.
CPU_FLAGS="-march=armv8.2-a -fno-emulated-tls"
# Shader-override validation tools. On Windows these are the .exe builds
# fetched by setup_host_tools.sh; on Linux CMake auto-detects the bundled
# dxc-linux plus any system glslangValidator/spirv-val, so only spirv-val
# from the NDK is pinned when present.
SHADER_TOOL_ARGS=()
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    SHADER_TOOL_ARGS+=(
      "-DGTA4_NATIVE_SPIRV_VAL=$(npath "$ANDROID_NDK/shader-tools/windows-x86_64/spirv-val.exe")"
      "-DGTA4_NATIVE_GLSLANG_VALIDATOR=$(npath "$REPO/out/host-tools/glslang/bin/glslangValidator.exe")"
      "-DGTA4_NATIVE_DXC_LIBRARY_PATH=$(npath "$REPO/out/host-tools/dxc/bin/x64")"
    )
    ;;
  *)
    if [ -x "$ANDROID_NDK/shader-tools/linux-x86_64/spirv-val" ]; then
      SHADER_TOOL_ARGS+=("-DGTA4_NATIVE_SPIRV_VAL=$ANDROID_NDK/shader-tools/linux-x86_64/spirv-val")
    fi
    if command -v glslangValidator >/dev/null 2>&1; then
      SHADER_TOOL_ARGS+=("-DGTA4_NATIVE_GLSLANG_VALIDATOR=$(command -v glslangValidator)")
    fi
    # DXC 1.9 for Linux (setup_host_tools.sh); the bundled 1.8 segfaults on
    # the Bink pixel shaders. Layout: dxc-linux/{bin/dxc,lib/}.
    if [ -x "$REPO/out/host-tools/dxc-linux/bin/dxc" ]; then
      SHADER_TOOL_ARGS+=("-DGTA4_NATIVE_DXC_LIBRARY_PATH=$REPO/out/host-tools/dxc-linux")
    fi
    ;;
esac
# Re-run CMake configure when the shader-tool set changes (e.g. glslangValidator
# installed after the first configure): the override compiler bakes the tool
# paths into the build, and without them the build fails at 848/1049.
SHADER_TOOL_STAMP="$BUILD_DIR/shader_tools.args"
if [ ! -f "$BUILD_DIR/build.ninja" ] || ! grep -q -- "-fno-emulated-tls" "$BUILD_DIR/CMakeCache.txt" \
   || [ ! -f "$SHADER_TOOL_STAMP" ] || [ "$(cat "$SHADER_TOOL_STAMP")" != "${SHADER_TOOL_ARGS[*]}" ]; then
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
    "${SHADER_TOOL_ARGS[@]}"
  printf '%s' "${SHADER_TOOL_ARGS[*]}" > "$SHADER_TOOL_STAMP"
fi

# Calls between unhooked recompiled functions bypass the weak sub_X aliases so
# the compiler can inline them (idempotent; a new codegen loses the rewrite).
# Off by default: no measurable gain at the bridge, neither before registers
# became C++ locals nor after (2026-10-10, with functions named by address in
# the sources kept indirect as nfsmw-android does), and the first build with
# them crashed in game code. DIRECT_CALLS_ENABLE=1 turns them on.
DIRECT_CALLS="$REPO/glue/rexglue-sdk-main/gta4-recomp/tools/direct_calls.py"
if [ "${DIRECT_CALLS_ENABLE:-0}" = "1" ]; then
  "$PYTHON" "$(npath "$DIRECT_CALLS")" apply
else
  "$PYTHON" "$(npath "$DIRECT_CALLS")" undo
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
"$PYTHON" "$(npath "$DIRECT_CALLS")" check "$(npath "$NM_OUT")" || {
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
