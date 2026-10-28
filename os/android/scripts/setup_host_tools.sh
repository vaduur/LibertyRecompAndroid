#!/bin/bash
# Host shader tools the gta4-native override cache needs at build time.
# Linux variant: the upstream script fetches the Windows builds and unpacks DXC
# with powershell. Both releases ship Linux archives, so they are used instead
# and installed under the same paths the build expects (the .exe names are kept
# so build_native.sh and the CMake cache need no change).
set -euo pipefail
. "$(dirname "$0")/env.sh"
DEST="$REPO/out/host-tools"
DXC_URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz"
GLSLANG_URL="https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-linux-x86_64-release.tar.gz"
mkdir -p "$DEST"
cd "$DEST"

if [ ! -x dxc/bin/x64/dxc.exe ]; then
  curl -fsSL -o dxc.tar.gz "$DXC_URL"
  rm -rf dxc_unpack && mkdir -p dxc_unpack
  tar xzf dxc.tar.gz -C dxc_unpack
  mkdir -p dxc/bin/x64 dxc/lib/x64
  # Layout differs between the Windows zip and the Linux tarball; find the bits.
  _dxc="$(find dxc_unpack -type f -name dxc -perm -u+x | head -1)"
  [ -n "$_dxc" ] || { echo "dxc binary not found in the Linux tarball"; exit 1; }
  cp "$_dxc" dxc/bin/x64/dxc.exe
  chmod +x dxc/bin/x64/dxc.exe
  # libdxcompiler.so is what GTA4_NATIVE_DXC_LIBRARY_PATH is pointed at.
  find dxc_unpack -type f -name 'libdxcompiler.so*' -exec cp {} dxc/bin/x64/ \;
  find dxc_unpack -type f -name 'libdxil.so*' -exec cp {} dxc/bin/x64/ \;
fi
if [ ! -x glslang/bin/glslangValidator.exe ]; then
  curl -fsSL -o glslang.tar.gz "$GLSLANG_URL"
  rm -rf glslang && mkdir -p glslang
  tar xzf glslang.tar.gz -C glslang
  [ -f glslang/bin/glslangValidator ] && cp glslang/bin/glslangValidator glslang/bin/glslangValidator.exe
  [ -f glslang/bin/glslang ] && cp glslang/bin/glslang glslang/bin/glslangValidator.exe
  chmod +x glslang/bin/glslangValidator.exe
fi
LD_LIBRARY_PATH="$DEST/dxc/bin/x64:${LD_LIBRARY_PATH:-}" dxc/bin/x64/dxc.exe --version
glslang/bin/glslangValidator.exe --version | head -1
