#pragma once
// CPU decoding of BC1/BC2/BC3 (DXT1/3/5) blocks to RGBA8, used when the GPU
// cannot sample block-compressed formats (ARM Mali exposes no
// textureCompressionBC). Decodes exactly as Vulkan defines the BC formats.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

namespace rex::graphics::gta4_native {

inline bool IsNativeBcFormat(VkFormat format) {
  return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format == VK_FORMAT_BC2_UNORM_BLOCK ||
         format == VK_FORMAT_BC3_UNORM_BLOCK;
}

inline uint32_t NativeBcBlockBytes(VkFormat format) {
  return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8u : 16u;
}

namespace bc_detail {
inline void Rgb565(uint16_t c, uint32_t out[3]) {
  const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  out[0] = (r << 3) | (r >> 2);
  out[1] = (g << 2) | (g >> 4);
  out[2] = (b << 3) | (b >> 2);
}

// Writes 16 RGBA texels (row-major 4x4) for a BC1-style color block.
inline void DecodeColor(const uint8_t* block, bool allow_punchthrough, uint8_t rgba[64]) {
  uint16_t c0, c1;
  uint32_t selectors;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  std::memcpy(&selectors, block + 4, 4);
  uint32_t p[4][4];
  Rgb565(c0, p[0]);
  Rgb565(c1, p[1]);
  p[0][3] = p[1][3] = 255;
  if (c0 > c1 || !allow_punchthrough) {
    for (int k = 0; k < 3; ++k) {
      p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3;
      p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      p[2][k] = (p[0][k] + p[1][k]) / 2;
      p[3][k] = 0;
    }
    p[2][3] = 255;
    p[3][3] = 0;
  }
  for (int i = 0; i < 16; ++i) {
    const uint32_t* c = p[(selectors >> (2 * i)) & 3];
    rgba[i * 4 + 0] = uint8_t(c[0]);
    rgba[i * 4 + 1] = uint8_t(c[1]);
    rgba[i * 4 + 2] = uint8_t(c[2]);
    rgba[i * 4 + 3] = uint8_t(c[3]);
  }
}

inline void DecodeBc3Alpha(const uint8_t* block, uint8_t rgba[64]) {
  const uint32_t a0 = block[0], a1 = block[1];
  uint32_t a[8] = {a0, a1};
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (uint32_t i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) rgba[i * 4 + 3] = uint8_t(a[(bits >> (3 * i)) & 7]);
}

inline void DecodeBc2Alpha(const uint8_t* block, uint8_t rgba[64]) {
  for (int i = 0; i < 16; ++i) {
    const uint32_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
    rgba[i * 4 + 3] = uint8_t(nibble * 17);
  }
}
}  // namespace bc_detail

// Decodes block rows [first_row, end_row) of one 2D slice. row_length is in
// texels (a multiple of 4); destination is row_length texels wide, RGBA8.
inline void DecodeNativeBcRows(VkFormat format, const uint8_t* source, uint32_t row_length,
                               uint32_t first_row, uint32_t end_row, uint8_t* destination) {
  const uint32_t blocks_x = (row_length + 3) / 4;
  const uint32_t block_bytes = NativeBcBlockBytes(format);
  const size_t dest_pitch = size_t(blocks_x) * 4 * 4;
  uint8_t texels[64];
  for (uint32_t by = first_row; by < end_row; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const uint8_t* block = source + (size_t(by) * blocks_x + bx) * block_bytes;
      if (format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) {
        bc_detail::DecodeColor(block, true, texels);
      } else if (format == VK_FORMAT_BC2_UNORM_BLOCK) {
        bc_detail::DecodeColor(block + 8, false, texels);
        bc_detail::DecodeBc2Alpha(block, texels);
      } else {
        bc_detail::DecodeColor(block + 8, false, texels);
        bc_detail::DecodeBc3Alpha(block, texels);
      }
      for (uint32_t y = 0; y < 4; ++y) {
        std::memcpy(destination + (size_t(by) * 4 + y) * dest_pitch + size_t(bx) * 16,
                    texels + y * 16, 16);
      }
    }
  }
}

// A small pool of worker threads that decodes large textures in parallel
// strips, so the decoding uses the idle CPU cores instead of one thread. The
// calling thread works too and returns when every strip is done. Created on
// first use and never destroyed (the workers are detached).
class NativeBcDecodePool {
 public:
  static NativeBcDecodePool& Get() {
    static NativeBcDecodePool* pool = new NativeBcDecodePool();
    return *pool;
  }

  size_t worker_count() const { return worker_count_; }

  // Runs task(0..count-1) on the workers and the calling thread.
  void Run(uint32_t count, const std::function<void(uint32_t)>& task) {
    if (count <= 1 || worker_count_ == 0) {
      for (uint32_t i = 0; i < count; ++i) task(i);
      return;
    }
    std::lock_guard run_lock(run_mutex_);  // one parallel job at a time
    {
      std::lock_guard lock(mutex_);
      job_ = &task;
      next_ = 0;
      total_ = count;
      remaining_ = count;
      ++generation_;
    }
    wake_.notify_all();
    Work();
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return remaining_ == 0; });
    job_ = nullptr;
  }

 private:
  NativeBcDecodePool() {
    const unsigned cores = std::thread::hardware_concurrency();
    worker_count_ = cores > 2 ? std::min(cores - 1, 6u) : 0;
    for (size_t i = 0; i < worker_count_; ++i) {
      std::thread([this] { Loop(); }).detach();
    }
  }

  void Loop() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
      }
      Work();
    }
  }

  void Work() {
    for (;;) {
      const std::function<void(uint32_t)>* job;
      uint32_t index;
      {
        std::lock_guard lock(mutex_);
        if (!job_ || next_ >= total_) return;
        job = job_;
        index = next_++;
      }
      (*job)(index);
      std::lock_guard lock(mutex_);
      if (--remaining_ == 0) done_.notify_all();
    }
  }

  size_t worker_count_ = 0;
  std::mutex run_mutex_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  const std::function<void(uint32_t)>* job_ = nullptr;
  uint32_t next_ = 0;
  uint32_t total_ = 0;
  uint32_t remaining_ = 0;
  uint64_t generation_ = 0;
};

// Decodes one 2D slice. row_length/image_height are in texels (multiples of
// 4, as in VkBufferImageCopy); destination is row_length*image_height RGBA8.
// Slices of 256x256 texels or more are split into strips across the pool.
inline void DecodeNativeBcSlice(VkFormat format, const uint8_t* source, uint32_t row_length,
                                uint32_t image_height, uint8_t* destination) {
  const uint32_t blocks_x = (row_length + 3) / 4, blocks_y = (image_height + 3) / 4;
  constexpr uint32_t kParallelBlocks = 64 * 64;  // 256x256 texels
  NativeBcDecodePool& pool = NativeBcDecodePool::Get();
  if (uint64_t(blocks_x) * blocks_y < kParallelBlocks || pool.worker_count() == 0) {
    DecodeNativeBcRows(format, source, row_length, 0, blocks_y, destination);
    return;
  }
  // A few strips per thread so faster cores take more of them.
  const uint32_t strips =
      std::min<uint32_t>(blocks_y, uint32_t(pool.worker_count() + 1) * 4);
  const uint32_t rows_per_strip = (blocks_y + strips - 1) / strips;
  pool.Run(strips, [&](uint32_t strip) {
    const uint32_t first = strip * rows_per_strip;
    const uint32_t end = std::min(blocks_y, first + rows_per_strip);
    if (first < end) DecodeNativeBcRows(format, source, row_length, first, end, destination);
  });
}

}  // namespace rex::graphics::gta4_native
