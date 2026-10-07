#pragma once
// The persistent vertex storage the geometry cache fills (gx/geometry_cache.hpp): one buffer that
// outlives the frames, bound in place of the frame's vertex buffer by the draws that read it. The
// Metroid Prime port's design (its gfx/geometry_buffer), adapted to this renderer's staging.

#include <cstddef>
#include <cstdint>
#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {

// The size the buffer is created with at its first use, in MiB; 0 (the default) leaves the cache off.
// Any thread, read when the buffer is created.
void set_geometry_buffer_capacity_mb(uint32_t mb) noexcept;
// Creates the buffer and its bind group when the capacity allows and the device is up; the buffer's
// size in bytes afterwards (0: none). GX thread, under the renderer lock.
uint64_t ensure_geometry_buffer() noexcept;
// The buffer's size, 0 without one. Any thread.
uint64_t geometry_buffer_capacity() noexcept;
// Copies `size` bytes (a multiple of 4) to `offset` of the buffer through this frame's vertex staging:
// encoded before the frame's render passes (end_batch_impl), so a draw recorded after this call reads
// them. False when the staging has no room left. GX thread.
bool queue_geometry_upload(uint32_t offset, const uint8_t* data, uint32_t size);

namespace detail {
const wgpu::Buffer& geometry_buffer() noexcept;
// Group 0 for a draw reading the buffer: the static layout, with the buffer first and the frame
// storage buffer second.
const wgpu::BindGroup& geometry_bind_group() noexcept;
// Encodes the queued uploads from the frame's staging buffer (whose vertex region starts at 0) and
// forgets them; the frame's seal.
void encode_geometry_uploads(const wgpu::CommandEncoder& cmd, const wgpu::Buffer& staging);
// Forgets the queued uploads without encoding them; an aborted frame.
void drop_geometry_uploads() noexcept;
// Releases the buffer; device shutdown.
void shutdown_geometry_buffer() noexcept;
} // namespace detail

} // namespace aurora::gfx
