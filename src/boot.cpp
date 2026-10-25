#include "xbox.h"
#include "kernel_names.h"
#include <sstream>

namespace smbd {
std::string check_boot(Xbe& image,const std::filesystem::path& disc,std::uint32_t budget,const std::filesystem::path& capture) {
    if (!budget || budget > 200000000) throw std::runtime_error("Boot diagnostic budget must be 1..200000000 checkpoints");
    std::vector<std::byte> ram(64 * 1024 * 1024);
    image.load(std::span(ram).subspan(image.base, image.image_size));
    Memory memory(0,ram);
    XboxHost host;
    host.main_address = 0x00014B90; // Verified game dispatcher; not the CRT entry.
    host.capture=capture;
    Xbox xbox(image,memory,ram,disc,std::move(host));
    const auto result = xbox.run(budget);
    const bool passed=result.capture_error.empty() && (result.boundary.empty() || result.boundary=="Native window closed" ||
        result.boundary.ends_with("execution diagnostic budget exhausted"));
    std::ostringstream output;
    output << "{\"format\":\"smbd-boot-v1\",\"sha256\":" << json(image.sha256)
           << ",\"passed\":" << (passed?"true":"false")
           << ",\"entry_returned\":" << (result.entry_returned ? "true" : "false")
           << ",\"main_reached\":" << (result.main_reached ? "true" : "false")
           << ",\"boundary\":" << json(result.boundary) << ",\"eip\":" << json(hex32(result.cpu.eip))
           << ",\"active_thread\":" << result.active_thread << ",\"threads_created\":" << result.threads_created
           << ",\"checkpoints\":" << result.diagnostic.visits
           << ",\"graphics\":{\"adapter\":" << json(result.adapter) << ",\"draws\":" << result.draws
           << ",\"frames\":" << result.frames << ",\"submissions\":" << result.submissions << ",\"vertices\":" << result.vertices
           << ",\"capture_written\":" << (result.capture_written?"true":"false") << ",\"capture_error\":" << json(result.capture_error) << "},\"registers\":[";
    bool first = true;
    for (const auto value : result.cpu.registers) { if (!first) output << ','; first=false; output << json(hex32(value)); }
    output << "],\"history\":[";
    first = true;
    for (std::size_t i=0; i<std::min<std::uint64_t>(result.diagnostic.visits,result.diagnostic.history.size()); ++i) {
        if (!first) output << ','; first=false;
        const auto index=(result.diagnostic.visits-1-i)%result.diagnostic.history.size();
        output << json(hex32(result.diagnostic.history[index]));
    }
    output << "],\"stack\":[";
    for (unsigned i=0;i<result.stack_word_count;++i) { if(i)output<<',';output<<json(hex32(result.stack_words[i])); }
    output << "],\"last_failed_allocation\":[";
    for(unsigned i=0;i<result.last_failed_allocation.size();++i) {if(i)output<<',';output<<json(hex32(result.last_failed_allocation[i]));}
    output << "],\"allocations\":["; first=true;
    for(const auto& block:result.allocations) {
        if(!first)output<<',';first=false;
        output << "{\"address\":" << json(hex32(block.address)) << ",\"size\":" << block.size << ",\"committed\":" << block.committed << '}';
    }
    output << "],\"services\":["; first=true;
    for (unsigned i=0;i<result.service_calls.size();++i) if(result.service_calls[i]) {
        if(!first)output<<',';first=false;
        output << "{\"ordinal\":" << i << ",\"name\":" << json(kernel_name(i)) << ",\"calls\":" << result.service_calls[i] << '}';
    }
    output << "],\"io\":["; first=true;
    for(const auto& request:result.io) {
        if(!first)output<<',';first=false;
        output << "{\"ordinal\":" << request.ordinal << ",\"path\":" << json(request.path)
               << ",\"status\":" << json(hex32(request.status)) << ",\"bytes\":" << request.information << '}';
    }
    output << "],\"threads\":["; first=true;
    for(const auto& thread:result.threads) {
        if(!first)output<<',';first=false;
        output << "{\"id\":" << thread.id << ",\"routine\":" << json(hex32(thread.routine))
               << ",\"eip\":" << json(hex32(thread.eip)) << ",\"state\":" << json(thread.state) << '}';
    }
    return output.str()+"]}";
}
}
