#include "upscaler_model.h"
#include <bit>
#include <format>
#include <map>

namespace smbd {
namespace {
// PyTorch's zip checkpoint stores each tensor as archive/data/<storage key>.
// The keys are allocation identifiers, so this order comes from data.pkl,
// inspected once with a whitelist-only unpickler (knowledge/hud_upscaling.txt).
// The pinned SHA-256 fixes the archive, so no pickle is executed or parsed here.
constexpr std::array<std::string_view,53> storage_keys{
    "94173432687680","94173485819440","94173431317632","94173430607232","94173434055008","94173430579408",
    "94173434707312","94173436388064","94173437731584","94173434039184","94173436016032","94173436051696",
    "94173433380880","94173434050832","94173432718336","94173441930368","94173434056272","94173432013680",
    "94173439748256","94173485818288","94173432019808","94173441392608","94173440419872","94173441955744",
    "94173430572560","94173438105296","94173432017824","94173432709280","94173432030656","94173485836608",
    "94173434050304","94173439075360","94173431291136","94173441927424","94173431995968","94173431991584",
    "94173441395360","94173437733280","94173431289104","94173485819632","94173438706336","94173432041696",
    "94173439747616","94173485847712","94173432663104","94173436759120","94173430609200","94173441479888",
    "94173431301936","94173431302240","94173431302032","94173433360608","94173432020400"};
}
std::array<UpscalerLayer,upscaler_layers> upscaler_layout() {
    std::array<UpscalerLayer,upscaler_layers> layers{};
    std::size_t offset=0;
    for(unsigned l=0;l<upscaler_layers;++l) {
        auto& layer=layers[l];
        layer.inputs=l?upscaler_features:3;layer.outputs=l+1<upscaler_layers?upscaler_features:3*upscaler_scale*upscaler_scale;
        layer.weights=offset;offset+=std::size_t(layer.outputs)*layer.inputs*9;
        layer.bias=offset;offset+=layer.outputs;
        layer.slope=l+1<upscaler_layers?offset:SIZE_MAX;
        if(l+1<upscaler_layers) offset+=layer.outputs;
    }
    return layers;
}
std::size_t upscaler_model_floats() {
    const auto last=upscaler_layout().back();
    return last.bias+last.outputs;
}
std::vector<float> read_upscaler_model(const std::filesystem::path& path) {
    File file(path);
    if(file.size()!=upscaler_model_bytes) throw std::runtime_error("The HUD upscaler model has an unexpected size");
    const auto bytes=file.read(0,static_cast<std::size_t>(file.size()));
    if(sha256(bytes)!=upscaler_model_sha256) throw std::runtime_error("The HUD upscaler model differs from the verified checkpoint");
    // Locate stored entries through the central directory; the local headers
    // use data descriptors, so only the directory carries their sizes.
    const Bytes data(bytes);
    std::size_t end=data.size()<22?SIZE_MAX:data.size()-22;
    while(end!=SIZE_MAX && u32(data,end)!=0x06054B50U) end=end?end-1:SIZE_MAX;
    if(end==SIZE_MAX) throw std::runtime_error("The HUD upscaler model lacks its archive directory");
    const auto entries=u16(data,end+10);
    std::size_t cursor=u32(data,end+16);
    struct Entry {std::size_t offset,size;};
    std::map<std::string,Entry,std::less<>> stored;
    for(unsigned i=0;i<entries;++i) {
        if(cursor+46>data.size() || u32(data,cursor)!=0x02014B50U) throw std::runtime_error("Invalid HUD upscaler archive entry");
        const auto method=u16(data,cursor+10),name_size=u16(data,cursor+28),extra=u16(data,cursor+30),comment=u16(data,cursor+32);
        const std::size_t size=u32(data,cursor+24),compressed=u32(data,cursor+20),local=u32(data,cursor+42);
        if(cursor+46+name_size>data.size()) throw std::runtime_error("Invalid HUD upscaler archive name");
        std::string name(reinterpret_cast<const char*>(data.data()+cursor+46),name_size);
        if(method!=0 || compressed!=size || local+30>data.size() || u32(data,local)!=0x04034B50U)
            throw std::runtime_error("HUD upscaler archive entries must be stored");
        const std::size_t start=local+30+u16(data,local+26)+u16(data,local+28);
        if(start+size>data.size()) throw std::runtime_error("HUD upscaler archive entry exceeds its file");
        stored.emplace(std::move(name),Entry{start,size});
        cursor+=46+name_size+extra+comment;
    }
    std::vector<float> model(upscaler_model_floats());
    std::size_t key=0;
    const auto copy=[&](std::size_t offset,std::size_t count) {
        const auto found=stored.find(std::format("archive/data/{}",storage_keys.at(key++)));
        if(found==stored.end() || found->second.size!=count*4) throw std::runtime_error("HUD upscaler tensor differs from its architecture");
        for(std::size_t i=0;i<count;++i) model[offset+i]=std::bit_cast<float>(u32(data,found->second.offset+i*4));
    };
    for(const auto& layer:upscaler_layout()) {
        copy(layer.weights,std::size_t(layer.outputs)*layer.inputs*9);
        copy(layer.bias,layer.outputs);
        if(layer.slope!=SIZE_MAX) copy(layer.slope,layer.outputs);
    }
    if(key!=storage_keys.size()) throw std::runtime_error("HUD upscaler tensor count differs");
    return model;
}
}
