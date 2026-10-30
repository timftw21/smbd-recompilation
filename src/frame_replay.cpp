#include "gpu.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>

namespace smbd {
namespace {
// Read only the bounded capture schema, one JSONL record at a time.
std::size_t skip_value(std::string_view text,std::size_t first,unsigned depth=0) {
    if(first>=text.size() || depth>32) throw std::runtime_error("Malformed capture JSON");
    if(text[first]=='"') {
        for(auto i=first+1;i<text.size();++i) {
            if(text[i]=='\\') ++i;
            else if(text[i]=='"') return i+1;
        }
    } else if(text[first]=='[' || text[first]=='{') {
        const auto end=text[first]=='['?']':'}';
        for(auto i=first+1;i<text.size();) {
            if(text[i]==end) return i+1;
            if(std::isspace(static_cast<unsigned char>(text[i])) || text[i]==',' || text[i]==':') ++i;
            else i=skip_value(text,i,depth+1);
        }
    } else {
        const auto end=text.find_first_of(",]} \t\r\n",first);
        if(end==first) throw std::runtime_error("Unexpected capture JSON delimiter");
        return end==text.npos?text.size():end;
    }
    throw std::runtime_error("Unterminated capture JSON value");
}
struct Json {
    std::string_view text;
    std::string_view string() const {
        if(text.size()<2 || text.front()!='"' || text.back()!='"' || text.find('\\')!=text.npos)
            throw std::runtime_error("Expected a plain capture string");
        return text.substr(1,text.size()-2);
    }
    std::uint32_t number() const {
        std::uint32_t value=0;
        const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
        if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid capture number");
        return value;
    }
    std::uint32_t hex() const {
        const auto value=string();std::uint32_t bits=0;
        if(!value.starts_with("0x")) throw std::runtime_error("Invalid capture address");
        const auto result=std::from_chars(value.data()+2,value.data()+value.size(),bits,16);
        if(result.ec!=std::errc{} || result.ptr!=value.data()+value.size()) throw std::runtime_error("Invalid capture address");
        return bits;
    }
    Json at(std::string_view name) const {
        if(text.empty() || text.front()!='{') throw std::runtime_error("Expected capture object");
        for(std::size_t i=1;i<text.size();) {
            if(text[i]==',' || std::isspace(static_cast<unsigned char>(text[i]))) {++i;continue;}
            if(text[i]=='}') break;
            const auto end=skip_value(text,i);const Json key{text.substr(i,end-i)};i=end;
            while(i<text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if(i==text.size() || text[i++]!=':') throw std::runtime_error("Invalid capture object field");
            while(i<text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            const auto finish=skip_value(text,i);
            if(key.string()==name) return {text.substr(i,finish-i)};
            i=finish;
        }
        throw std::runtime_error("Missing capture field: "+std::string(name));
    }
    std::vector<Json> array(unsigned limit) const {
        if(text.empty() || text.front()!='[') throw std::runtime_error("Expected capture array");
        std::vector<Json> result;
        for(std::size_t i=1;i<text.size();) {
            if(text[i]==',' || std::isspace(static_cast<unsigned char>(text[i]))) {++i;continue;}
            if(text[i]==']') return result;
            if(result.size()==limit) throw std::runtime_error("Capture array exceeds its bound");
            const auto end=skip_value(text,i);result.push_back({text.substr(i,end-i)});i=end;
        }
        throw std::runtime_error("Unterminated capture array");
    }
};
template<class F> void records(const std::filesystem::path& path,F&& action) {
    if(std::filesystem::file_size(path)>48*1024*1024) throw std::runtime_error("Frame trace exceeds 48 MiB");
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("Cannot open frame trace");
    std::vector<char> line(256*1024);
    while(input.getline(line.data(),static_cast<std::streamsize>(line.size()))) {
        const auto size=std::strlen(line.data());
        if(size && !action(Json{std::string_view(line.data(),size)})) return;
    }
    if(!input.eof()) throw std::runtime_error("Frame trace record exceeds 256 KiB or could not be read");
}
unsigned nibble(char value) {
    if(value>='0' && value<='9') return value-'0';
    if(value>='A' && value<='F') return value-'A'+10;
    if(value>='a' && value<='f') return value-'a'+10;
    throw std::runtime_error("Invalid captured hexadecimal bytes");
}
template<class T> void restore(T& value,Json source) {
    const auto text=source.string();auto bytes=std::as_writable_bytes(std::span(&value,1));
    if(text.size()!=bytes.size()*2) throw std::runtime_error("Captured state has the wrong size");
    for(std::size_t i=0;i<bytes.size();++i) bytes[i]=std::byte(nibble(text[i*2])*16+nibble(text[i*2+1]));
}
std::vector<std::byte> payload(const std::filesystem::path& root,Json name,std::uint32_t expected) {
    const auto file=name.string();
    if(file.empty() || file=="." || file==".." || file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")!=file.npos)
        throw std::runtime_error("Invalid captured resource filename");
    if(expected>64*1024*1024) throw std::runtime_error("Captured resource exceeds 64 MiB");
    File input(root/std::string(file));
    if(input.size()!=expected) throw std::runtime_error("Captured resource has the wrong size");
    return input.read(0,expected);
}
}
std::string replay_frame(const std::filesystem::path& capture,const std::filesystem::path& output,
                         std::string_view sha,std::uint32_t through,std::uint32_t only,bool join_arrays) {
    const auto started=std::chrono::steady_clock::now();
    const auto original=load_image(capture/"frame.png");
    unsigned scale=1;
    records(capture/"draws.jsonl",[&](Json record) {
        if(record.at("type").string()=="header" && record.text.find("\"resolution_scale\"")!=record.text.npos) scale=record.at("resolution_scale").number();
        return false;
    });
    if(scale<1 || scale>4 || original.width%scale || original.height%scale) throw std::runtime_error("Invalid captured resolution scale");
    Renderer renderer(original.width/scale,original.height/scale,false,true,scale);
    struct Surface {std::array<std::uint32_t,4> key;std::uint32_t width,height,native;};
    std::map<std::uint32_t,Surface> surfaces;
    std::map<std::uint32_t,std::uint32_t> resources;
    std::set<std::uint32_t> retired;
    std::uint64_t payload_bytes=0;
    const auto read_payload=[&](Json name,std::uint32_t size) {
        if(size>64*1024*1024-payload_bytes) throw std::runtime_error("Replay resources exceed 64 MiB");
        payload_bytes+=size;return payload(capture,name,size);
    };
    bool verified=false;
    records(capture/"draws.jsonl",[&](Json record) {
        const auto type=record.at("type").string();
        if(type=="header") {
            if(record.at("format").string()!="smbd-gpu-frame-v1" || record.at("source_sha256").string()!=sha)
                throw std::runtime_error("Frame capture does not match the verified executable");
            verified=true;
        } else if(type=="release") retired.insert(record.at("resource").number());
        else if(type=="surface") {
            if(surfaces.size()==64) throw std::runtime_error("Capture exceeds the surface bound");
            Surface s{{record.at("color").hex(),record.at("depth").hex(),record.at("format").hex(),record.at("pitch").hex()},
                record.at("width").number(),record.at("height").number(),0};
            std::uint32_t shared=UINT32_MAX;
            for(const auto& [id,other]:surfaces) if(!retired.contains(id) && s.key[0]==other.key[0] && (s.key[2]&~255U)==(other.key[2]&~255U) &&
                ((s.key[2]&15)==3)==((other.key[2]&15)==3) && (s.key[3]&65535)==(other.key[3]&65535) && s.width==other.width && s.height==other.height) shared=other.native;
            s.native=renderer.create_target(s.width,s.height,(s.key[2]&15)==3?SurfaceColor::rgb565:SurfaceColor::rgba8,
                ((s.key[2]>>4)&15)==1?SurfaceDepth::z16:SurfaceDepth::z24s8,shared);
            const auto id=record.at("resource").number();resources.emplace(id,s.native);surfaces.emplace(id,s);
            renderer.set_target(s.native);renderer.clear(0);
        }
        return true;
    });
    if(!verified || surfaces.empty()) throw std::runtime_error("Frame capture has no verified header or surfaces");
    struct Draw {
        std::array<std::uint32_t,2048> registers{};
        VertexProgram program;VertexConstants constants;ProgramVertex current;ProgramView view;
        RenderState state;VertexLayout layout;
        std::vector<std::vector<std::byte>> buffers;
        std::vector<std::uint32_t> strides,addresses,indices;
        std::uint32_t first=0,count=0,command=0,target=0;
        bool arrays=true;
    };
    std::optional<Draw> pending;
    std::uint32_t draws=0,selected=0,last_target=0;
    const auto emit=[&] {
        if(!pending) return;
        const auto& d=*pending;last_target=d.target;renderer.set_target(last_target);
        const auto native=renderer.prepare_layout(prepare_gpu_program(renderer,d.registers,d.program),d.layout);
        std::vector<VertexStream> streams;
        for(unsigned i=0;i<d.buffers.size();++i) streams.push_back({d.buffers[i],d.strides[i]});
        renderer.prepare(d.state);renderer.begin(0,1,false);
        renderer.draw_streams(d.state,native,d.constants,d.view,streams,d.current,d.count,d.indices);renderer.end();
        ++draws;pending.reset();
    };
    records(capture/"draws.jsonl",[&](Json record) {
        const auto type=record.at("type").string();
        if(type=="clear" || type=="blit" || type=="release") emit();
        if(type=="texture") {
            auto bytes=read_payload(record.at("guest_data"),record.at("size").number());
            const auto texture=renderer.upload({record.at("width").number(),record.at("height").number(),TextureFormat(record.at("format").number()),bytes,
                record.at("mips").number(),record.at("pitch").number(),record.at("cubemap").text=="true",record.at("depth").number()});
            if(!resources.emplace(record.at("resource").number(),texture).second) throw std::runtime_error("Duplicate captured resource");
        } else if(type=="clear" && only==UINT32_MAX) {
            last_target=resources.at(record.at("target").number());renderer.set_target(last_target);
            const auto value=record.at("depth_stencil").hex(),format=record.at("format").hex();
            const auto rectangle=record.at("rectangle").array(4);if(rectangle.size()!=4) throw std::runtime_error("Invalid captured clear rectangle");
            std::array<std::int32_t,4> bounds{};for(unsigned i=0;i<4;++i) bounds[i]=static_cast<std::int32_t>(rectangle[i].number());
            renderer.clear_buffers(record.at("flags").number(),record.at("color").hex(),((format>>4)&15)==1?float(value&65535)/65535:float(value>>8)/16777215,
                static_cast<std::uint8_t>(value),bounds);
        } else if(type=="blit" && only==UINT32_MAX) {
            const auto from=record.at("source_xy").array(2),to=record.at("destination_xy").array(2),size=record.at("size").array(2);
            if(from.size()!=2 || to.size()!=2 || size.size()!=2) throw std::runtime_error("Invalid captured copy rectangle");
            renderer.copy_region(resources.at(record.at("destination").number()),resources.at(record.at("source").number()),
                from[0].number(),from[1].number(),to[0].number(),to[1].number(),size[0].number(),size[1].number());
        } else if(type=="draw") {
            const auto draw=record.at("draw").number();
            if(draw>through) return false;
            if(only!=UINT32_MAX && draw!=only) return true;
            Draw next;
            auto& registers=next.registers;auto& program=next.program;auto& constants=next.constants;
            auto& current=next.current;auto& view=next.view;
            restore(registers,record.at("registers"));restore(program.tokens,record.at("program"));program.start=record.at("program_start").number();
            registers[0x1810/4]=0; // Only this range word changes within a DRAW_ARRAYS packet.
            restore(constants,record.at("constants"));restore(current.attributes,record.at("current"));restore(view,record.at("view"));
            auto state=gpu_render_state(registers,record.at("primitive").number());
            const auto textures=record.at("textures").array(4);if(textures.size()!=4) throw std::runtime_error("Invalid captured textures");
            for(unsigned i=0;i<4;++i) if(const auto id=textures[i].number()) state.textures[i]=resources.at(id);
            const auto attributes=record.at("attributes").array(16);if(attributes.size()!=16) throw std::runtime_error("Invalid captured vertex layout");
            auto& layout=next.layout;
            for(unsigned i=0;i<16;++i) {const auto a=attributes[i].array(3);if(a.size()!=3) throw std::runtime_error("Invalid captured attribute");layout[i]={a[0].number(),a[1].number(),a[2].number()};}
            for(const auto stream:record.at("streams").array(16)) {
                next.buffers.push_back(read_payload(stream.at("data"),stream.at("size").number()));
                next.strides.push_back(stream.at("stride").number());
                const auto address=stream.at("address");next.addresses.push_back(address.text=="null"?0:address.hex());
                next.arrays&=address.text!="null";
            }
            const auto index=record.at("indices");const auto count=index.at("count").number();
            if(count>32768 || index.at("element_bytes").number()!=4) throw std::runtime_error("Captured index data exceeds its bound or has the wrong format");
            next.indices.resize(count);next.arrays&=count==0;
            if(count) {const auto bytes=read_payload(index.at("data"),count*4);std::memcpy(next.indices.data(),bytes.data(),bytes.size());}
            next.target=resources.at(record.at("target").number());next.state=state;
            next.first=record.at("first").number();next.count=record.at("count").number();next.command=record.at("command_address").hex();
            if(!next.count || next.count>32768) throw std::runtime_error("Captured vertex count exceeds its bound");
            bool merge=join_arrays && pending && next.arrays && pending->arrays && next.command==pending->command+4 &&
                next.first==pending->first+pending->count && next.registers==pending->registers && next.program==pending->program &&
                next.layout==pending->layout && next.target==pending->target && next.strides==pending->strides && next.state.textures==pending->state.textures &&
                !std::memcmp(&next.constants,&pending->constants,sizeof(constants)) && !std::memcmp(&next.current,&pending->current,sizeof(current)) &&
                !std::memcmp(&next.view,&pending->view,sizeof(view));
            for(unsigned i=0;merge && i<next.addresses.size();++i)
                merge=std::uint64_t(next.addresses[i])==std::uint64_t(pending->addresses[i])+std::uint64_t(pending->count)*next.strides[i];
            if(merge) {
                if(next.count>32768-pending->count) throw std::runtime_error("Joined array primitive exceeds its vertex bound");
                for(unsigned i=0;i<next.buffers.size();++i) {
                    auto& buffer=pending->buffers[i];const auto offset=std::size_t(pending->count)*next.strides[i];
                    if(offset+next.buffers[i].size()>8*1024*1024) throw std::runtime_error("Joined vertex stream exceeds 8 MiB");
                    buffer.resize(offset);buffer.insert(buffer.end(),next.buffers[i].begin(),next.buffers[i].end());
                }
                pending->count+=next.count;pending->command=next.command;
            } else {emit();pending=std::move(next);}
            if(++selected>2048) throw std::runtime_error("Frame exceeds the captured draw bound");
            if(only!=UINT32_MAX) return false;
        }
        return true;
    });
    emit();
    if(!draws) throw std::runtime_error("No captured draws selected");
    renderer.set_target(last_target);renderer.copy_to_main();const auto actual=renderer.readback();
    Image difference{actual.width,actual.height,std::vector<std::byte>(actual.rgba.size())};std::uint32_t changed=0;
    for(std::size_t i=0;i<actual.rgba.size();i+=4) {
        bool differs=false;
        for(unsigned c=0;c<3;++c) {const auto delta=std::abs(int(actual.rgba[i+c])-int(original.rgba[i+c]));difference.rgba[i+c]=std::byte(delta);differs|=delta!=0;}
        difference.rgba[i+3]=std::byte{255};changed+=differs;
    }
    if(!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    if(!std::filesystem::create_directory(output)) throw std::runtime_error("Replay output directory already exists");
    save_png(actual,output/"frame.png");save_png(difference,output/"difference.png");
    const auto errors=renderer.errors();std::string messages;
    for(const auto& message:errors) {if(!messages.empty()) messages+=',';messages+=json(message);}
    const auto result=std::format("{{\"format\":\"smbd-frame-replay-v1\",\"source_sha256\":{},\"adapter\":{},\"passed\":{},\"draws\":{},\"captured_draws_selected\":{},\"join_arrays\":{},\"pixels_changed_from_capture\":{},\"replay_ms\":{},\"errors\":[{}]}}",
        json(sha),json(renderer.adapter()),errors.empty(),draws,selected,join_arrays,changed,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),messages);
    write_text(output/"report.json",result+'\n',false);return result;
}
}
