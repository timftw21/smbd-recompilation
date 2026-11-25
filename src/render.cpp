#include "render.h"
#include "antialiasing.h"
#include "interface_upscaler.h"
#include "upscaler_model.h"
#include "shaders.h"
#include "performance.h"
#include <SDL3/SDL.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#ifdef _WIN32
#include <Windows.h>
#include <wincodec.h>
#else
#include <png.h>
#include <jpeglib.h>
#include <csetjmp>
#include <cstdlib>
#endif
#include "com_ptr.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace smbd {
namespace {
void checked(HRESULT result, std::string_view operation) {
    if (FAILED(result)) throw std::runtime_error(std::format("{} failed: 0x{:08X}", operation, static_cast<unsigned>(result)));
}
void extent(std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > 4096 || height > 4096)
        throw std::runtime_error("Image dimensions must be 1..4096");
}
Float4 color(std::uint32_t argb) {
    return {float((argb >> 16) & 255) / 255, float((argb >> 8) & 255) / 255,
            float(argb & 255) / 255, float(argb >> 24) / 255};
}
#ifdef _WIN32
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComScope() { if (result != RPC_E_CHANGED_MODE) checked(result, "COM initialization"); }
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
ComPtr<IWICImagingFactory> image_factory() {
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&factory)), "WIC factory");
    return factory;
}
#endif
void validate_fragment(const RenderState& state) {
    const auto count=state.combiner_control&255;
    if(count>8) throw std::runtime_error("Combiner stage count exceeds eight");
    if(state.alpha_test && (state.alpha_function<0x200 || state.alpha_function>0x207))
        throw std::runtime_error("Invalid alpha comparison function");
    const auto input=[](unsigned word,bool final) {
        const auto reg=word&15;
        if(!(reg<=5 || (reg>=8 && reg<=13) || (final && reg>=14)))
            throw std::runtime_error("Reserved combiner source register");
    };
    const auto output=[](unsigned word,unsigned allowed) {
        const auto flags=word>>12,mapping=flags&0x38;
        if(flags&~allowed || mapping==0x28 || mapping==0x38) throw std::runtime_error("Unsupported combiner output flags");
        for(unsigned shift:{0U,4U,8U}) {
            const auto reg=(word>>shift)&15;
            if(!(reg==0 || reg==4 || reg==5 || (reg>=8 && reg<=13)))
                throw std::runtime_error("Reserved combiner destination register");
        }
    };
    for(unsigned stage=0;stage<count;++stage) {
        for(unsigned shift:{0U,8U,16U,24U}) {input(state.color_inputs[stage]>>shift,false);input(state.alpha_inputs[stage]>>shift,false);}
        output(state.color_outputs[stage],0xFF);output(state.alpha_outputs[stage],0x3C);
    }
    if(state.final_inputs[0] || state.final_inputs[1]) {
        for(unsigned shift:{0U,8U,16U,24U}) input(state.final_inputs[0]>>shift,true);
        for(unsigned shift:{8U,16U,24U}) input(state.final_inputs[1]>>shift,true);
    }
    for(unsigned i=0;i<4;++i) {
        const auto mode=state.texture_modes[i];
        if(mode!=0 && mode!=1 && mode!=2 && mode!=3 && mode!=4 && mode!=5 && mode!=6 && mode!=15 && mode!=16)
            throw std::runtime_error("Texture shader mode is not supported yet: "+hex32(mode));
        if(mode==5 && state.texture_clip[i]&~15U) throw std::runtime_error("Invalid texture clip-plane comparisons");
        if((mode==6 || mode==15 || mode==16) && state.texture_sources[i]>=i) throw std::runtime_error("Dependent texture must read an earlier stage");
        if((mode==2 || mode==3) && (((state.texture_filter[i]>>16)&63)==7 || ((state.texture_filter[i]>>24)&15)==4))
            throw std::runtime_error("Convolution texture filtering requires a 2D texture");
    }
}
unsigned simple_combiner(const RenderState& s) {
    // These complete instruction patterns reduce exactly to vertex color or
    // color times texture. Other programs retain the general compiled shader.
    if(s.texture_modes[1] || s.texture_modes[2] || s.texture_modes[3]) return 4;
    const auto count=s.combiner_control&255;
    if(!count && !s.texture_modes[0] && s.final_inputs==std::array<std::uint32_t,2>{4,0x1400}) return 0;
    const bool direct=(!s.final_inputs[0] && !s.final_inputs[1]) ||
        (s.final_inputs[0]==12 && (s.final_inputs[1]&0xFFFFFF00U)==0x1C00);
    if(count!=1 || !direct || (s.color_outputs[0]!=0xC0 && s.color_outputs[0]!=0xC00) ||
       (s.alpha_outputs[0]!=0xC0 && s.alpha_outputs[0]!=0xC00)) return 4;
    if(!s.texture_modes[0] && s.color_inputs[0]==0x04200000 && s.alpha_inputs[0]==0x14200000) return 0;
    if((s.color_inputs[0]!=0x04080000 && s.color_inputs[0]!=0x08040000) ||
       (s.alpha_inputs[0]!=0x14180000 && s.alpha_inputs[0]!=0x18140000)) return 4;
    switch(s.texture_modes[0]) {case 1:return 1;case 3:return 2;case 2:return 3;default:return 4;}
}
D3D11_COMPARISON_FUNC comparison(std::uint32_t value) {
    if (value < 0x200 || value > 0x207) throw std::runtime_error("Invalid depth comparison function");
    return static_cast<D3D11_COMPARISON_FUNC>(value - 0x200 + 1);
}
D3D11_BLEND blend_factor(std::uint32_t value) {
    switch (value) {
    case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
    case 0x300: return D3D11_BLEND_SRC_COLOR; case 0x301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA; case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA; case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return D3D11_BLEND_DEST_COLOR; case 0x307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: throw std::runtime_error("Unsupported blend factor: " + hex32(value));
    }
}
D3D11_BLEND alpha_blend_factor(D3D11_BLEND factor) {
    switch (factor) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    // The source-alpha-saturate factor has alpha component one.
    case D3D11_BLEND_SRC_ALPHA_SAT: return D3D11_BLEND_ONE;
    default: return factor;
    }
}
D3D11_BLEND_OP blend_op(std::uint32_t value) {
    switch (value) {
    case 0x8006: return D3D11_BLEND_OP_ADD; case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX; case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: throw std::runtime_error("Unsupported blend equation");
    }
}
D3D11_TEXTURE_ADDRESS_MODE address_mode(unsigned value) {
    switch (value) {
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP; case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP; case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: throw std::runtime_error("Unsupported texture address mode");
    }
}
D3D11_STENCIL_OP stencil_operation(std::uint32_t value) {
    switch(value) {
    case 0x1E00:return D3D11_STENCIL_OP_KEEP;
    case 0:return D3D11_STENCIL_OP_ZERO;
    case 0x1E01:return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02:return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03:return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A:return D3D11_STENCIL_OP_INVERT;
    case 0x8507:return D3D11_STENCIL_OP_INCR;
    case 0x8508:return D3D11_STENCIL_OP_DECR;
    default:throw std::runtime_error("Unsupported stencil operation");
    }
}
using PipelineKey = std::array<std::uint32_t, 44>;
PipelineKey pipeline_key(const RenderState& state) {
    PipelineKey key{};
    std::size_t i = 0;
    for (auto value : {std::uint32_t(state.blend), std::uint32_t(state.depth_test), std::uint32_t(state.depth_write),
         std::uint32_t(state.cull), std::uint32_t(state.front_ccw),
         state.depth_function, state.blend_source, state.blend_destination, state.blend_equation,
         state.cull_face, state.color_mask}) key[i++] = value;
    for (unsigned n = 0; n < 4; ++n) for (auto value : {state.texture_address[n],state.texture_border[n],std::uint32_t(state.linear_filter[n]),state.texture_filter[n],state.texture_control[n]}) key[i++] = value;
    for(auto value : {std::uint32_t(state.stencil_test),state.stencil_function,state.stencil_read_mask,state.stencil_write_mask,
        state.stencil_fail,state.stencil_depth_fail,state.stencil_pass,std::uint32_t(state.polygon_offset),
        std::bit_cast<std::uint32_t>(state.polygon_offset_scale),std::bit_cast<std::uint32_t>(state.polygon_offset_bias),std::uint32_t(state.scissor_test)}) key[i++]=value;
    return key;
}
// The NV097 values each D3D11 state object consumes. Every kind is cached on
// its own: the game combines a few dozen of each into thousands of distinct
// draw states over a long session, so their cross product is unbounded.
using BlendKey=std::array<std::uint32_t,5>;
using DepthKey=std::array<std::uint32_t,10>;
using RasterKey=std::array<std::uint32_t,7>;
using SamplerKey=std::array<std::uint32_t,5>;
BlendKey blend_key(const RenderState& s) {
    return {std::uint32_t(s.blend),s.blend_source,s.blend_destination,s.blend_equation,s.color_mask};
}
DepthKey depth_key(const RenderState& s) {
    return {std::uint32_t(s.depth_test),std::uint32_t(s.depth_write),s.depth_function,std::uint32_t(s.stencil_test),s.stencil_function,
        s.stencil_read_mask,s.stencil_write_mask,s.stencil_fail,s.stencil_depth_fail,s.stencil_pass};
}
RasterKey raster_key(const RenderState& s) {
    return {std::uint32_t(s.cull),s.cull_face,std::uint32_t(s.front_ccw),std::uint32_t(s.scissor_test),std::uint32_t(s.polygon_offset),
        std::bit_cast<std::uint32_t>(s.polygon_offset_scale),std::bit_cast<std::uint32_t>(s.polygon_offset_bias)};
}
SamplerKey sampler_key(const RenderState& s,unsigned stage) {
    return {s.texture_address[stage],s.texture_border[stage],std::uint32_t(s.linear_filter[stage]),s.texture_filter[stage],s.texture_control[stage]};
}
// Unsampled stages keep a filter derived from their legacy linear flag.
std::uint32_t sampler_filter(const SamplerKey& key) {
    const auto [address,border,linear,filter,control]=key;
    return filter?filter:(linear?0x02060000U:0x01030000U);
}
bool gaussian_filter(std::uint32_t filter) {return ((filter>>16)&63)==7 && ((filter>>24)&15)==4 && ((filter>>13)&7)==2;}
// One D3D11 state kind with 1-based ids. D3D11 allows 4096 distinct objects
// of each kind per device, so this limit is the device's own.
template<class Key,class State> struct StateCache {
    static constexpr std::size_t limit=4096;
    std::map<Key,std::uint16_t> ids;
    std::vector<ComPtr<State>> states;
    template<class Create> std::uint16_t intern(const Key& key,const char* kind,Create&& create) {
        if(const auto found=ids.find(key);found!=ids.end()) return found->second;
        if(states.size()>=limit) throw std::runtime_error(std::format("D3D11 {} state limit reached",kind));
        auto state=create(key);
        states.push_back(std::move(state));
        const auto id=static_cast<std::uint16_t>(states.size());
        ids.emplace(key,id);
        return id;
    }
    // Recreates every object for changed settings; existing ids stay valid.
    template<class Create> void rebuild(Create&& create) {
        std::vector<ComPtr<State>> replaced(states.size());
        for(const auto& [key,id]:ids) replaced[id-1]=create(key);
        states=std::move(replaced);
    }
    State* get(std::uint16_t id) const {return states[id-1].Get();}
};
struct alignas(16) Constants {
    std::array<Float4, 8> factor0, factor1;
    Float4 final0, final1, fog, alpha;
    std::array<std::array<std::uint32_t,4>,8> combiners;
    std::array<std::array<std::uint32_t,4>,4> stages;
    std::array<std::uint32_t,4> controls;
    std::array<Float4,4> bump_matrices;
    std::array<std::uint32_t,4> gaussian_scale; // Zero disables; otherwise physical texels per guest texel.
    std::array<Float4,4> sample_bounds;          // Frontend quads: half a texel inside their texture rectangle.
    std::array<std::uint32_t,4> bounded_samples; // Nonzero clamps the stage's 2D coordinates to it.
};
struct alignas(16) ProgramSettings {
    Float4 viewport,fog;
    std::array<Float4,4> texture_scales;
    std::array<std::uint32_t,16> input_formats{};
    std::array<std::uint32_t,4> fixed_flags{};
    std::array<std::array<std::uint32_t,4>,4> fixed_texgen{};
    std::array<std::uint32_t,4> fixed_matrices{};
};
struct ProgramUniforms {
    const VertexConstants& constants;
    ProgramSettings settings;
};
constexpr auto program_uniform_bytes=sizeof(VertexConstants)+sizeof(ProgramSettings);
static_assert(sizeof(VertexConstants)==192*16 && sizeof(ProgramSettings)==16*16);
struct AttributeFormat {DXGI_FORMAT native;unsigned bytes,fetched;};
AttributeFormat attribute_format(std::uint32_t format) {
    const unsigned type=format&15,size=(format>>4)&15;
    if(format==0x16) return {DXGI_FORMAT_R16G16_UNORM,4,4};
    if(format==0x72) return {DXGI_FORMAT_R32G32B32_FLOAT,12,12};
    if(format==0x40) return {DXGI_FORMAT_R8G8B8A8_UNORM,4,4};
    if(size<1 || size>4 || format&~255U) throw std::runtime_error("Invalid NV2A vertex format");
    constexpr DXGI_FORMAT floats[]={DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32G32B32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT};
    constexpr DXGI_FORMAT shorts[]={DXGI_FORMAT_R16_SNORM,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R16G16B16A16_SNORM,DXGI_FORMAT_R16G16B16A16_SNORM};
    constexpr DXGI_FORMAT raw_shorts[]={DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_R16G16_UNORM,DXGI_FORMAT_R16G16B16A16_UNORM,DXGI_FORMAT_R16G16B16A16_UNORM};
    constexpr DXGI_FORMAT bytes[]={DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R8G8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM};
    switch(type) {
    case 1:return {shorts[size-1],size*2,(size==3?4:size)*2};
    case 2:return {floats[size-1],size*4,size*4};
    case 4:return {bytes[size-1],size,size==3?4:size};
    case 5:return {raw_shorts[size-1],size*2,(size==3?4:size)*2};
    default:throw std::runtime_error("Unsupported NV2A vertex format: "+hex32(format));
    }
}
}

void save_png(const Image& image, const std::filesystem::path& path, bool replace) {
    extent(image.width, image.height);
    if (image.rgba.size() != std::size_t(image.width) * image.height * 4) throw std::runtime_error("Invalid RGBA image size");
#ifdef _WIN32
    ComScope scope;
    auto factory = image_factory();
    ComPtr<IStream> stream;
    checked(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "PNG stream");
    ComPtr<IWICBitmapEncoder> encoder;
    checked(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    checked(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG initialization");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    checked(encoder->CreateNewFrame(&frame, &properties), "PNG frame");
    checked(frame->Initialize(properties.Get()), "PNG frame initialization");
    checked(frame->SetSize(image.width, image.height), "PNG dimensions");
    auto format = GUID_WICPixelFormat32bppBGRA;
    checked(frame->SetPixelFormat(&format), "PNG format");
    if (format != GUID_WICPixelFormat32bppBGRA && format != GUID_WICPixelFormat32bppRGBA)
        throw std::runtime_error("PNG encoder cannot preserve RGBA channels");
    ComPtr<IWICBitmap> bitmap;
    checked(factory->CreateBitmapFromMemory(image.width,image.height,GUID_WICPixelFormat32bppRGBA,
        image.width*4,static_cast<UINT>(image.rgba.size()),
        reinterpret_cast<BYTE*>(const_cast<std::byte*>(image.rgba.data())),&bitmap), "PNG source bitmap");
    ComPtr<IWICFormatConverter> converter;
    checked(factory->CreateFormatConverter(&converter), "PNG format converter");
    checked(converter->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,
                                  WICBitmapPaletteTypeCustom), "PNG channel conversion");
    checked(frame->WriteSource(converter.Get(),nullptr), "PNG pixels");
    checked(frame->Commit(), "PNG frame commit");
    checked(encoder->Commit(), "PNG commit");
    STATSTG info{};
    checked(stream->Stat(&info, STATFLAG_NONAME), "PNG stream size");
    if (info.cbSize.QuadPart > 128 * 1024 * 1024) throw std::runtime_error("Encoded PNG exceeds limit");
    HGLOBAL handle;
    checked(GetHGlobalFromStream(stream.Get(), &handle), "PNG stream memory");
    const auto bytes = static_cast<const char*>(GlobalLock(handle));
    if (!bytes) throw std::runtime_error("Cannot lock PNG stream");
    try { write_text(path, {bytes, static_cast<std::size_t>(info.cbSize.QuadPart)}, replace); }
    catch (...) { GlobalUnlock(handle); throw; }
    GlobalUnlock(handle);
#else
    png_image png{};png.version=PNG_IMAGE_VERSION;png.width=image.width;png.height=image.height;png.format=PNG_FORMAT_RGBA;
    png_alloc_size_t size=0;
    if(!png_image_write_to_memory(&png,nullptr,&size,0,image.rgba.data(),0,nullptr) || size>128*1024*1024)
        throw std::runtime_error("Cannot size PNG output");
    std::string bytes(size,0);
    if(!png_image_write_to_memory(&png,bytes.data(),&size,0,image.rgba.data(),0,nullptr)) throw std::runtime_error(png.message);
    bytes.resize(size);write_text(path,bytes,replace);
#endif
}
Image load_image(const std::filesystem::path& path) {
#ifdef _WIN32
    ComScope scope;
    auto factory = image_factory();
    ComPtr<IWICBitmapDecoder> decoder;
    checked(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "Image decoder");
    ComPtr<IWICBitmapFrameDecode> frame;
    checked(decoder->GetFrame(0, &frame), "Image frame");
    Image image{};
    checked(frame->GetSize(&image.width, &image.height), "Image size");
    extent(image.width, image.height);
    image.rgba.resize(std::size_t(image.width) * image.height * 4);
    ComPtr<IWICFormatConverter> converter;
    checked(factory->CreateFormatConverter(&converter), "Image converter");
    checked(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                  nullptr, 0, WICBitmapPaletteTypeCustom), "RGBA conversion");
    checked(converter->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(image.rgba.size()),
                                  reinterpret_cast<BYTE*>(image.rgba.data())), "Image pixels");
    return image;
#else
    File input(path);const auto magic=input.read(0,std::min<std::uint64_t>(8,input.size()));
    if(magic.size()==8 && !png_sig_cmp(reinterpret_cast<const unsigned char*>(magic.data()),0,8)) {
        png_image png{};png.version=PNG_IMAGE_VERSION;
        if(!png_image_begin_read_from_file(&png,path.c_str())) throw std::runtime_error(png.message);
        try {
            extent(png.width,png.height);png.format=PNG_FORMAT_RGBA;
            Image result{png.width,png.height,std::vector<std::byte>(PNG_IMAGE_SIZE(png))};
            if(!png_image_finish_read(&png,nullptr,result.rgba.data(),0,nullptr)) throw std::runtime_error(png.message);
            png_image_free(&png);return result;
        } catch(...) {png_image_free(&png);throw;}
    }
    // libjpeg's error callback must not unwind through C. Keep C++ allocations
    // outside the setjmp scope and copy the bounded decoded pixels afterwards.
    struct JpegError {jpeg_error_mgr base;jmp_buf jump;char message[JMSG_LENGTH_MAX];};
    auto error=std::make_unique<JpegError>();auto decoder=std::make_unique<jpeg_decompress_struct>();
    decoder->err=jpeg_std_error(&error->base);
    error->base.error_exit=[](j_common_ptr value) {auto* e=reinterpret_cast<JpegError*>(value->err);value->err->format_message(value,e->message);longjmp(e->jump,1);};
    FILE* file=std::fopen(path.c_str(),"rb");if(!file) throw std::runtime_error("Cannot open image");
    unsigned char* volatile pixels=nullptr;
    if(setjmp(error->jump)) {std::free(pixels);jpeg_destroy_decompress(decoder.get());std::fclose(file);throw std::runtime_error(error->message);}
    jpeg_create_decompress(decoder.get());jpeg_stdio_src(decoder.get(),file);jpeg_read_header(decoder.get(),TRUE);
    if(!decoder->image_width || !decoder->image_height || decoder->image_width>4096 || decoder->image_height>4096) {
        jpeg_destroy_decompress(decoder.get());std::fclose(file);throw std::runtime_error("Image dimensions must be 1..4096");
    }
    decoder->out_color_space=JCS_EXT_RGBA;jpeg_start_decompress(decoder.get());
    const auto width=decoder->output_width,height=decoder->output_height;
    pixels=static_cast<unsigned char*>(std::malloc(std::size_t(width)*height*4));
    if(!pixels) {jpeg_destroy_decompress(decoder.get());std::fclose(file);throw std::bad_alloc();}
    while(decoder->output_scanline<height) {auto* row=pixels+std::size_t(decoder->output_scanline)*width*4;jpeg_read_scanlines(decoder.get(),&row,1);}
    jpeg_finish_decompress(decoder.get());jpeg_destroy_decompress(decoder.get());std::fclose(file);
    Image result{width,height,{}};
    try {result.rgba.assign(reinterpret_cast<std::byte*>(pixels),reinterpret_cast<std::byte*>(pixels)+std::size_t(width)*height*4);}
    catch(...) {std::free(pixels);throw;}
    std::free(pixels);return result;
#endif
}
ImageComparison compare_images(const Image& a,const Image& b,unsigned tolerance) {
    if (tolerance > 255) throw std::runtime_error("Image tolerance must be 0..255");
    if (a.width != b.width || a.height != b.height) throw std::runtime_error("Image dimensions differ; align reference viewpoint and size first");
    ImageComparison result{{a.width,a.height,std::vector<std::byte>(a.rgba.size())}};
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        unsigned pixel_error=0;
        for (unsigned lane = 0; lane < 3; ++lane) {
            const auto delta = static_cast<unsigned>(std::abs(int(a.rgba[i + lane]) - int(b.rgba[i + lane])));
            result.total_error+=delta;pixel_error=std::max(pixel_error,delta);
            result.difference.rgba[i+lane]=std::byte(delta);
        }
        result.difference.rgba[i+3]=std::byte{255};
        result.changed+=pixel_error!=0;result.over_tolerance+=pixel_error>tolerance;
        result.maximum=std::max(result.maximum,pixel_error);
    }
    return result;
}
std::string compare_images(const std::filesystem::path& actual,const std::filesystem::path& reference,
                           const std::filesystem::path& difference,unsigned tolerance) {
    const auto comparison=compare_images(load_image(actual),load_image(reference),tolerance);
    const auto& diff=comparison.difference;save_png(diff,difference);
    return std::format("{{\"format\":\"smbd-image-compare-v1\",\"passed\":{},\"width\":{},\"height\":{},\"tolerance\":{},\"pixels_over_tolerance\":{},\"maximum_error\":{},\"mean_absolute_error\":{},\"difference\":{}}}",
                       comparison.over_tolerance==0,diff.width,diff.height,tolerance,comparison.over_tolerance,comparison.maximum,
                       double(comparison.total_error)/(double(diff.width)*diff.height*3),json(path_text(difference)));
}

// The union (or complement) is constant between consecutive window Y edges.
// Emit disjoint spans so depth, stencil and blending execute once per pixel.
template<class Draw> void draw_clip_windows(const RenderState& state,Draw draw) {
    std::array<int,18> edges{state.scissor[1],state.scissor[3]};
    unsigned edge_count=2;
    for(const auto& rect:std::span(state.clip_windows).first(state.window_count)) {
        edges[edge_count++]=rect[1];edges[edge_count++]=rect[3];
    }
    std::sort(edges.begin(),edges.begin()+edge_count);
    for(unsigned edge=1;edge<edge_count;++edge) {
        const auto top=edges[edge-1],bottom=edges[edge];
        if(top==bottom) continue;
        std::array<std::pair<int,int>,8> spans;unsigned count=0;
        for(const auto& rect:std::span(state.clip_windows).first(state.window_count))
            if(rect[1]<=top && top<rect[3]) spans[count++]={rect[0],rect[2]};
        std::sort(spans.begin(),spans.begin()+count);
        auto cursor=state.scissor[0];
        for(const auto& [left,right]:std::span(spans).first(count)) {
            if(state.exclude_windows) {
                if(cursor<left) draw(ScissorRect{cursor,top,left,bottom});
            } else if(cursor<right) draw(ScissorRect{std::max(cursor,left),top,right,bottom});
            cursor=std::max(cursor,right);
        }
        if(state.exclude_windows && cursor<state.scissor[2]) draw(ScissorRect{cursor,top,state.scissor[2],bottom});
    }
}

struct Renderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<ComPtr<ID3D11PixelShader>,4> simple_combiners;
    ComPtr<ID3D11Texture2D> target, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    std::array<ComPtr<ID3D11PixelShader>,9> combiners;
    std::array<ComPtr<ID3D11VertexShader>,7> fixed_vertices;
    std::array<Bytes,7> fixed_signatures;
    std::array<ComPtr<ID3D11InputLayout>,7> fixed_layouts;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices, current_vertices, index_buffer, factor_buffer, program_constants;
    ProgramVertex last_current_vertex{};
    bool current_vertex_uploaded=false,current_vertex_bound=false;
    Constants last_factors{};
    VertexConstants last_program_constants{};
    ProgramSettings last_program_settings{};
    bool factors_uploaded=false,program_uniforms_uploaded=false;
    UINT vertex_cursor=0,index_cursor=0;
    ComPtr<ID3D11Buffer> launch_constants, launch_inputs, launch_readback;
    ComPtr<ID3D11UnorderedAccessView> launch_output;
    ComPtr<ID3D11ShaderResourceView> launch_input;
    struct Timing {ComPtr<ID3D11Query> timer,start,finish;bool pending=false;std::uint64_t frame=0,source=0,generation=0;};
    std::array<Timing,64> timings;
    unsigned timing_index=64;
    std::uint64_t frame_id=0,latest_timing=0;
    Performance* profile=nullptr;
    std::uint64_t source_frame=1,profile_generation=0;
    struct ProfileFrame {std::uint64_t id=0,ns=0;unsigned expected=0,ready=0,dropped=0;bool closed=false;};
    std::array<ProfileFrame,64> profile_frames{};
    ProfileFrame& profile_frame(std::uint64_t id) {
        auto& frame=profile_frames[id%profile_frames.size()];
        if(frame.id!=id) frame={id};
        return frame;
    }
    void report_gpu(ProfileFrame& frame) {
        if(profile && frame.closed) profile->gpu_frame(frame.id,{frame.ns,0,0},frame.expected,
            frame.dropped+frame.expected-frame.ready,frame.expected==frame.ready && !frame.dropped);
    }
    ComPtr<ID3D11InfoQueue> info;
    struct FrameTarget {
        std::uint32_t width,height;
        SurfaceColor color_format=SurfaceColor::rgba8;
        SurfaceDepth depth_format=SurfaceDepth::z24s8;
        ComPtr<ID3D11Texture2D> color,depth,resolved;
        ComPtr<ID3D11RenderTargetView> color_view;
        ComPtr<ID3D11DepthStencilView> depth_view;
        ComPtr<ID3D11ShaderResourceView> texture_view,multisample_view,depth_texture,stencil_texture;
        std::shared_ptr<bool> dirty=std::make_shared<bool>(false);
    };
    std::map<std::uint32_t,FrameTarget> targets;
    std::uint32_t current_target=0;
    std::size_t target_bytes=0;
    std::size_t target_limit=256*1024*1024;
    unsigned samples=1,msaa_supported=0,antialiasing_quality=0,anisotropy=0;
    bool unlimited_render_distance=false;
    std::unique_ptr<FrameAntialiasing> antialiasing;
    std::unique_ptr<MultisampleCopy> multisample_copy;
    bool upscaled_movies=false;
    std::unique_ptr<MovieUpscaler> movie_upscaler;
    // Live enlarged frames by texture slot, plus one released image that the
    // game's double-buffered movie upload reuses for its next decoded frame.
    std::map<std::uint32_t,MovieUpscaler::Image> movie_images;
    std::optional<MovieUpscaler::Image> movie_spare;
    std::size_t movie_bytes() const {return (movie_upscaler?movie_upscaler->bytes():0)+(movie_spare?movie_spare->bytes():0);}
    // HUD/menu textures marked by the original immediate frontend. Enlarged
    // images replace a slot's view until released, evicted or disabled.
    bool upscaled_hud=false;
    std::unique_ptr<InterfaceUpscaler> interface_upscaler;
    // Background GPU time per published frame uses the GPU's idle share of
    // the measured presentation interval (interval minus the scene's own GPU
    // time), never less than 12% of the interval (0.25..2 ms).
    std::chrono::steady_clock::time_point last_publish{};
    double publish_interval_ms=1000.0/60,scene_gpu_ms=0;
    double interface_budget() const {
        const double floor=std::clamp(0.12*publish_interval_ms,0.25,2.0);
        return std::max(floor,std::min(0.75*(publish_interval_ms-scene_gpu_ms),0.75*publish_interval_ms));
    }
    struct InterfaceTexture {ComPtr<ID3D11ShaderResourceView> original;InterfaceUpscaler::Result result;unsigned width=0,height=0;std::uint64_t content=0;};
    std::map<std::uint32_t,InterfaceTexture> interface_textures;
    // Released enlargements by content hash: menus reload the same artwork
    // into new allocations, which then reuse the image without new work.
    struct CachedInterface {InterfaceUpscaler::Result result;std::uint64_t used=0;};
    std::map<std::uint64_t,CachedInterface> interface_cache;
    std::vector<std::uint64_t> texture_used{0}; // Last frame each slot was sampled.
    std::size_t interface_bytes=0;
    static constexpr std::size_t interface_limit=256*1024*1024;
    // Optional bounded disk cache of finished enlargements by content, so
    // artwork seen in an earlier session is enlarged on its first frame.
    // Each file holds level 0 (RGBA8); mips are regenerated on load.
    std::filesystem::path interface_disk;
    static constexpr std::uintmax_t interface_disk_limit=512ULL*1024*1024;
    std::uintmax_t interface_disk_bytes=0;
    struct InterfaceDiskHeader {
        char magic[8];std::uint32_t version,factor,width,height,network;std::uint8_t model[32];
    };
    static constexpr std::uint32_t interface_disk_version=1;
    struct InterfaceSave {ComPtr<ID3D11Texture2D> staging;std::filesystem::path path;InterfaceDiskHeader header;};
    std::vector<InterfaceSave> interface_saves;
    unsigned interface_factor() const {return std::min(resolution_scale,4U);}
    bool interface_active() const {return upscaled_hud && interface_factor()>1;}
    void restore_interface(std::uint32_t id,InterfaceTexture& entry) {
        if(!entry.result.view) return;
        unbind_textures();textures[id]=entry.original;interface_bytes-=entry.result.bytes;entry.result={};
    }
    std::filesystem::path interface_disk_path(std::uint64_t content,unsigned factor) const {
        return interface_disk/std::format("{:016X}-{}.hud",content,factor);
    }
    InterfaceDiskHeader interface_disk_header(unsigned width,unsigned height,bool network) const {
        InterfaceDiskHeader header{{'S','M','B','D','H','U','D','1'},interface_disk_version,interface_factor(),width,height,network,{}};
        for(unsigned i=0;i<32;++i) header.model[i]=static_cast<std::uint8_t>(std::stoul(std::string(upscaler_model_sha256.substr(i*2,2)),nullptr,16));
        return header;
    }
    void prune_interface_disk(std::uintmax_t target) {
        std::vector<std::pair<std::filesystem::file_time_type,std::filesystem::path>> files;
        std::error_code error;interface_disk_bytes=0;
        for(const auto& item:std::filesystem::directory_iterator(interface_disk,error)) {
            if(!item.is_regular_file(error) || item.path().extension()!=".hud") continue;
            interface_disk_bytes+=item.file_size(error);files.emplace_back(item.last_write_time(error),item.path());
        }
        std::ranges::sort(files);
        for(const auto& [time,path]:files) {
            if(interface_disk_bytes<=target) break;
            const auto size=std::filesystem::file_size(path,error);
            if(!error && std::filesystem::remove(path,error)) interface_disk_bytes-=std::min(size,interface_disk_bytes);
        }
    }
    // Releases room for an enlargement: cached images first, then the least
    // recently sampled live ones. False if it still does not fit.
    bool make_interface_room(std::size_t bytes) {
        while(interface_bytes+bytes>interface_limit && !interface_cache.empty()) {
            auto oldest=interface_cache.begin();
            for(auto item=interface_cache.begin();item!=interface_cache.end();++item) if(item->second.used<oldest->second.used) oldest=item;
            interface_bytes-=oldest->second.result.bytes;interface_cache.erase(oldest);
        }
        while(interface_bytes+bytes>interface_limit) {
            std::uint32_t oldest=0;std::uint64_t used=UINT64_MAX;
            for(const auto& [id,entry]:interface_textures) if(entry.result.view && texture_used[id]<used) {oldest=id;used=texture_used[id];}
            if(!oldest) break;
            restore_interface(oldest,interface_textures.at(oldest));++stats.hud_evictions;
        }
        return interface_bytes+bytes<=interface_limit;
    }
    bool load_interface(std::uint32_t id,InterfaceTexture& entry) {
        if(interface_disk.empty() || !entry.content) return false;
        const auto factor=interface_factor(),width=entry.width*factor,height=entry.height*factor;
        const auto path=interface_disk_path(entry.content,factor);
        std::ifstream file(path,std::ios::binary);
        if(!file) return false;
        InterfaceDiskHeader header{},expected=interface_disk_header(width,height,false);
        if(!file.read(reinterpret_cast<char*>(&header),sizeof(header))) return false;
        expected.network=header.network;
        if(std::memcmp(&header,&expected,sizeof(header))) return false;
        std::vector<std::byte> pixels(std::size_t(width)*height*4);
        if(!file.read(reinterpret_cast<char*>(pixels.data()),static_cast<std::streamsize>(pixels.size())) ||
           file.peek()!=std::char_traits<char>::eof()) return false;
        std::size_t bytes=0;
        for(unsigned w=width,h=height;;w=std::max(1U,w/2),h=std::max(1U,h/2)) {bytes+=std::size_t(w)*h*4;if(w==1 && h==1) break;}
        if(!make_interface_room(bytes)) return false;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=0;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
        checked(device->CreateTexture2D(&desc,nullptr,&texture),"Cached HUD texture");
        checked(device->CreateShaderResourceView(texture.Get(),nullptr,&view),"Cached HUD view");
        context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),width*4,0);
        context->GenerateMips(view.Get());
        unbind_textures();textures[id]=view;entry.result={view,width,height,bytes,header.network!=0};interface_bytes+=bytes;
        ++stats.hud_disk_loads;++(header.network?stats.hud_upscales:stats.hud_filtered);
        std::error_code error;std::filesystem::last_write_time(path,std::filesystem::file_time_type::clock::now(),error);
        return true;
    }
    void save_interface(const InterfaceTexture& entry) {
        if(interface_disk.empty() || !entry.content || interface_saves.size()>=32) return;
        const auto path=interface_disk_path(entry.content,interface_factor());
        std::error_code error;if(std::filesystem::exists(path,error)) return;
        ComPtr<ID3D11Resource> resource;entry.result.view->GetResource(&resource);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=entry.result.width;desc.Height=entry.result.height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        InterfaceSave save{nullptr,path,interface_disk_header(entry.result.width,entry.result.height,entry.result.network)};
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&save.staging))) return;
        context->CopySubresourceRegion(save.staging.Get(),0,0,0,0,resource.Get(),0,nullptr);
        interface_saves.push_back(std::move(save));
    }
    // Writes finished copies without waiting for the GPU.
    void write_interface_saves(bool wait=false) {
        for(auto save=interface_saves.begin();save!=interface_saves.end();) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(context->Map(save->staging.Get(),0,D3D11_MAP_READ,wait?0:D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)!=S_OK) {++save;continue;}
            const auto& header=save->header;auto temporary=save->path;temporary+=".tmp";
            bool written=false;
            {
                std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
                file.write(reinterpret_cast<const char*>(&header),sizeof(header));
                for(unsigned y=0;y<header.height;++y)
                    file.write(static_cast<const char*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,static_cast<std::streamsize>(header.width)*4);
                written=static_cast<bool>(file);
            }
            context->Unmap(save->staging.Get(),0);
            std::error_code error;
            if(written) std::filesystem::rename(temporary,save->path,error);
            if(!written || error) std::filesystem::remove(temporary,error);
            else {
                interface_disk_bytes+=sizeof(header)+std::uintmax_t(header.width)*header.height*4;++stats.hud_disk_saves;
                if(interface_disk_bytes>interface_disk_limit) prune_interface_disk(interface_disk_limit*3/4);
            }
            save=interface_saves.erase(save);
        }
    }
    void request_interface(std::uint32_t id,InterfaceTexture& entry) {
        if(!interface_active()) return;
        if(const auto cached=interface_cache.find(entry.content);entry.content && cached!=interface_cache.end()) {
            if(cached->second.result.width==entry.width*interface_factor() && cached->second.result.height==entry.height*interface_factor()) {
                unbind_textures();textures[id]=cached->second.result.view;entry.result=std::move(cached->second.result);
                interface_cache.erase(cached);++stats.hud_reused;return;
            }
            interface_bytes-=cached->second.result.bytes;interface_cache.erase(cached);
        }
        if(load_interface(id,entry)) return;
        if(!interface_upscaler) interface_upscaler=std::make_unique<InterfaceUpscaler>(device.Get(),context.Get(),stats.shader_compilations);
        interface_upscaler->request(id,entry.original,entry.width,entry.height,interface_factor());
    }
    void forget_interface_cache() {
        for(const auto& [content,cached]:interface_cache) interface_bytes-=cached.result.bytes;
        interface_cache.clear();
    }
    // Restores every original image, then requests enlargement again when
    // enabled; a changed internal resolution changes the enlargement factor.
    void reset_interface() {
        for(auto& [id,entry]:interface_textures) restore_interface(id,entry);
        forget_interface_cache();
        if(interface_upscaler) interface_upscaler->clear();
        if(!interface_active()) {interface_upscaler.reset();return;}
        for(auto& [id,entry]:interface_textures) request_interface(id,entry);
    }
    void finish_interface(const std::vector<InterfaceUpscaler::Finished>& finished) {
        for(const auto& done:finished) {
            const auto found=interface_textures.find(done.id);
            if(found==interface_textures.end() || !make_interface_room(done.result.bytes)) continue;
            unbind_textures();textures[done.id]=done.result.view;found->second.result=done.result;interface_bytes+=done.result.bytes;
            ++(done.result.network?stats.hud_upscales:stats.hud_filtered);
            save_interface(found->second);
        }
    }
    HWND window=nullptr;
    ComPtr<IDXGISwapChain2> swapchain;
    ComPtr<ID3D11Texture2D> published;
    ComPtr<ID3D11ShaderResourceView> published_view;
#ifdef _WIN32
    HANDLE frame_ready=nullptr;
#endif
    bool frame_acquired=false,allow_tearing=false;
    UINT swapchain_flags=0;
    std::size_t published_bytes=0;
    bool published_ready=false;
    ComPtr<ID3D11RenderTargetView> window_view;
    ComPtr<ID3D11VertexShader> display_vs;
    ComPtr<ID3D11PixelShader> display_ps;
    ComPtr<ID3D11SamplerState> display_sampler;
    ComPtr<ID3D11SamplerState> image_sampler;
    ComPtr<ID3D11RasterizerState> display_raster;
    ComPtr<ID3D11DepthStencilState> display_depth;
    ComPtr<ID3D11VertexShader> clear_vs;
    ComPtr<ID3D11PixelShader> clear_ps;
    ComPtr<ID3D11Buffer> clear_constants;
    ComPtr<ID3D11RasterizerState> clear_raster;
    std::array<ComPtr<ID3D11BlendState>,16> clear_blends;
    std::array<ComPtr<ID3D11DepthStencilState>,4> clear_depths;
    std::uint32_t window_width=0,window_height=0;
    bool displaying=false;
    struct DrawBindings {
        ID3D11InputLayout* layout;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        ID3D11VertexShader* vertex;
        ID3D11PixelShader* pixel;
        ID3D11BlendState* blend;
        ID3D11DepthStencilState* depth;
        ID3D11RasterizerState* raster;
        UINT stencil_reference;
        std::array<ID3D11ShaderResourceView*,12> views;
        std::array<ID3D11SamplerState*,4> samplers;
        std::array<LONG,4> scissor;
    };
    std::optional<DrawBindings> bound_draw;
    StateCache<BlendKey,ID3D11BlendState> blend_states;
    StateCache<DepthKey,ID3D11DepthStencilState> depth_states;
    StateCache<RasterKey,ID3D11RasterizerState> raster_states;
    StateCache<SamplerKey,ID3D11SamplerState> sampler_states;
    // Remembers which state objects complete draw states select. It only
    // shortcuts the caches above, so a full memo is forgotten, never fatal.
    static constexpr std::size_t pipeline_limit=4096;
    std::map<PipelineKey,PipelineStates> pipelines;
    decltype(pipelines)::iterator last_pipeline=pipelines.end();
    ComPtr<ID3D11BlendState> create_blend(const BlendKey&) const;
    ComPtr<ID3D11DepthStencilState> create_depth(const DepthKey&) const;
    ComPtr<ID3D11RasterizerState> create_raster(const RasterKey&) const;
    ComPtr<ID3D11SamplerState> create_sampler(const SamplerKey&);
    PipelineStates prepared(const RenderState&) const;
    struct Program {
        VertexProgram key;
        bool launch;
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11ComputeShader> compute;
        ComPtr<ID3D11InputLayout> layout;
        Bytes signature;
        bool fixed=false;
        FixedTransform fixed_key{};
    };
    std::vector<Program> programs;
    std::array<std::vector<std::uint32_t>,2> program_indices;
    struct StreamLayout {
        std::uint32_t program;VertexLayout attributes,native_attributes;
        std::uint32_t repacked=0;
        std::array<unsigned,16> required{},fetched{},alignments{};
        unsigned stream_count=0;
        bool uses_current=false;
        ComPtr<ID3D11InputLayout> native;
    };
    std::vector<StreamLayout> stream_layouts;
    struct StreamUpload {
        std::array<UINT,16> offsets,strides,sizes;
        UINT size,offset;
        bool uses_current;
    };
    struct PreparedStreams {
        const StreamLayout& layout;
        const StreamDraw& draw;
        const StreamUpload& upload;
        const VertexConstants*& bound_constants;
    };
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures{1};
    std::vector<unsigned> texture_dimensions{0};
    std::vector<std::size_t> texture_sizes{0};
    std::vector<std::uint32_t> free_textures;
    std::vector<std::uint32_t> converted;
    std::string adapter;
    std::uint32_t width, height,resolution_scale=1;
    float display_aspect=4.0f/3;
    static unsigned color_size(SurfaceColor color) {return color==SurfaceColor::rgb565?2U:4U;}
    static unsigned depth_size(SurfaceDepth depth) {
        switch(depth) {
        case SurfaceDepth::z24s8:return 4;
        case SurfaceDepth::z16:return 2;
        case SurfaceDepth::none:return 0;
        }
        throw std::runtime_error("Invalid depth format");
    }
    std::size_t target_size(unsigned w,unsigned h,SurfaceColor color=SurfaceColor::rgba8,
                            SurfaceDepth depth=SurfaceDepth::z24s8,bool shared=false) const {
        return std::size_t(w)*h*((shared?0:color_size(color)*(samples+(samples>1?1:0)))+depth_size(depth)*samples)*resolution_scale*resolution_scale;
    }
    std::size_t presentation_bytes() const {return published_bytes+(antialiasing?antialiasing->bytes():0);}
    RenderStats stats;
    std::size_t texture_bytes = 0;
    bool recording = false, ended = false;
    std::uint32_t store_texture(ComPtr<ID3D11ShaderResourceView> view,unsigned dimension,std::size_t bytes,bool reuse) {
        std::uint32_t id;
        if(reuse && !free_textures.empty()) {id=free_textures.back();free_textures.pop_back();}
        else {
            if(textures.size()==4096) throw std::runtime_error("Texture resource limit reached");
            id=static_cast<std::uint32_t>(textures.size());textures.emplace_back();texture_dimensions.push_back(0);texture_sizes.push_back(0);texture_used.push_back(0);
        }
        textures[id]=std::move(view);texture_dimensions[id]=dimension;texture_sizes[id]=bytes;texture_bytes+=bytes;
        return id;
    }
    void release_slot(std::uint32_t id) {
        unbind_textures();texture_bytes-=texture_sizes[id];texture_sizes[id]=0;
        if(const auto found=interface_textures.find(id);found!=interface_textures.end()) {
            if(interface_upscaler) interface_upscaler->cancel(id);
            auto& entry=found->second;
            if(entry.result.view && entry.content) {
                auto& cached=interface_cache[entry.content];
                if(cached.result.view) interface_bytes-=cached.result.bytes;
                cached={std::move(entry.result),frame_id};
            } else if(entry.result.view) interface_bytes-=entry.result.bytes;
            interface_textures.erase(found);
        }
        textures[id].Reset();texture_dimensions[id]=0;texture_used[id]=0;free_textures.push_back(id);
        if(const auto found=movie_images.find(id);found!=movie_images.end()) {
            if(upscaled_movies) movie_spare=std::move(found->second);
            movie_images.erase(found);
        }
    }
    std::uint32_t upload_movie(const D3D11_SUBRESOURCE_DATA& pixels,const TextureData& texture,DXGI_FORMAT format,bool reuse) {
        const auto w=texture.width*resolution_scale,h=texture.height*resolution_scale;
        const auto bytes=std::size_t(w)*h*4;
        if(bytes>256*1024*1024-texture_bytes) throw std::runtime_error("Texture resources exceed 256 MiB");
        if(!movie_upscaler) movie_upscaler=std::make_unique<MovieUpscaler>(device.Get(),context.Get(),stats.shader_compilations);
        MovieUpscaler::Image image;
        if(movie_spare && movie_spare->width==w && movie_spare->height==h) image=std::move(*movie_spare);
        else image=movie_upscaler->create(w,h);
        movie_spare.reset();
        unbind_textures();context->OMSetRenderTargets(0,nullptr,nullptr);
        try {movie_upscaler->upscale(pixels,format,texture.width,texture.height,resolution_scale,image);}
        catch(...) {select_target(current_target);throw;}
        select_target(current_target);
        ++stats.movie_upscales;++stats.texture_uploads;stats.texture_upload_bytes+=texture.bytes.size();
        const auto id=store_texture(image.view,1,bytes,reuse);
        movie_images.insert_or_assign(id,std::move(image));
        return id;
    }
    static constexpr UINT max_vertices = 32768, max_indices = max_vertices * 6;
    static constexpr UINT vertex_capacity=max_vertices*sizeof(ProgramVertex);
    std::byte* append_buffer(ID3D11Buffer* buffer,UINT capacity,UINT& cursor,std::size_t size,UINT& offset) {
        if(!size || size>capacity) throw std::runtime_error("Upload exceeds dynamic buffer storage");
        offset=(cursor+15)&~UINT(15);
        if(size>capacity-offset) offset=0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // Earlier draws retain their bytes. Discard only when wrapping so the
        // driver does not rename a multi-megabyte allocation for every draw.
        checked(context->Map(buffer,0,offset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&mapped),"Dynamic buffer append");
        cursor=offset+static_cast<UINT>(size);
        ++stats.buffer_maps;stats.total_vertex_bytes+=size;
        return static_cast<std::byte*>(mapped.pData)+offset;
    }
    Bytes shader(ShaderKind kind,const VertexProgram* program=nullptr) {
        const auto result=shader_binary(kind,program);stats.shader_compilations+=result.compiled;return result.bytes;
    }
    Impl(std::uint32_t w, std::uint32_t h, bool warp, bool debug,unsigned scale) : width(w), height(h),resolution_scale(scale) {
#ifndef _WIN32
        if(warp) throw std::runtime_error("WARP is Windows-only; use a Vulkan software driver on Linux.");
        if(debug) throw std::runtime_error("The D3D11 debug layer is Windows-only.");
        SDL_setenv_unsafe("DXVK_WSI_DRIVER","SDL3",1);
#endif
        if(scale<1 || scale>4) throw std::runtime_error("Internal resolution must be 1x..4x");
        extent(w, h);
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL level;
        const UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | (debug ? D3D11_CREATE_DEVICE_DEBUG : 0);
        checked(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                  nullptr, flags, levels, 1, D3D11_SDK_VERSION, &device, &level, &context), "D3D11 device");
        ComPtr<IDXGIDevice> dxgi;
        checked(device.As(&dxgi), "DXGI device");
        ComPtr<IDXGIAdapter> gpu;
        checked(dxgi->GetAdapter(&gpu), "DXGI adapter");
        DXGI_ADAPTER_DESC description{};
        checked(gpu->GetDesc(&description), "Adapter description");
        adapter = utf8(description.Description);
        target_limit=std::clamp<std::size_t>(description.DedicatedVideoMemory/4,256ULL*1024*1024,2ULL*1024*1024*1024);
        for(unsigned count:{2U,4U,8U}) {
            bool supported=true;
            for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B5G6R5_UNORM,DXGI_FORMAT_D16_UNORM,DXGI_FORMAT_D24_UNORM_S8_UINT}) {
                UINT levels=0;const auto status=device->CheckMultisampleQualityLevels(format,count,&levels);
                supported&=SUCCEEDED(status) && levels!=0;
            }
            for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B5G6R5_UNORM,DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_R24_UNORM_X8_TYPELESS,DXGI_FORMAT_X24_TYPELESS_G8_UINT}) {
                UINT flags=0;supported&=SUCCEEDED(device->CheckFormatSupport(format,&flags)) && (flags&D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD)!=0;
                if(format==DXGI_FORMAT_R8G8B8A8_UNORM || format==DXGI_FORMAT_B5G6R5_UNORM) supported&=(flags&D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)!=0;
            }
            if(supported) msaa_supported|=count;
        }
        if (debug) checked(device.As(&info), "D3D11 debug queue");
        resize(w,h);
        const auto code = shader(ShaderKind::passthrough);
        checked(device->CreateVertexShader(code.data(),code.size(),nullptr,&vs), "Vertex shader");
        const D3D11_INPUT_ELEMENT_DESC inputs[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,0,96,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",4,DXGI_FORMAT_R32_FLOAT,0,112,D3D11_INPUT_PER_VERTEX_DATA,0}};
        static_assert(sizeof(Vertex) == 116);
        checked(device->CreateInputLayout(inputs, static_cast<UINT>(std::size(inputs)), code.data(),code.size(), &layout), "Vertex layout");
        for(unsigned i=0;i<combiners.size();++i) {
            const auto pixel=shader(ShaderKind(unsigned(ShaderKind::combiner0)+i));
            checked(device->CreatePixelShader(pixel.data(),pixel.size(),nullptr,&combiners[i]),"Combiner shader");
        }
        for(unsigned i=0;i<simple_combiners.size();++i) {
            const auto pixel=shader(ShaderKind(unsigned(ShaderKind::color)+i));
            checked(device->CreatePixelShader(pixel.data(),pixel.size(),nullptr,&simple_combiners[i]),"Simple combiner shader");
        }
        std::array<D3D11_INPUT_ELEMENT_DESC,16> program_inputs{};
        for(unsigned i=0;i<program_inputs.size();++i)
            program_inputs[i]={"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,i*16,D3D11_INPUT_PER_VERTEX_DATA,0};
        for(unsigned i=0;i<fixed_vertices.size();++i) {
            const auto fixed=shader(ShaderKind(unsigned(ShaderKind::fixed0)+i));fixed_signatures[i]=fixed;
            checked(device->CreateVertexShader(fixed.data(),fixed.size(),nullptr,&fixed_vertices[i]),"Fixed transform shader");
            checked(device->CreateInputLayout(program_inputs.data(),16,fixed.data(),fixed.size(),&fixed_layouts[i]),"Fixed transform layout");
        }
        const auto buffer = [&](UINT bytes, UINT bind, ComPtr<ID3D11Buffer>& output) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = bytes; desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = bind; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            checked(device->CreateBuffer(&desc, nullptr, &output), "Dynamic buffer");
        };
        buffer(vertex_capacity, D3D11_BIND_VERTEX_BUFFER, vertices);
        buffer(sizeof(ProgramVertex), D3D11_BIND_VERTEX_BUFFER, current_vertices);
        buffer(max_indices * sizeof(std::uint32_t), D3D11_BIND_INDEX_BUFFER, index_buffer);
        buffer(sizeof(Constants), D3D11_BIND_CONSTANT_BUFFER, factor_buffer);
        buffer(program_uniform_bytes,D3D11_BIND_CONSTANT_BUFFER,program_constants);
        buffer(32,D3D11_BIND_CONSTANT_BUFFER,clear_constants);
        converted.reserve(max_indices);
        for(auto& timing:timings) {
            D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
            checked(device->CreateQuery(&query,&timing.timer),"GPU timer");query.Query=D3D11_QUERY_TIMESTAMP;
            checked(device->CreateQuery(&query,&timing.start),"GPU start timestamp");
            checked(device->CreateQuery(&query,&timing.finish),"GPU end timestamp");
        }
        const auto clear_vertex=shader(ShaderKind::clear_vertex),clear_pixel=shader(ShaderKind::clear_pixel);
        checked(device->CreateVertexShader(clear_vertex.data(),clear_vertex.size(),nullptr,&clear_vs),"Clear vertex shader");
        checked(device->CreatePixelShader(clear_pixel.data(),clear_pixel.size(),nullptr,&clear_ps),"Clear pixel shader");
        for(unsigned mask=0;mask<clear_blends.size();++mask) {
            D3D11_BLEND_DESC desc{};auto& output=desc.RenderTarget[0];
            output.RenderTargetWriteMask=static_cast<UINT8>(mask);
            output.SrcBlend=output.SrcBlendAlpha=D3D11_BLEND_ONE;
            output.DestBlend=output.DestBlendAlpha=D3D11_BLEND_ZERO;
            output.BlendOp=output.BlendOpAlpha=D3D11_BLEND_OP_ADD;
            checked(device->CreateBlendState(&desc,&clear_blends[mask]),"Clear channel mask");
        }
        for(unsigned mask=0;mask<clear_depths.size();++mask) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable=(mask&1)!=0;desc.DepthWriteMask=mask&1?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.DepthFunc=D3D11_COMPARISON_ALWAYS;desc.StencilEnable=(mask&2)!=0;
            desc.StencilReadMask=desc.StencilWriteMask=255;
            desc.FrontFace=desc.BackFace={D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};
            checked(device->CreateDepthStencilState(&desc,&clear_depths[mask]),"Clear depth/stencil mask");
        }
        D3D11_RASTERIZER_DESC clear_desc{};clear_desc.FillMode=D3D11_FILL_SOLID;clear_desc.CullMode=D3D11_CULL_NONE;
        clear_desc.DepthClipEnable=clear_desc.ScissorEnable=TRUE;
        checked(device->CreateRasterizerState(&clear_desc,&clear_raster),"Clear rectangle state");
    }
    void collect_timings() {
        for(auto& timing:timings) if(timing.pending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};UINT64 start=0,finish=0;
            const auto status=context->GetData(timing.timer.Get(),&result,sizeof(result),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            checked(status,"GPU timing query");
            if(status!=S_OK) continue;
            const auto first=context->GetData(timing.start.Get(),&start,sizeof(start),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const auto last=context->GetData(timing.finish.Get(),&finish,sizeof(finish),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            checked(first,"GPU start query");checked(last,"GPU finish query");
            if(first!=S_OK || last!=S_OK) continue;
            timing.pending=false;
            if(profile && timing.generation==profile_generation && timing.source) {
                auto& frame=profile_frame(timing.source);++frame.ready;
                if(!result.Disjoint && result.Frequency && finish>=start)
                    frame.ns+=(finish-start)*1000000000ULL/result.Frequency;
                else ++frame.dropped;
                report_gpu(frame);
            }
            if(!result.Disjoint && result.Frequency && timing.frame>=latest_timing) {
                stats.gpu_ms=double(finish-start)*1000/double(result.Frequency);stats.gpu_timing_valid=true;latest_timing=timing.frame;
            }
        }
    }
#ifdef _WIN32
    ~Impl() {if(frame_ready) CloseHandle(frame_ready);}
#endif
    bool acquire_frame(unsigned timeout_ms) {
#ifdef _WIN32
        if(frame_acquired) return true;
        const auto status=WaitForSingleObject(frame_ready,timeout_ms);
        if(status==WAIT_TIMEOUT) return false;
        if(status!=WAIT_OBJECT_0) throw std::runtime_error("Window frame availability wait failed");
        frame_acquired=true;
#else
        (void)timeout_ms; // DXVK bounds the frame queue in Present.
#endif
        return true;
    }
    void draw(const RenderState&, Bytes, std::size_t, std::span<const std::uint32_t>,
              const Program* = nullptr, ProgramUniforms* = nullptr,const PreparedStreams* = nullptr);
    StreamUpload plan_streams(const StreamDraw&) const;
    void pack_streams(const StreamDraw&,const StreamUpload&,std::byte*) const;
    void draw_prepared_streams(const StreamDraw&,const StreamUpload&,const VertexConstants*&);
    FrameTarget make_target(std::uint32_t w,std::uint32_t h,SurfaceColor color_format=SurfaceColor::rgba8,
                           SurfaceDepth depth_format=SurfaceDepth::z24s8,const FrameTarget* shared=nullptr,unsigned sample_count=0,unsigned scale=0) {
        extent(w,h);
        FrameTarget output{w,h,color_format,depth_format};
        const auto native_color=color_format==SurfaceColor::rgb565?DXGI_FORMAT_B5G6R5_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D11_TEXTURE2D_DESC texture{};
        if(!scale) scale=resolution_scale;
        texture.Width=w*scale; texture.Height=h*scale; texture.MipLevels=texture.ArraySize=1;
        extent(texture.Width,texture.Height);
        texture.Format=native_color; texture.SampleDesc.Count=sample_count?sample_count:samples;
        texture.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if(shared) {
            if(shared->width!=w || shared->height!=h || shared->color_format!=color_format)
                throw std::runtime_error("Shared color image dimensions or format differ");
            output.color=shared->color;output.color_view=shared->color_view;output.texture_view=shared->texture_view;
            output.resolved=shared->resolved;output.multisample_view=shared->multisample_view;output.dirty=shared->dirty;
        } else {
            checked(device->CreateTexture2D(&texture,nullptr,&output.color), "Offscreen target");
            checked(device->CreateRenderTargetView(output.color.Get(),nullptr,&output.color_view), "Offscreen view");
            output.resolved=output.color;
            if(texture.SampleDesc.Count>1) {
                checked(device->CreateShaderResourceView(output.color.Get(),nullptr,&output.multisample_view),"Multisample colour view");
                const auto count=texture.SampleDesc.Count;texture.SampleDesc.Count=1;
                checked(device->CreateTexture2D(&texture,nullptr,&output.resolved),"Resolved target");texture.SampleDesc.Count=count;
            }
            checked(device->CreateShaderResourceView(output.resolved.Get(),nullptr,&output.texture_view),"Target texture view");
        }
        if(depth_format==SurfaceDepth::none) return output;
        texture.Format=depth_format==SurfaceDepth::z16?DXGI_FORMAT_R16_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;
        texture.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
        checked(device->CreateTexture2D(&texture,nullptr,&output.depth), "Depth target");
        D3D11_DEPTH_STENCIL_VIEW_DESC depth_view{};depth_view.Format=depth_format==SurfaceDepth::z16?DXGI_FORMAT_D16_UNORM:DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_view.ViewDimension=texture.SampleDesc.Count>1?D3D11_DSV_DIMENSION_TEXTURE2DMS:D3D11_DSV_DIMENSION_TEXTURE2D;
        checked(device->CreateDepthStencilView(output.depth.Get(),&depth_view,&output.depth_view), "Depth view");
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.ViewDimension=texture.SampleDesc.Count>1?D3D11_SRV_DIMENSION_TEXTURE2DMS:D3D11_SRV_DIMENSION_TEXTURE2D;
            if(texture.SampleDesc.Count==1) view.Texture2D.MipLevels=1;
            view.Format=depth_format==SurfaceDepth::z16?DXGI_FORMAT_R16_UNORM:DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            checked(device->CreateShaderResourceView(output.depth.Get(),&view,&output.depth_texture),"Multisample depth view");
            if(depth_format!=SurfaceDepth::z16) {view.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;
                checked(device->CreateShaderResourceView(output.depth.Get(),&view,&output.stencil_texture),"Multisample stencil view");}
        }
        return output;
    }
    void resolve(FrameTarget& image) {
        if(samples==1 || !*image.dirty) return;
        unbind_textures();context->OMSetRenderTargets(0,nullptr,nullptr);
        const auto format=image.color_format==SurfaceColor::rgb565?DXGI_FORMAT_B5G6R5_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        context->ResolveSubresource(image.resolved.Get(),0,image.color.Get(),0,format);*image.dirty=false;++stats.target_copies;
        auto output=rtv.Get();context->OMSetRenderTargets(1,&output,dsv.Get());
    }
    void unbind_textures() {
        bound_draw.reset();
        std::array<ID3D11ShaderResourceView*,12> empty{};
        context->PSSetShaderResources(0,static_cast<UINT>(empty.size()),empty.data());
    }
    void select_target(std::uint32_t id) {
        const auto found=targets.find(id);
        if(found==targets.end()) throw std::runtime_error("Render target was not created");
        unbind_textures();context->OMSetRenderTargets(0,nullptr,nullptr);
        const auto& output=found->second;
        width=output.width;height=output.height;current_target=id;
        target=output.color;depth=output.depth;rtv=output.color_view;dsv=output.depth_view;
        auto color=rtv.Get();context->OMSetRenderTargets(1,&color,dsv.Get());
        const D3D11_VIEWPORT viewport{0,0,float(width*resolution_scale),float(height*resolution_scale),0,1};
        context->RSSetViewports(1,&viewport);
    }
    void resize(std::uint32_t w, std::uint32_t h) {
        extent(w,h);
        if (recording || current_target) throw std::runtime_error("Resize the main target outside frame submission");
        if (target && width==w && height==h) return;
        const auto required=target_size(w,h);
        const auto prior=target?target_size(width,height):0;
        if(required>target_limit-(target_bytes-prior)) throw std::runtime_error("Render targets exceed the graphics memory budget; reduce resolution or MSAA.");
        auto output=make_target(w,h);
        targets.insert_or_assign(0,std::move(output));target_bytes=target_bytes-prior+required;
        select_target(0);ended=false;
        if(antialiasing) antialiasing=std::make_unique<FrameAntialiasing>(device.Get(),context.Get(),w*resolution_scale,h*resolution_scale,antialiasing_quality,stats.shader_compilations);
        if(window) prepare_presentation();
    }
    void prepare_presentation() {
        published_view.Reset();published.Reset();published_bytes=0;published_ready=false;
        D3D11_TEXTURE2D_DESC description{};targets.at(0).resolved->GetDesc(&description);
        description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const auto bytes=std::size_t(description.Width)*description.Height*4;
        if(bytes+(antialiasing?antialiasing->bytes():0)>target_limit-target_bytes) throw std::runtime_error("Presentation image exceeds the graphics memory budget");
        checked(device->CreateTexture2D(&description,nullptr,&published),"Completed presentation image");
        checked(device->CreateShaderResourceView(published.Get(),nullptr,&published_view),"Completed presentation view");
        published_bytes=bytes;
    }
};
Renderer::Renderer(std::uint32_t width, std::uint32_t height, bool warp, bool debug,unsigned scale)
    : impl_(std::make_unique<Impl>(width, height, warp, debug,scale)) {
    for(const auto& shader:static_vertex_shaders()) prepare_program(shader.program,shader.launch);
}
Renderer::~Renderer() = default;
void Renderer::resize(std::uint32_t width, std::uint32_t height) { impl_->resize(width,height); }
void Renderer::aspect_ratio(float aspect) {
    if(!std::isfinite(aspect) || aspect<1 || aspect>4) throw std::runtime_error("Invalid display aspect ratio");
    impl_->display_aspect=aspect;
}
bool Renderer::supports_msaa(unsigned samples) const {return samples>=2 && samples<=8 && std::has_single_bit(samples) && (impl_->msaa_supported&samples)!=0;}
void Renderer::enhancements(unsigned quality,unsigned anisotropy,bool unlimited_render_distance,bool upscaled_movies,bool upscaled_hud) {
    image_quality(impl_->resolution_scale,quality,anisotropy,unlimited_render_distance,upscaled_movies,upscaled_hud);
}
void Renderer::image_quality(unsigned scale,unsigned quality,unsigned anisotropy,bool unlimited_render_distance,bool upscaled_movies,bool upscaled_hud) {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("Change image quality between completed frames.");
    if(scale<1 || scale>4 || quality>6 || anisotropy>16 || (anisotropy && !std::has_single_bit(anisotropy)))
        throw std::runtime_error("Invalid internal resolution or image quality.");
    // Movie frames already uploaded keep their image until the next decoded frame.
    if(upscaled_movies!=r.upscaled_movies || scale!=r.resolution_scale) {
        r.movie_spare.reset();if(!upscaled_movies) r.movie_upscaler.reset();
    }
    r.upscaled_movies=upscaled_movies;
    // HUD enlargements follow the internal resolution, so either change
    // restores the originals and requests fresh images.
    const bool reset_hud=upscaled_hud!=r.upscaled_hud || (upscaled_hud && scale!=r.resolution_scale);
    r.upscaled_hud=upscaled_hud;
    if(scale==r.resolution_scale && quality==r.antialiasing_quality && anisotropy==r.anisotropy &&
       unlimited_render_distance==r.unlimited_render_distance) {if(reset_hud) r.reset_interface();return;}
    const unsigned samples=quality>=4?1U<<(quality-3):1U;
    if(samples>1 && !supports_msaa(samples)) throw std::runtime_error("This MSAA level is unavailable on your graphics device.");
    const bool rebuild=scale!=r.resolution_scale || samples!=r.samples;
    const bool replace_aa=rebuild || quality!=r.antialiasing_quality;
    const auto& main=r.targets.at(0);
    const auto pixels=std::size_t(main.width)*main.height*scale*scale;
    std::size_t bytes=rebuild?0:r.target_bytes;
    std::map<ID3D11Texture2D*,std::uint32_t> colors;
    if(rebuild) for(const auto& [id,image]:r.targets) {
        const bool shared=!colors.emplace(image.color.Get(),id).second;
        bytes+=std::size_t(image.width)*image.height*scale*scale*
            ((shared?0:r.color_size(image.color_format)*(samples+(samples>1?1:0)))+r.depth_size(image.depth_format)*samples);
    }
    const auto presentation=r.window?pixels*4:0;
    const auto aa_bytes=quality && quality<=3?pixels*4:0;
    if(bytes+presentation+aa_bytes>r.target_limit)
        throw std::runtime_error("Reduce internal resolution or MSAA to fit the graphics memory budget.");
    std::unique_ptr<FrameAntialiasing> aa;
    std::unique_ptr<MultisampleCopy> copy;
    if(replace_aa && aa_bytes) aa=std::make_unique<FrameAntialiasing>(r.device.Get(),r.context.Get(),main.width*scale,main.height*scale,quality,r.stats.shader_compilations);
    if(rebuild && !r.multisample_copy) copy=std::make_unique<MultisampleCopy>(r.device.Get(),r.context.Get(),r.stats.shader_compilations);
    std::map<std::uint32_t,Impl::FrameTarget> targets;
    ComPtr<ID3D11Texture2D> published;
    ComPtr<ID3D11ShaderResourceView> published_view;
    if(rebuild) {
        for(const auto& [id,image]:r.targets) {
            const auto owner=colors.at(image.color.Get());
            targets.emplace(id,r.make_target(image.width,image.height,image.color_format,image.depth_format,
                owner==id?nullptr:&targets.at(owner),samples,scale));
        }
        if(r.window) {
            D3D11_TEXTURE2D_DESC description{};targets.at(0).resolved->GetDesc(&description);
            description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            checked(r.device->CreateTexture2D(&description,nullptr,&published),"Resized presentation image");
            checked(r.device->CreateShaderResourceView(published.Get(),nullptr,&published_view),"Resized presentation view");
        }
        r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
        auto& transfer=copy?*copy:*r.multisample_copy;
        try {
            for(const auto& [id,image]:r.targets) {
                auto& output=targets.at(id);
                transfer.resample(r.samples>1?image.multisample_view.Get():image.texture_view.Get(),image.depth_texture.Get(),image.stencil_texture.Get(),
                    output.color_view.Get(),output.depth_view.Get(),image.width,image.height,r.resolution_scale,scale,r.samples,samples);
                *output.dirty=samples>1;
            }
        } catch(...) {r.select_target(r.current_target);throw;}
        r.targets=std::move(targets);r.target_bytes=bytes;r.resolution_scale=scale;r.samples=samples;
        for(const auto& [id,image]:r.targets) if(id) r.textures[id]=image.texture_view;
        if(copy) r.multisample_copy=std::move(copy);
        r.published=std::move(published);r.published_view=std::move(published_view);r.published_bytes=presentation;r.published_ready=false;
        r.select_target(r.current_target);
    }
    if(replace_aa) r.antialiasing=std::move(aa);
    r.antialiasing_quality=quality;r.anisotropy=anisotropy;
    r.unlimited_render_distance=unlimited_render_distance;
    // Rasterizer and sampler objects follow these settings. Rebuilding them
    // in place keeps prepared and queued draws valid.
    r.raster_states.rebuild([&](const RasterKey& key) {return r.create_raster(key);});
    r.sampler_states.rebuild([&](const SamplerKey& key) {return r.create_sampler(key);});
    if(reset_hud) r.reset_interface();
}
void Renderer::interface_texture(std::uint32_t id,std::uint64_t content) {
    auto& r=*impl_;
    // Ordinary 2D textures only: never render targets, cube/volume maps or
    // enlarged movie frames. Marking is idempotent and survives toggling.
    if(!id || id>=r.textures.size() || !r.textures[id] || r.targets.contains(id) || r.texture_dimensions[id]!=1 ||
       r.movie_images.contains(id) || r.interface_textures.contains(id)) return;
    ComPtr<ID3D11Resource> resource;r.textures[id]->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.ArraySize!=1 || (desc.MiscFlags&D3D11_RESOURCE_MISC_TEXTURECUBE)) return;
    auto& entry=r.interface_textures[id];
    entry.original=r.textures[id];entry.width=desc.Width;entry.height=desc.Height;entry.content=content;
    r.request_interface(id,entry);
}
void Renderer::finish_interface_upscales() {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("Finish HUD upscaling between frames");
    if(r.interface_upscaler) {r.unbind_textures();r.finish_interface(r.interface_upscaler->step(true));}
    if(!r.interface_saves.empty()) r.write_interface_saves(true);
}
void Renderer::hud_cache(const std::filesystem::path& directory) {
    auto& r=*impl_;
    r.interface_saves.clear();r.interface_disk.clear();r.interface_disk_bytes=0;
    if(directory.empty()) return;
    std::error_code error;std::filesystem::create_directories(directory,error);
    if(error || !std::filesystem::is_directory(directory,error)) return;
    r.interface_disk=directory;r.prune_interface_disk(Impl::interface_disk_limit);
}
std::uint32_t Renderer::create_target(std::uint32_t width,std::uint32_t height,SurfaceColor color,SurfaceDepth depth,std::uint32_t shared_color,bool reuse_slot) {
    auto& r=*impl_;extent(width,height);
    if(r.recording) throw std::runtime_error("Create render targets before frame submission");
    const auto shared=r.targets.find(shared_color);
    if(shared_color!=UINT32_MAX && (!shared_color || shared==r.targets.end()))
        throw std::runtime_error("Share the color image of an existing auxiliary target");
    const auto required=r.target_size(width,height,color,depth,shared!=r.targets.end());
    if(required>r.target_limit-r.target_bytes-r.presentation_bytes())
        throw std::runtime_error("Render target resource limit reached");
    auto output=r.make_target(width,height,color,depth,shared==r.targets.end()?nullptr:&shared->second);
    ++r.stats.target_creations;
    const auto id=r.store_texture(output.texture_view,1,0,reuse_slot);
    r.targets.emplace(id,std::move(output));r.target_bytes+=required;
    if(color==SurfaceColor::rgb565) {
        RenderState conversion;conversion.texture_modes[0]=1;conversion.textures[0]=id;conversion.final_inputs={0x00000008,0x00001800};
        prepare(conversion);
    }
    return id;
}
void Renderer::release_texture(std::uint32_t id) {
    auto& r=*impl_;
    if(r.recording || !id || id>=r.textures.size() || !r.textures[id] || r.targets.contains(id))
        throw std::runtime_error("Release an existing ordinary texture outside submission");
    r.release_slot(id);
}
void Renderer::release_target(std::uint32_t id) {
    auto& r=*impl_;const auto found=r.targets.find(id);
    if(r.recording || !id || found==r.targets.end()) throw std::runtime_error("Release an existing auxiliary target outside submission");
    if(r.current_target==id) r.select_target(0);
    const auto& surface=found->second;
    const bool shared=std::ranges::any_of(r.targets,[&](const auto& entry){return entry.first!=id && entry.second.color==surface.color;});
    r.target_bytes-=r.target_size(surface.width,surface.height,surface.color_format,surface.depth_format,shared);
    r.targets.erase(found);r.release_slot(id);
}
void Renderer::set_target(std::uint32_t target) {impl_->select_target(target);}
void Renderer::grow_target(std::uint32_t target,std::uint32_t width,std::uint32_t height) {
    auto& r=*impl_;extent(width,height);
    const auto found=r.targets.find(target);
    if(!target || found==r.targets.end() || r.recording) throw std::runtime_error("Grow an existing auxiliary target outside submission");
    const auto original=found->second;
    if(width<original.width || height<original.height) throw std::runtime_error("Target growth cannot shrink either dimension");
    if(width==original.width && height==original.height) return;
    std::vector<std::uint32_t> aliases{target};
    for(const auto& [id,image]:r.targets) if(id!=target && image.color==original.color) aliases.push_back(id);
    unsigned channels=r.color_size(original.color_format)*(r.samples+(r.samples>1?1:0));
    for(const auto id:aliases) channels+=r.depth_size(r.targets.at(id).depth_format)*r.samples;
    const auto previous=std::size_t(original.width)*original.height*channels*r.resolution_scale*r.resolution_scale;
    const auto required=std::size_t(width)*height*channels*r.resolution_scale*r.resolution_scale;
    if(required>r.target_limit-(r.target_bytes-previous)-r.presentation_bytes()) throw std::runtime_error("Render targets exceed the graphics memory budget; reduce resolution or MSAA.");
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    for(const auto identifier:aliases) {
        const auto prior=r.targets.at(identifier);
        auto output=r.make_target(width,height,prior.color_format,prior.depth_format,
            identifier==target?nullptr:&r.targets.at(target));
        constexpr float black[4]{};
        if(identifier==target) {
            r.context->ClearRenderTargetView(output.color_view.Get(),black);
            if(r.samples==1) r.context->CopySubresourceRegion(output.color.Get(),0,0,0,0,prior.color.Get(),0,nullptr);
        }
        if(output.depth)
            r.context->ClearDepthStencilView(output.depth_view.Get(),D3D11_CLEAR_DEPTH|(prior.depth_format==SurfaceDepth::z16?0:D3D11_CLEAR_STENCIL),1,0);
        if(r.samples>1) {
            if(identifier==target || output.depth) {
                r.multisample_copy->grow(prior.multisample_view.Get(),prior.depth_texture.Get(),prior.stencil_texture.Get(),
                    output.color_view.Get(),output.depth_view.Get(),prior.width*r.resolution_scale,prior.height*r.resolution_scale);
                *output.dirty=true;
            }
        } else if(output.depth) {
            // Depth/stencil resources require whole copies with matching dimensions.
            // Migrate their packed bits through ordinary typeless textures, then copy
            // the complete enlarged image back into its depth/stencil resource.
            D3D11_TEXTURE2D_DESC packed{};packed.Width=prior.width*r.resolution_scale;packed.Height=prior.height*r.resolution_scale;
            packed.MipLevels=packed.ArraySize=1;packed.Format=prior.depth_format==SurfaceDepth::z16?DXGI_FORMAT_R16_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;packed.SampleDesc.Count=1;
            ComPtr<ID3D11Texture2D> previous_depth,grown_depth;
            checked(r.device->CreateTexture2D(&packed,nullptr,&previous_depth),"Previous depth transfer");
            packed.Width=width*r.resolution_scale;packed.Height=height*r.resolution_scale;
            checked(r.device->CreateTexture2D(&packed,nullptr,&grown_depth),"Grown depth transfer");
            r.context->CopyResource(previous_depth.Get(),prior.depth.Get());
            r.context->CopyResource(grown_depth.Get(),output.depth.Get());
            r.context->CopySubresourceRegion(grown_depth.Get(),0,0,0,0,previous_depth.Get(),0,nullptr);
            r.context->CopyResource(output.depth.Get(),grown_depth.Get());
        }
        r.targets.at(identifier)=std::move(output);r.textures[identifier]=r.targets.at(identifier).texture_view;
    }
    r.target_bytes=r.target_bytes-previous+required;r.select_target(r.current_target);
}
void Renderer::copy_target(std::uint32_t destination) {
    auto& r=*impl_;
    const auto found=r.targets.find(destination);
    if(found==r.targets.end() || destination==r.current_target || found->second.width!=r.width || found->second.height!=r.height ||
        found->second.color_format!=r.targets.at(r.current_target).color_format)
        throw std::runtime_error("Target copy requires a distinct target with matching dimensions");
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    r.context->CopyResource(found->second.color.Get(),r.target.Get());
    *found->second.dirty=true;
    auto view=r.rtv.Get();r.context->OMSetRenderTargets(1,&view,r.dsv.Get());
}
void Renderer::write_region(std::uint32_t target,std::uint32_t x,std::uint32_t y,
                            std::uint32_t width,std::uint32_t height,Bytes pixels,std::uint32_t pitch) {
    auto& r=*impl_;
    const auto found=r.targets.find(target);
    if(r.recording || found==r.targets.end()) throw std::runtime_error("Write an existing color image outside frame submission");
    const auto& output=found->second;
    if(x>output.width || y>output.height || width>output.width-x || height>output.height-y)
        throw std::runtime_error("Image write exceeds its color surface");
    if(!width || !height) return;
    const auto bpp=output.color_format==SurfaceColor::rgb565?2U:4U;
    const auto row=width*bpp;
    if(pitch<row || std::uint64_t(height-1)*pitch+row>pixels.size()) throw std::runtime_error("Truncated image write payload");
    PerfScope scope(r.profile,PerfPhase::texture_upload);
    // Compact rows so a short final row never reads beyond mapped guest RAM.
    std::vector<std::byte> packed(std::size_t(row)*height);
    for(unsigned i=0;i<height;++i) std::memcpy(packed.data()+std::size_t(i)*row,pixels.data()+std::size_t(i)*pitch,row);
    D3D11_TEXTURE2D_DESC description{};description.Width=width;description.Height=height;
    description.MipLevels=description.ArraySize=description.SampleDesc.Count=1;
    description.Format=bpp==2?DXGI_FORMAT_B5G6R5_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;
    description.Usage=D3D11_USAGE_IMMUTABLE;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{packed.data(),row,0};
    ComPtr<ID3D11Texture2D> image;ComPtr<ID3D11ShaderResourceView> view;
    checked(r.device->CreateTexture2D(&description,&data,&image),"Guest color image");
    checked(r.device->CreateShaderResourceView(image.Get(),nullptr,&view),"Guest color image view");
    if(!r.display_vs) {
        const auto vs=r.shader(ShaderKind::display_vertex),ps=r.shader(ShaderKind::display_pixel);
        checked(r.device->CreateVertexShader(vs.data(),vs.size(),nullptr,&r.display_vs),"Image vertex shader");
        checked(r.device->CreatePixelShader(ps.data(),ps.size(),nullptr,&r.display_ps),"Image pixel shader");
    }
    if(!r.image_sampler) {
        D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sampler.MaxLOD=D3D11_FLOAT32_MAX;
        checked(r.device->CreateSamplerState(&sampler,&r.image_sampler),"Image point sampler");
    }
    // A point-sampled pass also fills every scaled pixel and every MSAA sample.
    r.unbind_textures();auto color=output.color_view.Get();r.context->OMSetRenderTargets(1,&color,nullptr);
    const auto scale=r.resolution_scale;
    const D3D11_VIEWPORT viewport{float(x*scale),float(y*scale),float(width*scale),float(height*scale),0,1};
    const D3D11_RECT rectangle{LONG(x*scale),LONG(y*scale),LONG((x+width)*scale),LONG((y+height)*scale)};
    r.context->RSSetViewports(1,&viewport);r.context->RSSetScissorRects(1,&rectangle);r.context->RSSetState(r.clear_raster.Get());
    r.context->OMSetBlendState(nullptr,nullptr,UINT_MAX);r.context->OMSetDepthStencilState(r.clear_depths[0].Get(),0);
    r.context->IASetInputLayout(nullptr);r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetShader(r.display_vs.Get(),nullptr,0);r.context->PSSetShader(r.display_ps.Get(),nullptr,0);
    auto input=view.Get();auto sampler=r.image_sampler.Get();
    r.context->PSSetShaderResources(0,1,&input);r.context->PSSetSamplers(0,1,&sampler);r.context->Draw(3,0);
    *output.dirty=true;++r.stats.texture_uploads;r.stats.texture_upload_bytes+=packed.size();
    r.select_target(r.current_target);
}
void Renderer::copy_region(std::uint32_t destination,std::uint32_t source,
                          std::uint32_t sx,std::uint32_t sy,std::uint32_t dx,std::uint32_t dy,
                          std::uint32_t width,std::uint32_t height) {
    auto& r=*impl_;
    const auto from=r.targets.find(source),to=r.targets.find(destination);
    if(r.recording || from==r.targets.end() || to==r.targets.end())
        throw std::runtime_error("Copy color images between ended native targets");
    const auto& input=from->second;const auto& output=to->second;
    if(input.color_format!=output.color_format || sx>input.width || sy>input.height ||
       width>input.width-sx || height>input.height-sy || dx>output.width || dy>output.height ||
       width>output.width-dx || height>output.height-dy)
        throw std::runtime_error("Image copy rectangle or format differs from its surfaces");
    if(!width || !height || (input.color==output.color && sx==dx && sy==dy)) return;
    sx*=r.resolution_scale;sy*=r.resolution_scale;dx*=r.resolution_scale;dy*=r.resolution_scale;
    width*=r.resolution_scale;height*=r.resolution_scale;++r.stats.target_copies;
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    const D3D11_BOX region{sx,sy,0,sx+width,sy+height,1};
    if(r.samples>1) {
        auto view=input.multisample_view;
        ComPtr<ID3D11Texture2D> snapshot;
        if(input.color==output.color) {
            D3D11_TEXTURE2D_DESC description{};input.color->GetDesc(&description);description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            checked(r.device->CreateTexture2D(&description,nullptr,&snapshot),"Overlapping multisample image copy");
            r.context->CopyResource(snapshot.Get(),input.color.Get());view.Reset();
            checked(r.device->CreateShaderResourceView(snapshot.Get(),nullptr,&view),"Multisample copy source");
        }
        r.multisample_copy->color(view.Get(),output.color_view.Get(),sx,sy,dx,dy,width,height);*output.dirty=true;
        auto target=r.rtv.Get();r.context->OMSetRenderTargets(1,&target,r.dsv.Get());return;
    }
    if(input.color==output.color) {
        D3D11_TEXTURE2D_DESC description{};input.color->GetDesc(&description);
        description.Width=width;description.Height=height;description.BindFlags=0;
        ComPtr<ID3D11Texture2D> temporary;
        checked(r.device->CreateTexture2D(&description,nullptr,&temporary),"Overlapping image copy");
        r.context->CopySubresourceRegion(temporary.Get(),0,0,0,0,input.color.Get(),0,&region);
        r.context->CopySubresourceRegion(output.color.Get(),0,dx,dy,0,temporary.Get(),0,nullptr);
    } else r.context->CopySubresourceRegion(output.color.Get(),0,dx,dy,0,input.color.Get(),0,&region);
    auto view=r.rtv.Get();r.context->OMSetRenderTargets(1,&view,r.dsv.Get());
}
void Renderer::copy_to_main() {
    auto& r=*impl_;
    if(r.recording || !r.current_target) throw std::runtime_error("Copy an ended auxiliary target to the main image");
    const auto source=r.target;
    const auto identifier=r.current_target;
    const auto format=r.targets.at(identifier).color_format;
    const auto width=r.width,height=r.height;
    const auto& main=r.targets.at(0);
    if(main.width!=width || main.height!=height) throw std::runtime_error("Presentation target dimensions differ");
    r.select_target(0);
    if(format==SurfaceColor::rgb565) {
        RenderState state;state.texture_modes[0]=1;state.textures[0]=identifier;state.final_inputs={0x00000008,0x00001800};
        prepare(state);begin(0,1,false);
        std::array<Vertex,4> vertices{};
        constexpr std::array<Float4,4> positions{{{-1,1,0,1},{1,1,0,1},{1,-1,0,1},{-1,-1,0,1}}};
        for(unsigned i=0;i<4;++i) {vertices[i].position=positions[i];vertices[i].uv[0]={(i==1 || i==2)?1.0f:0.0f,i>=2?1.0f:0.0f,0,1};}
        constexpr std::array<std::uint32_t,6> indices{0,1,2,0,2,3};draw(state,vertices,indices);end();return;
    }
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    r.context->CopyResource(r.target.Get(),source.Get());
    *r.targets.at(0).dirty=true;
    r.ended=true;
}
ID3D11Device* Renderer::native_device() const {return impl_->device.Get();}
ID3D11DeviceContext* Renderer::native_context() const {return impl_->context.Get();}
void Renderer::performance(Performance* profile,std::uint64_t next_frame) {
    auto& r=*impl_;r.profile=profile;r.source_frame=next_frame;++r.profile_generation;r.profile_frames={};
}
Performance* Renderer::performance() const {return impl_->profile;}
void Renderer::performance_frame(std::uint64_t next_frame) {
    auto& r=*impl_;
    if(r.profile && r.profile->active()) {
        auto& frame=r.profile_frame(r.source_frame);frame.closed=true;r.report_gpu(frame);
    }
    r.source_frame=next_frame;r.collect_timings();
}
void Renderer::finish_performance() {
    auto& r=*impl_;r.context->Flush();const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
    do {
        r.collect_timings();
        if(!std::ranges::any_of(r.timings,[&](const auto& t){return t.pending && t.generation==r.profile_generation;})) break;
        std::this_thread::yield();
    } while(std::chrono::steady_clock::now()<deadline);
    for(auto& frame:r.profile_frames) if(frame.id) r.report_gpu(frame);
}
void Renderer::performance_resources() {
    auto& r=*impl_;if(!r.profile || !r.profile->resources_due()) return;
    PerfScope scope(r.profile,PerfPhase::diagnostics);
    ComPtr<IDXGIDevice> device;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{},shared{};bool valid=false;
    if(SUCCEEDED(r.device.As(&device)) && SUCCEEDED(device->GetAdapter(&adapter)) && SUCCEEDED(adapter.As(&adapter3)))
        valid=SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local)) &&
            SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&shared));
    const auto live=std::ranges::count_if(r.textures,[](const auto& view){return bool(view);});
    r.profile->resources(local.CurrentUsage,local.Budget,shared.CurrentUsage,valid,live,r.targets.size(),r.texture_bytes,r.target_bytes+r.presentation_bytes());
}
void Renderer::attach_window(SDL_Window* sdl_window) {
    auto& r=*impl_;
    if(r.window || !sdl_window || !SDL_GetWindowID(sdl_window))
        throw std::runtime_error("Attach one valid SDL window to the renderer");
#ifdef _WIN32
    const auto window=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    if(!IsWindow(window)) throw std::runtime_error("SDL did not provide a Win32 window");
#else
    const auto window=reinterpret_cast<HWND>(sdl_window);
#endif
    ComPtr<IDXGIDevice> device;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    checked(r.device.As(&device),"Presentation device");checked(device->GetAdapter(&adapter),"Presentation adapter");
    checked(adapter->GetParent(IID_PPV_ARGS(&factory)),"Presentation factory");
    ComPtr<IDXGIFactory5> modern;BOOL tearing=FALSE;
    r.allow_tearing=SUCCEEDED(factory.As(&modern)) &&
        SUCCEEDED(modern->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))) && tearing;
    r.swapchain_flags=r.allow_tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
#ifdef _WIN32
    r.swapchain_flags|=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
#endif
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width=r.width;description.Height=r.height;description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count=1;description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount=2;description.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;description.Scaling=DXGI_SCALING_STRETCH;
    description.Flags=r.swapchain_flags;
    ComPtr<IDXGISwapChain1> swapchain;
    checked(factory->CreateSwapChainForHwnd(r.device.Get(),window,&description,nullptr,nullptr,&swapchain),"Window swapchain");
    checked(swapchain.As(&r.swapchain),"Window frame latency interface");
#ifdef _WIN32
    checked(r.swapchain->SetMaximumFrameLatency(1),"Window frame latency");
    r.frame_ready=r.swapchain->GetFrameLatencyWaitableObject();
    if(!r.frame_ready) throw std::runtime_error("Window frame availability handle is missing");
    checked(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER),"Window association");
#else
    ComPtr<IDXGIDevice1> latency_device;
    checked(device.As(&latency_device),"Presentation latency device");
    checked(latency_device->SetMaximumFrameLatency(1),"Presentation queue latency");
#endif
    const auto vs=r.shader(ShaderKind::display_vertex),ps=r.shader(ShaderKind::display_pixel);
    checked(r.device->CreateVertexShader(vs.data(),vs.size(),nullptr,&r.display_vs),"Display vertex shader");
    checked(r.device->CreatePixelShader(ps.data(),ps.size(),nullptr,&r.display_ps),"Display pixel shader");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    sampler.ComparisonFunc=D3D11_COMPARISON_ALWAYS;
    checked(r.device->CreateSamplerState(&sampler,&r.display_sampler),"Display sampler");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=true;
    checked(r.device->CreateRasterizerState(&raster,&r.display_raster),"Display rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    depth.FrontFace=depth.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    checked(r.device->CreateDepthStencilState(&depth,&r.display_depth),"Display depth state");
    r.window=window;r.prepare_presentation();
}
void Renderer::publish_frame() {
    auto& r=*impl_;
    if(!r.window || !r.ended || r.recording || r.current_target) throw std::runtime_error("Publish a completed main image on an attached window");
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    auto& main=r.targets.at(0);r.resolve(main);
    auto* source=main.resolved.Get();
    if(r.antialiasing) {r.antialiasing->publish(main.texture_view.Get());source=r.antialiasing->image();}
    r.context->CopyResource(r.published.Get(),source);
    r.published_ready=true;
    // HUD enlargement advances in bounded steps after each published frame.
    const auto now=std::chrono::steady_clock::now();
    if(r.last_publish!=std::chrono::steady_clock::time_point{})
        r.publish_interval_ms=0.9*r.publish_interval_ms+0.1*std::clamp(std::chrono::duration<double,std::milli>(now-r.last_publish).count(),1.0,100.0);
    r.last_publish=now;
    if(r.stats.gpu_timing_valid) r.scene_gpu_ms=r.scene_gpu_ms>0?0.8*r.scene_gpu_ms+0.2*r.stats.gpu_ms:r.stats.gpu_ms;
    if(r.interface_upscaler && r.interface_upscaler->queued())
        r.finish_interface(r.interface_upscaler->step(false,r.interface_budget()));
    if(!r.interface_saves.empty()) r.write_interface_saves();
    r.select_target(r.current_target);
}
bool Renderer::wait_for_window_frame(unsigned timeout_ms) {
    return impl_->window && impl_->acquire_frame(timeout_ms);
}
bool Renderer::display(bool published,bool wait) {
    auto& r=*impl_;
    if(published && (!r.published_ready || r.recording)) return false;
    if(!r.window || r.recording || (!published && (!r.ended || r.current_target)))
        throw std::runtime_error("Display a completed image on its attached window");
    if(!published) {
        auto& main=r.targets.at(0);r.resolve(main);
        if(r.antialiasing) {r.unbind_textures();r.antialiasing->publish(main.texture_view.Get());}
    }
#ifdef _WIN32
    RECT rectangle{};if(!GetClientRect(r.window,&rectangle)) throw std::runtime_error("Cannot read window dimensions");
    const auto width=static_cast<std::uint32_t>(rectangle.right),height=static_cast<std::uint32_t>(rectangle.bottom);
#else
    int pixel_width=0,pixel_height=0;
    if(!SDL_GetWindowSizeInPixels(reinterpret_cast<SDL_Window*>(r.window),&pixel_width,&pixel_height))
        throw std::runtime_error(SDL_GetError());
    const auto width=static_cast<std::uint32_t>(pixel_width),height=static_cast<std::uint32_t>(pixel_height);
#endif
    if(!width || !height) return false;
    if(width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::runtime_error("Window dimensions exceed the Direct3D 11 limit");
    if(!r.acquire_frame(wait?UINT32_MAX:0)) return false;
    r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
    if(!r.window_view || r.window_width!=width || r.window_height!=height) {
        r.window_view.Reset();checked(r.swapchain->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,r.swapchain_flags),"Window resize");
        ComPtr<ID3D11Texture2D> buffer;checked(r.swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Window buffer");
        checked(r.device->CreateRenderTargetView(buffer.Get(),nullptr,&r.window_view),"Window render target");
        r.window_width=width;r.window_height=height;
    }
    auto output=r.window_view.Get();r.context->OMSetRenderTargets(1,&output,nullptr);
    const float black[4]={0,0,0,1};r.context->ClearRenderTargetView(output,black);
    const auto image_height=std::min(float(height),float(width)/r.display_aspect),image_width=image_height*r.display_aspect;
    const D3D11_VIEWPORT viewport{(width-image_width)/2,(height-image_height)/2,image_width,image_height,0,1};r.context->RSSetViewports(1,&viewport);
    r.context->RSSetState(r.display_raster.Get());r.context->OMSetDepthStencilState(r.display_depth.Get(),0);
    r.context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
    r.context->IASetInputLayout(nullptr);r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetShader(r.display_vs.Get(),nullptr,0);r.context->PSSetShader(r.display_ps.Get(),nullptr,0);
    auto texture=published?r.published_view.Get():(r.antialiasing?r.antialiasing->view():r.targets.at(0).texture_view.Get());auto sampler=r.display_sampler.Get();
    r.context->PSSetShaderResources(0,1,&texture);r.context->PSSetSamplers(0,1,&sampler);r.context->Draw(3,0);
    r.unbind_textures();r.displaying=true;return true;
}
bool Renderer::present(bool vsync,bool wait) {
    auto& r=*impl_;
    if(!r.displaying) throw std::runtime_error("Display a frame before presenting");
    const auto flags=(wait?0U:DXGI_PRESENT_DO_NOT_WAIT)|(!vsync && r.allow_tearing?DXGI_PRESENT_ALLOW_TEARING:0U);
    const auto status=r.swapchain->Present(vsync?1:0,flags);
    r.displaying=false;r.select_target(r.current_target);
    if(!wait && status==DXGI_ERROR_WAS_STILL_DRAWING) return false;
    checked(status,"Window presentation");r.frame_acquired=false;return status!=DXGI_STATUS_OCCLUDED;
}

std::uint32_t Renderer::upload(const TextureData& texture,bool reuse_slot) {
    auto& r = *impl_;
    PerfScope scope(r.profile,PerfPhase::texture_upload);
    if (r.recording) throw std::runtime_error("Upload textures before frame submission");
    extent(texture.width, texture.height);
    if (!texture.depth || texture.depth>2048 || (texture.depth>1 && (texture.width>2048 || texture.height>2048)) ||
        texture.bytes.size() > 64 * 1024 * 1024 || !texture.mip_levels ||
        texture.mip_levels > static_cast<unsigned>(std::bit_width(std::max({texture.width, texture.height,texture.depth}))))
        throw std::runtime_error("Invalid texture size or mip count");
    DXGI_FORMAT format;
    unsigned bpp = 0, block = 0;
    bool swizzled = false;
    const bool expand_luminance=texture.format==TextureFormat::a8l8;
    switch (texture.format) {
    case TextureFormat::a8l8: format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 2; swizzled = true; break;
    case TextureFormat::argb4: format = DXGI_FORMAT_B4G4R4A4_UNORM; bpp = 2; swizzled = true; break;
    case TextureFormat::rgb565: format = DXGI_FORMAT_B5G6R5_UNORM; bpp = 2; swizzled = true; break;
    case TextureFormat::rgb565_linear: format = DXGI_FORMAT_B5G6R5_UNORM; bpp = 2; break;
    case TextureFormat::argb8: format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 4; swizzled = true; break;
    case TextureFormat::xrgb8: format = DXGI_FORMAT_B8G8R8X8_UNORM; bpp = 4; swizzled = true; break;
    case TextureFormat::argb8_linear: format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 4; break;
    case TextureFormat::xrgb8_linear: format = DXGI_FORMAT_B8G8R8X8_UNORM; bpp = 4; break;
    case TextureFormat::dxt1: format = DXGI_FORMAT_BC1_UNORM; block = 8; break;
    case TextureFormat::dxt3: format = DXGI_FORMAT_BC2_UNORM; block = 16; break;
    case TextureFormat::dxt5: format = DXGI_FORMAT_BC3_UNORM; block = 16; break;
    default: throw std::runtime_error("Unsupported NV097 texture format: " + hex32(static_cast<std::uint32_t>(texture.format)));
    }
    if (swizzled && (!std::has_single_bit(texture.width) || !std::has_single_bit(texture.height) || !std::has_single_bit(texture.depth)))
        throw std::runtime_error("Swizzled texture dimensions must be powers of two");
    if (block && ((texture.width % 4) || (texture.height % 4)))
        throw std::runtime_error("BC top-level dimensions must be multiples of four");
    if (texture.pitch && (swizzled || block || texture.mip_levels != 1))
        throw std::runtime_error("An explicit pitch requires a single linear mip");
    const auto resident_bytes=texture.bytes.size()*(expand_luminance?2U:1U);
    if (resident_bytes > 256 * 1024 * 1024 - r.texture_bytes)
        throw std::runtime_error("Texture resources exceed 256 MiB");
    if(texture.cubemap && (texture.width!=texture.height || texture.depth!=1)) throw std::runtime_error("Cubemap faces must be square and two-dimensional");
    const auto faces=texture.cubemap?6U:1U;
    std::vector<D3D11_SUBRESOURCE_DATA> data(texture.mip_levels*faces);
    std::vector<std::vector<std::byte>> linear(texture.mip_levels*faces);
    std::size_t offset = 0;
    for(unsigned face=0;face<faces;++face) for (unsigned mip = 0; mip < texture.mip_levels; ++mip) {
        const auto subresource=face*texture.mip_levels+mip;
        const auto w = std::max(1U, texture.width >> mip), h = std::max(1U, texture.height >> mip), d=std::max(1U,texture.depth>>mip);
        const auto pitch = texture.pitch ? texture.pitch : block ? ((w + 3) / 4) * block : w * bpp;
        if (pitch < w * bpp) throw std::runtime_error("Texture pitch is shorter than a row");
        const auto slice = std::size_t(pitch) * (block ? (h + 3) / 4 : h),size=slice*d;
        if (offset > texture.bytes.size() || size > texture.bytes.size() - offset) throw std::runtime_error("Truncated texture mip payload");
        const auto input_bytes = texture.bytes.subspan(offset, size);
        const void* pixels = input_bytes.data();
        if (swizzled) {
            const auto output_bpp=expand_luminance?4U:bpp;
            auto& output = linear[subresource]; output.resize(std::size_t(w)*h*d*output_bpp);
            std::uint32_t mx = 0, my = 0, mz=0,bit = 1;
            for (unsigned dimension = 1; dimension < std::max({w,h,d}); dimension <<= 1) {
                if (dimension < w) { mx |= bit; bit <<= 1; }
                if (dimension < h) { my |= bit; bit <<= 1; }
                if (dimension < d) { mz |= bit; bit <<= 1; }
            }
            std::uint32_t sz=0;
            for(unsigned z=0;z<d;++z) {
                std::uint32_t sy = 0;
                for (unsigned y = 0; y < h; ++y) {
                    std::uint32_t sx = 0;
                    for (unsigned x = 0; x < w; ++x) {
                        auto* destination=output.data()+((std::size_t(z)*h+y)*w+x)*output_bpp;
                        const auto* source=input_bytes.data()+(sx+sy+sz)*bpp;
                        if(expand_luminance) {
                            destination[0]=destination[1]=destination[2]=source[0];destination[3]=source[1];
                        } else std::memcpy(destination,source,bpp);
                        sx = (sx - mx) & mx;
                    }
                    sy = (sy - my) & my;
                }
                sz=(sz-mz)&mz;
            }
            pixels = output.data();
        }
        const auto upload_pitch=expand_luminance?w*4:static_cast<unsigned>(pitch);
        const auto upload_slice=expand_luminance?std::size_t(upload_pitch)*h:slice;
        data[subresource] = {pixels, upload_pitch, static_cast<UINT>(upload_slice)};
        offset += size;
        if(texture.cubemap && mip+1==texture.mip_levels) offset=static_cast<std::size_t>(cubemap_face_size(offset));
    }
    if (offset != texture.bytes.size()) throw std::runtime_error("Texture payload has trailing bytes; specify its full mip count");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = texture.width; desc.Height = texture.height; desc.MipLevels = texture.mip_levels;
    desc.ArraySize = faces; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.MiscFlags=texture.cubemap?D3D11_RESOURCE_MISC_TEXTURECUBE:0;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11ShaderResourceView> view;
    if(texture.depth>1) {
        D3D11_TEXTURE3D_DESC volume{};
        volume.Width=texture.width;volume.Height=texture.height;volume.Depth=texture.depth;
        volume.MipLevels=texture.mip_levels;volume.Format=format;
        volume.Usage=D3D11_USAGE_IMMUTABLE;volume.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture3D> resource;
        checked(r.device->CreateTexture3D(&volume,data.data(),&resource),"Volume texture upload");
        checked(r.device->CreateShaderResourceView(resource.Get(),nullptr,&view),"Volume texture view");
    } else if(texture.movie && r.upscaled_movies && r.resolution_scale>1 && !texture.cubemap && texture.mip_levels==1 &&
              !swizzled && !block && bpp==4) {
        return r.upload_movie(data[0],texture,format,reuse_slot);
    } else {
        ComPtr<ID3D11Texture2D> resource;
        checked(r.device->CreateTexture2D(&desc, data.data(), &resource), "Texture upload");
        checked(r.device->CreateShaderResourceView(resource.Get(), nullptr, &view), "Texture view");
    }
    ++r.stats.texture_uploads;r.stats.texture_upload_bytes+=texture.bytes.size();
    return r.store_texture(std::move(view),texture.cubemap?3U:texture.depth>1?2U:1U,resident_bytes,reuse_slot);
}
ComPtr<ID3D11BlendState> Renderer::Impl::create_blend(const BlendKey& key) const {
    const auto [enable,source,destination,equation,mask]=key;
    D3D11_BLEND_DESC blend{};
    auto& output = blend.RenderTarget[0];
    output.BlendEnable = enable;
    output.SrcBlend = blend_factor(source); output.DestBlend = blend_factor(destination);
    output.SrcBlendAlpha = alpha_blend_factor(output.SrcBlend); output.DestBlendAlpha = alpha_blend_factor(output.DestBlend);
    output.BlendOp = output.BlendOpAlpha = blend_op(equation);
    if (mask & ~0x01010101U) throw std::runtime_error("Invalid NV097 color write mask");
    output.RenderTargetWriteMask = ((mask & 0x10000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
        ((mask & 0x100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((mask & 1) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
        ((mask & 0x1000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    ComPtr<ID3D11BlendState> state;
    checked(device->CreateBlendState(&blend, &state), "Blend state");
    return state;
}
ComPtr<ID3D11DepthStencilState> Renderer::Impl::create_depth(const DepthKey& key) const {
    const auto [test,write,function,stencil,stencil_function,read_mask,write_mask,fail,depth_fail,pass]=key;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = test; depth.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = comparison(function);
    depth.StencilEnable=stencil;
    if(read_mask>255 || write_mask>255) throw std::runtime_error("Stencil masks must fit eight bits");
    depth.StencilReadMask=static_cast<UINT8>(read_mask); depth.StencilWriteMask=static_cast<UINT8>(write_mask);
    depth.FrontFace = depth.BackFace = {stencil_operation(fail),stencil_operation(depth_fail),
        stencil_operation(pass),comparison(stencil_function)};
    ComPtr<ID3D11DepthStencilState> state;
    checked(device->CreateDepthStencilState(&depth, &state), "Depth state");
    return state;
}
ComPtr<ID3D11RasterizerState> Renderer::Impl::create_raster(const RasterKey& key) const {
    const auto [cull,face,front_ccw,scissor,offset,offset_scale,offset_bias]=key;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.DepthClipEnable = TRUE;
    raster.MultisampleEnable=samples>1;
    raster.CullMode = D3D11_CULL_NONE;
    if (cull) {
        if (face == 0x404) raster.CullMode = D3D11_CULL_FRONT;
        else if (face == 0x405) raster.CullMode = D3D11_CULL_BACK;
        else throw std::runtime_error("Unsupported cull face");
    }
    raster.FrontCounterClockwise = front_ccw;
    raster.ScissorEnable=scissor;
    if(offset) {
        const auto scale=std::bit_cast<float>(offset_scale),bias=std::bit_cast<float>(offset_bias);
        if(!std::isfinite(scale) || !std::isfinite(bias) || std::abs(bias)>=2147483648.0f) throw std::runtime_error("Invalid polygon offset");
        raster.SlopeScaledDepthBias=scale; raster.DepthBias=static_cast<INT>(bias);
    }
    ComPtr<ID3D11RasterizerState> state;
    checked(device->CreateRasterizerState(&raster, &state), "Rasterizer state");
    return state;
}
ComPtr<ID3D11SamplerState> Renderer::Impl::create_sampler(const SamplerKey& key) {
    const auto [address,border_color,linear,texture_filter,control]=key;
    D3D11_SAMPLER_DESC sampler{};
    const auto filter=sampler_filter(key);
    const auto min=(filter>>16)&63,mag=(filter>>24)&15;
    const bool gaussian=gaussian_filter(filter);
    const bool mip=min>=3 && min<=6,min_linear=(min&1)==0;
    auto strength=anisotropy?anisotropy:1U<<((control>>4)&3);
    // The shader applies the Gaussian kernel at LOD zero. Its individual
    // taps interpolate linearly and must not acquire anisotropic filtering.
    sampler.Filter=gaussian?D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT:
        static_cast<D3D11_FILTER>((min_linear?0x10:0)|(mag==2?4:0)|(min>=5?1:0));
    if(!gaussian && strength>1 && min_linear && mag==2) sampler.Filter=D3D11_FILTER_ANISOTROPIC;
    else strength=1;
    sampler.AddressU = address_mode(address & 15);
    sampler.AddressV = address_mode((address >> 8) & 15);
    sampler.AddressW = address_mode((address >> 16) & 15);
    const auto border=color(border_color);std::copy(border.begin(),border.end(),sampler.BorderColor);
    sampler.MaxAnisotropy=strength;sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;
    int bias=filter&0x1FFF;if(bias&0x1000) bias-=0x2000;
    constexpr float min_lod_bias=-16.0f,max_lod_bias=15.99f; // D3D11 sampler limits.
    sampler.MipLODBias=mip?std::clamp(bias/256.0f,min_lod_bias,max_lod_bias):0;
    sampler.MinLOD=mip?float((control>>18)&0xFFF)/256:0;sampler.MaxLOD=mip?float((control>>6)&0xFFF)/256:0;
    if(unlimited_render_distance) sampler.MinLOD=sampler.MaxLOD=0;
    ComPtr<ID3D11SamplerState> state;
    checked(device->CreateSamplerState(&sampler, &state), "Sampler state");
    ++stats.sampler_misses;
    return state;
}
PipelineStates Renderer::Impl::prepared(const RenderState& state) const {
    const auto found=pipelines.find(pipeline_key(state));
    if(found==pipelines.end()) throw std::runtime_error("Pipeline was not prepared before submission");
    return found->second;
}
PipelineStates Renderer::prepare(const RenderState& state) {
    auto& r = *impl_;
    if (r.recording) throw std::runtime_error("Prepare pipelines before frame submission");
    validate_fragment(state);
    const auto key = pipeline_key(state);
    if(r.last_pipeline!=r.pipelines.end() && r.last_pipeline->first==key) return r.last_pipeline->second;
    if(const auto found=r.pipelines.find(key);found!=r.pipelines.end()) {
        r.last_pipeline=found;
        return found->second;
    }
    ++r.stats.state_misses;
    PipelineStates prepared;
    prepared.blend=r.blend_states.intern(blend_key(state),"blend",[&](const BlendKey& k) {return r.create_blend(k);});
    prepared.depth=r.depth_states.intern(depth_key(state),"depth-stencil",[&](const DepthKey& k) {return r.create_depth(k);});
    prepared.raster=r.raster_states.intern(raster_key(state),"rasterizer",[&](const RasterKey& k) {return r.create_raster(k);});
    for (unsigned i = 0; i < 4; ++i) {
        const auto sampler=sampler_key(state,i);
        const auto filter=sampler_filter(sampler),min=(filter>>16)&63,mag=(filter>>24)&15;
        if(!gaussian_filter(filter) && (min<1 || min>6 || mag<1 || mag>2))
            throw std::runtime_error(std::format("Unsupported texture filter at stage {}: {} (min {}, mag {}, shader {})",
                i,hex32(filter),min,mag,state.texture_modes[i]));
        prepared.samplers[i]=r.sampler_states.intern(sampler,"sampler",[&](const SamplerKey& k) {return r.create_sampler(k);});
    }
    // Forgetting combinations is safe: prepared and queued ids stay valid.
    if(r.pipelines.size()>=Impl::pipeline_limit) r.pipelines.clear();
    r.last_pipeline=r.pipelines.emplace(key,prepared).first;
    return prepared;
}
std::uint32_t Renderer::prepare_program(const VertexProgram& program,bool launch) {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("Prepare vertex programs before frame submission");
    const auto key=normalize_vertex_program(program);
    auto& index=r.program_indices[launch];
    const auto position=std::lower_bound(index.begin(),index.end(),key,[&](std::uint32_t id,const VertexProgram& candidate) {
        const auto& stored=r.programs[id-1].key;
        const auto order=std::memcmp(stored.tokens.data(),candidate.tokens.data(),sizeof(VertexTokens));
        return order?order<0:stored.start<candidate.start;
    });
    if(position!=index.end() && r.programs[*position-1].key==key) return *position;
    if(r.programs.size()>=1024) throw std::runtime_error("Vertex program cache limit reached");
    const auto code=r.shader(launch?ShaderKind::launch:ShaderKind::vertex,&key);
    Impl::Program prepared{key,launch};
    if(launch) {
        checked(r.device->CreateComputeShader(code.data(),code.size(),nullptr,&prepared.compute),"Transform launch shader");
        if(!r.launch_constants) {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth=sizeof(VertexConstants); description.StructureByteStride=sizeof(Float4);
            description.Usage=D3D11_USAGE_DEFAULT; description.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
            description.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            checked(r.device->CreateBuffer(&description,nullptr,&r.launch_constants),"Transform launch context");
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements=192;
            checked(r.device->CreateUnorderedAccessView(r.launch_constants.Get(),&view,&r.launch_output),"Transform context view");
            description.ByteWidth=sizeof(ProgramVertex); description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            checked(r.device->CreateBuffer(&description,nullptr,&r.launch_inputs),"Transform launch inputs");
            D3D11_SHADER_RESOURCE_VIEW_DESC input{};
            input.ViewDimension=D3D11_SRV_DIMENSION_BUFFER; input.Buffer.NumElements=16;
            checked(r.device->CreateShaderResourceView(r.launch_inputs.Get(),&input,&r.launch_input),"Transform input view");
            description.ByteWidth=sizeof(VertexConstants); description.BindFlags=0;
            description.MiscFlags=0; description.StructureByteStride=0;
            description.Usage=D3D11_USAGE_STAGING; description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            checked(r.device->CreateBuffer(&description,nullptr,&r.launch_readback),"Transform launch readback");
        }
    } else {
        prepared.signature=code;
        checked(r.device->CreateVertexShader(code.data(),code.size(),nullptr,&prepared.vertex),"Translated vertex shader");
        std::array<D3D11_INPUT_ELEMENT_DESC,16> inputs{};
        for(unsigned i=0;i<inputs.size();++i)
            inputs[i]={"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,i*static_cast<UINT>(sizeof(Float4)),D3D11_INPUT_PER_VERTEX_DATA,0};
        checked(r.device->CreateInputLayout(inputs.data(),static_cast<UINT>(inputs.size()),code.data(),code.size(),&prepared.layout),"Program vertex layout");
    }
    r.programs.push_back(std::move(prepared));
    index.insert(position,static_cast<std::uint32_t>(r.programs.size()));
    ++r.stats.program_creations;
    return static_cast<std::uint32_t>(r.programs.size());
}
std::uint32_t Renderer::prepare_fixed(const FixedTransform& transform) {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("Prepare fixed transforms before frame submission");
    validate_fixed_transform(transform);
    for(unsigned i=0;i<r.programs.size();++i) if(r.programs[i].fixed && r.programs[i].fixed_key==transform) return i+1;
    if(r.programs.size()>=1024) throw std::runtime_error("Vertex program cache limit reached");
    Impl::Program prepared{};prepared.fixed=true;prepared.fixed_key=transform;
    prepared.signature=r.fixed_signatures[transform.skin];prepared.vertex=r.fixed_vertices[transform.skin];prepared.layout=r.fixed_layouts[transform.skin];
    r.programs.push_back(std::move(prepared));return static_cast<std::uint32_t>(r.programs.size());
}
std::uint32_t Renderer::prepare_layout(std::uint32_t program,const VertexLayout& attributes) {
    auto& r=*impl_;
    if(r.recording || !program || program>r.programs.size() || r.programs[program-1].launch)
        throw std::runtime_error("Prepare a draw program before its vertex layout");
    for(unsigned i=0;i<r.stream_layouts.size();++i)
        if(r.stream_layouts[i].program==program && r.stream_layouts[i].attributes==attributes) return i+1;
    if(r.stream_layouts.size()>=1024) throw std::runtime_error("Vertex layout cache limit reached");
    ++r.stats.layout_misses;
    Impl::StreamLayout layout{program,attributes,attributes};
    for(const auto& attribute:attributes) {
        if(attribute.stream>16 || attribute.offset>2048) throw std::runtime_error("Invalid vertex attribute stream or offset");
        if(attribute.stream<16 && attribute.offset%std::min(attribute_format(attribute.format).fetched,4U))
            layout.repacked|=1U<<attribute.stream;
    }
    // Xbox streams can pack a float immediately after a single short. Keep
    // their source declaration intact and align only the native upload copy.
    std::array<unsigned,16> offsets{};
    std::array<D3D11_INPUT_ELEMENT_DESC,16> inputs{};
    for(unsigned i=0;i<inputs.size();++i) {
        auto& attribute=layout.native_attributes[i];
        const auto format=attribute.stream==16?AttributeFormat{DXGI_FORMAT_R32G32B32A32_FLOAT,16,16}:attribute_format(attribute.format);
        if(attribute.stream<16 && (layout.repacked&(1U<<attribute.stream))) {
            const auto alignment=std::min(format.fetched,4U);
            auto& offset=offsets[attribute.stream];offset=(offset+alignment-1)&~(alignment-1);
            attribute.offset=offset;offset+=format.fetched;
        }
        if(attribute.stream==16) layout.uses_current=true;
        else {
            const auto stream=attribute.stream;
            layout.stream_count=std::max(layout.stream_count,stream+1);
            layout.required[stream]=std::max(layout.required[stream],attributes[i].offset+format.bytes);
            layout.fetched[stream]=std::max(layout.fetched[stream],attribute.offset+format.fetched);
            layout.alignments[stream]=std::max(layout.alignments[stream],std::min(format.fetched,4U));
        }
        inputs[i]={"TEXCOORD",i,format.native,attribute.stream,attribute.stream==16?i*16:attribute.offset,D3D11_INPUT_PER_VERTEX_DATA,0};
    }
    const auto& code=r.programs[program-1].signature;
    const auto status=r.device->CreateInputLayout(inputs.data(),static_cast<UINT>(inputs.size()),code.data(),code.size(),&layout.native);
    if(FAILED(status)) {
        auto message=std::format("Native vertex stream layout failed: 0x{:08X}; program {}",static_cast<unsigned>(status),program);
        for(unsigned i=0;i<attributes.size();++i) if(attributes[i].stream<16)
            message+=std::format("; v{}=stream{}+{} format0x{:02X}",i,attributes[i].stream,attributes[i].offset,attributes[i].format);
        throw std::runtime_error(message);
    }
    r.stream_layouts.push_back(std::move(layout));return static_cast<std::uint32_t>(r.stream_layouts.size());
}
void Renderer::launch_program(std::uint32_t program,VertexConstants& constants,const ProgramVertex& input) {
    auto& r=*impl_;
    if(r.recording || !program || program>r.programs.size() || !r.programs[program-1].launch)
        throw std::runtime_error("Launch a prepared transform outside frame submission");
    r.context->UpdateSubresource(r.launch_constants.Get(),0,nullptr,constants.data(),0,0);
    r.context->UpdateSubresource(r.launch_inputs.Get(),0,nullptr,input.attributes.data(),0,0);
    auto output=r.launch_output.Get(); auto inputs=r.launch_input.Get();
    r.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
    r.context->CSSetShaderResources(0,1,&inputs);
    r.context->CSSetShader(r.programs[program-1].compute.Get(),nullptr,0);
    r.context->Dispatch(1,1,1);
    ID3D11UnorderedAccessView* no_output=nullptr; ID3D11ShaderResourceView* no_input=nullptr;
    r.context->CSSetUnorderedAccessViews(0,1,&no_output,nullptr); r.context->CSSetShaderResources(0,1,&no_input);
    r.context->CSSetShader(nullptr,nullptr,0);
    r.context->CopyResource(r.launch_readback.Get(),r.launch_constants.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(r.context->Map(r.launch_readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Transform context readback");
    std::memcpy(constants.data(),mapped.pData,sizeof(constants)); r.context->Unmap(r.launch_readback.Get(),0);
}
void Renderer::clear(std::uint32_t argb, float depth) {
    auto& r = *impl_;
    if (!std::isfinite(depth) || depth < 0 || depth > 1) throw std::runtime_error("Invalid clear depth");
    const auto rgba = color(argb);
    *r.targets.at(r.current_target).dirty=true;
    r.context->ClearRenderTargetView(r.rtv.Get(), rgba.data());
    if(r.dsv) r.context->ClearDepthStencilView(r.dsv.Get(),D3D11_CLEAR_DEPTH|
        (r.targets.at(r.current_target).depth_format==SurfaceDepth::z16?0:D3D11_CLEAR_STENCIL),depth,0);
    if(!r.recording) r.ended=true;
}
void Renderer::clear_buffers(std::uint32_t flags,std::uint32_t argb,float depth,std::uint8_t stencil,
                             std::span<const std::int32_t> rectangle) {
    auto& r=*impl_;
    if(flags&~0xF3U) throw std::runtime_error("Invalid clear channel flags");
    if(flags&0xF0U) *r.targets.at(r.current_target).dirty=true;
    if(!r.dsv) flags&=~3U;
    else if(r.targets.at(r.current_target).depth_format==SurfaceDepth::z16) flags&=~2U;
    if(!std::isfinite(depth) || depth<0 || depth>1) throw std::runtime_error("Invalid clear depth");
    if(!rectangle.empty() && rectangle.size()!=4) throw std::runtime_error("A clear rectangle requires four coordinates");
    const D3D11_RECT bounds=rectangle.empty()?D3D11_RECT{0,0,LONG(r.width),LONG(r.height)}:
        D3D11_RECT{std::clamp<LONG>(rectangle[0],0,r.width),std::clamp<LONG>(rectangle[1],0,r.height),
                   std::clamp<LONG>(rectangle[2],0,r.width),std::clamp<LONG>(rectangle[3],0,r.height)};
    if(!flags || bounds.left>=bounds.right || bounds.top>=bounds.bottom) return;
    if(bounds.left || bounds.top || bounds.right!=LONG(r.width) || bounds.bottom!=LONG(r.height) ||
        ((flags&0xF0U)!=0 && (flags&0xF0U)!=0xF0U)) {
        // One precompiled pass replaces only the requested channels and pixels.
        // Its state is independent of the preceding game's blend/depth/scissor.
        const std::array<Float4,2> constants{color(argb),Float4{depth,0,0,0}};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(r.context->Map(r.clear_constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Clear constants");
        std::memcpy(mapped.pData,constants.data(),sizeof(constants));r.context->Unmap(r.clear_constants.Get(),0);
        r.unbind_textures();auto target=r.rtv.Get();r.context->OMSetRenderTargets(1,&target,r.dsv.Get());
        const D3D11_VIEWPORT viewport{0,0,float(r.width*r.resolution_scale),float(r.height*r.resolution_scale),0,1};
        const auto s=LONG(r.resolution_scale);const D3D11_RECT scaled{bounds.left*s,bounds.top*s,bounds.right*s,bounds.bottom*s};
        r.context->RSSetViewports(1,&viewport);r.context->RSSetScissorRects(1,&scaled);r.context->RSSetState(r.clear_raster.Get());
        r.context->OMSetBlendState(r.clear_blends[(flags>>4)&15].Get(),nullptr,UINT_MAX);
        r.context->OMSetDepthStencilState(r.clear_depths[flags&3].Get(),stencil);
        r.context->IASetInputLayout(nullptr);r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        r.context->VSSetShader(r.clear_vs.Get(),nullptr,0);r.context->PSSetShader(r.clear_ps.Get(),nullptr,0);
        auto buffer=r.clear_constants.Get();r.context->VSSetConstantBuffers(0,1,&buffer);r.context->PSSetConstantBuffers(0,1,&buffer);
        r.context->Draw(3,0);if(!r.recording) r.ended=true;return;
    }
    if(flags&0xF0U) {const auto rgba=color(argb);r.context->ClearRenderTargetView(r.rtv.Get(),rgba.data());}
    const auto mask=((flags&1)?D3D11_CLEAR_DEPTH:0)|((flags&2)?D3D11_CLEAR_STENCIL:0);
    if(mask) r.context->ClearDepthStencilView(r.dsv.Get(),mask,depth,stencil);
    if(!r.recording) r.ended=true;
}
void Renderer::begin(std::uint32_t argb, float depth, bool clear_target) {
    auto& r = *impl_;
    if (r.recording) throw std::runtime_error("Frame already begun");
    // Presentation and other passes share this context between draw batches.
    r.bound_draw.reset();
    r.current_vertex_bound=false;
    if(clear_target) clear(argb, depth);
    r.stats.draws = r.stats.vertices = r.stats.vertex_bytes = 0; r.stats.gpu_ms = 0; r.stats.gpu_timing_valid = false;
    r.collect_timings();r.timing_index=static_cast<unsigned>(r.timings.size());++r.frame_id;
    const bool profiling=r.profile && r.profile->active();
    if(profiling) ++r.profile_frame(r.source_frame).expected;
    for(unsigned index=0;index<r.timings.size();++index) if(!r.timings[index].pending) {
        r.timing_index=index;auto& timing=r.timings[index];timing.frame=r.frame_id;
        timing.source=profiling?r.source_frame:0;timing.generation=r.profile_generation;
        r.context->Begin(timing.timer.Get());r.context->End(timing.start.Get());break;
    }
    if(profiling && r.timing_index==r.timings.size()) {auto& f=r.profile_frame(r.source_frame);--f.expected;++f.dropped;}
    r.recording = true; r.ended = false;
    auto target = r.rtv.Get();
    r.context->OMSetRenderTargets(1, &target, r.dsv.Get());
    const D3D11_VIEWPORT viewport{0, 0, float(r.width*r.resolution_scale), float(r.height*r.resolution_scale), 0, 1};
    r.context->RSSetViewports(1, &viewport);
}
void Renderer::clear_stencil(std::uint8_t value) {
    if(impl_->targets.at(impl_->current_target).depth_format==SurfaceDepth::z24s8)
        impl_->context->ClearDepthStencilView(impl_->dsv.Get(),D3D11_CLEAR_STENCIL,1,value);
}
void Renderer::draw(const RenderState& state,std::span<const Vertex> vertices,std::span<const std::uint32_t> indices) {
    impl_->draw(state,std::as_bytes(vertices),vertices.size(),indices);
}
void Renderer::draw_program(const RenderState& state,std::uint32_t program,const VertexConstants& constants,
                            const ProgramView& view,std::span<const ProgramVertex> vertices,std::span<const std::uint32_t> indices) {
    auto& r=*impl_;
    if(!program || program>r.programs.size() || r.programs[program-1].launch) throw std::runtime_error("Vertex program was not prepared");
    if(!std::isfinite(view.depth_scale) || view.depth_scale<=0) throw std::runtime_error("Invalid vertex depth scale");
    unsigned mode=0;
    if(state.fog_enable) switch(state.fog_mode) {
    case 0x2601:case 0x0804:mode=0;break;
    case 0x0800:case 0x0802:mode=1;break;
    case 0x0801:case 0x0803:mode=2;break;
    default:throw std::runtime_error(std::format("Unsupported NV2A fog mode 0x{:X}",state.fog_mode));
    }
    const bool absolute=state.fog_mode==0x0802 || state.fog_mode==0x0803 || state.fog_mode==0x0804;
    ProgramUniforms uniforms{constants,{{2.0f/r.width,2.0f/r.height,1/view.depth_scale,view.subpixel_bias},
        {state.fog_parameters[0],state.fog_parameters[1],float(mode),state.fog_enable?(absolute?2.0f:1.0f):0.0f},view.texture_scales}};
    r.draw(state,std::as_bytes(vertices),vertices.size(),indices,&r.programs[program-1],&uniforms);
}
void Renderer::draw_streams(const RenderState& state,std::uint32_t layout,const VertexConstants& constants,const ProgramView& view,
                            std::span<const VertexStream> streams,const ProgramVertex& current,std::uint32_t count,
                            std::span<const std::uint32_t> indices) {
    const StreamDraw draw{state,constants,view,streams,current,layout,count,indices};
    draw_streams(std::span(&draw,1));
}
Renderer::Impl::StreamUpload Renderer::Impl::plan_streams(const StreamDraw& draw) const {
    if(!draw.layout || draw.layout>stream_layouts.size() || draw.streams.size()>16)
        throw std::runtime_error("Native vertex streams were not prepared");
    if(!draw.count || draw.count>max_vertices || draw.indices.size()>max_vertices)
        throw std::runtime_error("Draw exceeds bounded vertex/index buffers");
    const auto& layout=stream_layouts[draw.layout-1];
    StreamUpload upload{};
    if(draw.streams.size()<layout.stream_count) throw std::runtime_error("Missing vertex attribute stream");
    upload.uses_current=layout.uses_current;
    const auto& required=layout.required;
    const auto& fetched=layout.fetched;
    const auto& alignments=layout.alignments;
    std::size_t total=0;
    for(unsigned i=0;i<draw.streams.size();++i) if(required[i]) {
        const auto& source=draw.streams[i];
        if(source.stride>2048 || (source.stride && required[i]>source.stride) ||
            source.bytes.size()<(draw.count-1)*source.stride+required[i]) throw std::runtime_error("Vertex stream stride or storage is too small");
        total=(total+15)&~std::size_t(15);
        upload.offsets[i]=static_cast<UINT>(total);
        const auto stride=(layout.repacked&(1U<<i))?fetched[i]:std::max(source.stride,fetched[i]);
        upload.strides[i]=source.stride?(stride+alignments[i]-1)&~(alignments[i]-1):0;
        upload.sizes[i]=source.stride?draw.count*upload.strides[i]:fetched[i];
        total+=upload.sizes[i];
    }
    if(total>vertex_capacity) throw std::runtime_error("Native vertex streams exceed upload storage");
    upload.size=static_cast<UINT>(total);
    return upload;
}
void Renderer::Impl::pack_streams(const StreamDraw& draw,const StreamUpload& upload,std::byte* destination) const {
    const auto& layout=stream_layouts[draw.layout-1];
    for(unsigned i=0;i<draw.streams.size();++i) if(const auto size=upload.sizes[i]) {
        const auto& source=draw.streams[i];
        auto* output=destination+upload.offsets[i];
        const auto stride=upload.strides[i];
        const auto copied=std::min<std::size_t>(size,source.bytes.size());
        if(layout.repacked&(1U<<i)) {
            struct Copy {unsigned source,destination,size;};
            std::array<Copy,16> copies{};unsigned count=0;
            for(unsigned a=0;a<layout.attributes.size();++a) {
                const auto& attribute=layout.attributes[a];
                if(attribute.stream==i) copies[count++]={attribute.offset,layout.native_attributes[a].offset,attribute_format(attribute.format).bytes};
            }
            std::memset(output,0,size);
            for(std::size_t n=0;n<(source.stride?draw.count:1);++n)
                for(const auto& copy:std::span(copies).first(count))
                    std::memcpy(output+n*stride+copy.destination,source.bytes.data()+n*source.stride+copy.source,copy.size);
        } else if(!source.stride || stride==source.stride) {
            std::memcpy(output,source.bytes.data(),copied);if(copied<size) std::memset(output+copied,0,size-copied);
        } else for(std::size_t n=0;n<draw.count;++n) {
            const auto bytes=std::min<std::size_t>(source.stride,source.bytes.size()-n*source.stride);
            std::memcpy(output+n*stride,source.bytes.data()+n*source.stride,bytes);
            std::memset(output+n*stride+bytes,0,stride-bytes);
        }
    }
}
void Renderer::draw_streams(std::span<const StreamDraw> draws) {
    auto& r=*impl_;
    const VertexConstants* bound_constants=nullptr;
    while(!draws.empty()) {
        std::array<Impl::StreamUpload,32> uploads;
        std::size_t count=0;
        {
            PerfScope scope(r.profile,PerfPhase::buffer_upload);
            UINT total=0;
            for(;count<std::min(draws.size(),uploads.size());++count) {
                auto upload=r.plan_streams(draws[count]);
                const auto offset=(total+15)&~UINT(15);
                if(upload.size) {
                    if(upload.size>Impl::vertex_capacity-offset) break;
                    upload.offset=offset;
                    total=offset+upload.size;
                }
                uploads[count]=upload;
            }
            if(total) {
                UINT base=0;
                auto* destination=r.append_buffer(r.vertices.Get(),Impl::vertex_capacity,r.vertex_cursor,total,base);
                for(std::size_t i=0;i<count;++i) if(auto& upload=uploads[i];upload.size) {
                    r.pack_streams(draws[i],upload,destination+upload.offset);
                    for(auto& offset:upload.offsets) offset+=base+upload.offset;
                    r.stats.vertex_bytes+=upload.size;
                }
                r.context->Unmap(r.vertices.Get(),0);
            }
        }
        // Submit this chunk before a subsequent upload can discard its storage.
        for(std::size_t i=0;i<count;++i) r.draw_prepared_streams(draws[i],uploads[i],bound_constants);
        draws=draws.subspan(count);
    }
}
void Renderer::Impl::draw_prepared_streams(const StreamDraw& source,const StreamUpload& upload,const VertexConstants*& bound_constants) {
    const auto& state=source.state;
    const auto& view=source.view;
    if(!std::isfinite(view.depth_scale) || view.depth_scale<=0) throw std::runtime_error("Invalid vertex depth scale");
    const auto& prepared=stream_layouts[source.layout-1];
    unsigned mode=0;
    if(state.fog_enable) switch(state.fog_mode) {
    case 0x2601:case 0x0804:mode=0;break;case 0x0800:case 0x0802:mode=1;break;case 0x0801:case 0x0803:mode=2;break;
    default:throw std::runtime_error(std::format("Unsupported NV2A fog mode 0x{:X}",state.fog_mode));
    }
    const bool absolute=state.fog_mode==0x0802 || state.fog_mode==0x0803 || state.fog_mode==0x0804;
    ProgramUniforms uniforms{source.constants,{{2.0f/width,2.0f/height,1/view.depth_scale,view.subpixel_bias},
        {state.fog_parameters[0],state.fog_parameters[1],float(mode),state.fog_enable?(absolute?2.0f:1.0f):0.0f},view.texture_scales}};
    for(unsigned i=0;i<16;++i) uniforms.settings.input_formats[i]=prepared.attributes[i].stream==16?0:prepared.attributes[i].format;
    const PreparedStreams input{prepared,source,upload,bound_constants};
    draw(state,{},source.count,source.indices,&programs[prepared.program-1],&uniforms,&input);
}
void Renderer::Impl::draw(const RenderState& state,Bytes vertex_bytes,std::size_t vertex_count,
                          std::span<const std::uint32_t> input_indices,const Program* program,ProgramUniforms* uniforms,const PreparedStreams* streams) {
    auto& r = *this;
    if(state.color_mask) *r.targets.at(r.current_target).dirty=true;
    if (!r.recording) throw std::runtime_error("Begin a frame before drawing");
    if (!vertex_count || vertex_count > Impl::max_vertices || input_indices.size() > Impl::max_vertices)
        throw std::runtime_error("Draw exceeds bounded vertex/index buffers");
    // Queued draws carry the state objects selected during preparation;
    // standalone draws resolve them by their complete state.
    const auto pipeline=streams && streams->draw.pipeline.blend?streams->draw.pipeline:r.prepared(state);
    for (const auto index : input_indices) if (index >= vertex_count) throw std::runtime_error("Vertex index outside draw");
    const auto count = input_indices.empty() ? vertex_count : input_indices.size();
    D3D11_PRIMITIVE_TOPOLOGY topology;
    auto indices = input_indices;
    r.converted.clear();
    const auto index = [&](std::size_t n) { return input_indices.empty() ? static_cast<std::uint32_t>(n) : input_indices[n]; };
    const auto triangle = [&](std::size_t a, std::size_t b, std::size_t c) {
        r.converted.push_back(index(a)); r.converted.push_back(index(b)); r.converted.push_back(index(c));
    };
    // NV097 discards an unfinished primitive at End. D3D11 does this for
    // native lists/strips; the converted topologies must do the same.
    switch (state.primitive) {
    case 1: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 2: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case 3:
        if (count < 2) return;
        topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        for (std::size_t n = 0; n < count; ++n) r.converted.push_back(index(n));
        r.converted.push_back(index(0)); indices = r.converted; break;
    case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case 5: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case 6: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 7: case 10:
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (count < 3) return;
        for (std::size_t n = 2; n < count; ++n) triangle(0, n - 1, n);
        indices = r.converted; break;
    case 8:
        if (count < 4) return;
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (std::size_t n = 0; n + 3 < count; n += 4) { triangle(n, n + 1, n + 2); triangle(n, n + 2, n + 3); }
        indices = r.converted; break;
    case 9:
        if (count < 4) return;
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (std::size_t n = 2; n + 1 < count; n += 2) { triangle(n - 2, n - 1, n + 1); triangle(n - 2, n + 1, n); }
        indices = r.converted; break;
    default: throw std::runtime_error("Unsupported NV097 primitive: " + hex32(state.primitive));
    }
    if (indices.size() > Impl::max_indices) throw std::runtime_error("Converted indices exceed buffer limit");
    const auto upload = [&]<class T>(ID3D11Buffer* buffer,const T& data,T& previous,bool& uploaded) {
        // These private buffers retain their contents across draws and frames.
        // Compare bytes so NaNs and signed zero keep their exact shader values.
        if(uploaded && std::memcmp(&previous,&data,sizeof(T))==0) return false;
        PerfScope scope(r.profile,PerfPhase::buffer_upload);
        ++r.stats.buffer_maps;r.stats.total_vertex_bytes+=sizeof(T);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(r.context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Buffer map");
        std::memcpy(mapped.pData,&data,sizeof(T)); r.context->Unmap(buffer, 0);
        std::memcpy(&previous,&data,sizeof(T));uploaded=true;
        return true;
    };
    Constants constants{};
    for (unsigned i = 0; i < 8; ++i) {
        constants.factor0[i] = color(state.factor0[i]); constants.factor1[i] = color(state.factor1[i]);
        constants.combiners[i]={state.color_inputs[i],state.alpha_inputs[i],state.color_outputs[i],state.alpha_outputs[i]};
    }
    constants.final0 = color(state.final_factors[0]); constants.final1 = color(state.final_factors[1]);
    constants.fog = color(state.fog_color); constants.alpha[0] = float(state.alpha_reference & 255);
    constants.controls={state.combiner_control,state.final_inputs[0],state.final_inputs[1],state.alpha_test?state.alpha_function:0x207};
    std::array<ID3D11ShaderResourceView*, 12> views{};
    std::array<ID3D11SamplerState*, 4> samplers{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto mode=state.texture_modes[i];
        const bool sampled=mode && mode!=4 && mode!=5;
        if (sampled && (!state.textures[i] || state.textures[i] >= r.textures.size()))
            throw std::runtime_error("Enabled texture is not uploaded");
        if(sampled && state.textures[i]==r.current_target)
            throw std::runtime_error("Copy the active render target before sampling its contents");
        if(sampled && r.texture_dimensions[state.textures[i]]!=(mode==3?3U:mode==2?2U:1U))
            throw std::runtime_error("Texture resource dimension does not match its shader mode");
        if(sampled) {
            if(auto image=r.targets.find(state.textures[i]);image!=r.targets.end()) r.resolve(image->second);
            views[i+(mode==3?4:mode==2?8:0)] = r.textures[state.textures[i]].Get();
            r.texture_used[state.textures[i]]=r.frame_id;
        }
        constants.stages[i]={mode,state.texture_sources[i],state.texture_clip[i],std::uint32_t(state.alpha_kill[i])};
        constants.bump_matrices[i]=state.bump_matrices[i];
        if(((state.texture_filter[i]>>16)&63)==7)
            constants.gaussian_scale[i]=r.targets.contains(state.textures[i])?r.resolution_scale:1;
        // 1x keeps the original sampling, including its edge texels.
        if(sampled && mode==1 && r.resolution_scale>1 && ((state.bounded_samples>>i)&1)) {
            constants.sample_bounds[i]=state.sample_bounds[i];constants.bounded_samples[i]=1;++r.stats.bounded_samples;
        }
        samplers[i] = r.sampler_states.get(pipeline.samplers[i]);
    }
    UINT vertex_offset=0;
    if(streams) {
        // Missing inputs use the current attributes with a zero stride. Keep
        // them out of the streamed vertices so identical draws share one copy.
        if(streams->upload.uses_current) {
            PerfScope scope(r.profile,PerfPhase::buffer_upload);
            if(upload(r.current_vertices.Get(),streams->draw.current,r.last_current_vertex,r.current_vertex_uploaded))
                r.stats.vertex_bytes+=sizeof(ProgramVertex);
        }
    } else {
        PerfScope scope(r.profile,PerfPhase::buffer_upload);
        auto* destination=r.append_buffer(r.vertices.Get(),vertex_capacity,r.vertex_cursor,vertex_bytes.size(),vertex_offset);
        std::memcpy(destination,vertex_bytes.data(),vertex_bytes.size());r.context->Unmap(r.vertices.Get(),0);
        r.stats.vertex_bytes+=vertex_bytes.size();
    }
    upload(r.factor_buffer.Get(),constants,r.last_factors,r.factors_uploaded);
    if(uniforms) {
        auto& settings=uniforms->settings;
        if(program->fixed) {
            const auto& fixed=program->fixed_key;
            settings.fixed_flags={std::uint32_t(fixed.normalize),std::uint32_t(fixed.fog),fixed.fog_source,0};
            settings.fixed_texgen=fixed.texgen;
            for(unsigned i=0;i<4;++i) settings.fixed_matrices[i]=fixed.texture_matrix[i];
        }
        // Snapshot addresses stay valid within this batch. A different batch
        // or a standalone draw still compares the exact shader bytes.
        const auto& values=uniforms->constants;
        const bool constants_changed=!r.program_uniforms_uploaded || ((!streams || streams->bound_constants!=&values) &&
            std::memcmp(&values,&r.last_program_constants,sizeof(values)));
        if(constants_changed || std::memcmp(&settings,&r.last_program_settings,sizeof(settings))) {
            PerfScope scope(r.profile,PerfPhase::buffer_upload);
            ++r.stats.buffer_maps;r.stats.total_vertex_bytes+=program_uniform_bytes;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(r.context->Map(r.program_constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Buffer map");
            std::memcpy(mapped.pData,&values,sizeof(values));
            std::memcpy(static_cast<std::byte*>(mapped.pData)+sizeof(values),&settings,sizeof(settings));
            r.context->Unmap(r.program_constants.Get(),0);
            if(constants_changed) std::memcpy(&r.last_program_constants,&values,sizeof(values));
            std::memcpy(&r.last_program_settings,&settings,sizeof(settings));
            r.program_uniforms_uploaded=true;
        }
        if(streams) streams->bound_constants=&values;
    }
    const UINT stride = static_cast<UINT>(program?sizeof(ProgramVertex):sizeof(Vertex));
    auto vertex_buffer = r.vertices.Get();
    if(streams) {
        if(!streams->draw.streams.empty()) {
            std::array<ID3D11Buffer*,16> buffers{};buffers.fill(vertex_buffer);
            r.context->IASetVertexBuffers(0,static_cast<UINT>(streams->draw.streams.size()),buffers.data(),streams->upload.strides.data(),streams->upload.offsets.data());
        }
        if(streams->upload.uses_current && !r.current_vertex_bound) {
            auto current=r.current_vertices.Get();
            constexpr UINT zero=0;
            r.context->IASetVertexBuffers(16,1,&current,&zero,&zero);
            r.current_vertex_bound=true;
        }
    } else r.context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &vertex_offset);
    const auto simple=simple_combiner(state);
    DrawBindings bindings{streams?streams->layout.native.Get():program?program->layout.Get():r.layout.Get(),topology,
        program?program->vertex.Get():r.vs.Get(),
        simple<r.simple_combiners.size()?r.simple_combiners[simple].Get():r.combiners[state.combiner_control&255].Get(),
        r.blend_states.get(pipeline.blend),r.depth_states.get(pipeline.depth),r.raster_states.get(pipeline.raster),state.stencil_reference,views,samplers,{}};
    const auto& bound=r.bound_draw;
    if(!bound || bound->layout!=bindings.layout) r.context->IASetInputLayout(bindings.layout);
    if(!bound || bound->topology!=bindings.topology) r.context->IASetPrimitiveTopology(bindings.topology);
    if(!bound || bound->vertex!=bindings.vertex) r.context->VSSetShader(bindings.vertex,nullptr,0);
    if(!bound || bound->pixel!=bindings.pixel) r.context->PSSetShader(bindings.pixel,nullptr,0);
    if(!bound) {
        auto program_buffer=r.program_constants.Get(), factor_buffer=r.factor_buffer.Get();
        r.context->VSSetConstantBuffers(1,1,&program_buffer);
        r.context->PSSetConstantBuffers(0,1,&factor_buffer);
    }
    if(!bound || bound->views!=views) r.context->PSSetShaderResources(0,static_cast<UINT>(views.size()),views.data());
    if(!bound || bound->samplers!=samplers) r.context->PSSetSamplers(0,static_cast<UINT>(samplers.size()),samplers.data());
    if(!bound || bound->blend!=bindings.blend) r.context->OMSetBlendState(bindings.blend,nullptr,UINT_MAX);
    if(!bound || bound->depth!=bindings.depth || bound->stencil_reference!=bindings.stencil_reference)
        r.context->OMSetDepthStencilState(bindings.depth,bindings.stencil_reference);
    if(!bound || bound->raster!=bindings.raster) r.context->RSSetState(bindings.raster);
    if(!indices.empty()) {
        UINT index_offset=0;
        {
            PerfScope scope(r.profile,PerfPhase::buffer_upload);
            auto* destination=r.append_buffer(r.index_buffer.Get(),max_indices*sizeof(std::uint32_t),r.index_cursor,indices.size_bytes(),index_offset);
            std::memcpy(destination,indices.data(),indices.size_bytes());r.context->Unmap(r.index_buffer.Get(),0);
        }
        r.context->IASetIndexBuffer(r.index_buffer.Get(), DXGI_FORMAT_R32_UINT, index_offset);
    }
    const auto issue=[&] {
        if(indices.empty()) r.context->Draw(static_cast<UINT>(vertex_count),0);
        else r.context->DrawIndexed(static_cast<UINT>(indices.size()),0,0);
    };
    bool scissor_bound=bound && bound->raster==bindings.raster;
    if(bound) bindings.scissor=bound->scissor;
    const auto clipped=[&](const ScissorRect& rect) {
        if(rect[2]<rect[0] || rect[3]<rect[1]) throw std::runtime_error("Invalid scissor rectangle");
        if(rect[2]==rect[0] || rect[3]==rect[1]) return;
        std::array<LONG,4> scaled;
        for(unsigned i=0;i<4;++i) scaled[i]=rect[i]*LONG(r.resolution_scale);
        if(!scissor_bound || bindings.scissor!=scaled) {
            const D3D11_RECT native{scaled[0],scaled[1],scaled[2],scaled[3]};
            r.context->RSSetScissorRects(1,&native);bindings.scissor=scaled;scissor_bound=true;
        }
        issue();
    };
    if(!state.scissor_test) issue();
    else if(!state.window_count) clipped(state.scissor);
    else draw_clip_windows(state,clipped);
    r.bound_draw=bindings;
    ++r.stats.draws; r.stats.vertices += count;
}
void Renderer::end() {
    auto& r = *impl_;
    if (!r.recording) throw std::runtime_error("No frame to end");
    if(r.timing_index<r.timings.size()) {
        auto& timing=r.timings[r.timing_index];r.context->End(timing.finish.Get());r.context->End(timing.timer.Get());timing.pending=true;
    }
    r.recording = false; r.ended = true;
}
Image Renderer::readback() {
    auto& r = *impl_;
    if (!r.ended || r.recording) throw std::runtime_error("End a frame before reading back");
    auto& surface=r.targets.at(r.current_target);r.resolve(surface);
    if(!r.current_target && r.antialiasing) {
        r.unbind_textures();r.antialiasing->publish(surface.texture_view.Get());
        auto image=r.antialiasing->readback();r.select_target(0);return image;
    }
    Image image{r.width*r.resolution_scale,r.height*r.resolution_scale,
        std::vector<std::byte>(std::size_t(r.width)*r.height*r.resolution_scale*r.resolution_scale*4)};
    // CPU-readable storage belongs to explicit diagnostics, not every live
    // game surface. Shared color images are counted once in the resident pool.
    D3D11_TEXTURE2D_DESC description{};surface.resolved->GetDesc(&description);
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.MiscFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checked(r.device->CreateTexture2D(&description,nullptr,&staging),"Diagnostic readback target");
    r.context->CopyResource(staging.Get(),surface.resolved.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(r.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Frame readback");
    const bool rgb565=r.targets.at(r.current_target).color_format==SurfaceColor::rgb565;
    for(unsigned y=0;y<image.height;++y) {
        const auto* row=static_cast<const std::byte*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
        auto* pixels=image.rgba.data()+std::size_t(y)*image.width*4;
        if(!rgb565) std::memcpy(pixels,row,std::size_t(image.width)*4);
        else for(unsigned x=0;x<image.width;++x) {
            std::uint16_t value;std::memcpy(&value,row+x*2,2);
            pixels[x*4]=std::byte((((value>>11)&31)*255+15)/31);
            pixels[x*4+1]=std::byte((((value>>5)&63)*255+31)/63);
            pixels[x*4+2]=std::byte(((value&31)*255+15)/31);pixels[x*4+3]=std::byte{255};
        }
    }
    r.context->Unmap(staging.Get(), 0);
    r.collect_timings();
    return image;
}
Image Renderer::read_target(std::uint32_t target) {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("End submission before capturing a target");
    const auto previous=r.current_target;
    r.select_target(target);
    try {auto image=readback();r.select_target(previous);return image;}
    catch(...) {r.select_target(previous);throw;}
}
std::vector<std::byte> Renderer::read_depth(std::uint32_t target) {
    auto& r=*impl_;
    if(r.recording) throw std::runtime_error("End submission before capturing depth");
    const auto& surface=r.targets.at(target);
    if(!surface.depth) return {};
    auto depth=surface.depth;
    if(r.samples>1) {
        auto single=r.make_target(surface.width,surface.height,surface.color_format,surface.depth_format,nullptr,1);
        r.unbind_textures();r.context->OMSetRenderTargets(0,nullptr,nullptr);
        r.context->ClearDepthStencilView(single.depth_view.Get(),D3D11_CLEAR_DEPTH|(surface.depth_format==SurfaceDepth::z16?0:D3D11_CLEAR_STENCIL),1,0);
        r.multisample_copy->grow(surface.multisample_view.Get(),surface.depth_texture.Get(),surface.stencil_texture.Get(),single.color_view.Get(),single.depth_view.Get(),
            surface.width*r.resolution_scale,surface.height*r.resolution_scale);
        depth=single.depth;r.select_target(r.current_target);
    }
    D3D11_TEXTURE2D_DESC description{};depth->GetDesc(&description);
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.MiscFlags=0;
    description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checked(r.device->CreateTexture2D(&description,nullptr,&staging),"Depth capture storage");
    const auto pitch=std::size_t(surface.width)*r.resolution_scale*(surface.depth_format==SurfaceDepth::z16?2:4);
    std::vector<std::byte> bytes(pitch*surface.height*r.resolution_scale);
    r.context->CopyResource(staging.Get(),depth.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(r.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Depth capture");
    for(unsigned y=0;y<surface.height*r.resolution_scale;++y) std::memcpy(bytes.data()+y*pitch,
        static_cast<const std::byte*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,pitch);
    r.context->Unmap(staging.Get(),0);return bytes;
}
Image Renderer::read_display() {
    auto& r=*impl_;
    if(!r.displaying || !r.swapchain) throw std::runtime_error("Display a frame before reading the window image");
    ComPtr<ID3D11Texture2D> buffer,staging;
    checked(r.swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Window image");
    D3D11_TEXTURE2D_DESC description{};buffer->GetDesc(&description);
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.MiscFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    checked(r.device->CreateTexture2D(&description,nullptr,&staging),"Window readback storage");
    r.context->CopyResource(staging.Get(),buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};checked(r.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Window readback");
    Image image{description.Width,description.Height,std::vector<std::byte>(std::size_t(description.Width)*description.Height*4)};
    for(unsigned y=0;y<image.height;++y) std::memcpy(image.rgba.data()+std::size_t(y)*image.width*4,
        static_cast<const std::byte*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,std::size_t(image.width)*4);
    r.context->Unmap(staging.Get(),0);return image;
}
RenderStats Renderer::stats() const {
    auto result=impl_->stats;result.texture_bytes=impl_->texture_bytes+impl_->movie_bytes();result.target_bytes=impl_->target_bytes+impl_->presentation_bytes();
    result.antialiasing=impl_->antialiasing_quality;result.anisotropy=impl_->anisotropy;
    result.unlimited_render_distance=impl_->unlimited_render_distance;result.upscaled_movies=impl_->upscaled_movies;
    result.upscaled_hud=impl_->upscaled_hud;result.hud_marked=impl_->interface_textures.size();result.hud_cached=impl_->interface_cache.size();
    result.hud_pending=impl_->interface_upscaler?impl_->interface_upscaler->queued():0;
    result.hud_bytes=impl_->interface_bytes+(impl_->interface_upscaler?impl_->interface_upscaler->bytes():0);
    result.hud_gpu_ms=impl_->interface_upscaler?impl_->interface_upscaler->gpu_ms():0;
    if(impl_->interface_upscaler) {
        const auto step=impl_->interface_upscaler->report();
        result.hud_step_budget=step.budget;result.hud_step_work=step.work;result.hud_step_units=step.units;result.hud_rate=step.rate;
        result.hud_waiting=step.waiting;result.hud_network=step.network;
    }
    result.texture_slots=static_cast<std::uint32_t>(impl_->textures.size()-1);result.resolution_scale=impl_->resolution_scale;return result;
}
std::string Renderer::adapter() const { return impl_->adapter; }
std::vector<std::string> Renderer::errors() const {
    std::vector<std::string> messages;
    const auto& queue = impl_->info;
    if (!queue) return messages;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        checked(queue->GetMessage(i, nullptr, &size), "Debug message size");
        std::vector<std::byte> storage(size);
        const auto message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        checked(queue->GetMessage(i, message, &size), "Debug message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            messages.emplace_back(message->pDescription, message->DescriptionByteLength - 1);
    }
    return messages;
}
std::string check_image_copy(bool warp,bool debug,unsigned antialiasing) {
    Renderer renderer(8,8,warp,debug);
    renderer.enhancements(antialiasing,16);
    std::vector<std::pair<std::string,bool>> cases;
    const auto copy=[](Image& destination,const Image& source,unsigned sx,unsigned sy,unsigned dx,unsigned dy,unsigned w,unsigned h) {
        for(unsigned y=0;y<h;++y) std::memcpy(destination.rgba.data()+(std::size_t(dy+y)*destination.width+dx)*4,
            source.rgba.data()+(std::size_t(sy+y)*source.width+sx)*4,std::size_t(w)*4);
    };
    const auto compare=[&](std::string name,const Image& expected) {
        const auto actual=renderer.readback();
        cases.emplace_back(std::move(name),actual.width==expected.width && actual.height==expected.height && actual.rgba==expected.rgba);
    };
    const auto source=renderer.create_target(8,8);
    const auto before_copy=renderer.stats().target_bytes;
    const auto copy_image=renderer.create_target(8,8,SurfaceColor::rgba8,SurfaceDepth::none);
    const auto samples=antialiasing>=4?1U<<(antialiasing-3):1U;
    cases.emplace_back("copy destination uses only color storage",renderer.read_depth(copy_image).empty() &&
        renderer.stats().target_bytes-before_copy==8*8*4*(samples+(samples>1?1:0)));
    renderer.set_target(source);renderer.clear(0xFF204060);const auto original=renderer.readback();
    renderer.set_target(copy_image);renderer.clear(0xFFCC8844);auto expected=renderer.readback();
    renderer.copy_region(copy_image,source,1,2,3,1,3,4);copy(expected,original,1,2,3,1,3,4);
    compare("partial rectangle preserves surrounding pixels",expected);
    const auto before_overlap=expected;
    renderer.copy_region(copy_image,copy_image,1,0,2,2,5,5);copy(expected,before_overlap,1,0,2,2,5,5);
    compare("overlapping copy retains source pixels",expected);
    renderer.copy_region(copy_image,source,0,0,0,0,8,8);compare("whole image copy",original);
    const auto destination=renderer.create_target(8,8,SurfaceColor::rgba8,SurfaceDepth::z24s8,copy_image);
    const auto alias=renderer.create_target(8,8,SurfaceColor::rgba8,SurfaceDepth::z16,destination);
    renderer.set_target(destination);renderer.clear_buffers(3,0,0.625f,0xA5);const auto original_depth=renderer.read_depth(destination);
    renderer.set_target(alias);renderer.clear(0xFF5A3C78,0.375f);const auto shared=renderer.readback();const auto alias_depth=renderer.read_depth(alias);
    renderer.set_target(destination);compare("color image shared across depth views",shared);
    renderer.grow_target(copy_image,10,9);
    Image grown{10,9,std::vector<std::byte>(10*9*4)};copy(grown,shared,0,0,0,0,8,8);
    compare("shared image growth preserves pixels",grown);
    renderer.set_target(alias);compare("alias follows image growth",grown);
    const auto preserved=[&](std::uint32_t target,const std::vector<std::byte>& expected,unsigned bytes) {
        const auto actual=renderer.read_depth(target);
        for(unsigned y=0;y<8;++y) if(std::memcmp(actual.data()+y*10*bytes,expected.data()+y*8*bytes,8*bytes)) return false;
        return true;
    };
    cases.emplace_back("D24 depth and stencil survive growth",preserved(destination,original_depth,4));
    cases.emplace_back("D16 alias depth survives growth",preserved(alias,alias_depth,2));
    const auto rgb_source=renderer.create_target(3,3,SurfaceColor::rgb565,SurfaceDepth::z16);
    const auto rgb_destination=renderer.create_target(3,3,SurfaceColor::rgb565,SurfaceDepth::z16);
    renderer.set_target(rgb_source);renderer.clear(0xFF00FF00);const auto green=renderer.readback();
    renderer.set_target(rgb_destination);renderer.clear(0xFF000000);auto rgb_expected=renderer.readback();
    renderer.copy_region(rgb_destination,rgb_source,1,0,0,1,2,2);copy(rgb_expected,green,1,0,0,1,2,2);
    compare("RGB565 rectangle copy",rgb_expected);
    bool rejected=false;
    try {renderer.copy_region(destination,source,UINT32_MAX,0,0,0,1,1);} catch(const std::runtime_error&) {rejected=true;}
    cases.emplace_back("invalid rectangle rejected",rejected);
    const std::array<std::byte,4> texel{std::byte{32},std::byte{64},std::byte{96},std::byte{255}};
    const auto ordinary=renderer.upload({1,1,TextureFormat::argb8_linear,texel});
    // Exercise live changes with existing shared colour, D24S8, D16 and RGB565 resources.
    for(const auto options:std::array<std::array<unsigned,3>,8>{{{2,4,2},{3,5,4},{1,6,8},{4,0,16},{1,1,0},{1,2,1},{2,3,4},{1,0,0}}}) {
        if(options[1]>=4 && !renderer.supports_msaa(1U<<(options[1]-3))) continue;
        const auto scale=renderer.stats().resolution_scale;
        renderer.set_target(destination);const auto before=renderer.readback();
        const auto depth=renderer.read_depth(destination),alias_before=renderer.read_depth(alias);
        renderer.image_quality(options[0],options[1],options[2]);
        renderer.set_target(destination);const auto after=renderer.readback();
        bool retained=after.width==10*options[0] && after.height==9*options[0];
        for(unsigned y=0;y<after.height;++y) for(unsigned x=0;x<after.width;++x) {
            const auto sx=(2*x+1)*scale/(2*options[0]),sy=(2*y+1)*scale/(2*options[0]);
            retained&=std::memcmp(after.rgba.data()+(std::size_t(y)*after.width+x)*4,before.rgba.data()+(std::size_t(sy)*before.width+sx)*4,4)==0;
        }
        renderer.set_target(alias);retained&=renderer.readback().rgba==after.rgba;
        const auto retained_depth=[&](std::uint32_t id,const std::vector<std::byte>& before,unsigned size) {
            const auto actual=renderer.read_depth(id);
            for(unsigned y=0;y<9*options[0];++y) for(unsigned x=0;x<10*options[0];++x) {
                const auto sx=(2*x+1)*scale/(2*options[0]),sy=(2*y+1)*scale/(2*options[0]);
                std::uint32_t a=0,b=0;
                std::memcpy(&a,actual.data()+(std::size_t(y)*10*options[0]+x)*size,size);
                std::memcpy(&b,before.data()+(std::size_t(sy)*10*scale+sx)*size,size);
                if(size==4 && (a>>24)!=(b>>24)) return false;
                const auto mask=size==4?0xFFFFFFU:0xFFFFU;
                if(std::abs(std::int64_t(a&mask)-std::int64_t(b&mask))>1) return false;
            }
            return true;
        };
        retained&=retained_depth(destination,depth,4) && retained_depth(alias,alias_before,2);
        renderer.set_target(rgb_source);const auto rgb=renderer.readback();
        for(std::size_t i=0;i<rgb.rgba.size();i+=4) retained&=std::memcmp(rgb.rgba.data()+i,green.rgba.data(),4)==0;
        const auto stats=renderer.stats();
        retained&=stats.resolution_scale==options[0] && stats.antialiasing==options[1] && stats.anisotropy==options[2];
        cases.emplace_back(std::format("live {}x AA {} AF {} retains shared colour/depth/stencil and RGB565",options[0],options[1],options[2]),retained);
    }
    renderer.release_texture(ordinary);
    const auto large=renderer.create_target(1536,1536);
    const auto before_rejection=renderer.stats();rejected=false;
    try {renderer.image_quality(4,6,16);} catch(const std::runtime_error&) {rejected=true;}
    const auto after_rejection=renderer.stats();
    cases.emplace_back("rejected live change preserves active settings and resources",rejected &&
        before_rejection.resolution_scale==after_rejection.resolution_scale && before_rejection.antialiasing==after_rejection.antialiasing &&
        before_rejection.anisotropy==after_rejection.anisotropy && before_rejection.target_bytes==after_rejection.target_bytes);
    renderer.release_target(large);
    // A long session combines a few dozen D3D11 state objects into thousands
    // of distinct draw states (issue 3 exhausted a 1024-entry cache after 31
    // minutes). Neither that nor a live quality change may invalidate a draw.
    {
        const auto texture=renderer.upload({1,1,TextureFormat::argb8_linear,texel});
        RenderState plain;plain.texture_modes[0]=1;plain.textures[0]=texture;plain.final_inputs={0x00000008,0x00001800};
        std::array<Vertex,6> quad;
        constexpr Float4 positions[]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
        for(unsigned i=0;i<quad.size();++i) quad[i].position=positions[i];
        const auto draw=[&] {renderer.set_target(0);renderer.begin();renderer.draw(plain,quad);renderer.end();return renderer.readback();};
        const auto plain_states=renderer.prepare(plain);
        const auto reference=draw();
        auto state=plain;
        std::size_t distinct=0;bool prepared=true;
        for(const std::uint32_t source:{0x0U,0x1U,0x302U,0x300U}) for(const std::uint32_t destination:{0x0U,0x1U,0x303U,0x301U})
        for(std::uint32_t depth=0x200;depth<=0x207;++depth) for(std::uint32_t u=1;u<=3;++u) for(std::uint32_t v=1;v<=3;++v)
        for(std::uint32_t w=1;w<=3;++w) for(const bool cull:{false,true}) {
            state.blend=true;state.blend_source=source;state.blend_destination=destination;
            state.depth_test=true;state.depth_function=depth;state.cull=cull;
            state.texture_address[0]=u|(v<<8)|0x10000;state.texture_address[1]=w|(w<<8)|0x10000;
            try {renderer.prepare(state);++distinct;} catch(const std::runtime_error&) {prepared=false;}
        }
        cases.emplace_back(std::format("{} distinct draw states share bounded state caches",distinct),prepared && distinct>4096);
        renderer.enhancements(0,renderer.stats().anisotropy==4?8:4);
        cases.emplace_back("prepared draw states survive forgotten combinations and a live quality change",
            renderer.prepare(plain)==plain_states && draw().rgba==reference.rgba);
        renderer.release_texture(texture);
    }
    const auto errors=renderer.errors();
    cases.emplace_back("D3D11 validation clean",errors.empty());
    const auto passed=std::ranges::all_of(cases,[](const auto& test){return test.second;});
    std::string report="{\"format\":\"smbd-image-copy-v1\",\"adapter\":"+json(renderer.adapter())+
        ",\"antialiasing\":"+std::to_string(antialiasing)+",\"msaa_supported\":[";
    bool first=true;for(unsigned samples:{2U,4U,8U}) if(renderer.supports_msaa(samples)) {if(!first) report+=',';report+=std::to_string(samples);first=false;}
    report+="],\"passed\":"+std::string(passed?"true":"false")+",\"cases\":[";
    for(std::size_t i=0;i<cases.size();++i) {
        if(i) report+=',';
        report+="{\"name\":"+json(cases[i].first)+",\"passed\":"+(cases[i].second?"true":"false")+'}';
    }
    report+="],\"errors\":[";
    for(std::size_t i=0;i<errors.size();++i) {if(i) report+=',';report+=json(errors[i]);}
    return report+"]}";
}
std::string check_movie_upscaling(const std::filesystem::path& directory,const std::filesystem::path& frame,bool warp,bool debug) {
    if(std::filesystem::exists(directory)) throw std::runtime_error("Use a fresh movie upscaling diagnostic directory");
    constexpr std::uint32_t width=640,height=480;
    // Flat colour, an aliased black/white diagonal and smooth gradients, in
    // the movie's linear ARGB byte order.
    std::vector<std::byte> synthetic(std::size_t(width)*height*4),captured;
    for(std::uint32_t y=0;y<height;++y) for(std::uint32_t x=0;x<width;++x) {
        std::array<unsigned,3> bgr{};
        if(x<200) bgr={0xC0,0x80,0x40};
        else if(x<440) bgr.fill((x-200)*2>y?255U:0U);
        else bgr={128U,y*255/(height-1),(x-440)*255/199};
        auto* p=synthetic.data()+(std::size_t(y)*width+x)*4;
        p[0]=std::byte(bgr[0]);p[1]=std::byte(bgr[1]);p[2]=std::byte(bgr[2]);p[3]=std::byte{255};
    }
    // Sofdec-style 4:2:0 colour: an antialiased red/green diagonal of equal
    // brightness, sampled per 2x2 block and repeated over each block.
    std::vector<std::byte> colour_edge(synthetic.size());
    const auto edge_x=[](double y) {return 160+0.37*y;};
    for(std::uint32_t y=0;y<height;++y) for(std::uint32_t x=0;x<width;++x) {
        const double cx=(x&~1U)+0.5,cy=(y&~1U)+0.5,red=std::clamp(0.5-(cx-edge_x(cy))/2,0.0,1.0);
        auto* p=colour_edge.data()+(std::size_t(y)*width+x)*4;
        p[0]=std::byte{40};p[1]=std::byte(int(std::lround(121-81*red)));p[2]=std::byte(int(std::lround(40+160*red)));p[3]=std::byte{255};
    }
    // RMS distance of the colour boundary from a straight line, in source pixels.
    const auto edge_deviation=[&](const Image& image,unsigned scale) {
        std::vector<std::pair<double,double>> points;
        for(std::uint32_t y=16*scale;y<(height-16)*scale;++y) {
            const double sy=(y+0.5)/scale-0.5,expected=edge_x(sy);
            const auto first=std::uint32_t(std::max(1.0,(expected-12)*scale)),last=std::uint32_t((expected+12)*scale);
            const auto redness=[&](std::uint32_t x) {return (int(image.rgba[(std::size_t(y)*image.width+x)*4])-40)/160.0;};
            for(auto x=first;x<last;++x) if(redness(x)<0.5 && redness(x-1)>=0.5) {
                const double a=redness(x-1),b=redness(x),crossing=x-1+(a-0.5)/(a-b);
                points.emplace_back(sy,(crossing+0.5)/scale-0.5);break;
            }
        }
        if(points.size()<16) return 99.0;
        double my=0,mx=0;for(const auto& [py,px]:points) {my+=py;mx+=px;}my/=points.size();mx/=points.size();
        double sxy=0,syy=0;for(const auto& [py,px]:points) {sxy+=(py-my)*(px-mx);syy+=(py-my)*(py-my);}
        const double slope=sxy/syy;double residual=0;
        for(const auto& [py,px]:points) {const double d=px-(mx+slope*(py-my));residual+=d*d;}
        return std::sqrt(residual/points.size());
    };
    if(!frame.empty()) {
        File input(frame);
        if(input.size()!=synthetic.size()) throw std::runtime_error("A captured movie frame must be 640x480 linear ARGB");
        captured=input.read(0,synthetic.size());
    }
    std::filesystem::create_directories(directory);
    std::vector<std::pair<std::string,bool>> cases;
    std::vector<std::string> errors;
    std::string adapter,scales;
    const auto luma=[](const Image& image,std::uint32_t x,std::uint32_t y) {
        const auto* p=image.rgba.data()+(std::size_t(y)*image.width+x)*4;
        return 0.299*int(p[0])+0.587*int(p[1])+0.114*int(p[2]);
    };
    for(unsigned scale=1;scale<=4;++scale) {
        Renderer renderer(width,height,warp,debug,scale);adapter=renderer.adapter();
        RenderState state;state.texture_modes[0]=1;state.texture_filter[0]=0x02020000;state.final_inputs={0x00000008,0x00001800};
        std::array<Vertex,6> quad;
        constexpr Float4 positions[]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
        for(unsigned i=0;i<quad.size();++i) {quad[i].position=positions[i];quad[i].uv[0]={(positions[i][0]+1)/2,(1-positions[i][1])/2,0,1};}
        const auto upload=[&](Bytes pixels,bool movie) {return renderer.upload({width,height,TextureFormat::argb8_linear,pixels,1,width*4,false,1,movie});};
        const auto render=[&](Bytes pixels,bool upscaled) {
            renderer.enhancements(0,0,false,upscaled);
            const auto id=upload(pixels,true);state.textures[0]=id;
            renderer.set_target(0);renderer.prepare(state);renderer.begin();renderer.draw(state,quad);renderer.end();
            auto image=renderer.readback();renderer.release_texture(id);return image;
        };
        const auto before=renderer.stats().movie_upscales;
        const auto upscaled=render(synthetic,true);
        const auto upscales=renderer.stats().movie_upscales-before;
        const auto bilinear=render(synthetic,false);
        const bool released=renderer.stats().texture_bytes==0;
        if(scale==1) {
            cases.emplace_back("1x leaves movie frames unchanged",!upscales && upscaled.rgba==bilinear.rgba);
            for(auto& message:renderer.errors()) errors.push_back(std::move(message));
            continue;
        }
        bool flat=true;double smooth=0,energy_upscaled=0,energy_bilinear=0;unsigned smooth_max=0;std::size_t smooth_count=0;
        for(std::uint32_t y=0;y<upscaled.height;++y) for(std::uint32_t x=0;x<upscaled.width;++x) {
            const auto sx=x/scale,sy=y/scale;
            if(sy<8 || sy>=height-8) continue;
            const auto* a=upscaled.rgba.data()+(std::size_t(y)*upscaled.width+x)*4;
            const auto* b=bilinear.rgba.data()+(std::size_t(y)*bilinear.width+x)*4;
            if(sx>=8 && sx<192) {
                constexpr int expected[]{0x40,0x80,0xC0};
                for(unsigned c=0;c<3;++c) flat&=std::abs(int(a[c])-expected[c])<=1;
            } else if(sx>=448 && sx<632) for(unsigned c=0;c<3;++c) {
                const auto delta=unsigned(std::abs(int(a[c])-int(b[c])));smooth+=delta;smooth_max=std::max(smooth_max,delta);++smooth_count;
            } else if(sx>=216 && sx<424) {
                // Squared gradients grow as the same edge contrast becomes narrower.
                const auto du=luma(upscaled,x+1,y)-luma(upscaled,x,y),db=luma(bilinear,x+1,y)-luma(bilinear,x,y);
                energy_upscaled+=du*du;energy_bilinear+=db*db;
            }
        }
        smooth/=std::max<std::size_t>(smooth_count,1);
        const auto ratio=energy_upscaled/std::max(energy_bilinear,1.0);
        cases.emplace_back(std::format("{}x enlarges each decoded frame once",scale),upscales==1 &&
            upscaled.width==width*scale && upscaled.height==height*scale);
        cases.emplace_back(std::format("{}x preserves flat colour",scale),flat);
        cases.emplace_back(std::format("{}x keeps smooth gradients",scale),smooth<=1 && smooth_max<=6);
        cases.emplace_back(std::format("{}x sharpens edges",scale),ratio>1.05);
        cases.emplace_back(std::format("{}x releases filtering storage when disabled",scale),released);
        const auto colour_upscaled=render(colour_edge,true),colour_bilinear=render(colour_edge,false);
        const auto colour_deviation=edge_deviation(colour_upscaled,scale),stepped_deviation=edge_deviation(colour_bilinear,scale);
        cases.emplace_back(std::format("{}x smooths 2x2 colour blocks",scale),colour_deviation<0.3 && colour_deviation<0.5*stepped_deviation);
        save_png(upscaled,directory/std::format("synthetic-{}x-upscaled.png",scale));
        save_png(bilinear,directory/std::format("synthetic-{}x-bilinear.png",scale));
        save_png(colour_upscaled,directory/std::format("colour-{}x-upscaled.png",scale));
        save_png(colour_bilinear,directory/std::format("colour-{}x-bilinear.png",scale));
        if(!captured.empty()) {
            save_png(render(captured,true),directory/std::format("captured-{}x-upscaled.png",scale));
            save_png(render(captured,false),directory/std::format("captured-{}x-bilinear.png",scale));
        }
        // Throughput per decoded frame, including the upload both paths share.
        renderer.enhancements(0,0,false,true);
        constexpr unsigned frames=120;
        const auto time=[&](bool movie) {
            renderer.readback();const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<frames;++i) renderer.release_texture(upload(synthetic,movie));
            renderer.readback();
            return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/frames;
        };
        const auto ordinary_ms=time(false),movie_ms=time(true);
        if(!scales.empty()) scales+=',';
        scales+=std::format("{{\"scale\":{},\"output\":[{},{}],\"flat_colour\":{},\"smooth_mean_difference\":{:.4f},\"smooth_max_difference\":{},\"edge_energy_ratio\":{:.4f},\"colour_edge_rms_px\":{:.4f},\"stretched_colour_edge_rms_px\":{:.4f},\"ordinary_upload_ms\":{:.4f},\"upscaled_upload_ms\":{:.4f},\"upscale_overhead_ms\":{:.4f},\"frames\":{}}}",
            scale,upscaled.width,upscaled.height,flat,smooth,smooth_max,ratio,colour_deviation,stepped_deviation,ordinary_ms,movie_ms,movie_ms-ordinary_ms,frames);
        for(auto& message:renderer.errors()) errors.push_back(std::move(message));
    }
    cases.emplace_back("D3D11 validation clean",errors.empty());
    const auto passed=std::ranges::all_of(cases,[](const auto& test){return test.second;});
    std::string report=std::format("{{\"format\":\"smbd-movie-upscaling-v1\",\"adapter\":{},\"passed\":{},\"captured_frame\":{},\"scales\":[{}],\"cases\":[",
        json(adapter),passed,frame.empty()?std::string("null"):json(utf8(frame.wstring())),scales);
    for(std::size_t i=0;i<cases.size();++i) {
        if(i) report+=',';
        report+="{\"name\":"+json(cases[i].first)+",\"passed\":"+(cases[i].second?"true":"false")+'}';
    }
    report+="],\"errors\":[";
    for(std::size_t i=0;i<errors.size();++i) {if(i) report+=',';report+=json(errors[i]);}
    report+="]}";
    write_text(directory/"report.json",report+'\n',false);
    return report;
}
std::string check_hud_upscaling(const std::filesystem::path& directory,std::span<const std::filesystem::path> inputs,bool warp,bool debug) {
    if(std::filesystem::exists(directory)) throw std::runtime_error("Use a fresh HUD upscaling diagnostic directory");
    struct Texture {std::string name;unsigned width,height;std::vector<float> rgba;};
    const auto make=[](std::string name,unsigned width,unsigned height,auto&& pixel) {
        Texture texture{std::move(name),width,height,std::vector<float>(std::size_t(width)*height*4)};
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const std::array<float,4> value=pixel(float(x),float(y));
            std::copy(value.begin(),value.end(),texture.rgba.begin()+(std::size_t(y)*width+x)*4);
        }
        // Store exactly what an 8-bit texture holds.
        for(auto& v:texture.rgba) v=std::round(std::clamp(v,0.0f,1.0f)*255)/255;
        return texture;
    };
    // Outlined artwork on transparency (an amber disc with a dark rim), the
    // same idea opaque (stripes and a square), a soft glow and a sliver.
    const auto disc=[](float x,float y) {
        const float d=std::hypot(x-9.5f,y-5.5f);
        if(d>5.6f) return std::array<float,4>{0.2f,0.9f,0.3f,0};   // Arbitrary hidden colour.
        if(d>4.4f) return std::array<float,4>{0.25f,0.1f,0.02f,1};
        return std::array<float,4>{1,0.75f,0.1f,1};
    };
    std::vector<Texture> textures{
        make("outlined",20,12,disc),
        make("opaque",16,12,[](float x,float y) {
            const bool box=x>=4 && x<11 && y>=3 && y<9;
            return box?std::array<float,4>{0.95f,0.95f,0.95f,1}:int(x+y)%4<2?std::array<float,4>{0.1f,0.2f,0.7f,1}:std::array<float,4>{0.9f,0.3f,0.1f,1};
        }),
        make("glow",16,16,[](float x,float y) {
            const float d=std::hypot(x-7.5f,y-7.5f);return std::array<float,4>{1,1,1,std::clamp(1-d/8,0.0f,1.0f)};
        }),
        make("sliver",4,16,[](float x,float y) {return std::array<float,4>{x/3,y/15,0.5f,1};})};
    for(const auto& path:inputs) {
        const auto image=load_image(path);
        Texture texture{path_text(path.stem()),image.width,image.height,std::vector<float>(image.rgba.size())};
        for(std::size_t i=0;i<image.rgba.size();++i) texture.rgba[i]=std::to_integer<unsigned>(image.rgba[i])/255.0f;
        textures.push_back(std::move(texture));
    }
    std::filesystem::create_directories(directory);
    const auto weights=upscaler_weights();
    std::vector<float> model(weights.size()/4);std::memcpy(model.data(),weights.data(),weights.size());
    std::vector<std::pair<std::string,bool>> cases;
    std::vector<std::string> errors;
    std::string adapter,results;
    Renderer renderer(64,64,warp,debug,4);adapter=renderer.adapter();
    auto* device=renderer.native_device();auto* context=renderer.native_context();
    const auto source=[&](const Texture& texture) {
        std::vector<std::uint8_t> bytes(texture.rgba.size());
        for(std::size_t i=0;i<bytes.size();++i) bytes[i]=static_cast<std::uint8_t>(std::lround(texture.rgba[i]*255));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=texture.width;desc.Height=texture.height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{bytes.data(),texture.width*4,0};
        ComPtr<ID3D11Texture2D> resource;checked(device->CreateTexture2D(&desc,&data,&resource),"HUD check texture");
        ComPtr<ID3D11ShaderResourceView> view;checked(device->CreateShaderResourceView(resource.Get(),nullptr,&view),"HUD check view");
        return view;
    };
    const auto read=[&](ID3D11ShaderResourceView* view) {
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;checked(resource.As(&texture),"HUD check result");
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        desc.MipLevels=desc.ArraySize=1;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;checked(device->CreateTexture2D(&desc,nullptr,&staging),"HUD check readback");
        context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),0,nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"HUD check readback");
        Image image{desc.Width,desc.Height,std::vector<std::byte>(std::size_t(desc.Width)*desc.Height*4)};
        for(unsigned y=0;y<desc.Height;++y)
            std::memcpy(image.rgba.data()+std::size_t(y)*desc.Width*4,static_cast<const std::byte*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,desc.Width*4);
        context->Unmap(staging.Get(),0);return image;
    };
    const auto upscale=[&](const Texture& texture,unsigned factor,unsigned tile,double* milliseconds=nullptr) {
        std::uint64_t compilations=0;
        InterfaceUpscaler upscaler(device,context,compilations,tile);
        const auto view=source(texture);
        upscaler.request(1,view,texture.width,texture.height,factor);
        const auto start=std::chrono::steady_clock::now();
        auto finished=upscaler.step(true);
        if(finished.size()!=1) throw std::runtime_error("HUD upscaler did not finish its request");
        auto image=read(finished[0].result.view.Get());
        if(milliseconds) *milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        return std::pair{image,finished[0].result.network};
    };
    const auto compare=[](const Image& image,const std::vector<float>& reference) {
        unsigned maximum=0;double total=0;
        for(std::size_t i=0;i<reference.size();++i) {
            const auto d=unsigned(std::abs(int(std::to_integer<unsigned>(image.rgba[i]))-int(std::lround(reference[i]*255))));
            maximum=std::max(maximum,d);total+=d;
        }
        return std::pair{maximum,total/std::max<std::size_t>(reference.size(),1)};
    };
    // GPU path against the float CPU reference of the identical algorithm. The
    // GPU keeps 64-channel activations in half precision between layers.
    for(std::size_t index=0;index<textures.size();++index) {
        const auto& texture=textures[index];
        const bool synthetic=index<4;
        for(unsigned factor:{2U,3U,4U}) {
            if(!synthetic && factor!=4) continue;
            InterfaceDecision decision;
            const auto reference=synthetic?reference_interface_upscale(texture.rgba,texture.width,texture.height,factor,model,&decision):std::vector<float>{};
            if(!synthetic) decision=std::min(texture.width,texture.height)<interface_network_minimum?InterfaceDecision{}:classify_interface(texture.rgba,texture.width,texture.height);
            double milliseconds=0;
            const auto [image,network]=upscale(texture,factor,interface_tile,&milliseconds);
            const auto [maximum,mean]=synthetic?compare(image,reference):std::pair{0U,0.0};
            if(synthetic) {
                cases.emplace_back(std::format("{} {}x matches the reference",texture.name,factor),maximum<=4 && mean<=0.75);
                cases.emplace_back(std::format("{} {}x chooses the reference path",texture.name,factor),network==decision.network);
            }
            if(factor==4) {
                Image original{texture.width,texture.height,std::vector<std::byte>(texture.rgba.size())};
                for(std::size_t i=0;i<texture.rgba.size();++i) original.rgba[i]=std::byte(std::lround(texture.rgba[i]*255));
                // Nearest original, then the result, over a dark backdrop; wide
                // textures stack vertically to respect the PNG writer's bound.
                const bool stacked=image.width*2+4>4096;
                const unsigned sw=stacked?image.width:image.width*2+4,sh=stacked?image.height*2+4:image.height;
                Image sheet{sw,sh,std::vector<std::byte>(std::size_t(sw)*sh*4)};
                for(unsigned y=0;y<sh;++y) for(unsigned x=0;x<sw;++x) {
                    const unsigned along=stacked?y:x,size=stacked?image.height:image.width;
                    const unsigned ox=stacked?x:along-size-4,oy=stacked?along-size-4:y;
                    const std::byte* p=along<size?original.rgba.data()+(std::size_t(y/4)*texture.width+x/4)*4:
                        along>=size+4?image.rgba.data()+(std::size_t(oy)*image.width+ox)*4:nullptr;
                    auto* out=sheet.rgba.data()+(std::size_t(y)*sw+x)*4;
                    const float a=p?std::to_integer<unsigned>(p[3])/255.0f:0;const int backdrop[3]{46,51,82};
                    for(unsigned c=0;c<3;++c) out[c]=std::byte(int(std::lround((p?std::to_integer<unsigned>(p[c]):0)*a+backdrop[c]*(1-a))));
                    out[3]=std::byte{255};
                }
                save_png(sheet,directory/(texture.name+"-4x.png"));
            }
            if(!results.empty()) results+=',';
            results+=std::format("{{\"texture\":{},\"size\":[{},{}],\"factor\":{},\"network\":{},\"edge\":{:.4f},\"transparent\":{},\"max_difference\":{},\"mean_difference\":{:.4f},\"ms\":{:.3f}}}",
                json(texture.name),texture.width,texture.height,factor,network,decision.edge,decision.transparent,maximum,mean,milliseconds);
        }
    }
    // Throughput with a warmed upscaler: request, run and read back one texture.
    std::string benchmarks;
    for(const unsigned size:{32U,128U,256U}) {
        const auto texture=make(std::format("bench{}",size),size,size,[&](float x,float y) {return disc(x*20/size,y*12/size);});
        std::uint64_t compilations=0;
        InterfaceUpscaler upscaler(device,context,compilations);
        const auto view=source(texture);
        double best=1e9;
        for(unsigned run=0;run<3;++run) {
            const auto start=std::chrono::steady_clock::now();
            upscaler.request(1,view,size,size,4);
            const auto finished=upscaler.step(true);
            if(finished.size()!=1) throw std::runtime_error("HUD upscaler benchmark did not finish");
            read(finished[0].result.view.Get());
            best=std::min(best,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        const auto tiles=(size+interface_tile-1)/interface_tile,extent=std::min(size,interface_tile)+2*interface_halo;
        const double work=2.0*tiles*tiles*extent*extent*(3*64*9+16*64*64*9+64*48*9); // Colour and alpha passes.
        if(!benchmarks.empty()) benchmarks+=',';
        benchmarks+=std::format("{{\"size\":{},\"ms\":{:.3f},\"gmac_per_s\":{:.1f}}}",size,best,work/best/1e6);
    }
    {
        // Sixteen 32x32 glyphs at once, as a menu screen requests them.
        const auto glyph=make("glyph",32,32,[&](float x,float y) {return disc(x*20/32,y*12/32);});
        std::uint64_t compilations=0;
        InterfaceUpscaler upscaler(device,context,compilations);
        const auto view=source(glyph);
        double best=1e9;
        for(unsigned run=0;run<3;++run) {
            const auto start=std::chrono::steady_clock::now();
            for(std::uint32_t id=1;id<=16;++id) upscaler.request(id,view,32,32,4);
            const auto finished=upscaler.step(true);
            if(finished.size()!=16) throw std::runtime_error("HUD upscaler glyph benchmark did not finish");
            for(const auto& done:finished) read(done.result.view.Get());
            best=std::min(best,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        const double extent=32+2*interface_halo,work=16*2.0*extent*extent*(3*64*9+16*64*64*9+64*48*9);
        benchmarks+=std::format(",{{\"size\":\"16 x 32\",\"ms\":{:.3f},\"gmac_per_s\":{:.1f}}}",best,work/best/1e6);
    }
    cases.emplace_back("outlined artwork uses the network",classify_interface(textures[0].rgba,20,12).network);
    cases.emplace_back("soft glow uses the filter",!classify_interface(textures[2].rgba,16,16).network);
    // Tiles of 8 texels split the outlined artwork into six tiles across one
    // batch; the 12-texel halo keeps their seams within a few levels.
    {
        const auto [whole,network_whole]=upscale(textures[0],4,interface_tile);
        const auto [tiled,network_tiled]=upscale(textures[0],4,8);
        unsigned seam=0;
        for(std::size_t i=0;i<whole.rgba.size();++i)
            seam=std::max(seam,unsigned(std::abs(int(std::to_integer<unsigned>(whole.rgba[i]))-int(std::to_integer<unsigned>(tiled.rgba[i])))));
        cases.emplace_back("tile seams stay within 3 levels",seam<=3 && network_whole && network_tiled);
    }
    // Renderer integration: marking, completion, toggling, scale and release.
    {
        const auto& texture=textures[0];
        std::vector<std::byte> bgra(texture.rgba.size());
        for(std::size_t i=0;i<texture.rgba.size();i+=4) for(unsigned c=0;c<4;++c)
            bgra[i+(c<3?2-c:3)]=std::byte(std::lround(texture.rgba[i+c]*255));
        Renderer scene(texture.width,texture.height,warp,debug,4);
        RenderState state;state.texture_modes[0]=1;state.texture_filter[0]=0x01010000;state.final_inputs={0x00000008,0x00001800};
        std::array<Vertex,6> quad;
        constexpr Float4 positions[]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
        for(unsigned i=0;i<quad.size();++i) {quad[i].position=positions[i];quad[i].uv[0]={(positions[i][0]+1)/2,(1-positions[i][1])/2,0,1};}
        const auto id=scene.upload({texture.width,texture.height,TextureFormat::argb8_linear,bgra,1,texture.width*4});
        state.textures[0]=id;
        const auto draw=[&] {scene.set_target(0);scene.prepare(state);scene.begin();scene.draw(state,quad);scene.end();return scene.readback();};
        const auto original=draw();
        constexpr std::uint64_t content=0x5EED;
        scene.interface_texture(id,content);scene.finish_interface_upscales();
        const bool idle_when_disabled=scene.stats().hud_marked==1 && scene.stats().hud_upscales==0 && draw().rgba==original.rgba;
        scene.enhancements(0,0,false,false,true);scene.finish_interface_upscales();
        const auto enlarged=draw();
        const auto [expected,network]=upscale(texture,4,interface_tile);
        bool matches=enlarged.width==expected.width && enlarged.height==expected.height;
        // Point sampling at 4x maps every pixel onto one enlarged texel; the
        // combiner passes colour, so compare RGB where the texel is opaque.
        for(std::size_t i=0;matches && i<expected.rgba.size();i+=4) if(expected.rgba[i+3]==std::byte{255})
            for(unsigned c=0;c<3;++c) matches&=std::abs(int(std::to_integer<unsigned>(enlarged.rgba[i+c]))-int(std::to_integer<unsigned>(expected.rgba[i+c])))<=1;
        cases.emplace_back("renderer waits for the setting",idle_when_disabled);
        cases.emplace_back("renderer samples the enlarged image",matches && scene.stats().hud_upscales==1 && network);
        scene.enhancements(0,0,false,false,false);
        cases.emplace_back("disabling restores the original",draw().rgba==original.rgba && scene.stats().hud_bytes==0);
        scene.enhancements(0,0,false,false,true);scene.finish_interface_upscales();
        cases.emplace_back("re-enabling enlarges again",draw().rgba==enlarged.rgba && scene.stats().hud_upscales==2);
        // A reload of identical artwork into a new slot reuses the released image.
        scene.release_texture(id);
        const bool cached=scene.stats().hud_cached==1 && scene.stats().hud_bytes>0;
        const auto reloaded=scene.upload({texture.width,texture.height,TextureFormat::argb8_linear,bgra,1,texture.width*4});
        state.textures[0]=reloaded;scene.interface_texture(reloaded,content);
        cases.emplace_back("reloaded artwork reuses its enlargement",cached && scene.stats().hud_reused==1 && scene.stats().hud_pending==0 &&
            scene.stats().hud_cached==0 && draw().rgba==enlarged.rgba && scene.stats().hud_upscales==2);
        scene.image_quality(1,0,0,false,false,true);scene.finish_interface_upscales();
        cases.emplace_back("1x keeps the original",scene.stats().hud_upscales==2 && scene.stats().hud_pending==0);
        scene.image_quality(4,0,0,false,false,true);
        scene.release_texture(reloaded);scene.finish_interface_upscales();
        cases.emplace_back("release cancels pending work",scene.stats().hud_marked==0 && scene.stats().hud_pending==0 &&
            scene.stats().hud_upscales==2 && scene.stats().texture_bytes==0 && scene.stats().hud_cached==0);
        for(auto& message:scene.errors()) errors.push_back(std::move(message));
    }
    // Frontend sample bounds: one 16x16 cell of a two-cell atlas whose
    // neighbour starts with an opaque red column. At 3x the last pixels sample
    // within half a texel of the cell edge and blend in the red; clamping half
    // a texel inside the cell rectangle keeps them inside the cell.
    {
        constexpr unsigned cell=16;
        std::vector<std::byte> atlas(cell*2*cell*4);
        for(unsigned y=0;y<cell;++y) for(unsigned x=0;x<cell*2;++x) {
            auto* p=atlas.data()+(std::size_t(y)*cell*2+x)*4; // BGRA
            const bool glyph=x>=4 && x<12 && y>=4 && y<12,neighbour=x==cell;
            p[0]=std::byte(neighbour?0:255);p[1]=std::byte(neighbour || glyph?0:255);p[2]=std::byte(glyph?0:255);p[3]=std::byte(glyph || neighbour?255:0);
        }
        Renderer scene(cell,cell,warp,debug,3);
        RenderState state;state.texture_modes[0]=1;state.texture_filter[0]=0x02020000;state.final_inputs={0x00000008,0x00001800};
        std::array<Vertex,6> quad;
        constexpr Float4 positions[]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
        for(unsigned i=0;i<quad.size();++i) {quad[i].position=positions[i];quad[i].uv[0]={(positions[i][0]+1)/4,(1-positions[i][1])/2,0,1};}
        state.textures[0]=scene.upload({cell*2,cell,TextureFormat::argb8_linear,atlas,1,cell*2*4});
        const auto draw=[&] {scene.set_target(0);scene.prepare(state);scene.begin();scene.draw(state,quad);scene.end();return scene.readback();};
        const auto unbounded=draw();
        state.sample_bounds[0]={0.5f/(cell*2),0.5f/cell,(cell-0.5f)/(cell*2),(cell-0.5f)/cell};state.bounded_samples=1;
        const auto before=scene.stats().bounded_samples;
        const auto bounded=draw();
        const auto pixel=[](const Image& image,unsigned x,unsigned y) {return image.rgba.data()+(std::size_t(y)*image.width+x)*4;};
        const unsigned last=bounded.width-1;
        bool bleeds=false,clean=bounded.width==cell*3,interior=clean;
        for(unsigned y=0;clean && y<bounded.height;++y) {
            bleeds|=std::to_integer<unsigned>(pixel(unbounded,last,y)[1])<240;
            for(unsigned c=0;c<3;++c) clean&=pixel(bounded,last,y)[c]==std::byte{255};
            for(unsigned x=3;x+3<bounded.width;++x) interior&=std::memcmp(pixel(bounded,x,y),pixel(unbounded,x,y),4)==0;
        }
        cases.emplace_back("unbounded 3x edge blends the neighbouring cell",bleeds);
        cases.emplace_back("sample bounds keep the edge inside its cell",clean && scene.stats().bounded_samples>before);
        cases.emplace_back("sample bounds leave interior pixels unchanged",interior);
        for(auto& message:scene.errors()) errors.push_back(std::move(message));
    }
    for(auto& message:renderer.errors()) errors.push_back(std::move(message));
    cases.emplace_back("D3D11 validation clean",errors.empty());
    const auto passed=std::ranges::all_of(cases,[](const auto& test){return test.second;});
    std::string report=std::format("{{\"format\":\"smbd-hud-upscaling-v1\",\"adapter\":{},\"passed\":{},\"model_sha256\":{},\"benchmarks\":[{}],\"results\":[{}],\"cases\":[",
        json(adapter),passed,json(upscaler_model_sha256),benchmarks,results);
    for(std::size_t i=0;i<cases.size();++i) {
        if(i) report+=',';
        report+="{\"name\":"+json(cases[i].first)+",\"passed\":"+(cases[i].second?"true":"false")+'}';
    }
    report+="],\"errors\":[";
    for(std::size_t i=0;i<errors.size();++i) {if(i) report+=',';report+=json(errors[i]);}
    report+="]}";
    write_text(directory/"report.json",report+'\n',false);
    return report;
}
}
