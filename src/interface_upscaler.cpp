#include "interface_upscaler.h"
#include "shaders.h"
#include "upscaler_model.h"
#include <algorithm>
#include <memory>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

namespace smbd {
namespace {
void checked_ui(HRESULT status,const char* operation) {
    if(FAILED(status)) throw std::runtime_error(std::format("{} failed (0x{:08X})",operation,static_cast<unsigned>(status)));
}
// Mirrors the HLSL Job and Layer constant buffers in shaders.cpp.
struct JobConstants {
    std::int32_t image[2]{},origin[2]{},extent[2]{},halo[2]{},interior[2]{},slots[2]{};
    std::uint32_t mode=0,factor=1;
    std::int32_t target[2]{};
};
static_assert(sizeof(JobConstants)==64);
struct LayerConstants {
    std::array<Float4,2304> weights{};
    std::array<Float4,4> bias{},slope{};
    std::array<std::uint32_t,4> layer{};
};
static_assert(sizeof(LayerConstants)==2313*16);
struct BatchConstants {std::int32_t extents[interface_batch][4]{};};
static_assert(sizeof(BatchConstants)==interface_batch*16);
constexpr unsigned channel_groups(const UpscalerLayer& layer) {return layer.outputs/16;}
constexpr unsigned divide(unsigned value,unsigned by) {return (value+by-1)/by;}
std::vector<float> embedded_model() {
    const auto bytes=upscaler_weights();
    if(bytes.size()!=upscaler_model_floats()*4) throw std::runtime_error("The embedded HUD upscaler model has an unexpected size");
    std::vector<float> model(upscaler_model_floats());
    std::memcpy(model.data(),bytes.data(),bytes.size());
    return model;
}
float catmull_rom(float x) {
    x=std::abs(x);
    return x<1?(1.5f*x-2.5f)*x*x+1:x<2?((-0.5f*x+2.5f)*x-4)*x+2:0;
}
}

InterfaceDecision classify_interface(std::span<const float> rgba,unsigned width,unsigned height) {
    if(rgba.size()!=std::size_t(width)*height*4) throw std::runtime_error("Interface texture size differs from its pixels");
    InterfaceDecision decision;
    const auto premultiplied=[&](unsigned x,unsigned y,unsigned c) {
        const auto* p=&rgba[(std::size_t(y)*width+x)*4];return c<3?p[c]*p[3]:p[3];
    };
    std::uint32_t edge=0;
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        float step=0;
        for(unsigned c=0;c<4;++c) {
            const auto v=premultiplied(x,y,c);
            if(x+1<width) step=std::max(step,std::abs(premultiplied(x+1,y,c)-v));
            if(y+1<height) step=std::max(step,std::abs(premultiplied(x,y+1,c)-v));
        }
        edge=std::max(edge,static_cast<std::uint32_t>(std::lround(std::clamp(step,0.0f,1.0f)*65535)));
        decision.transparent|=rgba[(std::size_t(y)*width+x)*4+3]<254.5f/255;
    }
    decision.edge=edge/65535.0f;
    decision.network=std::min(width,height)>=interface_network_minimum && decision.edge>=interface_network_edge;
    return decision;
}

std::vector<float> reference_interface_upscale(std::span<const float> rgba,unsigned width,unsigned height,unsigned factor,
                                               std::span<const float> model,InterfaceDecision* result) {
    if(factor<1 || factor>4 || model.size()!=upscaler_model_floats()) throw std::runtime_error("Invalid interface reference request");
    auto decision=std::min(width,height)<interface_network_minimum?InterfaceDecision{}:classify_interface(rgba,width,height);
    if(result) *result=decision;
    const auto at=[&](std::span<const float> image,int x,int y) {
        x=std::clamp(x,0,int(width)-1);y=std::clamp(y,0,int(height)-1);
        return &image[(std::size_t(y)*width+x)*4];
    };
    std::vector<float> output(std::size_t(width)*factor*height*factor*4);
    if(!decision.network) {
        for(unsigned oy=0;oy<height*factor;++oy) for(unsigned ox=0;ox<width*factor;++ox) {
            const float ux=(ox+0.5f)/factor-0.5f,uy=(oy+0.5f)/factor-0.5f;
            const float cx=std::floor(ux),cy=std::floor(uy),fx=ux-cx,fy=uy-cy;
            const int bx=int(cx),by=int(cy);
            const auto texel=[&](int x,int y,unsigned c) {const auto* p=at(rgba,x,y);return c<3?p[c]*p[3]:p[3];};
            std::array<float,4> sum{},low{},high{};float total=0;
            for(int y=-1;y<=2;++y) for(int x=-1;x<=2;++x) {
                const float w=catmull_rom(fx-x)*catmull_rom(fy-y);total+=w;
                for(unsigned c=0;c<4;++c) sum[c]+=texel(bx+x,by+y,c)*w;
            }
            auto* out=&output[(std::size_t(oy)*width*factor+ox)*4];
            for(unsigned c=0;c<4;++c) {
                low[c]=std::min({texel(bx,by,c),texel(bx+1,by,c),texel(bx,by+1,c),texel(bx+1,by+1,c)});
                high[c]=std::max({texel(bx,by,c),texel(bx+1,by,c),texel(bx,by+1,c),texel(bx+1,by+1,c)});
                out[c]=std::clamp(sum[c]/total,low[c],high[c]);
            }
            if(out[3]>1.0f/1024) for(unsigned c=0;c<3;++c) out[c]/=out[3];
            else {
                const auto* v=at(rgba,int(std::lround(ux)),int(std::lround(uy)));
                out[0]=v[0];out[1]=v[1];out[2]=v[2];out[3]=0;
            }
            for(unsigned c=0;c<4;++c) out[c]=std::clamp(out[c],0.0f,1.0f);
        }
        return output;
    }
    // Colour spread into transparent texels, exactly as interface_bleed.
    std::vector<float> colour(rgba.begin(),rgba.end());
    if(decision.transparent) for(unsigned pass=0;pass<interface_bleed_passes;++pass) {
        std::vector<float> next(colour.size());
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const auto* v=&colour[(std::size_t(y)*width+x)*4];auto* n=&next[(std::size_t(y)*width+x)*4];
            if(v[3]>0) {n[0]=v[0];n[1]=v[1];n[2]=v[2];n[3]=1;continue;}
            float sum[3]{};float count=0;
            for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                const int qx=int(x)+dx,qy=int(y)+dy;
                if(qx<0 || qy<0 || qx>=int(width) || qy>=int(height)) continue;
                const auto* q=&colour[(std::size_t(qy)*width+qx)*4];
                if(q[3]>0) {sum[0]+=q[0];sum[1]+=q[1];sum[2]+=q[2];count+=1;}
            }
            if(count>0) {n[0]=sum[0]/count;n[1]=sum[1]/count;n[2]=sum[2]/count;n[3]=1;}
        }
        colour=std::move(next);
    }
    const unsigned halo=interface_halo,ew=width+2*halo,eh=height+2*halo;
    const auto layers=upscaler_layout();
    const auto run=[&](bool alpha) {
        std::vector<float> current(std::size_t(ew)*eh*3);
        for(unsigned y=0;y<eh;++y) for(unsigned x=0;x<ew;++x) {
            const int sx=int(x)-int(halo),sy=int(y)-int(halo);
            const auto* v=at(alpha?rgba:std::span<const float>(colour),sx,sy);
            for(unsigned c=0;c<3;++c) current[(std::size_t(y)*ew+x)*3+c]=alpha?v[3]:v[c];
        }
        for(const auto& layer:layers) {
            std::vector<float> weights(std::size_t(9)*layer.inputs*layer.outputs);
            for(unsigned o=0;o<layer.outputs;++o) for(unsigned c=0;c<layer.inputs;++c) for(unsigned t=0;t<9;++t)
                weights[(std::size_t(t)*layer.inputs+c)*layer.outputs+o]=model[layer.weights+(std::size_t(o)*layer.inputs+c)*9+t];
            std::vector<float> next(std::size_t(ew)*eh*layer.outputs);
            for(unsigned y=0;y<eh;++y) for(unsigned x=0;x<ew;++x) {
                float* out=&next[(std::size_t(y)*ew+x)*layer.outputs];
                for(unsigned o=0;o<layer.outputs;++o) out[o]=model[layer.bias+o];
                for(unsigned t=0;t<9;++t) {
                    const int qx=int(x)+int(t%3)-1,qy=int(y)+int(t/3)-1;
                    if(qx<0 || qy<0 || qx>=int(ew) || qy>=int(eh)) continue; // Zero padding.
                    const float* in=&current[(std::size_t(qy)*ew+qx)*layer.inputs];
                    for(unsigned c=0;c<layer.inputs;++c) {
                        const float v=in[c];const float* w=&weights[(std::size_t(t)*layer.inputs+c)*layer.outputs];
                        for(unsigned o=0;o<layer.outputs;++o) out[o]+=v*w[o];
                    }
                }
                if(layer.slope!=SIZE_MAX) for(unsigned o=0;o<layer.outputs;++o) if(out[o]<0) out[o]*=model[layer.slope+o];
            }
            current=std::move(next);
        }
        return current;
    };
    const auto colour_net=run(false);
    const auto alpha_net=decision.transparent?run(true):std::vector<float>{};
    const auto network=[&](unsigned sx,unsigned sy) {
        const unsigned lx=sx/4,ly=sy/4,i=sy&3,j=sx&3;
        const std::size_t e=(std::size_t(ly+halo)*ew+lx+halo)*48;
        const auto* base=&colour[(std::size_t(ly)*width+lx)*4];const float alpha=rgba[(std::size_t(ly)*width+lx)*4+3];
        std::array<float,4> value{};
        for(unsigned c=0;c<3;++c) value[c]=std::clamp(colour_net[e+c*16+i*4+j]+base[c],0.0f,1.0f);
        if(decision.transparent) {
            float a=0;for(unsigned c=0;c<3;++c) a+=std::clamp(alpha_net[e+c*16+i*4+j]+alpha,0.0f,1.0f);
            value[3]=a/3;
        } else value[3]=alpha;
        return value;
    };
    for(unsigned oy=0;oy<height*factor;++oy) for(unsigned ox=0;ox<width*factor;++ox) {
        auto* out=&output[(std::size_t(oy)*width*factor+ox)*4];
        std::array<float,4> value;
        if(factor==4) value=network(ox,oy);
        else {
            const float lx=float(ox)*4/factor,ly=float(oy)*4/factor,hx=float(ox+1)*4/factor,hy=float(oy+1)*4/factor;
            std::array<float,4> sum{};std::array<float,3> plain{};float total=0;
            for(int y=int(std::floor(ly));y<int(std::ceil(hy));++y) {
                const float wy=std::min(hy,float(y+1))-std::max(ly,float(y));
                for(int x=int(std::floor(lx));x<int(std::ceil(hx));++x) {
                    const float w=wy*(std::min(hx,float(x+1))-std::max(lx,float(x)));
                    const auto v=network(unsigned(x),unsigned(y));
                    for(unsigned c=0;c<3;++c) {sum[c]+=v[c]*v[3]*w;plain[c]+=v[c]*w;}
                    sum[3]+=v[3]*w;total+=w;
                }
            }
            for(auto& v:sum) v/=total;
            value=sum;
            for(unsigned c=0;c<3;++c) value[c]=sum[3]>1.0f/1024?sum[c]/sum[3]:plain[c]/total;
        }
        for(unsigned c=0;c<4;++c) out[c]=std::clamp(value[c],0.0f,1.0f);
    }
    return output;
}


struct InterfaceUpscaler::Job {
    enum class Stage {classify,decide,bleed,network,filter,mips,done};
    std::uint32_t id;
    Ptr<ID3D11ShaderResourceView> source;
    unsigned width,height,factor;
    Stage stage=Stage::classify;
    Ptr<ID3D11Buffer> statistics,readback;
    Ptr<ID3D11UnorderedAccessView> statistics_view;
    InterfaceDecision decision;
    std::array<Ptr<ID3D11Texture2D>,2> bleed;
    std::array<Ptr<ID3D11ShaderResourceView>,2> bleed_view;
    std::array<Ptr<ID3D11UnorderedAccessView>,2> bleed_output;
    unsigned bleed_pass=0;
    Ptr<ID3D11Texture2D> output;
    Ptr<ID3D11ShaderResourceView> view;
    Ptr<ID3D11UnorderedAccessView> output_view;
    std::size_t bytes=0,working=0;
    unsigned tiles=0,next_tile=0,composed=0;
    ID3D11ShaderResourceView* colour() const {
        return decision.transparent?bleed_view[(interface_bleed_passes-1)%2].Get():source.Get();
    }
};

InterfaceUpscaler::InterfaceUpscaler(ID3D11Device* device,ID3D11DeviceContext* context,std::uint64_t& compilations,unsigned tile)
    :device_(device),context_(context),tile_(tile) {
    if(tile<8 || tile>interface_tile) throw std::runtime_error("Invalid HUD upscaler tile size");
    const std::pair<ShaderKind,Ptr<ID3D11ComputeShader>*> shaders[]{
        // std::addressof: the POSIX ComPtr's operator& releases and returns T**.
        {ShaderKind::interface_classify,std::addressof(classify_)},{ShaderKind::interface_bleed,std::addressof(bleed_)},
        {ShaderKind::interface_conv_first,std::addressof(first_)},{ShaderKind::interface_conv,std::addressof(conv_)},
        {ShaderKind::interface_compose,std::addressof(compose_)},{ShaderKind::interface_filter,std::addressof(filter_)}};
    for(const auto& [kind,shader]:shaders) {
        const auto binary=shader_binary(kind);compilations+=unsigned(binary.compiled);
        checked_ui(device->CreateComputeShader(binary.bytes.data(),binary.bytes.size(),nullptr,shader->ReleaseAndGetAddressOf()),"HUD upscaler shader");
    }
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(JobConstants);buffer.Usage=D3D11_USAGE_DYNAMIC;
    buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    checked_ui(device->CreateBuffer(&buffer,nullptr,&job_constants_),"HUD upscaler parameters");
    buffer.ByteWidth=sizeof(BatchConstants);
    checked_ui(device->CreateBuffer(&buffer,nullptr,&batch_constants_),"HUD upscaler batch");
    // Repack each convolution once into constant buffers of 16 outputs, so
    // every unrolled multiply reads a uniform constant.
    const auto model=embedded_model();
    const auto layout=upscaler_layout();
    auto constants=std::make_unique<LayerConstants>();
    for(unsigned l=0;l<layout.size();++l) {
        const auto& layer=layout[l];layer_index_[l]=static_cast<unsigned>(layers_.size());
        for(unsigned g=0;g<channel_groups(layer);++g) {
            *constants={};constants->layer={g*4,0,0,0};
            for(unsigned k=0;k<4;++k) for(unsigned m=0;m<4;++m) {
                const auto o=g*16+k*4+m;
                constants->bias[k][m]=model[layer.bias+o];
                constants->slope[k][m]=layer.slope==SIZE_MAX?1.0f:model[layer.slope+o];
                for(unsigned c=0;c<layer.inputs;++c) for(unsigned t=0;t<9;++t)
                    constants->weights[(t*layer.inputs+c)*4+k][m]=model[layer.weights+(std::size_t(o)*layer.inputs+c)*9+t];
            }
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(LayerConstants);desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            const D3D11_SUBRESOURCE_DATA data{constants.get()};
            checked_ui(device->CreateBuffer(&desc,&data,&layers_.emplace_back()),"HUD upscaler weights");
        }
    }
    layer_index_.back()=static_cast<unsigned>(layers_.size());
    for(auto& timing:timings_) {
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
        checked_ui(device->CreateQuery(&query,&timing.disjoint),"HUD upscaler timing");
        query.Query=D3D11_QUERY_TIMESTAMP;
        checked_ui(device->CreateQuery(&query,&timing.start),"HUD upscaler timing");
        checked_ui(device->CreateQuery(&query,&timing.finish),"HUD upscaler timing");
    }
}
InterfaceUpscaler::~InterfaceUpscaler()=default;

void InterfaceUpscaler::request(std::uint32_t id,Ptr<ID3D11ShaderResourceView> source,unsigned width,unsigned height,unsigned factor) {
    if(!source || !width || !height || factor<2 || factor>4 || width*factor>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       height*factor>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) throw std::runtime_error("Invalid HUD upscaling request");
    cancel(id);
    auto& job=jobs_.emplace_back();
    job.id=id;job.source=std::move(source);job.width=width;job.height=height;job.factor=factor;
    if(std::min(width,height)<interface_network_minimum) job.stage=Job::Stage::filter;
}
InterfaceUpscaler::Job* InterfaceUpscaler::find(std::uint32_t id) {
    const auto found=std::ranges::find_if(jobs_,[&](const Job& job) {return job.id==id;});
    return found==jobs_.end()?nullptr:&*found;
}
void InterfaceUpscaler::abort_batch(std::uint32_t cancelled) {
    // Tiles already composed stay done; every other job takes its pending
    // tiles back for a later batch.
    const std::size_t pending=batch_layer_==upscaler_layers?batch_index_:0;
    for(std::size_t i=pending;i<batch_.size();++i) if(batch_[i].job!=cancelled)
        if(auto* job=find(batch_[i].job)) job->next_tile=std::min(job->next_tile,batch_[i].tile);
    batch_.clear();batch_layer_=batch_index_=0;
}
void InterfaceUpscaler::cancel(std::uint32_t id) {
    if(std::ranges::any_of(batch_,[&](const Entry& entry) {return entry.job==id;})) abort_batch(id);
    std::erase_if(jobs_,[&](const Job& job) {return job.id==id;});
}
void InterfaceUpscaler::clear() {jobs_.clear();batch_.clear();batch_layer_=batch_index_=0;features_={};}
bool InterfaceUpscaler::queued(std::uint32_t id) const {
    return std::ranges::any_of(jobs_,[&](const Job& job) {return job.id==id;});
}
std::size_t InterfaceUpscaler::bytes() const {
    std::size_t total=0;
    if(features_[0].texture) total=std::size_t(tile_+2*interface_halo)*(tile_+2*interface_halo)*8*16*interface_batch*2;
    for(const auto& job:jobs_) total+=job.working+job.bytes;
    return total;
}
void InterfaceUpscaler::constants(ID3D11Buffer* buffer,const void* data,std::size_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked_ui(context_->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"HUD upscaler parameters");
    std::memcpy(mapped.pData,data,size);context_->Unmap(buffer,0);
}
void InterfaceUpscaler::dispatch(ID3D11ComputeShader* shader,std::initializer_list<ID3D11ShaderResourceView*> inputs,
                                 ID3D11UnorderedAccessView* output,std::initializer_list<ID3D11Buffer*> buffers,
                                 unsigned x,unsigned y,unsigned z) {
    // Release prior inputs first; an output may have been the previous input.
    ID3D11ShaderResourceView* const empty[4]{};
    context_->CSSetShaderResources(0,4,empty);
    context_->CSSetUnorderedAccessViews(0,1,&output,nullptr);
    context_->CSSetShaderResources(0,static_cast<UINT>(inputs.size()),inputs.begin());
    context_->CSSetConstantBuffers(0,static_cast<UINT>(buffers.size()),buffers.begin());
    context_->CSSetShader(shader,nullptr,0);
    context_->Dispatch(x,y,z);
}
void InterfaceUpscaler::allocate_features() {
    if(features_[0].texture) return;
    const unsigned size=tile_+2*interface_halo;
    for(auto& plane:features_) {
        D3D11_TEXTURE2D_DESC texture{};texture.Width=texture.Height=size;texture.MipLevels=1;texture.ArraySize=16*interface_batch;
        texture.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;texture.SampleDesc.Count=1;
        texture.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        checked_ui(device_->CreateTexture2D(&texture,nullptr,&plane.texture),"HUD upscaler features");
        checked_ui(device_->CreateShaderResourceView(plane.texture.Get(),nullptr,&plane.view),"HUD upscaler feature view");
        D3D11_UNORDERED_ACCESS_VIEW_DESC access{};access.Format=texture.Format;access.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        access.Texture2DArray.ArraySize=texture.ArraySize;
        checked_ui(device_->CreateUnorderedAccessView(plane.texture.Get(),&access,&plane.output),"HUD upscaler feature output");
    }
}
void InterfaceUpscaler::create_output(Job& job) {
    if(job.output) return;
    const auto w=job.width*job.factor,h=job.height*job.factor;
    D3D11_TEXTURE2D_DESC texture{};texture.Width=w;texture.Height=h;texture.MipLevels=0;texture.ArraySize=1;
    texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.SampleDesc.Count=1;
    texture.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET;
    texture.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
    checked_ui(device_->CreateTexture2D(&texture,nullptr,&job.output),"Upscaled HUD texture");
    checked_ui(device_->CreateShaderResourceView(job.output.Get(),nullptr,&job.view),"Upscaled HUD view");
    D3D11_UNORDERED_ACCESS_VIEW_DESC access{};access.Format=texture.Format;access.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
    checked_ui(device_->CreateUnorderedAccessView(job.output.Get(),&access,&job.output_view),"Upscaled HUD output");
    for(unsigned mw=w,mh=h;;mw=std::max(1U,mw/2),mh=std::max(1U,mh/2)) {
        job.bytes+=std::size_t(mw)*mh*4;if(mw==1 && mh==1) break;
    }
}
bool InterfaceUpscaler::decide(Job& job,bool wait) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto status=context_->Map(job.readback.Get(),0,D3D11_MAP_READ,wait?0:D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if(status==DXGI_ERROR_WAS_STILL_DRAWING) return false;
    checked_ui(status,"HUD texture classification");
    std::uint32_t values[2];std::memcpy(values,mapped.pData,sizeof(values));
    context_->Unmap(job.readback.Get(),0);
    job.statistics.Reset();job.statistics_view.Reset();job.readback.Reset();
    job.decision.edge=values[0]/65535.0f;job.decision.transparent=values[1]!=0;
    job.decision.network=job.decision.edge>=interface_network_edge;
    if(!job.decision.network) {job.stage=Job::Stage::filter;return true;}
    job.tiles=divide(job.width,tile_)*divide(job.height,tile_);
    if(job.decision.transparent) {
        for(unsigned i=0;i<2;++i) {
            D3D11_TEXTURE2D_DESC texture{};texture.Width=job.width;texture.Height=job.height;texture.MipLevels=texture.ArraySize=1;
            texture.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;texture.SampleDesc.Count=1;
            texture.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            checked_ui(device_->CreateTexture2D(&texture,nullptr,&job.bleed[i]),"HUD colour spread");
            checked_ui(device_->CreateShaderResourceView(job.bleed[i].Get(),nullptr,&job.bleed_view[i]),"HUD colour spread view");
            checked_ui(device_->CreateUnorderedAccessView(job.bleed[i].Get(),nullptr,&job.bleed_output[i]),"HUD colour spread output");
        }
        job.working=std::size_t(job.width)*job.height*16;
        job.stage=Job::Stage::bleed;
    } else job.stage=Job::Stage::network;
    return true;
}
std::uint64_t InterfaceUpscaler::run(Job& job) {
    const auto w=job.width,h=job.height,f=job.factor;
    JobConstants c;c.image[0]=int(w);c.image[1]=int(h);c.factor=f;
    switch(job.stage) {
    case Job::Stage::filter:
        create_output(job);constants(job_constants_.Get(),&c,sizeof(c));
        dispatch(filter_.Get(),{job.source.Get()},job.output_view.Get(),{job_constants_.Get()},divide(w*f,8),divide(h*f,8));
        job.stage=Job::Stage::mips;
        return std::uint64_t(w)*h*f*f*64;
    case Job::Stage::bleed: {
        constants(job_constants_.Get(),&c,sizeof(c));
        const auto pass=job.bleed_pass;
        dispatch(bleed_.Get(),{pass?job.bleed_view[(pass-1)%2].Get():job.source.Get()},job.bleed_output[pass%2].Get(),{job_constants_.Get()},
                 divide(w,8),divide(h,8));
        if(++job.bleed_pass==interface_bleed_passes) job.stage=Job::Stage::network;
        return std::uint64_t(w)*h*36;
    }
    case Job::Stage::mips:
        context_->GenerateMips(job.view.Get());
        job.bleed={};job.bleed_view={};job.bleed_output={};job.working=0;
        job.stage=Job::Stage::done;
        return std::uint64_t(w)*h*f*f*4;
    default:throw std::runtime_error("HUD upscaler job is not runnable");
    }
}
bool InterfaceUpscaler::form_batch() {
    // Tiles from consecutive jobs share each convolution dispatch; a
    // transparent tile occupies two slots, colour then alpha.
    batch_.clear();batch_layer_=batch_index_=0;
    for(auto& job:jobs_) {
        if(job.stage!=Job::Stage::network) continue;
        const unsigned slots=job.decision.transparent?2:1,tiles_x=divide(job.width,tile_);
        while(job.next_tile<job.tiles && batch_.size()+slots<=interface_batch) {
            const unsigned tx=job.next_tile%tiles_x,ty=job.next_tile/tiles_x;
            Entry entry{job.id,job.next_tile,false,{},{},{}};
            entry.interior[0]=int(std::min(tile_,job.width-tx*tile_));entry.interior[1]=int(std::min(tile_,job.height-ty*tile_));
            entry.origin[0]=int(tx*tile_)-int(interface_halo);entry.origin[1]=int(ty*tile_)-int(interface_halo);
            entry.extent[0]=entry.interior[0]+2*int(interface_halo);entry.extent[1]=entry.interior[1]+2*int(interface_halo);
            batch_.push_back(entry);
            if(slots==2) {entry.alpha=true;batch_.push_back(entry);}
            create_output(job);++job.next_tile;
        }
        if(batch_.size()==interface_batch) break;
    }
    if(batch_.empty()) return false;
    allocate_features();
    BatchConstants batch;
    for(std::size_t i=0;i<batch_.size();++i) {batch.extents[i][0]=batch_[i].extent[0];batch.extents[i][1]=batch_[i].extent[1];}
    constants(batch_constants_.Get(),&batch,sizeof(batch));
    return true;
}
std::uint64_t InterfaceUpscaler::run_batch() {
    static const auto layout=upscaler_layout();
    const auto job_constants=[&](const Entry& entry,const Job& job) {
        JobConstants c;c.image[0]=int(job.width);c.image[1]=int(job.height);c.factor=job.factor;
        for(unsigned axis=0;axis<2;++axis) {
            c.origin[axis]=entry.origin[axis];c.extent[axis]=entry.extent[axis];c.interior[axis]=entry.interior[axis];
            c.halo[axis]=int(interface_halo);c.target[axis]=(entry.origin[axis]+int(interface_halo))*int(job.factor);
        }
        return c;
    };
    if(!batch_layer_) {
        const auto slot=batch_index_/4,group=batch_index_%4;
        const auto& entry=batch_[slot];auto* job=find(entry.job);
        if(!job) throw std::runtime_error("HUD upscaler batch lost its job");
        auto c=job_constants(entry,*job);c.slots[0]=int(slot);c.mode=entry.alpha;
        constants(job_constants_.Get(),&c,sizeof(c));
        dispatch(first_.Get(),{entry.alpha?job->source.Get():job->colour()},features_[0].output.Get(),
                 {job_constants_.Get(),layers_[layer_index_[0]+group].Get()},divide(unsigned(entry.extent[0]),8),divide(unsigned(entry.extent[1]),8));
        if(++batch_index_==batch_.size()*4) {batch_index_=0;batch_layer_=1;}
        return std::uint64_t(entry.extent[0])*entry.extent[1]*3*144;
    }
    if(batch_layer_<upscaler_layers) {
        const auto& layer=layout[batch_layer_];
        unsigned width=0,height=0;std::uint64_t work=0;
        for(const auto& entry:batch_) {
            width=std::max(width,unsigned(entry.extent[0]));height=std::max(height,unsigned(entry.extent[1]));
            work+=std::uint64_t(entry.extent[0])*entry.extent[1]*layer.inputs*144;
        }
        dispatch(conv_.Get(),{features_[(batch_layer_-1)%2].view.Get()},features_[batch_layer_%2].output.Get(),
                 {job_constants_.Get(),layers_[layer_index_[batch_layer_]+batch_index_].Get(),batch_constants_.Get()},
                 divide(width,8),divide(height,8),static_cast<unsigned>(batch_.size()));
        if(++batch_index_==channel_groups(layer)) {batch_index_=0;++batch_layer_;}
        return work;
    }
    // The final layer wrote features_[1]; compose one tile per step.
    const auto& entry=batch_[batch_index_];auto* job=find(entry.job);
    if(!job) throw std::runtime_error("HUD upscaler batch lost its job");
    const bool alpha=job->decision.transparent;
    auto c=job_constants(entry,*job);c.slots[0]=int(batch_index_);c.slots[1]=int(batch_index_+(alpha?1:0));c.mode=alpha;
    constants(job_constants_.Get(),&c,sizeof(c));
    dispatch(compose_.Get(),{job->colour(),job->source.Get(),features_[1].view.Get()},job->output_view.Get(),{job_constants_.Get()},
             divide(unsigned(entry.interior[0])*job->factor,8),divide(unsigned(entry.interior[1])*job->factor,8));
    batch_index_+=alpha?2:1;
    if(++job->composed==job->tiles) job->stage=Job::Stage::mips;
    if(batch_index_>=batch_.size()) {batch_.clear();batch_layer_=batch_index_=0;}
    return std::uint64_t(entry.interior[0])*entry.interior[1]*job->factor*job->factor*64;
}
void InterfaceUpscaler::collect_timings() {
    for(auto& timing:timings_) if(timing.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};UINT64 start=0,finish=0;
        const auto status=context_->GetData(timing.disjoint.Get(),&result,sizeof(result),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        checked_ui(status,"HUD upscaler timing");if(status!=S_OK) continue;
        const auto first=context_->GetData(timing.start.Get(),&start,sizeof(start),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const auto last=context_->GetData(timing.finish.Get(),&finish,sizeof(finish),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        checked_ui(first,"HUD upscaler timing");checked_ui(last,"HUD upscaler timing");
        if(first!=S_OK || last!=S_OK) continue;
        timing.pending=false;
        if(result.Disjoint || !result.Frequency || finish<start) continue;
        const double ns=double(finish-start)*1e9/double(result.Frequency);
        gpu_ms_=ns/1e6;
        // Learn this device's throughput from steps large enough to time.
        if(timing.work>=(8u<<20) && ns>0) work_per_ns_=std::clamp(0.75*work_per_ns_+0.25*double(timing.work)/ns,50.0,50000.0);
    }
}
std::vector<InterfaceUpscaler::Finished> InterfaceUpscaler::step(bool finish,double target_ms) {
    collect_timings();
    std::vector<Finished> finished;
    if(jobs_.empty()) return finished;
    auto& timing=timings_[timing_next_];
    const bool timed=!finish && !timing.pending;
    if(timed) {context_->Begin(timing.disjoint.Get());context_->End(timing.start.Get());}
    const auto budget=finish?UINT64_MAX:static_cast<std::uint64_t>(std::clamp(target_ms*1e6*work_per_ns_,double(16u<<20),2e10));
    std::uint64_t used=0,units=0;
    for(auto& job:jobs_) if(job.stage==Job::Stage::classify) {
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=8;buffer.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        checked_ui(device_->CreateBuffer(&buffer,nullptr,&job.statistics),"HUD classification");
        D3D11_UNORDERED_ACCESS_VIEW_DESC access{};access.Format=DXGI_FORMAT_R32_UINT;access.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        access.Buffer.NumElements=2;
        checked_ui(device_->CreateUnorderedAccessView(job.statistics.Get(),&access,&job.statistics_view),"HUD classification view");
        buffer={};buffer.ByteWidth=8;buffer.Usage=D3D11_USAGE_STAGING;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked_ui(device_->CreateBuffer(&buffer,nullptr,&job.readback),"HUD classification readback");
        const UINT zero[4]{};context_->ClearUnorderedAccessViewUint(job.statistics_view.Get(),zero);
        JobConstants c;c.image[0]=int(job.width);c.image[1]=int(job.height);constants(job_constants_.Get(),&c,sizeof(c));
        dispatch(classify_.Get(),{job.source.Get()},job.statistics_view.Get(),{job_constants_.Get()},divide(job.width,8),divide(job.height,8));
        context_->CopyResource(job.readback.Get(),job.statistics.Get());
        job.stage=Job::Stage::decide;used+=std::uint64_t(job.width)*job.height*16;
    }
    for(;;) {
        if(!batch_.empty()) used+=run_batch();
        else {
            // Single-texture work first, then a fresh batch of network tiles.
            Job* next=nullptr;
            for(auto& job:jobs_) {
                if(job.stage==Job::Stage::decide && !decide(job,finish)) continue;
                if(job.stage==Job::Stage::bleed || job.stage==Job::Stage::filter || job.stage==Job::Stage::mips) {next=&job;break;}
            }
            if(next) used+=run(*next);
            else if(form_batch()) used+=run_batch();
            else break;
        }
        for(auto job=jobs_.begin();job!=jobs_.end();) {
            if(job->stage!=Job::Stage::done) {++job;continue;}
            finished.push_back({job->id,{job->view,job->width*job->factor,job->height*job->factor,job->bytes,job->decision.network}});
            job=jobs_.erase(job);
        }
        ++units;
        if(used>=budget) break;
    }
    report_={budget,used,units,work_per_ns_,0,0};
    for(const auto& job:jobs_) {report_.waiting+=job.stage==Job::Stage::decide;report_.network+=job.stage==Job::Stage::network;}
    if(timed) {
        context_->End(timing.finish.Get());context_->End(timing.disjoint.Get());
        timing.work=used;timing.pending=true;timing_next_=(timing_next_+1)%timings_.size();
    }
    ID3D11ShaderResourceView* const empty[4]{};ID3D11UnorderedAccessView* const none=nullptr;
    context_->CSSetShaderResources(0,4,empty);context_->CSSetUnorderedAccessViews(0,1,&none,nullptr);
    context_->CSSetShader(nullptr,nullptr,0);
    return finished;
}
}
