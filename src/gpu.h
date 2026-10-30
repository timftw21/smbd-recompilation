#pragma once
#include "render.h"
#include <functional>

namespace smbd {
class Memory;
struct GpuStats {
    std::uint64_t submissions=0,packets=0,methods=0,draws=0,vertices=0,clears=0,flips=0,fences=0,blits=0,callbacks=0;
    std::uint64_t texture_uploads=0,texture_evictions=0,target_evictions=0;
};
struct GraphicsObject {std::uint32_t handle,kind;};
struct FrameCaptureStatus {
    std::filesystem::path directory;
    bool pending=false,complete=false,truncated=false;
    std::string error;
};
RenderState gpu_render_state(std::span<const std::uint32_t,2048>,std::uint32_t primitive);
std::uint32_t prepare_gpu_program(Renderer&,std::span<const std::uint32_t,2048>,const VertexProgram&);
std::string replay_frame(const std::filesystem::path& capture,const std::filesystem::path& output,
                         std::string_view source_sha256,std::uint32_t through=UINT32_MAX,std::uint32_t only=UINT32_MAX,bool join_arrays=false);
// The original SDK produces NV2A packets. This bridge shares the native renderer
// with offline diagnostics; it does not decode or execute CPU instructions.
class Gpu {
public:
    Gpu(Renderer&,Memory&,std::uint32_t semaphore_base,std::span<const GraphicsObject>);
    ~Gpu();
    void submit(std::uint32_t begin,std::uint32_t end,std::uint32_t callsite=0);
    bool busy();
    void wait();
    void snapshot();
    void invalidate_memory(std::uint32_t address,std::uint32_t size);
    static std::string check_texture_cache();
    void capture_frame(const std::filesystem::path&,std::string_view source_sha256);
    const FrameCaptureStatus& capture_status() const;
    void cancel_capture(std::string_view reason);
    GpuStats stats() const;
    std::function<void()> flip;
    std::function<void(std::uint32_t,std::uint32_t)> completion;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
