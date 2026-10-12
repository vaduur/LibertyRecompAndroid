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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <rex/dbg.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::graphics::texture_conversion {

using namespace rex::graphics::xenos;

void CopySwapBlock(xenos::Endian endian, void* output, const void* input, size_t length) {
  switch (endian) {
    case xenos::Endian::k8in16:
      memory::copy_and_swap_16_unaligned(output, input, length / 2);
      break;
    case xenos::Endian::k8in32:
      memory::copy_and_swap_32_unaligned(output, input, length / 4);
      break;
    case xenos::Endian::k16in32:
      // This helper counts 32-bit words; CopySwapBlock receives bytes.
      // Passing the byte length overruns both spans and corrupts adjacent blocks.
      memory::copy_and_swap_16_in_32_unaligned(output, input, length / sizeof(uint32_t));
      break;
    default:
    case xenos::Endian::kNone:
      std::memcpy(output, input, length);
      break;
  }
}

void ConvertTexelCTX1ToR8G8(xenos::Endian endian, void* output, const void* input, size_t length) {
  // https://fileadmin.cs.lth.se/cs/Personal/Michael_Doggett/talks/unc-xenos-doggett.pdf
  // (R is in the higher bits, according to how this format is used in
  //  4D5307E6).
  union {
    uint8_t data[8];
    struct {
      uint8_t g0, r0, g1, r1;
      uint32_t xx;
    };
  } block;
  static_assert(sizeof(block) == 8, "CTX1 block mismatch");

  const uint32_t bytes_per_block = 8;
  CopySwapBlock(endian, block.data, input, bytes_per_block);

  uint8_t cr[4] = {block.r0, block.r1,
                   static_cast<uint8_t>(2.f / 3.f * block.r0 + 1.f / 3.f * block.r1),
                   static_cast<uint8_t>(1.f / 3.f * block.r0 + 2.f / 3.f * block.r1)};
  uint8_t cg[4] = {block.g0, block.g1,
                   static_cast<uint8_t>(2.f / 3.f * block.g0 + 1.f / 3.f * block.g1),
                   static_cast<uint8_t>(1.f / 3.f * block.g0 + 2.f / 3.f * block.g1)};

  auto output_bytes = static_cast<uint8_t*>(output);
  for (uint32_t oy = 0; oy < 4; ++oy) {
    for (uint32_t ox = 0; ox < 4; ++ox) {
      uint8_t xx = (block.xx >> (((ox + (oy * 4)) * 2))) & 3;
      output_bytes[(oy * length) + (ox * 2) + 0] = cr[xx];
      output_bytes[(oy * length) + (ox * 2) + 1] = cg[xx];
    }
  }
}

static void ConvertTexelBC4ToR8(xenos::Endian endian, uint8_t* output, const void* input,
                                size_t output_pitch, size_t output_component,
                                size_t output_pixel_stride) {
  uint8_t block[8];
  CopySwapBlock(endian, block, input, sizeof(block));
  uint8_t palette[8] = {block[0], block[1]};
  if (block[0] > block[1]) {
    for (uint32_t index = 2; index < 8; ++index) {
      palette[index] = uint8_t(((8 - index) * uint32_t(block[0]) +
                                (index - 1) * uint32_t(block[1])) /
                               7);
    }
  } else {
    for (uint32_t index = 2; index < 6; ++index) {
      palette[index] = uint8_t(((6 - index) * uint32_t(block[0]) +
                                (index - 1) * uint32_t(block[1])) /
                               5);
    }
    palette[6] = 0;
    palette[7] = UINT8_MAX;
  }
  uint64_t selectors = 0;
  for (uint32_t byte = 0; byte < 6; ++byte) {
    selectors |= uint64_t(block[byte + 2]) << (byte * 8);
  }
  for (uint32_t pixel = 0; pixel < 16; ++pixel) {
    const uint32_t x = pixel & 3;
    const uint32_t y = pixel >> 2;
    const uint32_t selector = uint32_t((selectors >> (pixel * 3)) & 7);
    output[y * output_pitch + x * output_pixel_stride + output_component] = palette[selector];
  }
}

void ConvertTexelDXNToR8G8(xenos::Endian endian, void* output, const void* input, size_t length) {
  auto* output_bytes = static_cast<uint8_t*>(output);
  const auto* input_bytes = static_cast<const uint8_t*>(input);
  ConvertTexelBC4ToR8(endian, output_bytes, input_bytes, length, 0, 2);
  ConvertTexelBC4ToR8(endian, output_bytes, input_bytes + 8, length, 1, 2);
}

void ConvertTexelDXT5AToR8(xenos::Endian endian, void* output, const void* input, size_t length) {
  ConvertTexelBC4ToR8(endian, static_cast<uint8_t*>(output), input, length, 0, 1);
}

void ConvertTexelDXT3AToDXT3(xenos::Endian endian, void* output, const void* input, size_t) {
  auto output_bytes = static_cast<uint8_t*>(output);
  CopySwapBlock(endian, &output_bytes[0], input, 8);
  std::memset(&output_bytes[8], 0, 8);
}

namespace {

// Expands one RGB565 endpoint to 8-bit channels with bit replication.
inline void Expand565(uint16_t packed, uint8_t& red, uint8_t& green, uint8_t& blue) {
  const uint32_t r = packed >> 11, g = (packed >> 5) & 63, b = packed & 31;
  red = uint8_t((r << 3) | (r >> 2));
  green = uint8_t((g << 2) | (g >> 4));
  blue = uint8_t((b << 3) | (b >> 2));
}

// Decodes the 8-byte BC1 color block at `block` (already endian-swapped) into
// 16 RGBA pixels. force_opaque implements the BC2/BC3 rule (always four
// colors); BC1 uses punch-through transparency when color0 <= color1.
inline void DecodeBC1Colors(const uint8_t* block, uint8_t* rgba, size_t pitch, bool force_opaque) {
  const uint16_t color0 = uint16_t(block[0] | (block[1] << 8));
  const uint16_t color1 = uint16_t(block[2] | (block[3] << 8));
  uint8_t r[4], g[4], b[4], a[4] = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
  Expand565(color0, r[0], g[0], b[0]);
  Expand565(color1, r[1], g[1], b[1]);
  if (force_opaque || color0 > color1) {
    r[2] = uint8_t((2 * r[0] + r[1]) / 3);
    g[2] = uint8_t((2 * g[0] + g[1]) / 3);
    b[2] = uint8_t((2 * b[0] + b[1]) / 3);
    r[3] = uint8_t((r[0] + 2 * r[1]) / 3);
    g[3] = uint8_t((g[0] + 2 * g[1]) / 3);
    b[3] = uint8_t((b[0] + 2 * b[1]) / 3);
  } else {
    r[2] = uint8_t((r[0] + r[1]) / 2);
    g[2] = uint8_t((g[0] + g[1]) / 2);
    b[2] = uint8_t((b[0] + b[1]) / 2);
    r[3] = g[3] = b[3] = 0;
    a[3] = 0;
  }
  const uint32_t selectors =
      uint32_t(block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24));
  for (uint32_t pixel = 0; pixel < 16; ++pixel) {
    const uint32_t selector = (selectors >> (pixel * 2)) & 3;
    uint8_t* out = rgba + size_t(pixel >> 2) * pitch + size_t(pixel & 3) * 4;
    out[0] = r[selector];
    out[1] = g[selector];
    out[2] = b[selector];
    out[3] = a[selector];
  }
}

// Decodes one 8-byte BC4 alpha block into the alpha channel of 16 RGBA
// pixels (rgb untouched, 4-byte stride between pixels).
inline void DecodeBC4AlphaToRGBA(const uint8_t* block, uint8_t* rgba, size_t pitch) {
  uint8_t palette[8] = {block[0], block[1]};
  if (block[0] > block[1]) {
    for (uint32_t i = 2; i < 8; ++i) {
      palette[i] = uint8_t(((8 - i) * uint32_t(block[0]) + (i - 1) * uint32_t(block[1])) / 7);
    }
  } else {
    for (uint32_t i = 2; i < 6; ++i) {
      palette[i] = uint8_t(((6 - i) * uint32_t(block[0]) + (i - 1) * uint32_t(block[1])) / 5);
    }
    palette[6] = 0;
    palette[7] = UINT8_MAX;
  }
  uint64_t selectors = 0;
  for (uint32_t byte = 0; byte < 6; ++byte) {
    selectors |= uint64_t(block[byte + 2]) << (byte * 8);
  }
  for (uint32_t pixel = 0; pixel < 16; ++pixel) {
    const uint32_t selector = uint32_t((selectors >> (pixel * 3)) & 7);
    uint8_t* out = rgba + size_t(pixel >> 2) * pitch + size_t(pixel & 3) * 4;
    out[3] = palette[selector];
  }
}

}  // namespace

void ConvertTexelDXT3AToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length) {
  uint8_t block[8];
  CopySwapBlock(endian, block, input, sizeof(block));
  auto* rgba = static_cast<uint8_t*>(output);
  for (uint32_t pixel = 0; pixel < 16; ++pixel) {
    const uint32_t nibble = (block[pixel >> 1] >> ((pixel & 1) * 4)) & 15;
    uint8_t* out = rgba + size_t(pixel >> 2) * length + size_t(pixel & 3) * 4;
    out[0] = out[1] = out[2] = 0;
    out[3] = uint8_t(nibble * 17);
  }
}

void ConvertTexelBC1ToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length) {
  uint8_t block[8];
  CopySwapBlock(endian, block, input, sizeof(block));
  DecodeBC1Colors(block, static_cast<uint8_t*>(output), length, false);
}

void ConvertTexelBC2ToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length) {
  uint8_t block[16];
  CopySwapBlock(endian, block, input, sizeof(block));
  auto* rgba = static_cast<uint8_t*>(output);
  DecodeBC1Colors(block + 8, rgba, length, true);
  // Explicit 4-bit alpha nibbles, LSB-first (applied after color: the color
  // decoder always writes opaque alpha).
  for (uint32_t pixel = 0; pixel < 16; ++pixel) {
    const uint32_t nibble = (block[pixel >> 1] >> ((pixel & 1) * 4)) & 15;
    uint8_t* out = rgba + size_t(pixel >> 2) * length + size_t(pixel & 3) * 4;
    out[3] = uint8_t(nibble * 17);
  }
}

void ConvertTexelBC3ToRGBA8(xenos::Endian endian, void* output, const void* input, size_t length) {
  uint8_t block[16];
  CopySwapBlock(endian, block, input, sizeof(block));
  auto* rgba = static_cast<uint8_t*>(output);
  DecodeBC1Colors(block + 8, rgba, length, true);
  DecodeBC4AlphaToRGBA(block, rgba, length);
}

// https://github.com/BinomialLLC/crunch/blob/ea9b8d8c00c8329791256adafa8cf11e4e7942a2/inc/crn_decomp.h#L4108
static uint32_t TiledOffset2DRow(uint32_t y, uint32_t width, uint32_t log2_bpp) {
  uint32_t macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) +
         ((y & 1) << 4);
}

static uint32_t TiledOffset2DColumn(uint32_t x, uint32_t y, uint32_t log2_bpp,
                                    uint32_t base_offset) {
  uint32_t macro = (x / 32) << (log2_bpp + 7);
  uint32_t micro = (x & 7) << log2_bpp;
  uint32_t offset = base_offset + (macro + ((micro & ~0xF) << 1) + (micro & 0xF));
  return ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

void Untile(uint8_t* output_buffer, const uint8_t* input_buffer, const UntileInfo* untile_info) {
  SCOPE_profile_cpu_f("gpu");
  assert_not_null(untile_info);
  assert_not_null(untile_info->input_format_info);
  assert_not_null(untile_info->output_format_info);

  uint32_t input_bytes_per_block = untile_info->input_format_info->bytes_per_block();
  uint32_t output_bytes_per_block = untile_info->output_format_info->bytes_per_block();
  uint32_t output_pitch = untile_info->output_pitch * output_bytes_per_block;

  // Bytes per pixel
  auto log2_bpp =
      (input_bytes_per_block / 4) + ((input_bytes_per_block / 2) >> (input_bytes_per_block / 4));

  // Offset to the current row, in bytes.
  uint32_t output_row_offset = 0;
  for (uint32_t y = 0; y < untile_info->height; y++) {
    auto input_row_offset =
        TiledOffset2DRow(untile_info->offset_y + y, untile_info->input_pitch, log2_bpp);

    // Go block-by-block on this row.
    uint32_t output_offset = output_row_offset;

    for (uint32_t x = 0; x < untile_info->width; x++) {
      auto input_offset = TiledOffset2DColumn(untile_info->offset_x + x, untile_info->offset_y + y,
                                              log2_bpp, input_row_offset);
      input_offset >>= log2_bpp;

      untile_info->copy_callback(&output_buffer[output_offset],
                                 &input_buffer[input_offset * input_bytes_per_block],
                                 output_bytes_per_block);

      output_offset += output_bytes_per_block;
    }

    output_row_offset += output_pitch;
  }
}

}  // namespace rex::graphics::texture_conversion
