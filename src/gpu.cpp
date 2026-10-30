#include "gpu.h"
#include "performance.h"
#include "cpu.h"
#include "diagnostics.h"
#include "shaders.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace smbd {
using Microsoft::WRL::ComPtr;
namespace {
float real(std::uint32_t bits) {return std::bit_cast<float>(bits);}
unsigned attribute_bytes(std::uint32_t format) {
    const auto size=(format>>4)&15,type=format&15;
    if(format==0x16 || format==0x40) return 4;
    if(format==0x72) return 12;
    if(!size || size>4) throw std::runtime_error("Invalid NV097 vertex attribute size");
    switch(type) {case 2:return size*4;case 1:case 5:return size*2;case 4:return size;}
    throw std::runtime_error("Unsupported NV097 vertex attribute format");
}
void checked_gpu(HRESULT status) {if(FAILED(status)) throw std::runtime_error(std::format("GPU completion query failed: 0x{:08X}",static_cast<unsigned>(status)));}
}
RenderState gpu_render_state(std::span<const std::uint32_t,2048> registers,std::uint32_t primitive) {
    const auto reg=[&](unsigned method) {return registers[method/4];};
    RenderState s;
    const auto horizontal=reg(0x200),vertical=reg(0x204);
    s.scissor_test=true;s.scissor={std::int32_t(horizontal&0xFFFF),std::int32_t(vertical&0xFFFF),
        std::int32_t((horizontal&0xFFFF)+(horizontal>>16)),std::int32_t((vertical&0xFFFF)+(vertical>>16))};
    s.primitive=primitive;s.blend=reg(0x304)!=0;s.depth_test=reg(0x30C)!=0;s.depth_write=reg(0x35C)!=0;
    s.alpha_test=reg(0x300)!=0;s.cull=reg(0x308)!=0;s.front_ccw=reg(0x3A0)==0x901;
    s.depth_function=reg(0x354);s.alpha_function=reg(0x33C);s.alpha_reference=reg(0x340);
    s.blend_source=reg(0x344);s.blend_destination=reg(0x348);s.blend_equation=reg(0x350);
    s.cull_face=reg(0x39C);s.color_mask=reg(0x358);s.stencil_test=reg(0x32C)!=0;
    s.stencil_write_mask=reg(0x360)&255;s.stencil_function=reg(0x364);s.stencil_reference=reg(0x368)&255;s.stencil_read_mask=reg(0x36C)&255;
    s.stencil_fail=reg(0x370);s.stencil_depth_fail=reg(0x374);s.stencil_pass=reg(0x378);
    s.polygon_offset=reg(0x338)!=0;s.polygon_offset_scale=real(reg(0x384));s.polygon_offset_bias=real(reg(0x388));
    s.fog_enable=reg(0x2A4)!=0;const auto fog_color=reg(0x2A8);
    s.fog_color=(fog_color&0xFF00FF00U)|((fog_color&255)<<16)|((fog_color>>16)&255);s.fog_mode=reg(0x29C);
    s.fog_parameters={real(reg(0x9C0)),real(reg(0x9C4))};
    s.combiner_control=reg(0x1E60);s.final_inputs={reg(0x288),reg(0x28C)};s.final_factors={reg(0x1E20),reg(0x1E24)};
    for(unsigned i=0;i<8;++i) {
        s.color_inputs[i]=reg(0xAC0+i*4);s.alpha_inputs[i]=reg(0x260+i*4);
        s.color_outputs[i]=reg(0x1E40+i*4);s.alpha_outputs[i]=reg(0xAA0+i*4);
        s.factor0[i]=reg(0xA60+i*4);s.factor1[i]=reg(0xA80+i*4);
    }
    for(unsigned i=0;i<4;++i) {
        const auto method=0x1B00+i*0x40;
        s.texture_modes[i]=(reg(0x1E70)>>(i*5))&31;
        s.texture_sources[i]=i<2?0:(reg(0x1E78)>>(8+i*4))&15;
        s.texture_clip[i]=(reg(0x17F8)>>(i*4))&15;s.texture_address[i]=reg(method+8);
        const auto filter=reg(method+0x14);
        s.linear_filter[i]=((filter>>24)&15)==2;s.alpha_kill[i]=(reg(method+12)&4)!=0;
    }
    return s;
}
std::uint32_t prepare_gpu_program(Renderer& renderer,std::span<const std::uint32_t,2048> registers,const VertexProgram& program) {
    const auto reg=[&](unsigned method) {return registers[method/4];};
    const auto mode=reg(0x1E94)&3;
    std::uint32_t shader;
    if(mode==2) shader=renderer.prepare_program(program);
    else if(mode==0) {
        if(reg(0x314)) throw std::runtime_error("Fixed lighting requires the corresponding shader path");
        FixedTransform fixed{};fixed.skin=reg(0x328);fixed.normalize=reg(0x3A4)!=0;
        fixed.fog=reg(0x2A4)!=0;fixed.fog_source=reg(0x2A0);
        for(unsigned stage=0;stage<4;++stage) {
            fixed.texture_matrix[stage]=reg(0x420+stage*4)!=0;
            for(unsigned lane=0;lane<4;++lane) fixed.texgen[stage][lane]=reg(0x3C0+stage*16+lane*4);
        }
        shader=renderer.prepare_fixed(fixed);
    } else throw std::runtime_error("Unsupported NV097 transform mode");
    return shader;
}
struct Gpu::Impl {
    Renderer& renderer;Memory& memory;Gpu& owner;
    std::uint32_t semaphore_base;
    std::array<std::array<std::uint32_t,2048>,8> registers{};
    std::array<std::uint32_t,8> objects{};
    std::array<std::uint32_t,8> classes{};
    std::map<std::uint32_t,std::uint32_t> object_classes;
    std::map<std::uint32_t,std::array<std::uint32_t,2048>> object_state;
    VertexProgram program;
    VertexConstants constants{};
    ProgramVertex current{};
    std::uint32_t program_load=0,constant_load=0,primitive=0,target=0;
    std::uint32_t array_first=0,array_count=0,array_command=0,array_callsite=0;
    std::vector<std::uint32_t> indices,inline_words;
    struct Draw {
        RenderState state;VertexConstants constants;ProgramView view;ProgramVertex current;
        std::array<VertexStream,16> streams{};
        std::uint32_t layout=0,count=0,stream_count=0;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> packed;
    };
    std::vector<Draw> draws;
    unsigned draw_count=0;
    struct Surface {std::uint32_t identifier,width,height;};
    std::map<std::array<std::uint32_t,4>,Surface> targets;
    struct Texture {std::uint32_t identifier,size;};
    std::map<std::array<std::uint32_t,5>,Texture> textures;
    struct Fence {ComPtr<ID3D11Query> query;std::uint32_t address=0,value=0;bool callback=false;};
    std::array<Fence,64> fences;
    unsigned fence_head=0,fence_count=0;
    GpuStats statistics;
    FrameCaptureStatus capture;
    bool capture_active=false;
    std::string capture_source;
    std::ofstream capture_trace;
    std::set<std::uint32_t> capture_textures;
    std::uint64_t capture_bytes=0,capture_text_bytes=0,capture_image_bytes=0,capture_first_frame=0;
    unsigned capture_draws=0;
    std::uint32_t command_address=0,submission_callsite=0;
    // Four 4x color images need about 79 MB; three raw depths need 59 MB in
    // addition to shaders/resources. Each image is read/written separately.
    static constexpr std::uint64_t binary_limit=128*1024*1024,text_limit=48*1024*1024,image_limit=160*1024*1024;
    void capture_manifest() {
        write_text(capture.directory/"capture.json",std::format(
            "{{\"format\":\"smbd-frame-capture-v1\",\"source_sha256\":{},\"pending\":{},\"complete\":{},\"truncated\":{},\"error\":{},\"first_frame\":{},\"last_frame\":{},\"draws\":{},\"binary_bytes\":{},\"trace_bytes\":{},\"image_rgba_bytes\":{},\"texture_uploads_total\":{},\"texture_evictions_total\":{},\"target_evictions_total\":{},\"texture_cache_entries\":{}}}\n",
            json(capture_source),capture.pending,capture.complete,capture.truncated,json(capture.error),capture_first_frame,
            statistics.flips,capture_draws,capture_bytes,capture_text_bytes,capture_image_bytes,
            statistics.texture_uploads,statistics.texture_evictions,statistics.target_evictions,textures.size()),true);
    }
    void capture_abort(std::string_view reason) {
        capture.pending=false;capture.complete=false;capture.error=reason;capture_active=false;
        try {if(capture_trace.is_open()) capture_trace.close();} catch(...) {}
        try {capture_manifest();} catch(...) {}
        diagnostic_record("{\"type\":\"frame_capture_error\",\"directory\":"+json(path_text(capture.directory))+",\"message\":"+json(reason)+'}');
    }
    template<class F> void capture_try(F&& action) {
        try {action();} catch(const std::exception& error) {capture_abort(error.what());}
    }
    void capture_record(std::string_view record,bool final=false) {
        if(capture_text_bytes+record.size()+1>(final?text_limit:text_limit-4096)) {capture.truncated=true;return;}
        capture_trace<<record<<'\n';capture_text_bytes+=record.size()+1;
    }
    std::string capture_data(const std::string& name,Bytes bytes) {
        if(bytes.size()>binary_limit-capture_bytes) {capture.truncated=true;return "null";}
        std::ofstream output(capture.directory/name,std::ios::binary|std::ios::trunc);
        output.exceptions(std::ios::failbit|std::ios::badbit);
        output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));output.close();
        capture_bytes+=bytes.size();return json(name);
    }
    void capture_shader(ShaderKind kind,unsigned index=0,const VertexProgram* vertex=nullptr) {
        const auto binary=shader_binary(kind,vertex);
        const auto data=capture_data(std::format("shader-{:02}-{:03}.dxbc",unsigned(kind),index),binary.bytes);
        std::string record=std::format("{{\"type\":\"native_shader\",\"kind\":{},\"bytes\":{},\"data\":{}",unsigned(kind),binary.bytes.size(),data);
        if(vertex) record+=std::format(",\"program_start\":{},\"program\":{}",vertex->start,json(hex_bytes(std::as_bytes(std::span(vertex->tokens)))));
        capture_record(record+'}');
    }
    void capture_draw(std::uint32_t first,const Draw& draw,std::uint32_t shader,const VertexLayout& layout,
                      const std::array<std::uint32_t,16>& bases,bool inlined,std::span<const std::uint32_t> elements) {
        if(capture_draws==2048 || capture_text_bytes+64*1024>text_limit-4096) {capture.truncated=true;return;}
        const auto number=capture_draws++;
        std::ostringstream record;
        record<<std::format("{{\"type\":\"draw\",\"draw\":{},\"command_address\":{},\"callsite\":{},\"first\":{},\"count\":{},\"target\":{},\"shader\":{},\"program_start\":{},\"primitive\":{},\"registers\":{},\"program\":{},\"constants\":{},\"current\":{},\"view\":{},\"textures\":[",
            number,json(hex32(command_address)),json(hex32(submission_callsite)),first,draw.count,target,shader,program.start,draw.state.primitive,
            json(hex_bytes(std::as_bytes(std::span(registers[0])))),json(hex_bytes(std::as_bytes(std::span(program.tokens)))),
            json(hex_bytes(std::as_bytes(std::span(draw.constants)))),json(hex_bytes(std::as_bytes(std::span(draw.current.attributes)))),
            json(hex_bytes(std::as_bytes(std::span(&draw.view,1)))));
        for(unsigned i=0;i<4;++i) {if(i) record<<',';record<<draw.state.textures[i];}
        record<<"],\"streams\":[";
        for(unsigned i=0;i<draw.stream_count;++i) {
            if(i) record<<',';const auto& stream=draw.streams[i];
            const auto name=std::format("vertex-{:04}-{}.bin",number,i);
            record<<std::format("{{\"address\":{},\"stride\":{},\"size\":{},\"data\":{}}}",
                inlined?"null":json(hex32(bases[i]+first*stream.stride)),stream.stride,stream.bytes.size(),capture_data(name,stream.bytes));
        }
        record<<"],\"attributes\":[";
        for(unsigned i=0;i<layout.size();++i) {if(i) record<<',';const auto& a=layout[i];record<<std::format("[{},{},{}]",a.stream,a.offset,a.format);}
        record<<std::format("],\"indices\":{{\"count\":{},\"element_bytes\":4,\"data\":{}}}}}",elements.size(),
            elements.empty()?"null":capture_data(std::format("indices-{:04}.bin",number),std::as_bytes(elements)));
        capture_record(record.view());
    }
    void capture_surface(std::uint32_t id,std::uint32_t width,std::uint32_t height,const std::array<std::uint32_t,4>& key) {
        const auto name=id?std::format("target-{}-{:08X}.png",id,key[0]):"frame.png";
        std::string image="null",depth="null";
        const auto scale=renderer.stats().resolution_scale;
        const auto size=std::uint64_t(width)*height*4*scale*scale;
        if(size<=image_limit-capture_image_bytes) {
            save_png(renderer.read_target(id),capture.directory/name);image=json(name);capture_image_bytes+=size;
        } else capture.truncated=true;
        if(id) {
            const auto depth_size=std::uint64_t(width)*height*scale*scale*(((key[2]>>4)&15)==1?2:4);
            if(depth_size<=binary_limit-capture_bytes) depth=capture_data(std::format("depth-{}.bin",id),renderer.read_depth(id));
            else capture.truncated=true;
        }
        capture_record(std::format("{{\"type\":\"surface\",\"resource\":{},\"color\":{},\"depth\":{},\"format\":{},\"pitch\":{},\"width\":{},\"height\":{},\"image\":{},\"depth_data\":{}}}",
            id,json(hex32(key[0])),json(hex32(key[1])),json(hex32(key[2])),json(hex32(key[3])),width,height,image,depth));
    }
    void capture_flip() {
        capture_try([&] {
            if(capture_active) {
                const auto main=renderer.read_target(0);
                save_png(main,capture.directory/"frame.png");capture_image_bytes+=main.rgba.size();
                for(const auto& [key,surface]:targets) capture_surface(surface.identifier,surface.width,surface.height,key);
                capture_record(std::format("{{\"type\":\"end\",\"frame\":{},\"draws\":{},\"binary_bytes\":{},\"truncated\":{}}}",
                    statistics.flips,capture_draws,capture_bytes,capture.truncated),true);
                capture_trace.close();capture_active=false;capture.pending=false;capture.complete=true;capture_manifest();
                diagnostic_record("{\"type\":\"frame_capture_saved\",\"directory\":"+json(path_text(capture.directory))+",\"truncated\":"+(capture.truncated?"true":"false")+'}');
            } else {
                capture_first_frame=statistics.flips+1;capture_active=true;
                capture_trace.clear();capture_trace.open(capture.directory/"draws.jsonl",std::ios::binary|std::ios::trunc);
                capture_trace.exceptions(std::ios::failbit|std::ios::badbit);
                capture_record(std::format("{{\"type\":\"header\",\"format\":\"smbd-gpu-frame-v1\",\"source_sha256\":{},\"adapter\":{},\"frame\":{},\"resolution_scale\":{},\"byte_order\":\"little\",\"draw_limit\":2048,\"binary_limit\":{},\"trace_limit\":{},\"image_rgba_limit\":{}}}",
                    json(capture_source),json(renderer.adapter()),capture_first_frame,renderer.stats().resolution_scale,binary_limit,text_limit,image_limit));
                capture_record("{\"type\":\"initial_registers\",\"data\":"+json(hex_bytes(std::as_bytes(std::span(registers))))+'}');
                for(unsigned i=0;i<unsigned(ShaderKind::count);++i) {
                    const auto kind=ShaderKind(i);
                    if(kind!=ShaderKind::vertex && kind!=ShaderKind::launch && kind!=ShaderKind::gui_vertex && kind!=ShaderKind::gui_pixel) capture_shader(kind);
                }
                const auto vertices=static_vertex_shaders();
                for(unsigned i=0;i<vertices.size();++i) capture_shader(vertices[i].launch?ShaderKind::launch:ShaderKind::vertex,i,&vertices[i].program);
            }
        });
    }
    Impl(Renderer& r,Memory& m,Gpu& o,std::uint32_t base,std::span<const GraphicsObject> profile):renderer(r),memory(m),owner(o),semaphore_base(base) {
        for(const auto& object:profile) {
            if(!object_classes.emplace(object.handle,object.kind).second) throw std::runtime_error("Duplicate native graphics object handle");
            object_state.emplace(object.handle,std::array<std::uint32_t,2048>{});
        }
        for(auto& attribute:current.attributes) attribute={0,0,0,1};
        current.attributes[3]={1,1,1,1};draws.resize(256);indices.reserve(4096);inline_words.reserve(4096);
        const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};
        for(auto& fence:fences) checked_gpu(renderer.native_device()->CreateQuery(&description,&fence.query));
    }
    std::uint32_t reg(unsigned method) const {return registers[0][method/4];}
    void emit_arrays() {
        if(!array_count) return;
        const auto count=array_count;array_count=0;
        const auto command=command_address,callsite=submission_callsite;
        command_address=array_command;submission_callsite=array_callsite;
        draw(array_first,count);
        command_address=command;submission_callsite=callsite;
    }
    void flush() {
        if(!draw_count) return;
        PerfScope scope(renderer.performance(),PerfPhase::render);
        renderer.begin(0,1,false);
        try {
            for(const auto& draw:std::span(draws).first(draw_count)) {
                renderer.draw_streams(draw.state,draw.layout,draw.constants,draw.view,
                    std::span(draw.streams).first(draw.stream_count),draw.current,draw.count,draw.indices);
                statistics.vertices+=draw.indices.empty()?draw.count:draw.indices.size();++statistics.draws;
            }
        } catch(...) {renderer.end();draw_count=0;throw;}
        renderer.end();draw_count=0;
    }
    static bool same_color(const std::array<std::uint32_t,4>& a,const std::array<std::uint32_t,4>& b) {
        return a[0]==b[0] && (a[2]&~255U)==(b[2]&~255U) && ((a[2]&15)==3)==((b[2]&15)==3) &&
            (a[3]&0xFFFF)==(b[3]&0xFFFF);
    }
    void grow_color(const std::array<std::uint32_t,4>& key,Surface& surface,std::uint32_t width,std::uint32_t height) {
        flush();renderer.grow_target(surface.identifier,width,height);
        for(auto& [other,image]:targets) if(same_color(key,other)) {image.width=width;image.height=height;}
    }
    void select_target() {
        const auto horizontal=reg(0x200),vertical=reg(0x204);
        const auto clip_width=horizontal>>16,clip_height=vertical>>16;
        if(!clip_width || !clip_height)
            throw std::runtime_error(std::format("Unsupported NV097 surface clip H=0x{:08X}, V=0x{:08X}, format=0x{:08X}, pitch=0x{:08X}, offset=0x{:08X}",
                horizontal,vertical,reg(0x208),reg(0x20C),reg(0x210)));
        const auto color=reg(0x208)&15,zeta=(reg(0x208)>>4)&15;
        if((color!=3 && color!=4 && color!=5 && color!=8) || (zeta!=1 && zeta!=2))
            throw std::runtime_error(std::format("Unsupported NV097 surface format=0x{:08X}, pitch=0x{:08X}, color=0x{:08X}, depth=0x{:08X}",
                reg(0x208),reg(0x20C),reg(0x210),reg(0x214)));
        const auto format=reg(0x208),type=(format>>8)&15;
        if(type!=1 && type!=2) throw std::runtime_error("Unsupported NV097 surface layout");
        const auto width=type==2?1U<<((format>>16)&15):(horizontal&0xFFFF)+clip_width;
        const auto height=type==2?1U<<((format>>24)&15):(vertical&0xFFFF)+clip_height;
        const std::array key{reg(0x210),reg(0x214),format,reg(0x20C)};
        auto found=targets.find(key);
        if(found==targets.end()) {
            std::uint32_t shared=UINT32_MAX;
            for(const auto& [other,image]:targets) if(same_color(key,other) && image.width==width && image.height==height) {
                shared=image.identifier;break;
            }
            flush();found=targets.emplace(key,Surface{renderer.create_target(width,height,color==3?SurfaceColor::rgb565:SurfaceColor::rgba8,
                zeta==1?SurfaceDepth::z16:SurfaceDepth::z24s8,shared,!capture.pending),width,height}).first;
        } else if(width>found->second.width || height>found->second.height) {
            flush();auto& surface=found->second;
            const auto w=std::max(width,surface.width),h=std::max(height,surface.height);
            grow_color(key,surface,w,h);
        }
        if(target!=found->second.identifier) {flush();target=found->second.identifier;renderer.set_target(target);}
    }
    std::uint32_t texture(unsigned stage,ProgramView& view) {
        const auto method=0x1B00+stage*0x40,format=reg(method+4),image=reg(method+0x1C),pitch=reg(method+0x10)>>16;
        const auto code=(format>>8)&255;
        const bool linear=code==0x12 || code==0x1E;
        const auto width=linear?image>>16:1U<<((format>>20)&15),height=linear?image&0xFFFF:1U<<((format>>24)&15);
        const auto depth=((format>>4)&15)==3?1U<<(format>>28):1U;
        const auto mips=(format>>16)&15;
        if(!width || !height || width>4096 || height>4096 || !mips || depth>2048)
            throw std::runtime_error("Invalid NV097 texture dimensions");
        const bool cube=(format&4)!=0;
        // Sampling a GPU-produced surface must see its current GPU contents,
        // rather than the original RAM allocation used for ordinary textures.
        if(!cube && depth==1 && mips==1) for(const auto& [key,surface]:targets) {
            const auto surface_format=key[2],surface_color=surface_format&15,surface_type=(surface_format>>8)&15;
            const bool matching_color=surface_color==3?code==5:(code==6 || code==7 || code==0x12 || code==0x1E);
            if(key[0]==(reg(method)&0x0FFFFFFF) && surface.width==width && surface.height==height && matching_color &&
                ((surface_type==1)==linear) && (!linear || (key[3]&0xFFFF)==pitch)) {
                if(linear) view.texture_scales[stage]={1.0f/width,1.0f/height,1,1};
                return surface.identifier;
            }
        }
        unsigned bpp=0,block=0;
        switch(code) {case 4:case 5:case 0x1A:bpp=2;break;case 6:case 7:case 0x12:case 0x1E:bpp=4;break;
            case 0xC:block=8;break;case 0xE:case 0xF:block=16;break;
            default:throw std::runtime_error(std::format("Unsupported NV097 texture format 0x{:02X}",code));}
        std::uint64_t size=0;
        for(unsigned level=0;level<mips;++level) {
            const auto w=std::max(1U,width>>level),h=std::max(1U,height>>level),d=std::max(1U,depth>>level);
            size+=(block?std::uint64_t((w+3)/4)*((h+3)/4)*block:std::uint64_t(linear?pitch:w*bpp)*h)*d;
        }
        if(cube) size*=6;
        if(!size || size>64*1024*1024) throw std::runtime_error("NV097 texture payload exceeds its bound");
        const auto address=reg(method)&0x0FFFFFFF;
        const std::array key{address,format,image,pitch,reg(method+0x20)};
        auto found=textures.find(key);
        if(found==textures.end()) {
            const auto id=renderer.upload({width,height,static_cast<TextureFormat>(code),
                Bytes(static_cast<const std::byte*>(memory.access(address,static_cast<std::size_t>(size))),static_cast<std::size_t>(size)),
                mips,linear?pitch:0,cube,depth},!capture.pending);
            found=textures.emplace(key,Texture{id,static_cast<std::uint32_t>(size)}).first;++statistics.texture_uploads;
        }
        const auto id=found->second.identifier;
        if(capture_active && !capture_textures.contains(id)) capture_try([&] {
            capture_textures.insert(id);
            const auto data=capture_data(std::format("texture-{}.bin",id),
                Bytes(static_cast<const std::byte*>(memory.access(address,static_cast<std::size_t>(size))),static_cast<std::size_t>(size)));
            capture_record(std::format("{{\"type\":\"texture\",\"resource\":{},\"address\":{},\"width\":{},\"height\":{},\"depth\":{},\"format\":{},\"mips\":{},\"pitch\":{},\"cubemap\":{},\"size\":{},\"guest_data\":{}}}",
                id,json(hex32(address)),width,height,depth,code,mips,linear?pitch:0,cube,size,data));
        });
        if(linear) view.texture_scales[stage]={1.0f/width,1.0f/height,1.0f/depth,1};
        return id;
    }
    RenderState state(ProgramView& view) {
        auto s=gpu_render_state(registers[0],primitive);
        for(unsigned i=0;i<4;++i) if((reg(0x1B0C+i*0x40)&0x40000000U) && s.texture_modes[i]) s.textures[i]=texture(i,view);
        return s;
    }
    void draw(std::uint32_t first,std::uint32_t count,std::span<const std::uint32_t> elements={},bool inlined=false) {
        if(!primitive || !count || count>32768 || first>0xFFFFFF-count) throw std::runtime_error("Invalid NV097 draw range");
        select_target();
        auto& draw=draws[draw_count];draw.stream_count=0;draw.view={};draw.packed.clear();
        draw.current=current;draw.constants=constants;draw.count=count;
        draw.view.depth_scale=constants[58][2];
        if(draw.view.depth_scale<=0) throw std::runtime_error("NV097 viewport depth scale was not initialized");
        draw.state=state(draw.view);
        const auto shader=prepare_gpu_program(renderer,registers[0],program);
        VertexLayout layout{};
        std::array<std::uint32_t,16> bases{},strides{};
        unsigned inline_stride=0;
        for(unsigned attribute=0;attribute<16;++attribute) {
            const auto format=reg(0x1760+attribute*4),stride=format>>8,type=format&255;
            if(!((type>>4)&15)) continue;
            const auto bytes=attribute_bytes(type);
            if(inlined) {layout[attribute]={0,inline_stride,type};inline_stride+=(bytes+3)&~3U;continue;}
            if(!stride || stride>2048 || stride<bytes) throw std::runtime_error("Invalid NV097 vertex stream stride");
            const auto address=reg(0x1720+attribute*4)&0x0FFFFFFF;
            unsigned stream=0;
            for(;stream<draw.stream_count;++stream) if(strides[stream]==stride && address>=bases[stream] && address-bases[stream]+bytes<=stride) break;
            if(stream==draw.stream_count) {bases[stream]=address;strides[stream]=stride;++draw.stream_count;}
            layout[attribute]={stream,address-bases[stream],type};
        }
        if(inlined) {
            if(!inline_stride || inline_words.size()*4!=std::size_t(count)*inline_stride)
                throw std::runtime_error("NV097 inline vertex payload differs from its layout");
            draw.packed=inline_words;draw.stream_count=1;
            draw.streams[0]={std::as_bytes(std::span(draw.packed)),inline_stride};
        }
        for(unsigned stream=0;!inlined && stream<draw.stream_count;++stream) {
            unsigned final_bytes=0;
            for(const auto& attribute:layout) if(attribute.stream==stream) final_bytes=std::max(final_bytes,attribute.offset+attribute_bytes(attribute.format));
            const auto size=std::uint64_t(count-1)*strides[stream]+final_bytes;
            const auto address=std::uint64_t(bases[stream])+std::uint64_t(first)*strides[stream];
            if(address>UINT32_MAX || size>8*1024*1024) throw std::runtime_error("NV097 vertex stream exceeds its bound");
            draw.streams[stream]={Bytes(static_cast<const std::byte*>(memory.access(static_cast<std::uint32_t>(address),static_cast<std::size_t>(size))),static_cast<std::size_t>(size)),strides[stream]};
        }
        draw.layout=renderer.prepare_layout(shader,layout);renderer.prepare(draw.state);
        if(capture_active) capture_try([&] {capture_draw(first,draw,shader,layout,bases,inlined,elements);});
        draw.indices.assign(elements.begin(),elements.end());
        if(++draw_count==draws.size()) flush();
    }
    void blit(const std::array<std::uint32_t,2048>& state,std::uint32_t size) {
        const auto handle=state[0x19C/4];
        if(!object_classes.contains(handle) || object_classes.at(handle)!=0x62)
            throw std::runtime_error("Image blit has no bound linear surface object");
        if(state[0x2FC/4]!=3) throw std::runtime_error("Image blit operation is not source copy");
        const auto& context=object_state.at(handle);
        const auto format=context[0x300/4];
        const auto bpp=format==4?2U:(format==6 || format==7 || format==10)?4U:0U;
        if(!bpp) throw std::runtime_error(std::format("Unsupported image-blit color format 0x{:X}",format));
        const auto source=context[0x308/4]&0x07FFFFFF,destination=context[0x30C/4]&0x07FFFFFF;
        const auto source_pitch=context[0x304/4]&0xFFFF,destination_pitch=context[0x304/4]>>16;
        const auto sx=state[0x300/4]&0xFFFF,sy=state[0x300/4]>>16;
        const auto dx=state[0x304/4]&0xFFFF,dy=state[0x304/4]>>16;
        const auto width=size&0xFFFF,height=size>>16;
        if(!width || !height) return;
        if(!source_pitch || !destination_pitch || source_pitch%bpp || destination_pitch%bpp ||
           sx+width>source_pitch/bpp || dx+width>destination_pitch/bpp || sy+height>4096 || dy+height>4096)
            throw std::runtime_error("Image blit rectangle exceeds its linear surfaces");
        memory.access(source,std::size_t(sy+height-1)*source_pitch+std::size_t(sx+width)*bpp);
        memory.access(destination,std::size_t(dy+height-1)*destination_pitch+std::size_t(dx+width)*bpp);
        const auto find=[&](std::uint32_t address,std::uint32_t pitch) {
            return std::ranges::find_if(targets,[&](const auto& item) {
                const auto& [key,image]=item;
                return key[0]==address && ((key[2]>>8)&15)==1 && (key[3]&0xFFFF)==pitch &&
                    ((key[2]&15)==3)==(bpp==2);
            });
        };
        auto from=find(source,source_pitch);
        if(from==targets.end() || sx+width>from->second.width || sy+height>from->second.height)
            throw std::runtime_error("Image blit source is not a resident native color image");
        auto to=find(destination,destination_pitch);
        flush();
        if(to==targets.end()) {
            if(dx || dy || width!=destination_pitch/bpp)
                throw std::runtime_error("Initial image blit must define complete rows from the origin");
            const auto color=bpp==2?SurfaceColor::rgb565:SurfaceColor::rgba8;
            const std::array key{destination,0U,bpp==2?0x123U:0x128U,destination_pitch|(destination_pitch<<16)};
            to=targets.emplace(key,Surface{renderer.create_target(width,height,color,SurfaceDepth::z24s8,UINT32_MAX,!capture.pending),width,height}).first;
        } else if(dx+width>to->second.width || dy+height>to->second.height)
            grow_color(to->first,to->second,std::max(dx+width,to->second.width),std::max(dy+height,to->second.height));
        renderer.copy_region(to->second.identifier,from->second.identifier,sx,sy,dx,dy,width,height);
        if(capture_active) capture_try([&] {capture_record(std::format("{{\"type\":\"blit\",\"command_address\":{},\"source\":{},\"destination\":{},\"source_xy\":[{},{}],\"destination_xy\":[{},{}],\"size\":[{},{}]}}",
            json(hex32(command_address)),from->second.identifier,to->second.identifier,sx,sy,dx,dy,width,height));});
        ++statistics.blits;
    }
    void fence(std::uint32_t address,std::uint32_t value,bool callback=false) {
        flush();if(fence_count==fences.size()) poll(true);
        auto& result=fences[(fence_head+fence_count)%fences.size()];result.address=address;result.value=value;result.callback=callback;
        renderer.native_context()->End(result.query.Get());++fence_count;++statistics.fences;
    }
    void poll(bool wait) {
        PerfScope scope(renderer.performance(),PerfPhase::gpu_wait,wait?unsigned(PerfGpuWait::idle):unsigned(PerfGpuWait::poll));
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        if(wait) renderer.native_context()->Flush();
        while(fence_count) {
            BOOL finished=FALSE;
            const auto status=renderer.native_context()->GetData(fences[fence_head].query.Get(),&finished,sizeof(finished),wait?0:D3D11_ASYNC_GETDATA_DONOTFLUSH);
            checked_gpu(status);
            if(status!=S_OK || !finished) {
                if(!wait) return;
                if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("Native GPU completion timed out");
                SwitchToThread();continue;
            }
            const auto& completed=fences[fence_head];
            const auto address=completed.address,value=completed.value;const bool callback=completed.callback;
            if(!callback && address) memory.store<std::uint32_t>(address,value);
            fence_head=(fence_head+1)%static_cast<unsigned>(fences.size());--fence_count;
            if(callback) {owner.completion(address,value);++statistics.callbacks;}
        }
    }
    void method(unsigned subchannel,unsigned method,std::uint32_t data) {
        // DRAW_ARRAYS words are ranges in one primitive, not independent draws.
        // Keep adjacent ranges joined across packet/submission boundaries. End,
        // a state change, or a noncontiguous range finishes the pending draw.
        if(array_count && (subchannel || method!=0x1810 || (data&0xFFFFFF)!=array_first+array_count)) emit_arrays();
        ++statistics.methods;
        if(method==0) {
            const auto found=object_classes.find(data);
            if(found==object_classes.end()) throw std::runtime_error(std::format("Graphics object 0x{:X} has no verified class",data));
            objects[subchannel]=data;classes[subchannel]=found->second;return;
        }
        registers[subchannel][method/4]=data;
        if(classes[subchannel]!=0x97) {
            auto& state=object_state.at(objects[subchannel]);state[method/4]=data;
            if(method==0x104) throw std::runtime_error("Native 2D object notification is not bound");
            if(method==0x110) {flush();fence(0,0);poll(true);return;}
            if(method<0x300) return;
            // These commands set inputs for a later 2D operation. Preserve the
            // object-local state, including when it is rebound to a channel.
            switch(classes[subchannel]) {
            case 0x44: // NV4_PATTERN: format, shape, palette and bitmap.
                if(method==0x300 && (data<1 || data>3)) throw std::runtime_error("Invalid pattern color format");
                if(method==0x304 && (data<1 || data>2)) throw std::runtime_error("Invalid pattern bitmap format");
                if(method==0x308 && data>2) throw std::runtime_error("Invalid pattern bitmap shape");
                if(method==0x30C && (data<1 || data>2)) throw std::runtime_error("Invalid pattern type");
                if(method>=0x300 && method<=0x31C) return;
                if((method>=0x400 && method<0x440) || (method>=0x500 && method<0x580) ||
                   (method>=0x600 && method<0x680) || (method>=0x700 && method<0x800)) return;
                break;
            case 0x43: // Raster operation.
                if(method==0x300 && data<=255) return;
                break;
            case 0x12: case 0x72: // Constant blend factors.
                if(method==0x300) return;
                break;
            case 0x19: // Clip rectangle origin and extent.
                if(method==0x300 || method==0x304) return;
                break;
            case 0x62: // Linear surface format, pitches and offsets.
                if(method>=0x300 && method<=0x30C) return;
                break;
            case 0x9F: // Blit inputs; SIZE at 308 performs the copy.
                if(method==0x2FC || method==0x300 || method==0x304) return;
                if(method==0x308) {
                    blit(state,data);return;
                }
                break;
            }
            throw std::runtime_error(std::format("Unimplemented graphics class 0x{:X}, handle 0x{:X}, method 0x{:X}, value 0x{:X}",
                classes[subchannel],objects[subchannel],method,data));
            return;
        }
        if(subchannel!=0) throw std::runtime_error("Native 3D commands require the SDK's channel zero");
        if(method==0x100 && !data) return;
        if(method==0x17FC) {
            if(data) {if(primitive) throw std::runtime_error("Nested NV097 begin/end");primitive=data;indices.clear();inline_words.clear();}
            else {
                if(!inline_words.empty()) {
                    if(!indices.empty()) throw std::runtime_error("NV097 inline arrays and elements are mixed");
                    unsigned stride=0;
                    for(unsigned attribute=0;attribute<16;++attribute) {
                        const auto format=reg(0x1760+attribute*4)&255;
                        if((format>>4)&15) stride+=(attribute_bytes(format)+3)&~3U;
                    }
                    if(!stride || (inline_words.size()*4)%stride) throw std::runtime_error("Incomplete NV097 inline vertex");
                    draw(0,static_cast<std::uint32_t>(inline_words.size()*4/stride),{},true);
                }
                if(!indices.empty()) {const auto maximum=*std::ranges::max_element(indices);draw(0,maximum+1,indices);}
                primitive=0;indices.clear();
            }
        } else if(method==0x1810) {
            if(!primitive || !indices.empty() || !inline_words.empty()) throw std::runtime_error("NV097 array range has no primitive or mixes vertex sources");
            const auto count=(data>>24)+1;
            if(count>32768-array_count) throw std::runtime_error("NV097 array primitive exceeds its vertex bound");
            if(!array_count) {array_first=data&0xFFFFFF;array_command=command_address;array_callsite=submission_callsite;}
            array_count+=count;
        }
        else if(method==0x1800) {indices.push_back(data&0xFFFF);indices.push_back(data>>16);}
        else if(method==0x1808) indices.push_back(data);
        else if(method==0x1818) inline_words.push_back(data);
        else if(method>=0x1880 && method<=0x18FC) {
            const auto attribute=(method-0x1880)/8,lane=((method-0x1880)/4)%2;
            if(!lane) current.attributes[attribute]={real(data),0,0,1};else current.attributes[attribute][1]=real(data);
        } else if(method>=0x1900 && method<=0x193C) current.attributes[(method-0x1900)/4]={
            float(static_cast<std::int16_t>(data)),float(static_cast<std::int16_t>(data>>16)),0,1};
        else if(method>=0x1940 && method<=0x197C) current.attributes[(method-0x1940)/4]={
            float(data&255)/255,float((data>>8)&255)/255,float((data>>16)&255)/255,float(data>>24)/255};
        else if(method>=0x1980 && method<=0x19FC) {
            const auto attribute=(method-0x1980)/8,lane=((method-0x1980)/4)%2*2;
            current.attributes[attribute][lane]=float(static_cast<std::int16_t>(data));
            current.attributes[attribute][lane+1]=float(static_cast<std::int16_t>(data>>16));
        } else if(method>=0x1A00 && method<=0x1AFC) current.attributes[(method-0x1A00)/16][((method-0x1A00)/4)%4]=real(data);
        else if(method>=0x440 && method<=0x47C) constants[4+(method-0x440)/16][((method-0x440)/4)%4]=real(data);
        else if(method>=0x480 && method<=0x57C) {
            const auto index=(method-0x480)/4;constants[8+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x580 && method<=0x67C) {
            const auto index=(method-0x580)/4;constants[12+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x680 && method<=0x6BC) constants[(method-0x680)/16][((method-0x680)/4)%4]=real(data);
        else if(method>=0x6C0 && method<=0x7BC) {
            const auto index=(method-0x6C0)/4;constants[68+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x840 && method<=0x93C) {
            const auto index=(method-0x840)/4;constants[64+(index/16)*8+(index%16)/4][index%4]=real(data);
        } else if(method>=0x9D0 && method<=0x9DC) constants[57][(method-0x9D0)/4]=real(data);
        else if(method>=0xA50 && method<=0xA5C) constants[56][(method-0xA50)/4]=real(data);
        else if(method>=0xA20 && method<=0xA2C) constants[59][(method-0xA20)/4]=real(data);
        else if(method>=0xAF0 && method<=0xAFC) constants[58][(method-0xAF0)/4]=real(data);
        else if(method==0x1E9C) program_load=data;
        else if(method==0x1EA0) program.start=data;
        else if(method==0x1EA4) constant_load=data;
        else if(method>=0xB00 && method<=0xB7C) {
            if(program_load>=program.tokens.size()) throw std::runtime_error("NV097 program upload outside instruction memory");
            const auto lane=((method-0xB00)/4)%4;program.tokens[program_load][lane]=data;if(lane==3) ++program_load;
        } else if(method>=0xB80 && method<=0xBFC) {
            if(constant_load>=constants.size()) throw std::runtime_error("NV097 constant upload outside context memory");
            const auto lane=((method-0xB80)/4)%4;constants[constant_load][lane]=real(data);if(lane==3) ++constant_load;
        } else if(method==0x1E90) {
            flush();auto launch=program;launch.start=data;ProgramVertex input{};
            for(unsigned lane=0;lane<4;++lane) input.attributes[0][lane]=real(reg(0x1E80+lane*4));
            renderer.launch_program(renderer.prepare_program(launch,true),constants,input);
        } else if(method==0x1D94) {
            flush();select_target();
            const auto h=reg(0x1D98),v=reg(0x1D9C);
            const std::array<std::int32_t,4> rectangle{std::int32_t(h&0xFFFF),std::int32_t(v&0xFFFF),
                std::int32_t((h>>16)+1),std::int32_t((v>>16)+1)};
            const auto value=reg(0x1D8C),zeta=(reg(0x208)>>4)&15;
            renderer.clear_buffers(data,reg(0x1D90),zeta==1?float(value&0xFFFF)/65535:float(value>>8)/16777215,static_cast<std::uint8_t>(value),rectangle);
            if(capture_active) capture_try([&] {capture_record(std::format("{{\"type\":\"clear\",\"command_address\":{},\"target\":{},\"flags\":{},\"color\":{},\"depth_stencil\":{},\"format\":{},\"rectangle\":[{},{},{},{}]}}",
                json(hex32(command_address)),target,data,json(hex32(reg(0x1D90))),json(hex32(value)),json(hex32(reg(0x208))),rectangle[0],rectangle[1],rectangle[2],rectangle[3]));});
            ++statistics.clears;
        } else if(method==0x1D70) {
            if(reg(0x1A4)!=8 || reg(0x1D6C)>92) throw std::runtime_error("Unbound NV097 semaphore DMA range");
            fence(semaphore_base+reg(0x1D6C),data);
        } else if(method==0x100 && (data==6 || data==7)) {
            if(!owner.completion) throw std::runtime_error("Native graphics completion callbacks are not bound");
            fence(reg(0x1D8C),reg(0x1D90),true);
        } else if(method==0x110) {flush();fence(0,0);poll(true);}
        else if(method==0x12C) {
            flush();select_target();renderer.copy_to_main();target=0;++statistics.flips;
            if(capture.pending) capture_flip();
            if(owner.flip) owner.flip();
        }
        if(indices.size()>32768 || inline_words.size()>2*1024*1024) throw std::runtime_error("NV097 inline draw exceeds its bound");
    }
};
Gpu::Gpu(Renderer& renderer,Memory& memory,std::uint32_t semaphore_base,std::span<const GraphicsObject> profile):impl_(std::make_unique<Impl>(renderer,memory,*this,semaphore_base,profile)) {}
Gpu::~Gpu() {if(impl_->capture.pending) impl_->capture_abort("Game ended before the captured frame completed");}
void Gpu::invalidate_memory(std::uint32_t address,std::uint32_t size) {
    if(!size) return;
    auto& g=*impl_;address&=0x0FFFFFFF;
    const auto end=std::uint64_t(address)+size;
    if(end>0x10000000) throw std::runtime_error("Graphics invalidation wraps guest physical memory");
    const auto overlaps=[&](std::uint32_t start,std::uint64_t bytes) {return bytes && start<end && address<std::uint64_t(start)+bytes;};
    const auto affected=[&](const auto& item) {
        const auto& [key,surface]=item;const auto format=key[2],pitch=key[3];
        const bool swizzled=((format>>8)&15)==2;
        const auto color_bytes=std::uint64_t(swizzled?surface.width*((format&15)==3?2:4):pitch&65535)*surface.height;
        const auto depth_bytes=std::uint64_t(swizzled?surface.width*(((format>>4)&15)==1?2:4):pitch>>16)*surface.height;
        return overlaps(key[0],color_bytes) || (key[1] && overlaps(key[1],depth_bytes));
    };
    if(!std::ranges::any_of(g.textures,[&](const auto& item) {return overlaps(item.first[0],item.second.size);}) &&
       !std::ranges::any_of(g.targets,affected)) return;
    // Submit every queued use of the old views before releasing or recycling them.
    g.emit_arrays();g.flush();
    const auto retired=[&](std::uint32_t id) {
        if(g.capture_active) g.capture_try([&] {g.capture_record(std::format("{{\"type\":\"release\",\"resource\":{}}}",id));});
    };
    for(auto item=g.textures.begin();item!=g.textures.end();) {
        if(!overlaps(item->first[0],item->second.size)) {++item;continue;}
        const auto id=item->second.identifier;retired(id);g.renderer.release_texture(id);
        item=g.textures.erase(item);++g.statistics.texture_evictions;
    }
    for(auto item=g.targets.begin();item!=g.targets.end();) {
        if(!affected(*item)) {++item;continue;}
        const auto& surface=item->second;
        if(g.capture_active) g.capture_try([&] {g.capture_surface(surface.identifier,surface.width,surface.height,item->first);});
        retired(surface.identifier);
        if(g.target==surface.identifier) g.target=0;
        g.renderer.release_target(surface.identifier);item=g.targets.erase(item);++g.statistics.target_evictions;
    }
}
void Gpu::capture_frame(const std::filesystem::path& directory,std::string_view source_sha256) {
    auto& g=*impl_;
    if(directory.empty() || g.capture.pending) throw std::runtime_error("A frame capture is already pending or its directory is empty");
    if(!directory.parent_path().empty()) std::filesystem::create_directories(directory.parent_path());
    if(!std::filesystem::create_directory(directory)) throw std::runtime_error("Frame capture directory already exists");
    g.capture={directory,true};g.capture_active=false;g.capture_source=source_sha256;
    g.capture_bytes=g.capture_text_bytes=g.capture_image_bytes=g.capture_first_frame=0;g.capture_draws=0;g.capture_textures.clear();
    g.capture_try([&] {g.capture_manifest();});
    if(!g.capture.error.empty()) throw std::runtime_error(g.capture.error);
}
const FrameCaptureStatus& Gpu::capture_status() const {return impl_->capture;}
void Gpu::cancel_capture(std::string_view reason) {if(impl_->capture.pending) impl_->capture_abort(reason);}
void Gpu::submit(std::uint32_t begin,std::uint32_t end,std::uint32_t callsite) {
    auto& g=*impl_;
    PerfScope scope(g.renderer.performance(),PerfPhase::gpu_commands);
    if(end<begin || ((end-begin)&3) || end-begin>4*1024*1024) throw std::runtime_error("Invalid native GPU submission range");
    ++g.statistics.submissions;
    g.submission_callsite=callsite;
    // Push buffers are contiguous mapped RAM. Keep their checked mapping while
    // processing live bytes, rather than looking up the same RAM alias per word.
    const auto source_begin=begin;
    const auto* commands=begin<end?static_cast<const std::byte*>(g.memory.access(begin,end-begin)):nullptr;
    const auto load=[&](std::uint32_t address) {
        std::uint32_t value;std::memcpy(&value,commands+(address-source_begin),sizeof(value));
        if constexpr(std::endian::native==std::endian::big) value=std::byteswap(value);
        return value;
    };
    while(begin<end) {
        const auto header=load(begin);begin+=4;++g.statistics.packets;
        if(header&0xA0030003U) throw std::runtime_error(std::format("Unsupported NV2A packet 0x{:08X} at 0x{:08X}",header,begin-4));
        const auto count=(header>>18)&0x7FF,first=header&0x1FFC,subchannel=(header>>13)&7;
        if(count>(end-begin)/4 || (!(header&0x40000000U) && count && first+(count-1)*4>=0x2000))
            throw std::runtime_error("NV2A packet extends beyond its submission or method bank");
        // These aligned packets upload whole rows; dispatching each word repeats
        // the same state checks and address conversions up to 32 times.
        if(!subchannel && g.classes[0]==0x97 && !(header&0x40000000U) &&
           count && count<=32 && !(count&3) && (first==0xB00 || first==0xB80)) {
            const bool code=first==0xB00;
            auto& load=code?g.program_load:g.constant_load;
            const auto capacity=code?g.program.tokens.size():g.constants.size();
            if(load<capacity && count/4<=capacity-load) {
                if(g.array_count) g.emit_arrays();
                const auto bytes=count*4;
                const auto* source=commands+(begin-source_begin);
                std::memcpy(g.registers[0].data()+first/4,source,bytes);
                if(code) std::memcpy(g.program.tokens.data()+load,source,bytes);
                else std::memcpy(g.constants.data()+load,source,bytes);
                load+=count/4;g.statistics.methods+=count;
                if(g.capture_active) g.command_address=begin+bytes-4;
                begin+=bytes;continue;
            }
        }
        if(std::endian::native==std::endian::little && !subchannel && g.classes[0]==0x97 &&
           (header&0x40000000U) && first==0x1818 && count &&
           g.inline_words.size()+count<=2*1024*1024 && g.indices.size()<=32768) {
            if(g.capture_active) g.command_address=begin;
            if(g.array_count) g.emit_arrays();
            const auto previous=g.inline_words.size();const auto bytes=count*4;
            g.inline_words.resize(previous+count);
            std::memcpy(g.inline_words.data()+previous,commands+(begin-source_begin),bytes);
            g.registers[0][first/4]=load(begin+bytes-4);g.statistics.methods+=count;
            if(g.capture_active) g.command_address=begin+bytes-4;
            begin+=bytes;continue;
        }
        for(unsigned word=0;word<count;++word) {
            const auto method=first+((header&0x40000000U)?0:word*4);
            if(g.capture_active) g.command_address=begin;
            g.method(subchannel,method,load(begin));begin+=4;
        }
    }
    g.flush();g.renderer.native_context()->Flush();g.poll(false);
}
bool Gpu::busy() {auto& g=*impl_;g.poll(false);return g.fence_count!=0;}
void Gpu::wait() {auto& g=*impl_;g.emit_arrays();g.flush();g.fence(0,0);g.poll(true);}
void Gpu::snapshot() {
    auto& g=*impl_;g.emit_arrays();g.flush();
    // Main holds the last presented frame. The bound surface may belong to an
    // unfinished frame or an offscreen pass when execution stops.
    if(g.statistics.flips) g.renderer.set_target(0);
    else if(g.target) g.renderer.copy_to_main();
    g.target=0;
}
GpuStats Gpu::stats() const {return impl_->statistics;}
std::string Gpu::check_texture_cache() {
    Renderer renderer(8,8,false,true);
    std::vector<std::byte> ram(64*1024);Memory memory(0,ram);
    constexpr std::array profile{GraphicsObject{0xD,0x97}};
    Gpu gpu(renderer,memory,0,profile);auto& g=*gpu.impl_;
    const auto reg=[&](unsigned method)->std::uint32_t& {return g.registers[0][method/4];};
    reg(0x1B00)=0x80004000;reg(0x1B04)=(0x12U<<8)|(1U<<16)|(2U<<4);
    reg(0x1B10)=32U<<16;reg(0x1B1C)=(8U<<16)|8U;
    std::vector<std::pair<std::string,bool>> cases;
    const auto expect=[&](std::string name,bool passed) {cases.emplace_back(std::move(name),passed);};
    const auto sample=[&](std::uint32_t id) {
        RenderState state;state.primitive=8;state.texture_modes[0]=1;state.textures[0]=id;state.final_inputs={8,0x1800};
        std::array<Vertex,4> vertices{};
        constexpr std::array<Float4,4> positions{{{-1,1,0,1},{1,1,0,1},{1,-1,0,1},{-1,-1,0,1}}};
        constexpr std::array<Float4,4> uv{{{0,0,0,1},{1,0,0,1},{1,1,0,1},{0,1,0,1}}};
        for(unsigned i=0;i<4;++i) {vertices[i].position=positions[i];vertices[i].uv[0]=uv[i];}
        renderer.set_target(0);renderer.prepare(state);renderer.begin();renderer.draw(state,vertices);renderer.end();
        const auto image=renderer.readback();const auto pixel=(4*8+4)*4;
        return (std::uint32_t(image.rgba[pixel])<<16)|(std::uint32_t(image.rgba[pixel+1])<<8)|std::uint32_t(image.rgba[pixel+2]);
    };
    ProgramView view;
    memory.fill_elements<std::uint32_t>(0x4000,0xFFFF0000,64,false);
    const auto original=g.texture(0,view);
    expect("initial texture upload",sample(original)==0xFF0000);
    gpu.invalidate_memory(0x80005000,4096);
    expect("unrelated memory release retains the texture",g.texture(0,view)==original && g.statistics.texture_uploads==1);
    reg(0x1B20)=1;const auto alias=g.texture(0,view);reg(0x1B20)=0;
    expect("distinct views share the same guest allocation",alias!=original && g.textures.size()==2);
    gpu.invalidate_memory(0x80004010,4);
    expect("overlapping release evicts every view and address alias",g.textures.empty() && g.statistics.texture_evictions==2 && !renderer.stats().texture_bytes);
    memory.fill_elements<std::uint32_t>(0x4000,0xFF00FF00,64,false);
    expect("reused allocation uploads its new pixels",sample(g.texture(0,view))==0x00FF00);
    bool cycles=true;
    for(unsigned i=0;i<64;++i) {
        gpu.invalidate_memory(0x4000,256);
        const auto color=i&1?0xFF336699U:0xFFCC8844U;
        memory.fill_elements<std::uint32_t>(0x4000,color,64,false);
        const auto id=g.texture(0,view);
        cycles&=id<=2 && sample(id)==(color&0xFFFFFF);
    }
    expect("64 unload/reload cycles preserve pixels and reuse GPU slots",cycles && renderer.stats().texture_slots==2 && renderer.stats().texture_bytes==256);
    gpu.invalidate_memory(0x4000,256);
    reg(0x200)=reg(0x204)=8U<<16;reg(0x208)=0x128;reg(0x20C)=32U|(32U<<16);reg(0x210)=0x6000;reg(0x214)=0x7000;
    g.select_target();renderer.clear(0xFF112233);
    reg(0x214)=0x8000;g.select_target();const auto shared=g.target;
    expect("shared color storage is counted once",renderer.stats().target_bytes==8*8*20);
    gpu.invalidate_memory(0x80007000,256);
    const auto image=renderer.readback();const auto pixel=(4*8+4)*4;
    expect("retiring one depth allocation preserves its live color alias",g.targets.size()==1 && g.target==shared &&
        image.rgba[pixel]==std::byte{0x11} && image.rgba[pixel+1]==std::byte{0x22} && image.rgba[pixel+2]==std::byte{0x33});
    gpu.invalidate_memory(0x6000,256);
    expect("retiring target storage frees its view and restores the main target",g.targets.empty() && !g.target && renderer.stats().target_bytes==8*8*8);
    memory.fill_elements<std::uint32_t>(0x4000,0xFF0000FF,64,false);
    expect("a retired target slot can hold an ordinary texture",sample(g.texture(0,view))==0x0000FF);
    const TextureData data{8,8,TextureFormat::argb8_linear,Bytes(ram).subspan(0x4000,256),1,32};
    const auto temporary=renderer.upload(data);renderer.release_texture(temporary);
    const auto captured=renderer.upload(data,false);
    expect("capture uploads do not recycle earlier resource IDs",captured!=temporary);
    renderer.release_texture(captured);
    // Original InsertCallback packets use the native completion queue, in order
    // with preceding GPU work and semaphore writes.
    std::vector<std::pair<std::uint32_t,std::uint32_t>> notifications;
    bool ordered=true;
    gpu.completion=[&](std::uint32_t routine,std::uint32_t context) {
        ordered&=memory.load<std::uint32_t>(0x300)==0x12345678;
        notifications.emplace_back(routine,context);
    };
    g.fence(0x300,0x12345678);
    constexpr std::array commands{0x00040000U,0xDU,
        0x00081D8CU,0x1234U,0xAAU,0x00040100U,6U,
        0x00081D8CU,0x5678U,0xBBU,0x00040110U,0U,0x00040100U,7U};
    for(unsigned i=0;i<commands.size();++i) memory.store<std::uint32_t>(0x1000+i*4,commands[i]);
    gpu.submit(0x1000,0x1000+static_cast<unsigned>(commands.size()*4));gpu.wait();
    expect("graphics callbacks preserve packet order, target, context and preceding fences",ordered &&
        notifications==std::vector<std::pair<std::uint32_t,std::uint32_t>>{{0x1234,0xAA},{0x5678,0xBB}} && g.statistics.callbacks==2);
    struct Upload {unsigned first,count,load;bool repeat;};
    constexpr std::array uploads{Upload{0xB00,32,128,false},Upload{0xB80,32,184,false},
        Upload{0xB00,4,135,false},Upload{0xB80,4,191,false},Upload{0xB04,3,135,false},Upload{0xB84,3,191,false},
        Upload{0xB00,8,135,true},Upload{0xB80,8,191,true},Upload{0xB80,33,184,false},
        Upload{0xB00,8,135,false},Upload{0xB80,8,191,false}};
    bool uploads_match=true;
    for(const auto& upload:uploads) {
        g.program_load=g.constant_load=upload.load;
        const auto registers=g.registers[0];const auto program=g.program;
        const auto constants=g.constants;
        const auto methods=g.statistics.methods;
        std::array<std::uint32_t,33> data{};
        for(unsigned i=0;i<upload.count;++i) data[i]=i%3?0x80000000U+i*0x1020304U:0x7F800001U+i;
        std::string scalar_error,packet_error;
        try {for(unsigned i=0;i<upload.count;++i) g.method(0,upload.first+(upload.repeat?0:i*4),data[i]);}
        catch(const std::runtime_error& error) {scalar_error=error.what();}
        const auto expected_registers=g.registers[0];const auto expected_program=g.program;
        const auto expected_constants=g.constants;
        const auto expected_program_load=g.program_load,expected_constant_load=g.constant_load;
        const auto expected_methods=g.statistics.methods;
        g.registers[0]=registers;g.program=program;g.constants=constants;
        g.program_load=g.constant_load=upload.load;g.statistics.methods=methods;
        memory.store<std::uint32_t>(0x1000,(upload.count<<18)|upload.first|(upload.repeat?0x40000000U:0));
        for(unsigned i=0;i<upload.count;++i) memory.store<std::uint32_t>(0x1004+i*4,data[i]);
        try {gpu.submit(0x1000,0x1004+upload.count*4);}
        catch(const std::runtime_error& error) {packet_error=error.what();}
        uploads_match&=scalar_error==packet_error && g.registers[0]==expected_registers && g.program==expected_program &&
            !std::memcmp(&g.constants,&expected_constants,sizeof(g.constants)) && g.program_load==expected_program_load &&
            g.constant_load==expected_constant_load && g.statistics.methods==expected_methods;
    }
    expect("bulk shader uploads match scalar processing, including partial rows and bounds failures",uploads_match);
    memory.map(0x80001000U,std::span(ram).subspan(0x1000));
    bool inline_match=true;
    const auto registers=g.registers[0];const auto methods=g.statistics.methods;
    struct InlineUpload {unsigned count;bool repeat,alias;};
    for(const auto upload:std::array<InlineUpload,6>{{{0,true,false},{1,true,false},{8,true,false},
                                                   {2047,true,false},{3,false,false},{8,true,true}}}) {
        const auto reset=[&] {g.registers[0]=registers;g.statistics.methods=methods;g.inline_words.assign(1,0xAABBCCDDU);};
        reset();
        memory.store<std::uint32_t>(0x1000,(upload.count<<18)|0x1818|(upload.repeat?0x40000000U:0));
        for(unsigned i=0;i<upload.count;++i) {
            const auto data=0x3F800000U+i*0x10203;
            memory.store<std::uint32_t>(0x1004+i*4,data);
            g.method(0,0x1818+(upload.repeat?0:i*4),data);
        }
        const auto expected_registers=g.registers[0];const auto expected_words=g.inline_words;
        const auto expected_methods=g.statistics.methods;
        reset();const auto begin=upload.alias?0x80001000U:0x1000U;
        gpu.submit(begin,begin+4+upload.count*4);
        inline_match&=g.registers[0]==expected_registers && g.inline_words==expected_words && g.statistics.methods==expected_methods;
    }
    g.inline_words.clear();
    expect("inline vertex packets match scalar processing, including RAM aliases and maximum packet size",inline_match);
    const auto errors=renderer.errors();expect("D3D11 validation clean",errors.empty());
    const bool passed=std::ranges::all_of(cases,[](const auto& item) {return item.second;});
    std::ostringstream output;output<<std::format("{{\"format\":\"smbd-texture-cache-v1\",\"adapter\":{},\"passed\":{},\"reload_cycles\":64,\"resident_texture_bytes\":{},\"cases\":[",
        json(renderer.adapter()),passed,renderer.stats().texture_bytes);
    for(unsigned i=0;i<cases.size();++i) {if(i) output<<',';output<<std::format("{{\"name\":{},\"passed\":{}}}",json(cases[i].first),cases[i].second);}
    output<<"],\"errors\":[";
    for(unsigned i=0;i<errors.size();++i) {if(i) output<<',';output<<json(errors[i]);}
    return output.str()+"]}";
}
}
