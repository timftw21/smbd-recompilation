#include "subtitles.h"
#include "disc.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <stdexcept>

namespace smbd {
namespace {
using namespace std::literals;
// Script layout: u32 entry count, u32 table offset (8), then one offset per
// 64-byte entry. Entry +00 points to a cue label string and +2C to the text;
// 6B2E5..6B2F6 relocate both by the buffer base. Text uses \tTIME reveal
// marks, \pN styles and \l page breaks; the renderer at 6B935 treats a byte
// with its high bit set as the first of two bytes.
//
// Each edit replaces `remove` bytes at `offset` with `insert`, shifting the
// rest of [offset, window_end) and keeping the window's length: a shorter
// text leaves zeros at the window end, a longer one consumes zero padding.
// When a text grows into the cue label that follows it, the window moves that
// label into its own padding and a pointer edit updates its entry.
struct SubtitleEdit {std::uint32_t offset,remove;std::string_view insert;std::uint32_t window_end;};
struct SubtitlePointer {std::uint32_t offset,value;};
struct SubtitleFile {
    std::string_view name;std::uint32_t size;std::string_view sha256;
    std::span<const SubtitleEdit> edits;std::span<const SubtitlePointer> pointers;
};
// "any more" -> "anymore".
constexpr SubtitleEdit edits_1b[]{{0x07E0,1,""sv,0x08AF}};
// "So long, everyone!" and "Hang on, everyone!" (vocative commas).
constexpr SubtitleEdit edits_3[]{{0x0B0C,0,","sv,0x0B2A},{0x103A,0,","sv,0x1058}};
constexpr SubtitlePointer pointers_3[]{{0x0260,0x0B27},{0x05E0,0x1055}};
// "at high-speed" -> "at high speed".
constexpr SubtitleEdit edits_4[]{{0x0BFE,1," "sv,0x0C43}};
// "Aaaah..., Oh, no!" -> "Aaaah... Oh, no!".
constexpr SubtitleEdit edits_5[]{{0x1288,1,""sv,0x12FD}};
// "forget, AIAI!", "onto the planet", "That means you'll".
constexpr SubtitleEdit edits_9[]{{0x031F,0,","sv,0x033A},{0x038D,1,""sv,0x03AA},{0x040D,1,""sv,0x04A1}};
constexpr SubtitlePointer pointers_9[]{{0x0040,0x0334}};
// Endings: "I know it's a bery hard" loses its stray article in every script.
constexpr SubtitleEdit edits_end1[]{{0x0775,2,""sv,0x07A9}};
// "a long way to go / to become an Ultimate player."; the leftover Japanese
// two-byte code 84 D6 indexes outside the English glyph table at 39E6D8, and
// the parallel lines use "!".
constexpr SubtitleEdit edits_end1u[]{{0x0B71,2,""sv,0x0BA5},{0x0E02,20,"go\\t0.8\\t0.9\nto become an "sv,0x0E3E},{0x0EF1,2,"!"sv,0x0F03}};
constexpr SubtitlePointer pointers_end1u[]{{0x0580,0x0E3A}};
constexpr SubtitleEdit edits_end2[]{{0x09D1,2,""sv,0x0A05}};
constexpr SubtitleEdit edits_end2u[]{{0x088E,2,"!"sv,0x089E},{0x0BD1,2,""sv,0x0C05},
    {0x0E62,20,"go\\t0.8\\t0.9\nto become an "sv,0x0E9E},{0x0F51,2,"!"sv,0x0F63}};
constexpr SubtitlePointer pointers_end2u[]{{0x0580,0x0E9A}};
constexpr SubtitleEdit edits_end3[]{{0x08F1,2,""sv,0x0925}};
// Adds the missing final period and removes a duplicated "pwayer! " that
// begins the next speaker's line.
constexpr SubtitleEdit edits_end3u[]{{0x0D69,0,"."sv,0x0D7E},{0x0F31,2,""sv,0x0F65},{0x11E5,8,""sv,0x1218},{0x1331,2,"!"sv,0x1343}};
constexpr SubtitlePointer pointers_end3u[]{{0x0280,0x0D7A}};
// "hapiness" -> "happiness" in the shared closing chant.
constexpr SubtitleEdit edits_end4[]{{0x0777,0,"p"sv,0x0876},{0x0B91,2,""sv,0x0BC5}};
constexpr SubtitlePointer pointers_end4[]{{0x0180,0x0872}};
constexpr SubtitleEdit edits_end4u[]{{0x08F7,0,"p"sv,0x09F6},{0x0D11,2,""sv,0x0D45}};
constexpr SubtitlePointer pointers_end4u[]{{0x0180,0x09F2}};
constexpr SubtitleEdit edits_end5u[]{{0x08D7,0,"p"sv,0x09D6},{0x0A68,2,"!"sv,0x0A78},{0x0CF1,2,""sv,0x0D25}};
constexpr SubtitlePointer pointers_end5u[]{{0x0180,0x09D2}};
constexpr SubtitleEdit edits_end6u[]{{0x0681,2,"!"sv,0x0691},{0x0757,0,"p"sv,0x0856},{0x0B71,2,""sv,0x0BA5}};
constexpr SubtitlePointer pointers_end6u[]{{0x0180,0x0852}};
constexpr SubtitleFile files[]{
    {"scn1b_e.bin",3488,"8E4D2BD76A8E7697F5F5D483A1F15753A5CBDF0C624B82FD6C5C61038F66F697",edits_1b,{}},
    {"scn3_e.bin",5344,"108F69421D33E5849BF7CF1F6C6E611B637BF39844DECB03C4ADFF6C84947E06",edits_3,pointers_3},
    {"scn4_e.bin",5024,"9EF5A80E41E829390E22B1A28A1D664D5F1CAA8CFE4FE71227A847910B771440",edits_4,{}},
    {"scn5_e.bin",4864,"D8B9DA9D56E0C539EF9763D2957C1904219D840433A3BCCAB6E5F0DD8493BEFC",edits_5,{}},
    {"scn9_e.bin",1856,"04F3F45B9F3357FA1B2EA5FB5CC91512560BE356D95973CF6DF2188D955A7A61",edits_9,pointers_9},
    {"scnend1_e.bin",2412,"B4C83CCEC73D952EE7C1C30FFD797908E60B6F72FC6284BF731F12AB26263953",edits_end1,{}},
    {"scnend1u_e.bin",4128,"918DCE0F3B0577F0DF666CE1D28299CD5D35FDBC009EAB4084ADC6F830C83824",edits_end1u,pointers_end1u},
    {"scnend2_e.bin",3136,"84BD277A780E97268448991C9E7648EFC6EC017FD6126B7DF94B993749D682DB",edits_end2,{}},
    {"scnend2u_e.bin",4224,"7489981564B7C3B79B28758694E56773F7EAFE9AD6DD8A2E79113AC176DD49BB",edits_end2u,pointers_end2u},
    {"scnend3_e.bin",2528,"BF2663664DA02A5D3C88CAB3D88D3E54D4006B34F3FAAD572E573575678EA132",edits_end3,{}},
    {"scnend3u_e.bin",6016,"6EBE70537895A842D5122CDB80944F5538DE5CCB129AB185BD355F5BBD6884DE",edits_end3u,pointers_end3u},
    {"scnend4_e.bin",3808,"821CFA92AF18423031F181CBF1490C5D35F671BF36636C24A3AD949A9ECD855B",edits_end4,pointers_end4},
    {"scnend4u_e.bin",4928,"67997785387FD4B2201BF9E3A2FF32FBC819FB0EC73E4FC5470246366939F00E",edits_end4u,pointers_end4u},
    {"scnend5u_e.bin",4800,"E90BDD386A0E733F3F84CB560EA7B6AB86D3C235C2DD49AEFD50FEEB909F480F",edits_end5u,pointers_end5u},
    {"scnend6u_e.bin",3520,"DA463306FD39B759794535226FB52AFA8DE8D85AB25010D213E7646F60B87EFE",edits_end6u,pointers_end6u},
};
bool same_name(std::string_view a,std::string_view b) {
    return std::ranges::equal(a,b,[](char x,char y) {return std::tolower(static_cast<unsigned char>(x))==std::tolower(static_cast<unsigned char>(y));});
}
const SubtitleFile* match(std::string_view path) {
    const auto split=path.find_last_of("/\\");
    if(split==std::string_view::npos || !split) return nullptr;
    const auto parent=path.substr(0,split),name=path.substr(split+1);
    const auto directory=parent.substr(parent.find_last_of("/\\")==std::string_view::npos?0:parent.find_last_of("/\\")+1);
    if(!same_name(directory,"dlg")) return nullptr;
    const auto found=std::ranges::find_if(files,[&](const SubtitleFile& file) {return same_name(file.name,name);});
    return found==std::end(files)?nullptr:&*found;
}
void apply(const SubtitleFile& file,std::span<std::byte> data) {
    // Later windows first, so earlier offsets still name original bytes.
    for(auto edit=file.edits.rbegin();edit!=file.edits.rend();++edit) {
        const auto growth=std::ptrdiff_t(edit->insert.size())-std::ptrdiff_t(edit->remove);
        const auto region=data.subspan(edit->offset,edit->window_end-edit->offset);
        const std::vector<std::byte> kept(region.begin()+edit->remove,region.end()-std::max<std::ptrdiff_t>(growth,0));
        std::memcpy(region.data(),edit->insert.data(),edit->insert.size());
        std::ranges::copy(kept,region.begin()+std::ptrdiff_t(edit->insert.size()));
        std::fill(region.begin()+std::ptrdiff_t(edit->insert.size()+kept.size()),region.end(),std::byte{});
    }
    for(const auto& pointer:file.pointers) std::memcpy(data.data()+pointer.offset,&pointer.value,sizeof(pointer.value));
}
struct Script {
    std::vector<std::string> labels,texts;
};
std::string terminated(Bytes data,std::uint32_t offset) {
    if(offset>=data.size()) throw std::runtime_error("Subtitle string lies outside its script");
    const auto* first=data.data()+offset;const auto* last=data.data()+data.size();
    const auto* end=std::find(first,last,std::byte{});
    if(end==last) throw std::runtime_error("Subtitle string has no terminator");
    return std::string(reinterpret_cast<const char*>(first),static_cast<std::size_t>(end-first));
}
Script parse(Bytes data) {
    if(data.size()<8 || u32(data,4)!=8) throw std::runtime_error("Unexpected subtitle script header");
    const auto count=u32(data,0);
    if(count>1024 || 8+std::uint64_t(count)*4>data.size()) throw std::runtime_error("Subtitle entry table exceeds its script");
    Script script;
    for(std::uint32_t i=0;i<count;++i) {
        const auto entry=u32(data,8+i*4);
        if(std::uint64_t(entry)+0x40>data.size()) throw std::runtime_error("Subtitle entry exceeds its script");
        script.labels.push_back(terminated(data,u32(data,entry)));
        script.texts.push_back(terminated(data,u32(data,entry+0x2C)));
    }
    return script;
}
// Timing, style and page codes must survive every correction unchanged.
std::string codes(std::string_view text) {
    std::string result;
    for(std::size_t i=0;i<text.size();++i) {
        if(text[i]=='\n') {result+='|';continue;}
        if(text[i]!='\\' || i+1==text.size()) continue;
        result+=text[i+1];++i;
        while(i+1<text.size() && (std::isdigit(static_cast<unsigned char>(text[i+1])) || text[i+1]=='.')) result+=text[++i];
        result+=' ';
    }
    return result;
}
std::string display(std::string_view text) {
    std::string result;
    for(std::size_t i=0;i<text.size();++i) {
        if(text[i]=='\\' && i+1<text.size()) {
            ++i;
            while(i+1<text.size() && (std::isdigit(static_cast<unsigned char>(text[i+1])) || text[i+1]=='.')) ++i;
        } else if(text[i]=='\n') {if(!result.empty() && result.back()!=' ') result+=" / ";}
        else result+=text[i];
    }
    while(!result.empty() && (result.back()==' ' || result.back()=='/')) result.pop_back();
    return result;
}
}
bool correct_subtitles(std::string_view path,std::uint64_t offset,std::span<std::byte> data) {
    const auto* file=match(path);
    if(!file || offset || data.size()!=file->size || sha256(data)!=file->sha256) return false;
    apply(*file,data);
    return true;
}
std::string check_subtitles(const std::filesystem::path& path) {
    Disc disc(path);
    std::string changes;unsigned checks=0,changed=0,unchanged=0;
    const auto require=[&](bool condition,std::string_view what) {
        ++checks;if(!condition) throw std::runtime_error(std::format("Subtitle check failed: {}",what));
    };
    for(const auto& file:files) {
        const auto entry=disc.find(std::format("media/dlg/{}",file.name));
        std::vector<std::byte> original(entry.size);disc.read(entry,0,original);
        require(entry.size==file.size && sha256(original)==file.sha256,std::format("{} matches the verified disc",file.name));
        require(std::ranges::is_sorted(file.edits,{},&SubtitleEdit::offset),std::format("{} edits are ordered",file.name));
        auto corrected=original;
        const auto path_name=std::format("\\Device\\CdRom0\\media\\dlg\\{}",file.name);
        require(correct_subtitles(path_name,0,corrected),std::format("{} accepts its complete original read",file.name));
        auto partial=original;
        require(!correct_subtitles(path_name,0,std::span(partial).first(partial.size()-1)) && partial==original,std::format("{} ignores a partial read",file.name));
        auto modified=original;modified.back()^=std::byte{1};const auto before=modified;
        require(!correct_subtitles(path_name,0,modified) && modified==before,std::format("{} ignores modified data",file.name));
        require(!correct_subtitles(std::format("\\Device\\CdRom0\\media\\{}",file.name),0,original),std::format("{} requires its dlg directory",file.name));
        const auto old_script=parse(original),new_script=parse(corrected);
        require(old_script.labels==new_script.labels,std::format("{} keeps every cue label",file.name));
        require(old_script.texts.size()==new_script.texts.size(),std::format("{} keeps its entries",file.name));
        for(std::size_t i=0;i<old_script.texts.size();++i) {
            const auto& before_text=old_script.texts[i];const auto& after_text=new_script.texts[i];
            require(codes(before_text)==codes(after_text),std::format("{} entry {} keeps its timing and style codes",file.name,i));
            require(std::ranges::none_of(after_text,[](char c) {return static_cast<unsigned char>(c)>=0x80;}),
                std::format("{} entry {} contains only English font characters",file.name,i));
            if(before_text==after_text) {++unchanged;continue;}
            ++changed;if(!changes.empty()) changes+=',';
            changes+=std::format("{{\"file\":{},\"entry\":{},\"before\":{},\"after\":{}}}",json(file.name),i,json(display(before_text)),json(display(after_text)));
        }
    }
    return std::format("{{\"format\":\"smbd-subtitle-check-v1\",\"passed\":true,\"files\":{},\"checks\":{},\"changed_entries\":{},\"unchanged_entries\":{},\"changes\":[{}]}}",
        std::size(files),checks,changed,unchanged,changes);
}
}
