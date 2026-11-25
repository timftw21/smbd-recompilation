#pragma once
#include "render.h"
#include "interface_marks.h"
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
// The original Sofdec service E6050 locks [755F20+4*(byte 578B98!=0)] through
// SDK texture lock 31F560 and decodes into it; E59B0 returns the other slot for
// drawing, and E5A50 releases both and clears the slots. Each D3D texture
// object holds its data address at +4. Objects may lie in any mapped guest
// page; Load reads one guest dword, or returns zero where nothing is mapped.
template<class Load> bool movie_frame_texture(Load&& load,std::uint32_t address) {
    for(const std::uint32_t slot:{0x00755F20U,0x00755F24U}) {
        const std::uint32_t object=load(slot),data=object?load(object+4):0U;
        if(data && ((data^address)&0x0FFFFFFF)==0) return true;
    }
    return false;
}
RenderState gpu_render_state(std::span<const std::uint32_t,2048>,std::uint32_t primitive);
// A frontend quad's input UV bounds apply only with an identity texture matrix.
std::uint32_t frontend_atlas_stages(std::uint32_t candidates,const VertexConstants&);
std::uint32_t prepare_gpu_program(Renderer&,std::span<const std::uint32_t,2048>,const VertexProgram&);
std::string replay_frame(const std::filesystem::path& capture,const std::filesystem::path& output,
                         std::string_view source_sha256,std::uint32_t through=UINT32_MAX,std::uint32_t only=UINT32_MAX,bool join_arrays=false,
                         unsigned antialiasing=UINT32_MAX,unsigned anisotropy=UINT32_MAX,unsigned unlimited_render_distance=UINT32_MAX,
                         unsigned upscaled_movies=UINT32_MAX,unsigned upscaled_hud=UINT32_MAX,unsigned sample_bounds=UINT32_MAX,unsigned tolerance=0);
// The original SDK produces NV2A packets. This bridge shares the native renderer
// with offline diagnostics; it does not decode or execute CPU instructions.
class Gpu {
public:
    Gpu(Renderer&,Memory&,std::uint32_t semaphore_base,std::span<const GraphicsObject>);
    ~Gpu();
    void submit(std::uint32_t begin,std::uint32_t end,std::uint32_t callsite=0);
    bool busy();
    bool time_pending(std::uint32_t address,std::uint32_t issued,std::uint32_t requested);
    void wait();
    void snapshot();
    void invalidate_memory(std::uint32_t address,std::uint32_t size);
    // The original immediate frontend drew this texture data address as HUD or
    // menu artwork; the renderer may replace its image with an enlarged one.
    void interface_texture(std::uint32_t address);
    // The texture rectangle of one frontend quad, matched to its NV097 inline
    // vertex bytes when the quad is drawn.
    void interface_bounds(std::uint64_t quad,const InterfaceSampleBounds&);
    static std::string check_texture_cache();
    static std::string check_bump_mapping();
    static std::string check_image_blit(const std::filesystem::path&,bool warp,bool debug,unsigned antialiasing=0,unsigned scale=1);
    void capture_frame(const std::filesystem::path&,std::string_view source_sha256);
    const FrameCaptureStatus& capture_status() const;
    void cancel_capture(std::string_view reason);
    GpuStats stats() const;
    std::function<void()> flip;
    std::function<void(std::uint32_t,std::uint32_t)> completion;
    std::function<bool(std::uint32_t)> movie_texture; // Consulted only when uploading a texture.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
