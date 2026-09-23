#pragma once

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx {

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void set_skip_unready_pipelines(bool enabled) noexcept;
bool skip_unready_pipelines() noexcept;
uint32_t queued_pipeline_count() noexcept;
// Persists the pipeline caches now: Dawn's Vulkan pipeline cache when a pipeline was created
// since the last store, then every queued recipe row, and returns once both are on disk. Dawn
// holds the device while it serializes its cache (tens to hundreds of ms for a large one), but
// not while the result is compressed and written.
void store_pipeline_caches();
// store_pipeline_caches on a background thread, returning at once. Requests made while one is
// running coalesce into one more store.
void request_pipeline_cache_store();
// Lets a drained first-use burst store the caches itself, rate-limited. Off by default; a host
// turns it on while a stall is acceptable (menus) and off again for a race.
void set_pipeline_cache_idle_store(bool allowed) noexcept;

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

bool wait_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
bool try_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
// Waits indefinitely for a draw about to be committed to a persistent texture with no re-issue
// path. Separate from wait_pipeline so skip-unready mode never becomes blocking for other passes.
bool wait_pipeline_for_persistent_pass(PipelineRef ref, wgpu::RenderPipeline& pipeline);

} // namespace aurora::gfx
