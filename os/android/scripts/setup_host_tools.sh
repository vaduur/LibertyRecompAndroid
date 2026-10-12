#!/bin/bash
# Host shader tools the gta4-native override cache needs at build time.
#   - DXC: the copy bundled in tools/XenosRecomp (1.8.2407) crashes on the
#     Bink pixel shaders, so a newer official release is used.
#   - glslangValidator: GLSL overrides (motion blur).
#   - spirv-val ships with the NDK and needs no download.
# Everything lands in out/host-tools inside the repository.
set -euo pipefail
. "$(dirname "$0")/env.sh"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ;;
  *)
    # Linux: DXC 1.9 release build. The DXC 1.8 bundled in
    # tools/XenosRecomp/thirdparty/dxc-bin segfaults on GTA IV's Bink pixel
    # shaders (same crash as on Windows, see below), so fetch the 1.9 Linux
    # tarball from the same release the Windows zip comes from.
    DEST="$REPO/out/host-tools"
    DXC_LINUX_URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz"
    mkdir -p "$DEST"
    if [ ! -x "$DEST/dxc-linux/bin/dxc" ]; then
      echo "-- fetching DXC 1.9 for Linux"
      curl -fsSL -o "$DEST/dxc-linux.tar.gz" "$DXC_LINUX_URL"
      mkdir -p "$DEST/dxc-linux"
      tar -xzf "$DEST/dxc-linux.tar.gz" -C "$DEST/dxc-linux"
    fi
    echo "-- DXC: $(LD_LIBRARY_PATH="$DEST/dxc-linux/lib" "$DEST/dxc-linux/bin/dxc" --version 2>/dev/null | head -1)"
    exit 0 ;;
esac
DEST="$REPO/out/host-tools"
DXC_URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip"
GLSLANG_URL="https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip"
mkdir -p "$DEST"
cd "$DEST"

if [ ! -x dxc/bin/x64/dxc.exe ]; then
  curl -fsSL -o dxc.zip "$DXC_URL"
  # The archive uses backslash separators, which unzip mangles.
  powershell -NoProfile -Command "Expand-Archive -Force dxc.zip dxc"
fi
if [ ! -x glslang/bin/glslangValidator.exe ]; then
  curl -fsSL -o glslang.zip "$GLSLANG_URL"
  unzip -q -o glslang.zip -d glslang
  cp glslang/bin/glslang.exe glslang/bin/glslangValidator.exe
fi
dxc/bin/x64/dxc.exe --version
glslang/bin/glslangValidator.exe --version | head -1
