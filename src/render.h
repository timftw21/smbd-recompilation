#pragma once
#include "file.h"
#include "vertex_program.h"
#include <array>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

struct SDL_Window;

namespace smbd {
class Performance;
struct Image {
    std::uint32_t width, height;
    std::vector<std::byte> rgba;
};
void save_png(const Image&, const std::filesystem::path&, bool replace = false);
Image load_image(const std::filesystem::path&);
struct ImageComparison {
    Image difference;
    std::uint64_t changed=0,over_tolerance=0,total_error=0;
    unsigned maximum=0;
};
ImageComparison compare_images(const Image& actual,const Image& reference,unsigned tolerance);
std::string compare_images(const std::filesystem::path& actual,
                           const std::filesystem::path& reference,
                           const std::filesystem::path& difference, unsigned tolerance);

// NV097 color-format codes. DXT blocks are already in row-major block order.
enum class TextureFormat : std::uint32_t {
    argb4 = 0x04, rgb565 = 0x05, argb8 = 0x06, xrgb8 = 0x07,
    dxt1 = 0x0C, dxt3 = 0x0E, dxt5 = 0x0F,
    rgb565_linear = 0x11, argb8_linear = 0x12, a8l8 = 0x1A, xrgb8_linear = 0x1E
};
struct TextureData {
    std::uint32_t width, height;
    TextureFormat format;
    Bytes bytes;
    std::uint32_t mip_levels = 1, pitch = 0;
    bool cubemap = false;
    std::uint32_t depth = 1;
    bool movie = false; // A decoded FMV frame, identified from original game state.
};
// Xbox cubemap mip chains start each face on a 128-byte boundary.
inline constexpr std::uint64_t cubemap_face_size(std::uint64_t bytes) {return (bytes+127)&~std::uint64_t(127);}
enum class SurfaceColor {rgba8,rgb565};
enum class SurfaceDepth {z24s8,z16,none};

struct Vertex {
    Float4 position{0, 0, 0, 1}; // Homogeneous D3D clip coordinates.
    Float4 color{1, 1, 1, 1}, secondary{};
    std::array<Float4, 4> uv{};
    float fog = 1;
};
// Numeric primitive, comparison, blending and combiner fields retain NV097 values.
using ScissorRect=std::array<std::int32_t,4>;
struct RenderState {
    std::uint32_t primitive = 5;
    bool blend = false, depth_test = false, depth_write = false;
    bool alpha_test = false, cull = false, front_ccw = false;
    std::uint32_t depth_function = 0x0203, alpha_function = 0x0207, alpha_reference = 0;
    std::uint32_t blend_source = 1, blend_destination = 0, blend_equation = 0x8006;
    std::uint32_t cull_face = 0x0405, color_mask = 0x01010101;
    bool stencil_test = false, polygon_offset = false, scissor_test = false;
    std::uint32_t stencil_function = 0x0207, stencil_reference = 0, stencil_read_mask = 255, stencil_write_mask = 255;
    std::uint32_t stencil_fail = 0x1E00, stencil_depth_fail = 0x1E00, stencil_pass = 0x1E00;
    float polygon_offset_scale = 0, polygon_offset_bias = 0;
    ScissorRect scissor{};
    std::array<ScissorRect,8> clip_windows{};
    unsigned window_count=0;
    bool exclude_windows=false;
    std::array<std::uint32_t, 4> textures{}, texture_modes{}; // NV097 texture shader modes.
    std::array<std::uint32_t, 4> texture_sources{}, texture_clip{};
    std::array<Float4,4> bump_matrices{}; // M00, M01, M10, M11; normalized texture units.
    std::array<std::uint32_t, 4> texture_address{0x030303, 0x030303, 0x030303, 0x030303};
    std::array<std::uint32_t,4> texture_border{};
    std::array<bool, 4> linear_filter{}, alpha_kill{};
    std::array<std::uint32_t,4> texture_filter{},texture_control{0x0003FFC0,0x0003FFC0,0x0003FFC0,0x0003FFC0};
    std::uint32_t combiner_control = 0;
    std::array<std::uint32_t, 8> color_inputs{}, alpha_inputs{}, color_outputs{}, alpha_outputs{};
    std::array<std::uint32_t, 8> factor0{}, factor1{};
    std::array<std::uint32_t, 2> final_inputs{}, final_factors{};
    std::uint32_t fog_color = 0;
    bool fog_enable = false;
    std::uint32_t fog_mode = 0x2601;
    std::array<float,2> fog_parameters{};
    // Frontend quads: per stage {minimum u, minimum v, maximum u, maximum v},
    // half a texel inside the quad's texture rectangle. Higher internal
    // resolutions clamp 2D coordinates to it; bit N enables stage N.
    std::array<Float4,4> sample_bounds{};
    std::uint32_t bounded_samples=0;
};

struct ProgramView {
    float depth_scale = 1, subpixel_bias = 0.03125f;
    std::array<Float4,4> texture_scales{{{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}}};
};

struct VertexAttribute {
    std::uint32_t stream=16,offset=0,format=0x42; // Stream 16 supplies current values.
    auto operator<=>(const VertexAttribute&) const = default;
};
using VertexLayout=std::array<VertexAttribute,16>;
struct VertexStream { Bytes bytes;std::uint32_t stride; };
// D3D11 state objects selected by Renderer::prepare, as 1-based ids. They
// stay valid for the renderer's lifetime; zero resolves at submission.
struct PipelineStates {
    std::uint16_t blend=0,depth=0,raster=0;
    std::array<std::uint16_t,4> samplers{};
    bool operator==(const PipelineStates&) const = default;
};
struct StreamDraw {
    const RenderState& state;
    const VertexConstants& constants;
    const ProgramView& view;
    std::span<const VertexStream> streams;
    const ProgramVertex& current;
    std::uint32_t layout,count;
    std::span<const std::uint32_t> indices;
    PipelineStates pipeline{};
};

struct RenderStats {
    std::uint64_t draws = 0, vertices = 0, shader_compilations = 0, vertex_bytes = 0;
    double gpu_ms = 0;
    bool gpu_timing_valid = false;
    std::uint64_t texture_bytes=0,target_bytes=0;
    std::uint32_t texture_slots=0;
    std::uint32_t resolution_scale=1;
    std::uint32_t antialiasing=0,anisotropy=0;
    bool unlimited_render_distance=false,upscaled_movies=false,upscaled_hud=false;
    std::uint64_t movie_upscales=0;
    // Texture stages clamped inside their frontend quad's texture rectangle.
    std::uint64_t bounded_samples=0;
    // HUD textures: marked, waiting, enlarged by the network or the filter,
    // evicted for the budget; bytes include working storage.
    std::uint64_t hud_marked=0,hud_pending=0,hud_upscales=0,hud_filtered=0,hud_evictions=0,hud_bytes=0,hud_cached=0,hud_reused=0;
    std::uint64_t hud_disk_loads=0,hud_disk_saves=0; // Enlargements read from / written to the disk cache.
    double hud_gpu_ms=0;
    std::uint64_t hud_step_budget=0,hud_step_work=0,hud_step_units=0,hud_waiting=0,hud_network=0;double hud_rate=0;
    std::uint64_t total_vertex_bytes=0,texture_uploads=0,texture_upload_bytes=0,target_creations=0,target_copies=0;
    std::uint64_t buffer_maps=0,state_misses=0,layout_misses=0,sampler_misses=0,program_creations=0;
};

class Renderer {
public:
#ifdef _WIN32
    static constexpr bool debug_layer_available=true;
#else
    static constexpr bool debug_layer_available=false;
#endif
    explicit Renderer(std::uint32_t width, std::uint32_t height,
                      bool warp = false, bool debug = false,unsigned resolution_scale=1);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    std::uint32_t upload(const TextureData&,bool reuse_slot=true);
    void release_texture(std::uint32_t);
    void release_target(std::uint32_t);
    std::uint32_t create_target(std::uint32_t width,std::uint32_t height,
                                SurfaceColor=SurfaceColor::rgba8,SurfaceDepth=SurfaceDepth::z24s8,
                                std::uint32_t shared_color=UINT32_MAX,bool reuse_slot=true);
    void grow_target(std::uint32_t target,std::uint32_t width,std::uint32_t height);
    void write_region(std::uint32_t target,std::uint32_t x,std::uint32_t y,
                      std::uint32_t width,std::uint32_t height,Bytes pixels,std::uint32_t pitch);
    void set_target(std::uint32_t target=0);
    void copy_target(std::uint32_t destination); // GPU copy for later sampling.
    void copy_region(std::uint32_t destination,std::uint32_t source,
                     std::uint32_t source_x,std::uint32_t source_y,
                     std::uint32_t destination_x,std::uint32_t destination_y,
                     std::uint32_t width,std::uint32_t height);
    void copy_to_main();
    void resize(std::uint32_t width, std::uint32_t height);
    void aspect_ratio(float);
    void image_quality(unsigned resolution_scale,unsigned antialiasing,unsigned anisotropy,bool unlimited_render_distance=false,bool upscaled_movies=false,bool upscaled_hud=false);
    // AA: off, FXAA low/medium/high, MSAA 2/4/8x. AF: original or 1/2/4/8/16x.
    // Upscaled movies enlarge each decoded FMV frame to the internal resolution once.
    // Upscaled HUD enlarges marked HUD/menu textures once each, at 2x or higher.
    void enhancements(unsigned antialiasing,unsigned anisotropy,bool unlimited_render_distance=false,bool upscaled_movies=false,bool upscaled_hud=false);
    // The original immediate frontend samples this texture as HUD/menu artwork.
    // A nonzero content hash lets a reloaded copy reuse a released enlargement.
    void interface_texture(std::uint32_t id,std::uint64_t content=0);
    void finish_interface_upscales(); // Diagnostics: complete queued HUD enlargement now.
    // Keeps finished HUD enlargements in this directory (bounded, by image
    // content) for reuse in later sessions; an empty path disables it.
    void hud_cache(const std::filesystem::path& directory);
    bool supports_msaa(unsigned samples) const;
    void attach_window(SDL_Window* native_window);
    bool wait_for_window_frame(unsigned timeout_ms);
    void publish_frame(); // Retain one complete source image, independent of subsequent guest draws.
    bool display(bool published=false,bool wait=true);
    bool present(bool vsync,bool wait=true);
    ID3D11Device* native_device() const;
    ID3D11DeviceContext* native_context() const;
    void performance(Performance*,std::uint64_t next_frame);
    Performance* performance() const;
    void performance_frame(std::uint64_t next_frame);
    void finish_performance();
    void performance_resources();
    PipelineStates prepare(const RenderState&); // Cached D3D11 states; shaders are built ahead of time.
    std::uint32_t prepare_program(const VertexProgram&, bool launch = false);
    std::uint32_t prepare_fixed(const FixedTransform& = {});
    std::uint32_t prepare_layout(std::uint32_t program,const VertexLayout&);
    void launch_program(std::uint32_t program, VertexConstants&, const ProgramVertex&);
    void begin(std::uint32_t argb = 0xFF000000, float depth = 1, bool clear_target = true);
    void clear(std::uint32_t argb, float depth = 1);
    void clear_buffers(std::uint32_t flags,std::uint32_t argb,float depth,std::uint8_t stencil,
                       std::span<const std::int32_t> rectangle={});
    void clear_stencil(std::uint8_t value = 0);
    void draw(const RenderState&, std::span<const Vertex>, std::span<const std::uint32_t> indices = {});
    void draw_program(const RenderState&, std::uint32_t program, const VertexConstants&,
                      const ProgramView&, std::span<const ProgramVertex>, std::span<const std::uint32_t> indices = {});
    void draw_streams(const RenderState&,std::uint32_t layout,const VertexConstants&,const ProgramView&,
                      std::span<const VertexStream>,const ProgramVertex& current,std::uint32_t count,
                      std::span<const std::uint32_t> indices={});
    void draw_streams(std::span<const StreamDraw>);
    void end();
    Image readback(); // Explicit diagnostic operation; never implicit in draw/present.
    Image read_target(std::uint32_t target);
    std::vector<std::byte> read_depth(std::uint32_t target);
    Image read_display();
    RenderStats stats() const;
    std::string adapter() const;
    std::vector<std::string> errors() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string check_render(const std::filesystem::path& directory, bool warp, bool debug);
std::string check_image_copy(bool warp,bool debug,unsigned antialiasing=0);
// Optional FRAME is a captured 640x480 linear ARGB movie texture (texture-N.bin).
std::string check_movie_upscaling(const std::filesystem::path& directory,const std::filesystem::path& frame,bool warp,bool debug);
// Optional TEXTURES are captured HUD textures (PNG) to enlarge alongside synthetic cases.
std::string check_hud_upscaling(const std::filesystem::path& directory,std::span<const std::filesystem::path> textures,bool warp,bool debug);
std::string render_texture(const std::filesystem::path& input, TextureFormat format,
                           std::uint32_t width, std::uint32_t height,
                           const std::filesystem::path& output, bool warp);
std::string render_dictionary(const std::filesystem::path& input,
                              const std::filesystem::path& directory, bool warp);
}
