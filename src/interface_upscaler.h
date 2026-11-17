#pragma once
#include "render.h"
#include <d3d11.h>
#include "com_ptr.h"
#include <list>

namespace smbd {
// HUD and menu artwork enlargement. The original immediate frontend marks the
// textures it draws (complete_screen_primitives at 313B0); each is enlarged once
// with Real-ESRGAN realesr-animevideov3, or with a clamped Catmull-Rom filter
// when the artwork is soft (glows, gradients) or tiny. Work runs in bounded GPU
// steps between frames, so a texture keeps its original image until finished.
// The result keeps normalized texture coordinates; original draws sample it.
inline constexpr unsigned interface_tile=64;
inline constexpr unsigned interface_halo=12;        // Changes tile interiors by at most 2/255.
inline constexpr unsigned interface_batch=8;        // Tile passes per convolution dispatch.
inline constexpr unsigned interface_bleed_passes=6; // Colour spread into transparent texels.
inline constexpr unsigned interface_network_minimum=8;
inline constexpr float interface_network_edge=0.4f; // Largest premultiplied neighbour step.

struct InterfaceDecision {float edge=0;bool transparent=false,network=false;};
InterfaceDecision classify_interface(std::span<const float> rgba,unsigned width,unsigned height);
// CPU reference of the complete GPU path in float precision, for diagnostics:
// returns straight RGBA at width*factor by height*factor.
std::vector<float> reference_interface_upscale(std::span<const float> rgba,unsigned width,unsigned height,unsigned factor,
                                               std::span<const float> model,InterfaceDecision* decision=nullptr);

class InterfaceUpscaler {
public:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Result {
        Ptr<ID3D11ShaderResourceView> view;
        unsigned width=0,height=0;
        std::size_t bytes=0;
        bool network=false;
    };
    struct Finished {std::uint32_t id;Result result;};
    // A smaller tile only serves diagnostics that exercise tile seams.
    InterfaceUpscaler(ID3D11Device*,ID3D11DeviceContext*,std::uint64_t& compilations,unsigned tile=interface_tile);
    ~InterfaceUpscaler();
    InterfaceUpscaler(const InterfaceUpscaler&)=delete;
    InterfaceUpscaler& operator=(const InterfaceUpscaler&)=delete;
    void request(std::uint32_t id,Ptr<ID3D11ShaderResourceView> source,unsigned width,unsigned height,unsigned factor);
    void cancel(std::uint32_t id);
    void clear();
    bool queued(std::uint32_t id) const;
    std::size_t queued() const {return jobs_.size();}
    // Runs queued steps for roughly target_ms of GPU time, and at least one
    // step, or everything when finish is set. Leaves no compute bindings.
    std::vector<Finished> step(bool finish=false,double target_ms=0.35);
    std::size_t bytes() const;  // Working storage, excluding finished results.
    double gpu_ms() const {return gpu_ms_;}
    // Diagnostics: the last step's work budget, work done and units run, and
    // the learned throughput in multiply-adds per nanosecond.
    struct StepReport {std::uint64_t budget=0,work=0,units=0;double rate=0;std::size_t waiting=0,network=0;};
    StepReport report() const {return report_;}
private:
    struct Job;
    struct Timing {Ptr<ID3D11Query> disjoint,start,finish;std::uint64_t work=0;bool pending=false;};
    // One tile's colour or alpha pass in the current batch.
    struct Entry {std::uint32_t job;unsigned tile;bool alpha;int origin[2],extent[2],interior[2];};
    void collect_timings();
    Job* find(std::uint32_t id);
    bool decide(Job&,bool wait);
    std::uint64_t run(Job&);       // One single-texture step: colour spread, filter or mips.
    bool form_batch();
    std::uint64_t run_batch();     // One dispatch of the current batch.
    void abort_batch(std::uint32_t cancelled);
    void dispatch(ID3D11ComputeShader*,std::initializer_list<ID3D11ShaderResourceView*>,ID3D11UnorderedAccessView*,
                  std::initializer_list<ID3D11Buffer*>,unsigned x,unsigned y,unsigned z=1);
    void constants(ID3D11Buffer*,const void*,std::size_t);
    void allocate_features();
    void create_output(Job&);
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    unsigned tile_;
    Ptr<ID3D11ComputeShader> classify_,bleed_,first_,conv_,compose_,filter_;
    Ptr<ID3D11Buffer> job_constants_,batch_constants_;
    std::vector<Ptr<ID3D11Buffer>> layers_; // Per layer and group of 16 outputs.
    std::array<unsigned,19> layer_index_{};
    struct Features {Ptr<ID3D11Texture2D> texture;Ptr<ID3D11ShaderResourceView> view;Ptr<ID3D11UnorderedAccessView> output;};
    std::array<Features,2> features_; // Ping-pong planes, 16 slices per batch slot.
    std::list<Job> jobs_;
    std::vector<Entry> batch_;
    unsigned batch_layer_=0,batch_index_=0; // Layer (18 = compose) and slot/group within it.
    std::array<Timing,4> timings_;
    unsigned timing_next_=0;
    double work_per_ns_=2000,gpu_ms_=0; // Multiply-adds per nanosecond; 2000 is 2 TMAC/s.
    StepReport report_;
};
}
