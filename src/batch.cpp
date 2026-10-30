#include "batch.h"
#include "native_guest_roots.h"
#include <algorithm>
#include <deque>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>

namespace smbd {
namespace {
struct Unit {
    std::vector<std::uint32_t> calls;
    std::vector<std::uint32_t> kernel;
    std::vector<LoweringIssue> issues;
    std::string source, error,native_api;
    std::map<std::string,std::string> native_helpers;
    std::size_t instruction_count = 0;
    bool truncated = false, unresolved = false, ready = false, emit = false;
    std::vector<std::uint32_t> indirect;
    std::vector<std::uint32_t> fixed_zero_faults;
    std::vector<std::uint32_t> dormant_indirect;
    std::vector<std::string> indirect_instructions,resolved_indirect_instructions,indexed_writes,field_writes;
    std::vector<std::uint32_t> static_callbacks;
    std::vector<std::uint32_t> indirect_slots;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> callback_stores;
    bool floating = false;
    bool clock = false, traps = false;
};
constexpr std::uint32_t total_instruction_limit = 2097152;
constexpr std::size_t source_limit = 256 * 1024 * 1024;
std::string symbol(std::uint32_t address) { return std::format("guest_{:08X}", address); }
void addresses(std::ostream& out, std::span<const std::uint32_t> values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << json(hex32(values[i]));
    }
    out << ']';
}
void instruction_reports(std::ostream& out,std::span<const std::string> values) {
    out << '[';
    for(std::size_t i=0;i<values.size();++i) {if(i) out<<',';out<<values[i];}
    out << ']';
}
struct RecoverySite {
    std::string group,instruction;
    std::vector<std::string> nearby;
    std::set<std::uint32_t> owners;
    std::size_t scope=SIZE_MAX;
    std::optional<std::uint32_t> slot;
    bool mixed_slots=false;
};
struct PointerStore {std::uint32_t value;std::string instruction;std::set<std::uint32_t> owners;};
struct SlotWrite {std::uint32_t address;unsigned bytes;std::string instruction;std::set<std::uint32_t> owners;};
struct GameReachability {
    std::set<std::uint32_t> roots,reached,missing,live_indirect;
    std::map<std::uint32_t,RecoverySite> dormant;
    bool closed=false;
};
GameReachability close_dormant_dispatches(const Xbe& image,std::map<std::uint32_t,Unit>& units,
    std::map<std::uint32_t,RecoverySite>& sites,bool all_discovered) {
    GameReachability result;result.roots.insert(image.entry);
    result.roots.insert(native_graphics_helpers.begin(),native_graphics_helpers.end());
    result.roots.insert(native_goal_queries.begin(),native_goal_queries.end());
    if(image.tls) result.roots.insert(image.tls->callbacks.begin(),image.tls->callbacks.end());
    for(const auto& [entry,unit]:units) {
        // Conservatively include verified callbacks even in otherwise unused
        // code. This covers asynchronous native entries and metadata handlers.
        result.roots.insert(unit.static_callbacks.begin(),unit.static_callbacks.end());
        if(!unit.native_api.empty()) result.roots.insert(entry);
    }
    std::deque<std::uint32_t> pending(result.roots.begin(),result.roots.end());
    while(!pending.empty()) {
        const auto entry=pending.front();pending.pop_front();
        if(!result.reached.insert(entry).second) continue;
        const auto at=units.find(entry);
        if(at==units.end()) {result.missing.insert(entry);continue;}
        const auto& unit=at->second;
        result.live_indirect.insert(unit.indirect.begin(),unit.indirect.end());
        pending.insert(pending.end(),unit.calls.begin(),unit.calls.end());
        pending.insert(pending.end(),unit.static_callbacks.begin(),unit.static_callbacks.end());
    }
    result.closed=all_discovered && !image.non_kernel_imports && result.missing.empty() &&
        result.live_indirect.empty() && std::ranges::all_of(units,[](const auto& item) {return item.second.emit;});
    if(!result.closed) return result;
    for(auto at=sites.begin();at!=sites.end();) {
        if(std::ranges::any_of(at->second.owners,[&](auto entry) {return result.reached.contains(entry);})) {++at;continue;}
        result.dormant.emplace(at->first,at->second);at=sites.erase(at);
    }
    for(auto& [entry,unit]:units) {
        for(auto pc:unit.indirect) if(const auto at=result.dormant.find(pc);at!=result.dormant.end()) {
            unit.dormant_indirect.push_back(pc);
            std::erase(unit.indirect_instructions,at->second.instruction);
            auto binding=at->second.instruction;binding.pop_back();
            binding+=",\"game_unreachable\":true,\"diagnostic_dispatch\":true}";
            unit.resolved_indirect_instructions.push_back(std::move(binding));
        }
        if(unit.dormant_indirect.empty()) continue;
        std::erase_if(unit.indirect,[&](auto pc) {return result.dormant.contains(pc);});
        unit.unresolved=!unit.indirect.empty();unit.ready=unit.emit && !unit.unresolved;
        // Keep unit.source unchanged: isolated diagnostics still execute the
        // original operand through the fixed, verified AOT function registry.
    }
    return result;
}
void reachability_report(std::ostream& out,const Xbe& image,const GameReachability& proof) {
    const auto write_set=[&](const auto& values) {
        const std::vector<std::uint32_t> ordered(values.begin(),values.end());addresses(out,ordered);
    };
    out<<"{\"format\":\"smbd-game-reachability-v1\",\"closed\":"<<(proof.closed?"true":"false")
       <<",\"entry\":"<<json(hex32(image.entry))<<",\"fixed_native_roots\":[";
    bool first=true;
    for(auto roots:{std::span<const std::uint32_t>(native_graphics_helpers),std::span<const std::uint32_t>(native_goal_queries)})
        for(auto entry:roots) {if(!first) out<<',';first=false;out<<json(hex32(entry));}
    out<<"],\"tls_callbacks\":";
    if(image.tls) addresses(out,image.tls->callbacks);else out<<"[]";
    out<<",\"roots\":";write_set(proof.roots);
    out<<",\"reachable_functions\":";write_set(proof.reached);
    out<<",\"missing_entries\":";write_set(proof.missing);
    out<<",\"reachable_unresolved_sites\":";write_set(proof.live_indirect);
    out<<",\"dormant_sites\":[";first=true;
    for(const auto& [pc,site]:proof.dormant) {
        if(!first) out<<',';first=false;
        out<<"{\"instruction\":"<<site.instruction<<",\"owners\":";write_set(site.owners);out<<'}';
    }
    out<<"]}";
}
// This is a local origin proof, not a guess about the object's type. Incoming
// branches, calls and other writes stop the walk before they can hide an alias.
std::optional<ZydisDecodedOperand> load_origin(const Function& function,std::uint32_t pc,ZydisRegister reg) {
    auto wanted=ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,reg);
    for(unsigned steps=0;steps<64;++steps) {
        if(std::ranges::any_of(function.edges,[&](const Edge& edge) {
            return edge.to==pc && (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump");
        })) return {};
        auto at=function.instructions.lower_bound(pc);
        if(at==function.instructions.begin()) return {};
        const auto& instruction=(--at)->second;const auto& ins=instruction.decoded;
        if(instruction.address+ins.length!=pc || ins.meta.category==ZYDIS_CATEGORY_CALL ||
           ins.meta.category==ZYDIS_CATEGORY_UNCOND_BR || ins.meta.category==ZYDIS_CATEGORY_RET) return {};
        bool written=false;
        for(unsigned i=0;i<ins.operand_count;++i) {
            const auto& operand=instruction.operands[i];
            written |= operand.type==ZYDIS_OPERAND_TYPE_REGISTER && (operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
                ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,operand.reg.value)==wanted;
        }
        if(written) {
            const auto& destination=instruction.operands[0];const auto& source=instruction.operands[1];
            if(ins.mnemonic!=ZYDIS_MNEMONIC_MOV || destination.type!=ZYDIS_OPERAND_TYPE_REGISTER || destination.size!=32 ||
               ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,destination.reg.value)!=wanted || source.size!=32) return {};
            if(source.type==ZYDIS_OPERAND_TYPE_MEMORY && ins.address_width==32 && source.mem.segment==ZYDIS_REGISTER_DS) return source;
            if(source.type!=ZYDIS_OPERAND_TYPE_REGISTER) return {};
            wanted=ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,source.reg.value);
        }
        pc=instruction.address;
    }
    return {};
}
std::string recovery_group(const Function& function,const Instruction& instruction) {
    if(const auto slot=indirect_slot(function,instruction)) return "slot "+hex32(*slot);
    auto operand=instruction.operands[0];
    bool loaded=false;
    if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER) {
        const auto origin=load_origin(function,instruction.address,operand.reg.value);
        if(!origin) return "register";
        operand=*origin;loaded=true;
    }
    if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY) return "other";
    if(operand.mem.base==ZYDIS_REGISTER_NONE)
        return std::string(operand.mem.index==ZYDIS_REGISTER_NONE?"slot ":"indexed table ")+hex32(static_cast<std::uint32_t>(operand.mem.disp.value));
    if(operand.mem.index!=ZYDIS_REGISTER_NONE) return "indexed object field";
    const auto origin=loaded?std::nullopt:load_origin(function,instruction.address,operand.mem.base);
    const bool table=origin && origin->mem.base!=ZYDIS_REGISTER_NONE && origin->mem.index==ZYDIS_REGISTER_NONE && origin->mem.disp.value==0;
    return std::string(table?"method +":loaded?"loaded field +":"object field +")+hex32(static_cast<std::uint32_t>(operand.mem.disp.value));
}
void collect_recovery(const Xbe& image,const Function& function,const Unit& unit,
    std::map<std::uint32_t,RecoverySite>& sites,std::map<std::uint32_t,PointerStore>& stores,
    std::map<std::uint32_t,SlotWrite>& writes,std::map<std::uint32_t,PointerStore>& uses) {
    for(auto pc:unit.indirect) {
        auto& site=sites[pc];const auto source=unit.emit?indirect_slot(function,function.instructions.at(pc)):std::nullopt;
        if(site.owners.empty()) site.slot=source;
        else if(site.slot!=source) {site.slot.reset();site.mixed_slots=true;}
        site.owners.insert(function.entry);
        if(site.mixed_slots) site.group="mixed slot origins";
        if(function.instructions.size()>=site.scope) continue;
        const auto at=function.instructions.find(pc);site.scope=function.instructions.size();
        site.group=site.mixed_slots?"mixed slot origins":unit.emit?recovery_group(function,at->second):"translation blocked";
        site.instruction=at->second.report();site.nearby.clear();
        auto begin=at;for(unsigned i=0;i<16 && begin!=function.instructions.begin();++i) --begin;
        auto end=at;for(unsigned i=0;i<4 && end!=function.instructions.end();++i) ++end;
        for(auto cursor=begin;cursor!=end;++cursor) site.nearby.push_back(cursor->second.report());
    }
    for(const auto& [pc,instruction]:function.instructions) {
        for(unsigned i=0;i<instruction.decoded.operand_count_visible;++i) {
            const auto& operand=instruction.operands[i];std::optional<std::uint32_t> address;
            if(operand.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && !operand.imm.is_relative) address=static_cast<std::uint32_t>(operand.imm.value.u);
            if(instruction.decoded.mnemonic==ZYDIS_MNEMONIC_LEA && instruction.decoded.address_width==32 &&
               operand.type==ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base==ZYDIS_REGISTER_NONE && operand.mem.index==ZYDIS_REGISTER_NONE)
                address=static_cast<std::uint32_t>(operand.mem.disp.value);
            if(!address || *address<image.base || *address-image.base>=image.image_size) continue;
            auto& use=uses[pc];use.value=*address;use.instruction=instruction.report();use.owners.insert(function.entry);
        }
        for(unsigned i=0;i<instruction.decoded.operand_count;++i) {
            const auto& operand=instruction.operands[i];
            if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || !(operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) ||
               instruction.decoded.address_width!=32 || operand.mem.segment!=ZYDIS_REGISTER_DS ||
               operand.mem.base!=ZYDIS_REGISTER_NONE || operand.mem.index!=ZYDIS_REGISTER_NONE) continue;
            const auto address=static_cast<std::uint32_t>(operand.mem.disp.value);
            if(address<image.base || address-image.base>=image.image_size) continue;
            auto& write=writes[pc];write.address=address;write.bytes=(operand.size+7)/8;
            write.instruction=instruction.report();write.owners.insert(function.entry);
        }
        const auto& destination=instruction.operands[0];const auto& value=instruction.operands[1];
        if(instruction.decoded.mnemonic!=ZYDIS_MNEMONIC_MOV || destination.type!=ZYDIS_OPERAND_TYPE_MEMORY || destination.size!=32 ||
           destination.mem.base==ZYDIS_REGISTER_NONE || destination.mem.index!=ZYDIS_REGISTER_NONE || destination.mem.disp.value!=0 ||
           value.type!=ZYDIS_OPERAND_TYPE_IMMEDIATE) continue;
        const auto address=static_cast<std::uint32_t>(value.imm.value.u);
        if(!std::ranges::any_of(image.sections,[&](const Section& section) {
            return (section.name==".data" || section.name==".rdata") && address>=section.address &&
                static_cast<std::uint64_t>(address)+4<=static_cast<std::uint64_t>(section.address)+section.raw_size;
        })) continue;
        auto& store=stores[pc];store.value=address;store.instruction=instruction.report();store.owners.insert(function.entry);
    }
}
void recovery_report(std::ostream& out,const std::map<std::uint32_t,RecoverySite>& sites,const std::map<std::uint32_t,PointerStore>& stores,
    const std::map<std::uint32_t,SlotWrite>& writes,const std::map<std::uint32_t,PointerStore>& uses,const std::map<std::uint32_t,Unit>& units) {
    const auto set_addresses=[&](const std::set<std::uint32_t>& values) {
        const std::vector<std::uint32_t> ordered(values.begin(),values.end());addresses(out,ordered);
    };
    std::map<std::string,std::vector<std::uint32_t>> groups;
    std::set<std::uint32_t> slots;
    for(const auto& [pc,site]:sites) {
        groups[site.group].push_back(pc);if(site.slot) slots.insert(*site.slot);
    }
    std::vector<std::pair<std::string,std::vector<std::uint32_t>>> ranked(groups.begin(),groups.end());
    std::ranges::sort(ranked,[](const auto& a,const auto& b) {return a.second.size()!=b.second.size()?a.second.size()>b.second.size():a.first<b.first;});
    std::set<std::uint32_t> fixed_zero_faults;
    for(const auto& [entry,unit]:units) fixed_zero_faults.insert(unit.fixed_zero_faults.begin(),unit.fixed_zero_faults.end());
    out << "{\"unique_sites\":" << sites.size() << ",\"fixed_zero_fault_sites\":";
    set_addresses(fixed_zero_faults);
    out << ",\"groups\":[";
    bool first=true;
    for(const auto& [name,values]:ranked) {
        if(!first) out<<',';first=false;
        out<<"{\"pattern\":"<<json(name)<<",\"count\":"<<values.size()<<",\"sites\":";addresses(out,values);out<<'}';
    }
    out<<"],\"sites\":[";first=true;
    for(const auto& [pc,site]:sites) {
        if(!first) out<<',';first=false;
        out<<"{\"instruction\":"<<site.instruction<<",\"pattern\":"<<json(site.group)<<",\"owners\":";
        set_addresses(site.owners);if(site.slot) out<<",\"source_slot\":"<<json(hex32(*site.slot));
        out<<",\"nearby_instructions\":";instruction_reports(out,site.nearby);out<<'}';
    }
    out<<"],\"object_pointer_stores\":[";first=true;
    for(const auto& [pc,store]:stores) {
        if(!first) out<<',';first=false;
        out<<"{\"instruction\":"<<store.instruction<<",\"value\":"<<json(hex32(store.value))<<",\"owners\":";
        set_addresses(store.owners);out<<'}';
    }
    out<<"],\"absolute_slot_writes\":[";first=true;
    for(const auto& [pc,write]:writes) {
        if(!first) out<<',';first=false;
        out<<"{\"instruction\":"<<write.instruction<<",\"address\":"<<json(hex32(write.address))<<",\"bytes\":"<<write.bytes<<",\"owners\":";
        set_addresses(write.owners);out<<'}';
    }
    // Nearby address literals expose aggregate initializers and base+field
    // stores without treating those possible aliases as proven slot writes.
    out<<"],\"nearby_address_uses\":[";first=true;
    for(const auto& [pc,use]:uses) {
        const auto slot=slots.lower_bound(use.value);
        if(slot==slots.end() || *slot-use.value>=128) continue;
        if(!first) out<<',';first=false;
        out<<"{\"instruction\":"<<use.instruction<<",\"value\":"<<json(hex32(use.value))<<",\"owners\":";
        set_addresses(use.owners);out<<'}';
    }
    std::map<std::uint32_t,std::set<std::uint32_t>> callers;
    for(const auto& [entry,unit]:units) if(unit.emit) for(auto target:unit.calls)
        callers[target].insert(entry);
    out<<"],\"direct_callers\":[";first=true;
    for(const auto& [entry,values]:callers) {
        if(!first) out<<',';first=false;
        out<<"{\"entry\":"<<json(hex32(entry))<<",\"callers\":";set_addresses(values);out<<'}';
    }
    out<<"]}";
}
}

Batch recompile_batch(Xbe& image, std::span<const std::uint32_t> seeds,
                      std::uint32_t function_limit, std::uint32_t instruction_limit,
                      std::uint32_t shard_count, std::string_view header_name) {
    if (!image.supported()) throw std::runtime_error("Batch generation requires the verified target executable");
    if (seeds.empty() || seeds.size() > 65536) throw std::runtime_error("A batch needs 1..65536 seed addresses");
    if (!function_limit || function_limit > 65536) throw std::runtime_error("Function limit must be 1..65536");
    if (!instruction_limit || instruction_limit > 8192) throw std::runtime_error("Instruction limit must be 1..8192 per function");
    if(shard_count>512 || (shard_count && header_name.empty())) throw std::runtime_error("Sharding needs 1..512 files and a header name");
    Decoder decoder;
    std::map<std::uint32_t, Unit> units;
    std::set<std::uint32_t> scheduled;
    std::deque<std::uint32_t> pending;
    std::set<std::uint32_t> called_slots;
    std::map<std::uint32_t,std::set<std::uint32_t>> installed_callbacks;
    std::map<std::uint32_t,RecoverySite> recovery_sites;
    std::map<std::uint32_t,PointerStore> object_pointer_stores;
    std::map<std::uint32_t,SlotWrite> absolute_slot_writes;
    std::map<std::uint32_t,PointerStore> address_uses;
    const auto schedule = [&](std::uint32_t address) {
        if (scheduled.insert(address).second) pending.push_back(address);
    };
    for (const auto seed : seeds) schedule(seed);
    std::uint32_t instruction_count = 0;
    std::size_t generated_bytes = 0;
    std::map<std::string, unsigned> unsupported;
    while (!pending.empty() && units.size() < function_limit && instruction_count < total_instruction_limit) {
        const auto address = pending.front();
        pending.pop_front();
        Unit unit;
        const auto decoded_before = decoder.decoded_count();
        try {
            const auto function = recover(image, address,
                std::min(instruction_limit, total_instruction_limit - instruction_count), decoder);
            unit.truncated = function.budget_exhausted;
            unit.unresolved = function.unresolved;
            unit.static_callbacks=function.static_callbacks;
            for(auto callback:unit.static_callbacks) schedule(callback);
            unit.indirect_slots=function.indirect_slots;
            unit.callback_stores=function.callback_stores;
            for(const auto& [slot,callback]:unit.callback_stores) {
                installed_callbacks[slot].insert(callback);
                if(called_slots.contains(slot)) schedule(callback);
            }
            for(auto slot:unit.indirect_slots) {
                if(called_slots.insert(slot).second) try {
                    const auto initial=u32(image.data(slot,4),0);
                    if(initial) {image.code_section(initial);schedule(initial);}
                } catch(const std::exception&) {}
                if(installed_callbacks.contains(slot)) for(auto callback:installed_callbacks.at(slot)) schedule(callback);
            }
            for (const auto& edge : function.edges) if ((edge.kind == "call" || edge.kind=="tail-call") && edge.to) {
                unit.calls.push_back(*edge.to);
                schedule(*edge.to);
            }
            for(const auto& edge : function.edges)
                if((edge.kind=="call" || edge.kind=="jump") && !edge.to) unit.indirect.push_back(edge.from);
            for (const auto& edge : function.edges) if (edge.kind.starts_with("kernel-") && edge.to) unit.kernel.push_back(*edge.to);
            std::ranges::sort(unit.kernel);
            unit.kernel.erase(std::unique(unit.kernel.begin(), unit.kernel.end()), unit.kernel.end());
            for (const auto& [pc, instruction] : function.instructions) {
                unit.clock |= instruction.decoded.mnemonic == ZYDIS_MNEMONIC_RDTSC;
                unit.traps |= instruction.decoded.mnemonic == ZYDIS_MNEMONIC_INT3 || instruction.decoded.mnemonic == ZYDIS_MNEMONIC_INT;
                if(instruction.invalid_target) {unit.fixed_zero_faults.push_back(pc);unit.traps=true;}
                unit.traps |= !instruction.null_sources.empty();
                if(std::ranges::contains(unit.indirect,pc)) unit.indirect_instructions.push_back(instruction.report());
                if(!instruction.target() &&
                   (instruction.decoded.meta.category==ZYDIS_CATEGORY_CALL || instruction.decoded.meta.category==ZYDIS_CATEGORY_UNCOND_BR) &&
                   (instruction.kernel_ordinal || instruction.tail_target || instruction.zero_guard || instruction.invalid_target || !instruction.jump_targets.empty()))
                    unit.resolved_indirect_instructions.push_back(instruction.report());
                // Reuse the decoded batch for object-field writer research.
                // These are possible aliases, not object-type or closure proofs.
                for(unsigned i=0;i<instruction.decoded.operand_count;++i) {
                    const auto& operand=instruction.operands[i];
                    if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || !(operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) ||
                       instruction.decoded.address_width!=32 || operand.mem.base==ZYDIS_REGISTER_NONE ||
                       operand.mem.base==ZYDIS_REGISTER_ESP || operand.mem.disp.value<-8192 || operand.mem.disp.value>8192) continue;
                    auto write=instruction.report();write.pop_back();
                    write+=",\"base\":"+json(ZydisRegisterGetString(operand.mem.base))+
                        ",\"segment\":"+json(ZydisRegisterGetString(operand.mem.segment))+
                        ",\"offset\":"+std::to_string(operand.mem.disp.value)+
                        ",\"width_bytes\":"+std::to_string((operand.size+7)/8)+
                        ",\"repeated\":"+((instruction.decoded.attributes&(ZYDIS_ATTRIB_HAS_REP|ZYDIS_ATTRIB_HAS_REPE|ZYDIS_ATTRIB_HAS_REPNE))?"true":"false")+
                        ",\"indexed\":"+(operand.mem.index==ZYDIS_REGISTER_NONE?"false":"true")+'}';
                    unit.field_writes.push_back(std::move(write));
                }
                // Global arrays also use a base register as their index, byte
                // stores and read/modify/write operations. Report all such
                // possible writers; the index is research evidence, not a type proof.
                for(unsigned i=0;i<instruction.decoded.operand_count;++i) {
                    const auto& operand=instruction.operands[i];
                    if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || !(operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) ||
                       instruction.decoded.address_width!=32 || operand.mem.segment!=ZYDIS_REGISTER_DS ||
                       (operand.mem.base==ZYDIS_REGISTER_NONE && operand.mem.index==ZYDIS_REGISTER_NONE) ||
                       !operand.mem.disp.has_displacement) continue;
                    const auto base=static_cast<std::uint32_t>(operand.mem.disp.value);
                    if(base>=image.base && base-image.base<image.image_size) {
                        auto write=instruction.report();write.pop_back();
                        write+=",\"table_base\":"+json(hex32(base))+
                            ",\"base\":"+json(ZydisRegisterGetString(operand.mem.base))+
                            ",\"index\":"+json(ZydisRegisterGetString(operand.mem.index))+
                            ",\"scale\":"+std::to_string(operand.mem.scale)+
                            ",\"width_bytes\":"+std::to_string((operand.size+7)/8)+'}';
                        unit.indexed_writes.push_back(std::move(write));
                    }
                }
            }
            std::ranges::sort(unit.calls);
            unit.calls.erase(std::unique(unit.calls.begin(), unit.calls.end()), unit.calls.end());
            auto lowered = lower(function);
            unit.source = std::move(lowered.source);
            unit.native_helpers=std::move(lowered.native_helpers);
            unit.floating=lowered.floating;
            unit.native_api=std::move(lowered.native_api);
            if(!unit.native_api.empty()) {
                unit.unresolved=false;unit.calls.clear();unit.indirect.clear();unit.kernel.clear();unit.clock=unit.traps=false;
                unit.indirect_instructions.clear();unit.resolved_indirect_instructions.clear();unit.indexed_writes.clear();
                unit.fixed_zero_faults.clear();
                unit.field_writes.clear();
            }
            unit.issues = std::move(lowered.issues);
            for (const auto& issue : unit.issues)
                ++unsupported[ZydisMnemonicGetString(function.instructions.at(issue.address).decoded.mnemonic)];
            unit.emit = !unit.truncated && unit.issues.empty();
            unit.ready = unit.emit && !unit.unresolved;
            if(unit.native_api.empty()) collect_recovery(image,function,unit,recovery_sites,object_pointer_stores,absolute_slot_writes,address_uses);
            generated_bytes += unit.source.size();
        } catch (const std::exception& error) { unit.error = error.what(); unit.ready = false; }
        // Failed recovery still consumes the global work budget.
        unit.instruction_count = static_cast<std::size_t>(decoder.decoded_count() - decoded_before);
        instruction_count += static_cast<std::uint32_t>(unit.instruction_count);
        if (generated_bytes > source_limit) throw std::runtime_error("Generated batch exceeds the 256 MiB source limit");
        units.emplace(address, std::move(unit));
    }
    const auto game_reachability=close_dormant_dispatches(image,units,recovery_sites,pending.empty());
    // Keep proven closed coverage separate from executable partial coverage.
    // Missing dependencies are explicit diagnostic stops, never interpreted.
    bool changed;
    do {
        changed = false;
        for (auto& [address, unit] : units) if (unit.ready) {
            for (const auto target : unit.calls) {
                const auto found = units.find(target);
                if (found == units.end() || !found->second.ready) {
                    unit.ready = false;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);
    // Only external entry wrappers switch floating state; direct native calls
    // share the resident x87 stack and the guest MXCSR.
    do {
        changed=false;
        for(auto& [address,unit] : units) if(unit.emit && !unit.floating)
            for(const auto target : unit.calls) if(units.contains(target) && units.at(target).emit && units.at(target).floating) {
                unit.floating=true; changed=true; break;
            }
    } while(changed);

    Batch result;
    result.complete = pending.empty() && std::ranges::all_of(units, [](const auto& pair) { return pair.second.ready; });
    const auto ready_count = std::ranges::count_if(units, [](const auto& pair) { return pair.second.ready; });
    const auto emitted_count = std::ranges::count_if(units, [](const auto& pair) { return pair.second.emit; });
    result.discovered = units.size();
    result.compiled = static_cast<std::size_t>(emitted_count);
    result.instructions = instruction_count;
    std::size_t ready_instructions = 0;
    std::set<std::uint32_t> required_kernel;
    bool needs_clock = false, needs_exceptions = false;
    for (const auto& [address, unit] : units) if (unit.emit) {
        required_kernel.insert(unit.kernel.begin(), unit.kernel.end());
        needs_clock |= unit.clock;
        needs_exceptions |= unit.traps;
    }
    std::set<std::uint32_t> missing;
    for(const auto& [address,unit] : units) if(unit.emit) for(const auto target : unit.calls)
        if(!units.contains(target) || !units.at(target).emit) missing.insert(target);
    if (emitted_count) {
        std::ostringstream source, declarations;
        std::map<std::string,std::string> helpers;
        source << "// Generated from XBE SHA256 " << image.sha256 << "\n#include \"cpu.h\"\nnamespace smbd {\n";
        declarations << "#pragma once\n#include \"cpu.h\"\nnamespace smbd {\n";
        for(const auto& [address,unit] : units) if(unit.emit) {
            declarations << "void " << symbol(address) << "(Cpu&,Memory&);\n";
            if(unit.floating) declarations << "void entry_" << std::format("{:08X}",address) << "(Cpu&,Memory&);\n";
        }
        for(const auto address : missing) declarations << "void " << symbol(address) << "(Cpu&,Memory&);\n";
        declarations << "}\n";
        result.header=declarations.str();
        if(!shard_count) {
            const auto start=result.header.find("namespace smbd {")+std::string_view("namespace smbd {\n").size();
            source << result.header.substr(start,result.header.size()-start-2);
        }
        std::vector<std::ostringstream> pieces(shard_count);
        for(auto& piece : pieces) piece << "// Generated from XBE SHA256 " << image.sha256
            << "\n#include \"cpu.h\"\nnamespace smbd {\n";
        const auto shard_for=[&](std::uint32_t address) { return ((address>>4)^(address>>13)^(address>>22))%shard_count; };
        if(shard_count) {
            std::vector<std::set<std::uint32_t>> dependencies(shard_count);
            for(const auto& [address,unit] : units) if(unit.emit)
                dependencies[shard_for(address)].insert(unit.calls.begin(),unit.calls.end());
            for(std::uint32_t i=0;i<shard_count;++i) for(const auto address : dependencies[i])
                pieces[i] << "void " << symbol(address) << "(Cpu&,Memory&);\n";
        }
        for (const auto& [address, unit] : units) if (unit.emit) {
            auto& destination=shard_count?pieces[shard_for(address)]:source;
            destination << unit.source;
            helpers.insert(unit.native_helpers.begin(),unit.native_helpers.end());
            if(unit.floating) destination << "void entry_" << std::format("{:08X}",address)
                << "(Cpu& cpu,Memory& memory) { FloatingScope scope(cpu.floating); " << symbol(address) << "(cpu,memory); }\n";
            if(unit.native_api.empty()) ready_instructions += unit.instruction_count;
        }
        for(const auto address : missing) {
            source << "void " << symbol(address) << "(Cpu& cpu,[[maybe_unused]] Memory& memory) { cpu.eip="
                << std::format("0x{:08X}U",address) << "; throw NativeBoundary(cpu.eip,\"direct callee was not compiled\"); }\n";
        }
        if(shard_count) for(auto& piece : pieces) { piece << "}\n"; result.shards.push_back(piece.str()); }
        if(shard_count) for(const auto& [address,unit] : units) if(unit.emit)
            source << "void " << (unit.floating?std::format("entry_{:08X}",address):symbol(address)) << "(Cpu&,Memory&);\n";
        source << "namespace {\nconst std::array<CompiledFunction, " << emitted_count << "> functions{{\n";
        for (const auto& [address, unit] : units) if (unit.emit)
            source << "    {" << std::format("0x{:08X}U", address) << ", \"" << image.sha256 << "\", "
                   << (unit.floating?std::format("entry_{:08X}",address):symbol(address)) << "},\n";
        source << "}};\n}\nconst std::span<const CompiledFunction> compiled_batch{functions};\n}\n";
        result.source = source.str();
        result.assembly="; Native instruction helpers generated from verified XBE "+image.sha256+"\noption casemap:none\n.code\n";
        for(const auto& [name,assembly] : helpers) result.assembly+=assembly;
        result.assembly+="END\n";
        std::size_t total=result.source.size()+result.header.size()+result.assembly.size();
        for(const auto& piece : result.shards) total+=piece.size();
        if (total > source_limit) throw std::runtime_error("Generated batch exceeds the 256 MiB source limit");
    }
    std::ostringstream report;
    report << "{\"format\":\"smbd-batch-v1\",\"sha256\":" << json(image.sha256) << ",\"seeds\":";
    addresses(report, seeds);
    report << ",\"complete\":" << (result.complete ? "true" : "false")
           << ",\"function_limit\":" << function_limit << ",\"instruction_limit_per_function\":" << instruction_limit
           << ",\"total_instruction_limit\":" << total_instruction_limit << ",\"functions_discovered\":" << units.size()
           << ",\"instructions_discovered\":" << instruction_count << ",\"functions_compiled\":" << emitted_count
           << ",\"native_platform_functions\":" << std::ranges::count_if(units,[](const auto& pair){return pair.second.emit && !pair.second.native_api.empty();})
           << ",\"translated_functions\":" << std::ranges::count_if(units,[](const auto& pair){return pair.second.emit && pair.second.native_api.empty();})
           << ",\"closed_functions\":" << ready_count << ",\"shard_count\":" << shard_count
           << ",\"instructions_compiled\":" << ready_instructions << ",\"pending_functions\":[";
    bool first = true;
    for (const auto address : pending) {
        if (!first) report << ',';
        first = false;
        report << json(hex32(address));
    }
    report << "],\"missing_direct_functions\":[";
    first=true;
    for(const auto address : missing) { if(!first) report << ','; first=false; report << json(hex32(address)); }
    report << "],\"unsupported_instructions\":{";
    first = true;
    for (const auto& [mnemonic, count] : unsupported) {
        if (!first) report << ',';
        first = false;
        report << json(mnemonic) << ':' << count;
    }
    report << "},\"runtime_requirements\":{\"timestamp_counter\":" << (needs_clock ? "true" : "false")
        << ",\"exception_dispatch\":" << (needs_exceptions ? "true" : "false") << ",\"kernel_ordinals\":[";
    first = true;
    for (const auto ordinal : required_kernel) {
        if (!first) report << ',';
        first = false;
        const auto import = std::ranges::find(image.imports, ordinal, &KernelImport::ordinal);
        report << "{\"ordinal\":" << ordinal << ",\"name\":" << json(import->name) << '}';
    }
    report << "]},\"call_recovery\":";
    recovery_report(report,recovery_sites,object_pointer_stores,absolute_slot_writes,address_uses,units);
    report << ",\"game_reachability\":";
    reachability_report(report,image,game_reachability);
    report << ",\"functions\":[";
    first = true;
    for (const auto& [address, unit] : units) {
        if (!first) report << ',';
        first = false;
        report << "{\"entry\":" << json(hex32(address)) << ",\"instruction_count\":" << unit.instruction_count
               << ",\"compiled\":" << (unit.emit ? "true" : "false") << ",\"closed\":" << (unit.ready?"true":"false")
               << ",\"floating_point\":" << (unit.floating?"true":"false")
               << ",\"budget_exhausted\":" << (unit.truncated ? "true" : "false")
               << ",\"unresolved_control_flow\":" << (unit.unresolved ? "true" : "false") << ",\"calls\":";
        addresses(report, unit.calls);
        report << ",\"unresolved_indirect_sites\":";
        addresses(report,unit.indirect);
        report << ",\"diagnostic_only_indirect_sites\":";
        addresses(report,unit.dormant_indirect);
        report << ",\"unresolved_instructions\":";
        instruction_reports(report,unit.indirect_instructions);
        report << ",\"resolved_indirect_instructions\":";
        instruction_reports(report,unit.resolved_indirect_instructions);
        report << ",\"indexed_writes\":";
        instruction_reports(report,unit.indexed_writes);
        report << ",\"field_writes\":";
        instruction_reports(report,unit.field_writes);
        report << ",\"static_callback_targets\":";
        addresses(report,unit.static_callbacks);
        report << ",\"indirect_pointer_slots\":";
        addresses(report,unit.indirect_slots);
        report << ",\"callback_installations\":[";
        for(std::size_t i=0;i<unit.callback_stores.size();++i) {
            if(i) report<<',';
            report << "{\"slot\":" << json(hex32(unit.callback_stores[i].first)) << ",\"target\":" << json(hex32(unit.callback_stores[i].second)) << '}';
        }
        report << ']';
        report << ",\"kernel_ordinals\":[";
        for (std::size_t i = 0; i < unit.kernel.size(); ++i) {
            if (i) report << ',';
            report << unit.kernel[i];
        }
        report << ']';
        report << ",\"timestamp_counter\":" << (unit.clock ? "true" : "false")
            << ",\"exception_dispatch\":" << (unit.traps ? "true" : "false");
        report << ",\"blocked_by\":[";
        bool first_blocked = true;
        for (const auto target : unit.calls) {
            const auto found = units.find(target);
            if (found != units.end() && found->second.ready) continue;
            if (!first_blocked) report << ',';
            first_blocked = false;
            report << json(hex32(target));
        }
        report << "],\"issues\":[";
        for (std::size_t i = 0; i < unit.issues.size(); ++i) {
            const auto& issue = unit.issues[i];
            if (i) report << ',';
            report << "{\"address\":" << json(hex32(issue.address)) << ",\"bytes\":" << json(issue.bytes)
                   << ",\"instruction\":" << json(issue.instruction) << ",\"reason\":" << json(issue.reason) << '}';
        }
        report << ']';
        if (!unit.error.empty()) report << ",\"error\":" << json(unit.error);
        if(!unit.native_api.empty()) report << ",\"native_platform_api\":" << json(unit.native_api);
        report << '}';
    }
    result.report = report.str() + "]}";
    return result;
}
}
