#include "shaders.h"
#include "xbe.h"
#ifdef _WIN32
#include <d3dcompiler.h>
#include "com_ptr.h"
#else
#include <vkd3d_shader.h>
#endif
#include <algorithm>
#include <format>
#include <map>
#include <sstream>
#include <stdexcept>

namespace smbd {
namespace {
bool builtin_shader(ShaderKind kind) {
#ifdef _WIN32
    if(kind==ShaderKind::gui_vertex || kind==ShaderKind::gui_pixel) return false;
#endif
    return kind!=ShaderKind::vertex && kind!=ShaderKind::launch;
}
struct Key {
    ShaderKind kind;VertexProgram program{};
    auto operator<=>(const Key&) const = default;
};
std::map<Key,std::vector<std::byte>> bytecode;
std::string source(ShaderKind kind,const VertexProgram* program) {
    if(kind==ShaderKind::vertex || kind==ShaderKind::launch) {
        if(!program) throw std::runtime_error("A vertex shader requires its original program");
        return translate_vertex_program(*program,kind==ShaderKind::launch).source;
    }
    return shader_source(kind);
}
void array(std::ostringstream& output,std::string_view name,Bytes bytes) {
    // Keep large shader banks cheap to compile. An integer initializer for
    // every byte would require tens of millions of syntax nodes on Linux.
    constexpr std::string_view digits="0123456789ABCDEF";
    output<<"alignas(4) const char "<<name<<"[]=\n";
    for(std::size_t i=0;i<bytes.size();++i) {
        if(i%32==0) output<<'"';
        const auto byte=std::to_integer<unsigned>(bytes[i]);
        output<<"\\x"<<digits[byte>>4]<<digits[byte&15];
        if(i%32==31 || i+1==bytes.size()) output<<"\"\n";
    }
    output<<"\"\";\n";
}
void program(std::ostringstream& output,const StaticVertex& value) {
    output<<"StaticVertex{VertexProgram{VertexTokens{{";
    for(const auto& token:value.program.tokens) {
        output<<std::format("{{0x{:08X},0x{:08X},0x{:08X},0x{:08X}}},",token[0],token[1],token[2],token[3]);
        if(token[3]&1) break;
    }
    output<<"}},0},"<<(value.launch?"true":"false")<<"},\n";
}
}
static ShaderBinary compile_binary(ShaderKind kind,const VertexProgram* program=nullptr) {
    Key key{kind};if(program) key.program=normalize_vertex_program(*program);
    if(const auto found=bytecode.find(key);found!=bytecode.end()) return {found->second,false};
    const auto hlsl=source(kind,program?&key.program:nullptr);
#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3DBlob> code,errors,compact;
    auto status=D3DCompile(hlsl.data(),hlsl.size(),"smbd-offline",nullptr,nullptr,"main",shader_profile(kind),
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(status)) throw std::runtime_error(std::format("Offline shader {}: {}",unsigned(kind),
        errors?static_cast<const char*>(errors->GetBufferPointer()):"unknown compiler error"));
    status=D3DStripShader(code->GetBufferPointer(),code->GetBufferSize(),
        D3DCOMPILER_STRIP_DEBUG_INFO|D3DCOMPILER_STRIP_REFLECTION_DATA,&compact);
    if(FAILED(status)) throw std::runtime_error("Could not strip offline shader metadata");
    const auto* data=static_cast<const std::byte*>(compact->GetBufferPointer());
    const auto size=compact->GetBufferSize();
#else
    vkd3d_shader_hlsl_source_info hlsl_info{};
    hlsl_info.type=VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl_info.entry_point="main";
    hlsl_info.profile=shader_profile(kind);
    const vkd3d_shader_compile_option options[]={{VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG,1}};
    vkd3d_shader_compile_info info{};
    info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.next=&hlsl_info;
    info.source={hlsl.data(),hlsl.size()};info.source_type=VKD3D_SHADER_SOURCE_HLSL;
    info.target_type=VKD3D_SHADER_TARGET_DXBC_TPF;info.options=options;info.option_count=1;
    info.log_level=VKD3D_SHADER_LOG_WARNING;info.source_name="smbd-offline";
    struct Output {
        vkd3d_shader_code code{};
        char* messages=nullptr;
        ~Output() {vkd3d_shader_free_shader_code(&code);vkd3d_shader_free_messages(messages);}
    } output;
    if(vkd3d_shader_compile(&info,&output.code,&output.messages)<0)
        throw std::runtime_error(std::format("Offline shader {}: {}",unsigned(kind),output.messages?output.messages:"unknown compiler error"));
    const auto* data=static_cast<const std::byte*>(output.code.code);
    const auto size=output.code.size;
#endif
    auto [stored,inserted]=bytecode.emplace(std::move(key),std::vector(data,data+size));
    return {stored->second,inserted};
}
#ifndef SMBD_COMPILED_SHADERS
ShaderBinary shader_binary(ShaderKind kind,const VertexProgram* program) {return compile_binary(kind,program);}
std::span<const StaticVertex> static_vertex_shaders() {return {};}
#endif
std::string compile_shaders(const std::filesystem::path& executable,const std::filesystem::path& destination,bool replace) {
    Xbe image(executable);if(!image.supported()) throw std::runtime_error("Shader generation requires the verified executable");
    if(destination.extension()!=".cpp") throw std::runtime_error("Shader output must be a .cpp file");
    if(std::filesystem::exists(destination) && std::filesystem::equivalent(destination,executable))
        throw std::runtime_error("Shader output cannot replace the original executable");
    std::vector<StaticVertex> vertices;
    const auto add=[&](Bytes bytes,unsigned count,bool launch) {
        if(!count || count>136) throw std::runtime_error("Invalid original shader length");
        if(bytes.size()!=count*16) throw std::runtime_error("Original shader length differs from its header");
        StaticVertex value{{},launch};
        for(unsigned slot=0;slot<count;++slot) for(unsigned word=0;word<4;++word) value.program.tokens[slot][word]=u32(bytes,slot*16+word*4);
        if(!(value.program.tokens[count-1][3]&1)) throw std::runtime_error("Original shader lacks its final marker");
        value.program=normalize_vertex_program(value.program);
        compile_binary(launch?ShaderKind::launch:ShaderKind::vertex,&value.program);
        if(std::ranges::none_of(vertices,[&](const StaticVertex& prior){return prior.launch==launch && prior.program==value.program;}))
            vertices.push_back(value);
    };
    unsigned title_records=0;
    const auto media=executable.parent_path()/"media";
    for(const auto& file:std::filesystem::recursive_directory_iterator(media)) {
        if(!file.is_regular_file() || file.path().extension()!=".xvu") continue;
        if(++title_records>4096) throw std::runtime_error("Shader asset count exceeds its bound");
        File input(file.path());
        if(input.size()<4 || input.size()>4+136*16) throw std::runtime_error("Shader asset exceeds its format bound");
        const auto data=input.read(0,static_cast<std::size_t>(input.size()));
        if(u16(data,0)!=0x2078) throw std::runtime_error("Unsupported shader asset version: "+path_text(file.path()));
        add(Bytes(data).subspan(4),u16(data,2),false);
    }
    if(!title_records) throw std::runtime_error("No extracted vertex shader assets were found");
    // Original 31D4D0 selects these three SDK passthrough programs and copies
    // 48, 44 or 48 DWORDs respectively. They are not title-specific guesses.
    add(image.data(0x00329450,12*16),12,false);
    add(image.data(0x003293A0,11*16),11,false);
    add(image.data(0x003292E0,12*16),12,false);
    std::ostringstream output;output<<"// Generated ahead of time from the verified XBE and native HLSL.\n#include \"shaders.h\"\n#include <stdexcept>\nnamespace smbd {namespace {\n";
    std::size_t total=0;unsigned builtins=0;
    for(unsigned i=0;i<unsigned(ShaderKind::count);++i) {
        if(!builtin_shader(ShaderKind(i))) continue;
        const auto binary=compile_binary(ShaderKind(i));++builtins;total+=binary.bytes.size();array(output,"builtin_"+std::to_string(i),binary.bytes);
    }
    for(unsigned i=0;i<vertices.size();++i) {
        const auto& value=vertices[i];const auto binary=compile_binary(value.launch?ShaderKind::launch:ShaderKind::vertex,&value.program);
        total+=binary.bytes.size();array(output,"vertex_"+std::to_string(i),binary.bytes);
    }
    output<<std::format("const std::array<StaticVertex,{}> programs{{{{\n",vertices.size());
    for(const auto& value:vertices) program(output,value);
    output<<"}};\n}\nstd::span<const StaticVertex> static_vertex_shaders(){return programs;}\nShaderBinary shader_binary(ShaderKind kind,const VertexProgram* program){\nswitch(kind){\n";
    for(unsigned i=0;i<unsigned(ShaderKind::count);++i) {
        if(!builtin_shader(ShaderKind(i))) continue;
        output<<std::format("case ShaderKind({}):return {{Bytes(reinterpret_cast<const std::byte*>(builtin_{}),sizeof(builtin_{})-1),false}};\n",i,i,i);
    }
    output<<"default:break;}\nif(!program) throw std::runtime_error(\"Missing static shader program\");\nconst auto key=normalize_vertex_program(*program);\n";
    for(unsigned i=0;i<vertices.size();++i)
        output<<std::format("if(kind==ShaderKind::{} && key==programs[{}].program) return {{Bytes(reinterpret_cast<const std::byte*>(vertex_{}),sizeof(vertex_{})-1),false}};\n",
            vertices[i].launch?"launch":"vertex",i,i,i);
    output<<"throw std::runtime_error(\"Vertex program is missing from the compiled shader bank\");}\n}\n";
    write_text(destination,output.str(),replace);
    return std::format("{{\"format\":\"smbd-shaders-aot-v1\",\"sha256\":{},\"title_records\":{},\"sdk_programs\":3,\"vertex_programs\":{},\"builtin_shaders\":{},\"bytecode_bytes\":{},\"runtime_compiler\":false}}",
        json(image.sha256),title_records,vertices.size(),builtins,total);
}
}
