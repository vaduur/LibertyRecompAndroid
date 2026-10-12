#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <functional>
#include <memory>

#include <rex/assert.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_conversion {

typedef std::function<void(xenos::Endian, void*, const void*, size_t)> CopyBlockCallback;

void CopySwapBlock(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelCTX1ToR8G8(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelDXNToR8G8(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelDXT3AToDXT3(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelDXT5AToR8(xenos::Endian endian, void* output, const void* input, size_t length);
// DXT3A alpha block (8 explicit 4-bit nibbles) to RGBA8 with black color,
// mirroring ConvertTexelDXT3AToDXT3 (alpha preserved, color zeroed).
void ConvertTexelDXT3AToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length);
// Full BC1/BC2/BC3 block decoders to 16 RGBA8 pixels (4x4 block, row-major).
// Used where BC sampling is unavailable (Mali); DXT2/DXT4 premultiplied alpha
// is treated as straight alpha. `length` is the destination row pitch.
void ConvertTexelBC1ToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelBC2ToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelBC3ToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length);

typedef std::function<void(void*, const void*, size_t)> UntileCopyBlockCallback;

typedef struct UntileInfo {
  uint32_t offset_x;
  uint32_t offset_y;
  uint32_t width;
  uint32_t height;
  uint32_t input_pitch;
  uint32_t output_pitch;
  const FormatInfo* input_format_info;
  const FormatInfo* output_format_info;
  UntileCopyBlockCallback copy_callback;
} UntileInfo;

void Untile(uint8_t* output_buffer, const uint8_t* input_buffer, const UntileInfo* untile_info);

}  // namespace rex::graphics::texture_conversion
