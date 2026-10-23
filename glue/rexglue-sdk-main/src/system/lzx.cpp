/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/system/lzx.h>
#include <rex/system/util/xex2_info.h>
#include <rex/types.h>

#include <lzx.h>
#include <mspack.h>

typedef struct mspack_memory_file_t {
  mspack_system sys;
  void* buffer;
  off_t buffer_size;
  off_t offset;
} mspack_memory_file;

mspack_memory_file* mspack_memory_open(mspack_system* sys, void* buffer, const size_t buffer_size) {
  (void)sys;
  assert_true(buffer_size < INT_MAX);
  if (buffer_size >= INT_MAX) {
    return NULL;
  }
  auto memfile = (mspack_memory_file*)std::calloc(1, sizeof(mspack_memory_file));
  if (!memfile) {
    return NULL;
  }
  memfile->buffer = buffer;
  memfile->buffer_size = (off_t)buffer_size;
  memfile->offset = 0;
  return memfile;
}

void mspack_memory_close(mspack_memory_file* file) {
  auto memfile = (mspack_memory_file*)file;
  std::free(memfile);
}

int mspack_memory_read(mspack_file* file, void* buffer, int chars) {
  auto memfile = (mspack_memory_file*)file;
  const off_t remaining = memfile->buffer_size - memfile->offset;
  const off_t total = std::min(static_cast<off_t>(chars), remaining);
  std::memcpy(buffer, (uint8_t*)memfile->buffer + memfile->offset, total);
  memfile->offset += total;
  return (int)total;
}

int mspack_memory_write(mspack_file* file, void* buffer, int chars) {
  auto memfile = (mspack_memory_file*)file;
  const off_t remaining = memfile->buffer_size - memfile->offset;
  const off_t total = std::min(static_cast<off_t>(chars), remaining);
  std::memcpy((uint8_t*)memfile->buffer + memfile->offset, buffer, total);
  memfile->offset += total;
  return (int)total;
}

void* mspack_memory_alloc(mspack_system* sys, size_t chars) {
  return std::calloc(chars, 1);
}

void mspack_memory_free(void* ptr) {
  std::free(ptr);
}

void mspack_memory_copy(void* src, void* dest, size_t chars) {
  std::memcpy(dest, src, chars);
}

mspack_system* mspack_memory_sys_create() {
  auto sys = (mspack_system*)std::calloc(1, sizeof(mspack_system));
  if (!sys) {
    return NULL;
  }
  sys->read = mspack_memory_read;
  sys->write = mspack_memory_write;
  sys->alloc = mspack_memory_alloc;
  sys->free = mspack_memory_free;
  sys->copy = mspack_memory_copy;
  return sys;
}

void mspack_memory_sys_destroy(struct mspack_system* sys) {
  free(sys);
}

namespace rex::lzx {
namespace {

constexpr uint32_t kMinimumWindowBits = 15;
constexpr uint32_t kMaximumWindowBits = 21;
constexpr size_t kInputBufferSize = 0x8000;
constexpr uint64_t kFrameSize = 0x8000;

bool GetWindowBits(uint32_t window_size, uint32_t* window_bits) {
  if (!window_size || (window_size & (window_size - 1)) != 0) {
    return false;
  }
  const uint32_t bits = static_cast<uint32_t>(std::countr_zero(window_size));
  if (bits < kMinimumWindowBits || bits > kMaximumWindowBits) {
    return false;
  }
  *window_bits = bits;
  return true;
}

struct RebindableMemoryFile {
  const uint8_t* input = nullptr;
  uint8_t* output = nullptr;
  size_t size = 0;
  size_t offset = 0;
  size_t source_bytes_read = 0;
  bool reached_eof = false;
  bool writable = false;

  void BindInput(std::span<const uint8_t> bytes) {
    input = bytes.data();
    output = nullptr;
    size = bytes.size();
    offset = 0;
    source_bytes_read = 0;
    reached_eof = false;
    writable = false;
  }

  void BindOutput(std::span<uint8_t> bytes) {
    input = nullptr;
    output = bytes.data();
    size = bytes.size();
    offset = 0;
    source_bytes_read = 0;
    reached_eof = false;
    writable = true;
  }
};

int PersistentRead(mspack_file* file, void* destination, int requested) {
  auto* memory = reinterpret_cast<RebindableMemoryFile*>(file);
  if (!memory || memory->writable || requested < 0 || (!destination && requested != 0)) {
    return -1;
  }
  if (requested == 0) {
    return 0;
  }

  auto* bytes = static_cast<uint8_t*>(destination);
  const size_t request_size = static_cast<size_t>(requested);
  const size_t source_remaining = memory->size - memory->offset;
  const size_t source_count = std::min(request_size, source_remaining);
  if (source_count != 0) {
    std::memcpy(bytes, memory->input + memory->offset, source_count);
    memory->offset += source_count;
    memory->source_bytes_read += source_count;
  } else {
    memory->reached_eof = true;
  }

  return static_cast<int>(source_count);
}

int PersistentWrite(mspack_file* file, void* source, int requested) {
  auto* memory = reinterpret_cast<RebindableMemoryFile*>(file);
  if (!memory || !memory->writable || requested < 0 || (!source && requested != 0)) {
    return -1;
  }
  const size_t request_size = static_cast<size_t>(requested);
  const size_t remaining = memory->size - memory->offset;
  const size_t count = std::min(request_size, remaining);
  if (count != 0) {
    std::memcpy(memory->output + memory->offset, source, count);
    memory->offset += count;
  }
  return static_cast<int>(count);
}

void* PersistentAlloc(mspack_system*, size_t size) {
  return std::calloc(size, 1);
}

void PersistentFree(void* pointer) {
  std::free(pointer);
}

void PersistentCopy(void* source, void* destination, size_t size) {
  std::memcpy(destination, source, size);
}

}  // namespace

struct PersistentDecoder::Impl {
  explicit Impl(uint32_t requested_window_size)
      : window_size(requested_window_size) {
    system.read = PersistentRead;
    system.write = PersistentWrite;
    system.alloc = PersistentAlloc;
    system.free = PersistentFree;
    system.copy = PersistentCopy;
  }

  ~Impl() {
    if (stream) {
      lzxd_free(stream);
    }
  }

  bool Initialize() {
    uint32_t window_bits = 0;
    if (!GetWindowBits(window_size, &window_bits)) {
      return false;
    }
    input.BindInput({});
    output.BindOutput({});
    stream = lzxd_init(&system, reinterpret_cast<mspack_file*>(&input),
                       reinterpret_cast<mspack_file*>(&output),
                       static_cast<int>(window_bits), 0,
                       static_cast<int>(kInputBufferSize), 0, 0);
    if (stream) {
      // Xbox XMem resources omit CAB's pad byte after an odd raw block.
      // Retail sub_82A226B8 consumes exactly the raw length; sub_82A222D8
      // reloads the next header at that position. Keep generic lzx_decompress
      // on libmspack's padded default. Reset re-applies this per-stream mode.
      stream->uncompressed_block_padding = 0;
    }
    poisoned = stream == nullptr;
    total_output = 0;
    return stream != nullptr;
  }

  mspack_system system = {};
  RebindableMemoryFile input;
  RebindableMemoryFile output;
  lzxd_stream* stream = nullptr;
  uint64_t total_output = 0;
  uint32_t window_size = 0;
  bool poisoned = false;
};

std::unique_ptr<PersistentDecoder> PersistentDecoder::Create(uint32_t window_size) {
  uint32_t window_bits = 0;
  if (!GetWindowBits(window_size, &window_bits)) {
    return nullptr;
  }
  (void)window_bits;
  auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl(window_size));
  if (!impl || !impl->Initialize()) {
    return nullptr;
  }
  return std::unique_ptr<PersistentDecoder>(
      new (std::nothrow) PersistentDecoder(std::move(impl)));
}

PersistentDecoder::PersistentDecoder(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

PersistentDecoder::~PersistentDecoder() = default;
PersistentDecoder::PersistentDecoder(PersistentDecoder&&) noexcept = default;
PersistentDecoder& PersistentDecoder::operator=(PersistentDecoder&&) noexcept = default;

DecodeFrameResult PersistentDecoder::DecodeFrame(std::span<const uint8_t> input,
                                                 std::span<uint8_t> output) {
  if (!impl_ || impl_->poisoned || !impl_->stream) {
    return {DecodeStatus::kPoisoned, 0, 0};
  }
  if ((input.empty() && !output.empty()) || input.size() >= static_cast<size_t>(INT_MAX) ||
      output.size() >= static_cast<size_t>(INT_MAX) ||
      impl_->total_output > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
      output.size() > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) -
                          impl_->total_output) {
    return {DecodeStatus::kInvalidArgument, 0, 0};
  }
  if (output.empty()) {
    return {DecodeStatus::kSuccess, 0, 0};
  }

  impl_->input.BindInput(input);
  impl_->output.BindOutput(output);

  // GTA's wrapper presents one complete compressed frame per call and starts
  // each frame on a new word boundary. Keep the algorithmic LZX state while
  // discarding libmspack's speculative input look-ahead from the prior frame.
  impl_->stream->input = reinterpret_cast<mspack_file*>(&impl_->input);
  impl_->stream->output = reinterpret_cast<mspack_file*>(&impl_->output);
  impl_->stream->i_ptr = impl_->stream->inbuf;
  impl_->stream->i_end = impl_->stream->inbuf;
  impl_->stream->bit_buffer = 0;
  impl_->stream->bits_left = 0;
  impl_->stream->input_end = 0;

  const uint64_t next_total = impl_->total_output + output.size();
  lzxd_set_output_length(impl_->stream, static_cast<off_t>(next_total));
  int result = MSPACK_ERR_OK;
  if ((next_total % kFrameSize) == 0 && output.size() > 1) {
    // libmspack's end-frame calculation includes the frame containing the
    // requested end offset. At an exact 32 KiB boundary, a single request
    // therefore advances through a zero-sized phantom frame. Leave the final
    // byte buffered, then flush it separately so the cursor remains on the
    // retail stream's actual frame boundary.
    const off_t first_part = static_cast<off_t>(output.size() - 1);
    result = lzxd_decompress(impl_->stream, first_part);
    if (result == MSPACK_ERR_OK) {
      result = lzxd_decompress(impl_->stream, 1);
    }
  } else {
    result = lzxd_decompress(impl_->stream, static_cast<off_t>(output.size()));
  }
  const size_t written = impl_->output.offset;
  const size_t unread_buffered =
      static_cast<size_t>(impl_->stream->i_end - impl_->stream->i_ptr);
  // If the callback reached EOF, libmspack may have installed its own two
  // synthetic look-ahead bytes in inbuf. Those bytes are not part of the
  // caller's span and must not be subtracted from the actual-source count.
  const size_t unread_source = impl_->input.reached_eof
                                   ? 0
                                   : std::min(unread_buffered,
                                              impl_->input.source_bytes_read);
  const size_t consumed = impl_->input.source_bytes_read - unread_source;
  if (result != MSPACK_ERR_OK || written != output.size()) {
    impl_->poisoned = true;
    return {DecodeStatus::kDataError, written, consumed};
  }

  impl_->total_output = next_total;
  return {DecodeStatus::kSuccess, written, consumed};
}

bool PersistentDecoder::Reset() {
  if (!impl_) {
    return false;
  }
  if (impl_->stream) {
    lzxd_free(impl_->stream);
    impl_->stream = nullptr;
  }
  impl_->poisoned = false;
  return impl_->Initialize();
}

bool PersistentDecoder::poisoned() const {
  return !impl_ || impl_->poisoned;
}

uint64_t PersistentDecoder::total_output_bytes() const {
  return impl_ ? impl_->total_output : 0;
}

uint32_t PersistentDecoder::window_size() const {
  return impl_ ? impl_->window_size : 0;
}

}  // namespace rex::lzx

int lzx_decompress(const void* lzx_data, size_t lzx_len, void* dest, size_t dest_len,
                   uint32_t window_size, void* window_data, size_t window_data_len) {
  int result_code = 1;

  uint32_t window_bits;
  if (!rex::bit_scan_forward(window_size, &window_bits) ||
      (window_size & (window_size - 1)) != 0 || window_bits < 15 || window_bits > 21 ||
      (!lzx_data && lzx_len != 0) || (!dest && dest_len != 0) ||
      (window_data && window_data_len > window_size) || (!window_data && window_data_len != 0) ||
      lzx_len >= static_cast<size_t>(INT_MAX) || dest_len >= static_cast<size_t>(INT_MAX)) {
    return result_code;
  }

  mspack_system* sys = mspack_memory_sys_create();
  mspack_memory_file* lzxsrc = mspack_memory_open(sys, (void*)lzx_data, lzx_len);
  mspack_memory_file* lzxdst = mspack_memory_open(sys, dest, dest_len);
  lzxd_stream* lzxd = lzxd_init(sys, (mspack_file*)lzxsrc, (mspack_file*)lzxdst, window_bits, 0,
                                0x8000, (off_t)dest_len, 0);

  if (lzxd) {
    if (window_data) {
      // zero the window and then copy window_data to the end of it
      auto padding_len = window_size - window_data_len;
      std::memset(&lzxd->window[0], 0, padding_len);
      std::memcpy(&lzxd->window[padding_len], window_data, window_data_len);
      lzxd->ref_data_size = window_size;
    }

    result_code = lzxd_decompress(lzxd, (off_t)dest_len);

    lzxd_free(lzxd);
    lzxd = NULL;
  }

  if (lzxsrc) {
    mspack_memory_close(lzxsrc);
    lzxsrc = NULL;
  }

  if (lzxdst) {
    mspack_memory_close(lzxdst);
    lzxdst = NULL;
  }

  if (sys) {
    mspack_memory_sys_destroy(sys);
    sys = NULL;
  }

  return result_code;
}

int lzxdelta_apply_patch(rex::xex2_delta_patch* patch, size_t patch_len, uint32_t window_size,
                         void* dest) {
  void* patch_end = (char*)patch + patch_len;
  auto* cur_patch = patch;

  while (patch_end > cur_patch) {
    int patch_sz = -4;  // 0 byte patches need us to remove 4 byte from next
                        // patch addr because of patch_data field
    if (cur_patch->compressed_len == 0 && cur_patch->uncompressed_len == 0 &&
        cur_patch->new_addr == 0 && cur_patch->old_addr == 0)
      break;
    switch (cur_patch->compressed_len) {
      case 0:  // fill with 0
        std::memset((char*)dest + cur_patch->new_addr, 0, cur_patch->uncompressed_len);
        break;
      case 1:  // copy from old -> new
        // The source and the destination can overlap: the patch moves a block
        // inside the same buffer. memcpy is undefined for that, and on
        // aarch64 it corrupts the delta-patched image.
        std::memmove((char*)dest + cur_patch->new_addr, (char*)dest + cur_patch->old_addr,
                     cur_patch->uncompressed_len);
        break;
      default:                                     // delta patch
        patch_sz = cur_patch->compressed_len - 4;  // -4 because of patch_data field

        int result = lzx_decompress(cur_patch->patch_data, cur_patch->compressed_len,
                                    (char*)dest + cur_patch->new_addr, cur_patch->uncompressed_len,
                                    window_size, (char*)dest + cur_patch->old_addr,
                                    cur_patch->uncompressed_len);

        if (result) {
          return result;
        }
        break;
    }

    cur_patch++;
    cur_patch = reinterpret_cast<rex::xex2_delta_patch*>(
        reinterpret_cast<uint8_t*>(cur_patch) + patch_sz);
  }

  return 0;
}
