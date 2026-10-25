#include "decoder.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <bit>

namespace smbd {
namespace {
const Instruction* previous(const Function& function, std::uint32_t address) {
    const auto found = function.instructions.lower_bound(address);
    if (found == function.instructions.begin()) return nullptr;
    const auto& instruction = std::prev(found)->second;
    return instruction.address + instruction.decoded.length == address ? &instruction : nullptr;
}
struct TableGuard { std::uint32_t compare, branch, count; };
// A small modular interval proves masked, short-copy and reverse-copy table
// indices without retaining a decoded image or guessing table lengths.
struct Range {
    std::uint32_t first=0;
    unsigned count=0; // Zero means unknown; finite ranges contain at most 256 values.
    auto operator<=>(const Range&) const = default;
};
Range enclosing(std::vector<std::uint32_t> values) {
    if(values.empty()) return {};
    std::ranges::sort(values); const auto last=std::unique(values.begin(),values.end()); values.erase(last,values.end());
    std::uint32_t largest_gap=0,first=values.front();
    for(std::size_t i=0;i<values.size();++i) {
        const auto next=values[(i+1)%values.size()];
        const auto gap=next-values[i];
        if(gap>=largest_gap) {largest_gap=gap;first=next;}
    }
    if(values.size()==1) return {first,1};
    const auto count=static_cast<std::uint64_t>(UINT32_MAX)-largest_gap+2;
    return count<=256?Range{first,static_cast<unsigned>(count)}:Range{};
}
Range merge(Range first,Range second) {
    if(!first.count || !second.count) return {};
    if(first==second) return first;
    std::vector<std::uint32_t> values; values.reserve(first.count+second.count);
    for(unsigned i=0;i<first.count;++i) values.push_back(first.first+i);
    for(unsigned i=0;i<second.count;++i) values.push_back(second.first+i);
    return enclosing(std::move(values));
}
int general(ZydisRegister reg) {
    switch(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,reg)) {
    case ZYDIS_REGISTER_EAX:return 0;case ZYDIS_REGISTER_ECX:return 1;case ZYDIS_REGISTER_EDX:return 2;case ZYDIS_REGISTER_EBX:return 3;
    case ZYDIS_REGISTER_ESP:return 4;case ZYDIS_REGISTER_EBP:return 5;case ZYDIS_REGISTER_ESI:return 6;case ZYDIS_REGISTER_EDI:return 7;
    default:return -1;
    }
}
struct FlowState {
    std::array<Range,8> registers{};
    std::array<std::uint32_t,8> nonzero_masks{};
    Range stack_top;
    int compared=-1;
    std::uint32_t immediate=0;
    bool subtract=false;
    bool test=false;
    auto operator<=>(const FlowState&) const = default;
};
Range filter_range(Range range,ZydisMnemonic branch,std::uint32_t bound,bool taken) {
    if(branch!=ZYDIS_MNEMONIC_JB && branch!=ZYDIS_MNEMONIC_JBE && branch!=ZYDIS_MNEMONIC_JNB &&
       branch!=ZYDIS_MNEMONIC_JNBE && branch!=ZYDIS_MNEMONIC_JZ && branch!=ZYDIS_MNEMONIC_JNZ) return range;
    const auto accepts=[&](std::uint32_t value) {
        bool result;
        switch(branch) {
        case ZYDIS_MNEMONIC_JB:result=value<bound;break;
        case ZYDIS_MNEMONIC_JBE:result=value<=bound;break;
        case ZYDIS_MNEMONIC_JNB:result=value>=bound;break;
        case ZYDIS_MNEMONIC_JNBE:result=value>bound;break;
        case ZYDIS_MNEMONIC_JZ:result=value==bound;break;
        case ZYDIS_MNEMONIC_JNZ:result=value!=bound;break;
        default:return true;
        }
        return result==taken;
    };
    if(!range.count) {
        if((branch==ZYDIS_MNEMONIC_JZ && taken) || (branch==ZYDIS_MNEMONIC_JNZ && !taken)) return {bound,1};
        if(branch==ZYDIS_MNEMONIC_JZ || branch==ZYDIS_MNEMONIC_JNZ) return {};
        const bool upper=(taken && (branch==ZYDIS_MNEMONIC_JB || branch==ZYDIS_MNEMONIC_JBE)) ||
            (!taken && (branch==ZYDIS_MNEMONIC_JNB || branch==ZYDIS_MNEMONIC_JNBE));
        const auto inclusive=branch==ZYDIS_MNEMONIC_JBE || branch==ZYDIS_MNEMONIC_JNBE;
        const auto limit=static_cast<std::uint64_t>(bound)+(inclusive?1:0);
        if(upper && limit && limit<=256) range={0,static_cast<unsigned>(limit)};
        else if(!upper && limit<0x100000000ULL && 0x100000000ULL-limit<=256)
            range={static_cast<std::uint32_t>(limit),static_cast<unsigned>(0x100000000ULL-limit)};
        else return {};
    }
    std::vector<std::uint32_t> values;
    for(unsigned i=0;i<range.count;++i) if(accepts(range.first+i)) values.push_back(range.first+i);
    return enclosing(std::move(values));
}
std::map<std::uint32_t,std::vector<std::uint32_t>> bounded_tables(Xbe& image,const Function& function) {
    std::map<std::uint32_t,FlowState> states;
    std::deque<std::uint32_t> pending{function.entry}; states.emplace(function.entry,FlowState{});
    const auto update=[&](std::uint32_t pc,FlowState value) {
        if(!function.instructions.contains(pc)) return;
        const auto found=states.find(pc);
        if(found==states.end()) {states.emplace(pc,value);pending.push_back(pc);return;}
        auto combined=found->second;
        for(unsigned i=0;i<8;++i) combined.registers[i]=merge(combined.registers[i],value.registers[i]);
        combined.stack_top=merge(combined.stack_top,value.stack_top);
        for(unsigned i=0;i<8;++i) if(combined.nonzero_masks[i]!=value.nonzero_masks[i]) combined.nonzero_masks[i]=0;
        if(combined.compared!=value.compared || combined.immediate!=value.immediate || combined.subtract!=value.subtract || combined.test!=value.test) combined.compared=-1;
        if(combined!=found->second) {found->second=combined;pending.push_back(pc);}
    };
    unsigned iterations=0;
    while(!pending.empty() && ++iterations<=function.instructions.size()*32) {
        const auto pc=pending.front();pending.pop_front();
        const auto& instruction=function.instructions.at(pc); const auto& ins=instruction.decoded; const auto& operands=instruction.operands;
        auto state=states.at(pc); const auto original=state;
        const auto mnemonic=ins.mnemonic;
        for(unsigned i=0;i<ins.operand_count;++i) {
            const auto& operand=operands[i];
            if((operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
                (operand.type==ZYDIS_OPERAND_TYPE_MEMORY ||
                 (operand.type==ZYDIS_OPERAND_TYPE_REGISTER && general(operand.reg.value)==4))) state.stack_top={};
        }
        if(mnemonic==ZYDIS_MNEMONIC_PUSH && ins.operand_width==32) {
            if(operands[0].type==ZYDIS_OPERAND_TYPE_IMMEDIATE) state.stack_top={static_cast<std::uint32_t>(operands[0].imm.value.u),1};
            else if(operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER) {
                const auto index=general(operands[0].reg.value);
                if(index>=0) state.stack_top=original.registers[index];
            }
        }
        if(ins.cpu_flags && (ins.cpu_flags->modified || ins.cpu_flags->set_0 || ins.cpu_flags->set_1 || ins.cpu_flags->undefined)) state.compared=-1;
        for(unsigned i=0;i<ins.operand_count;++i) if(operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER && (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            const auto index=general(operands[i].reg.value);if(index>=0) {
                state.registers[index]={};state.nonzero_masks[index]=0; if(index==state.compared) state.compared=-1;
            }
        }
        const auto target=operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER?general(operands[0].reg.value):-1;
        if(target>=0 && operands[0].size==32) {
            const bool immediate=operands[1].type==ZYDIS_OPERAND_TYPE_IMMEDIATE;
            const auto value=immediate?static_cast<std::uint32_t>(operands[1].imm.value.u):0U;
            const auto source=operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER?general(operands[1].reg.value):-1;
            const auto previous_range=original.registers[target];
            if(mnemonic==ZYDIS_MNEMONIC_POP) state.registers[target]=original.stack_top;
            else if(mnemonic==ZYDIS_MNEMONIC_MOV) {
                if(immediate) state.registers[target]={value,1}; else if(source>=0 && operands[1].size==32) {
                    state.registers[target]=original.registers[source];state.nonzero_masks[target]=original.nonzero_masks[source];
                }
            } else if((mnemonic==ZYDIS_MNEMONIC_XOR || mnemonic==ZYDIS_MNEMONIC_SUB) && source==target) state.registers[target]={0,1};
            else if(mnemonic==ZYDIS_MNEMONIC_AND && immediate && value<=255) {
                const auto mask=original.nonzero_masks[target];const bool nonzero=mask && (mask&value)==mask;
                state.registers[target]={nonzero?1U:0U,nonzero?value:value+1}; state.nonzero_masks[target]=nonzero?mask:0;
            } else if(mnemonic==ZYDIS_MNEMONIC_AND && source>=0) {
                const auto first=previous_range,second=original.registers[source];
                const auto mask_bound=[](Range range)->std::optional<unsigned> {
                    if(range.count && static_cast<std::uint64_t>(range.first)+range.count-1<=255) return range.first+range.count-1;
                    return {};
                };
                const auto a=mask_bound(first),b=mask_bound(second);
                if(a || b) state.registers[target]={0,(a && b?std::min(*a,*b):a?*a:*b)+1};
            } else if(mnemonic==ZYDIS_MNEMONIC_MOVZX && operands[1].size==8) {
                state.registers[target]={0,256};
            }
            else if((mnemonic==ZYDIS_MNEMONIC_ADD || mnemonic==ZYDIS_MNEMONIC_SUB) && immediate && previous_range.count)
                state.registers[target]={previous_range.first+(mnemonic==ZYDIS_MNEMONIC_ADD?value:0U-value),previous_range.count};
            else if(mnemonic==ZYDIS_MNEMONIC_NEG && previous_range.count)
                state.registers[target]={0U-(previous_range.first+previous_range.count-1),previous_range.count};
            else if(mnemonic==ZYDIS_MNEMONIC_SHR && immediate && previous_range.count) {
                std::vector<std::uint32_t> values;
                for(unsigned i=0;i<previous_range.count;++i) values.push_back((previous_range.first+i)>>(value&31));
                state.registers[target]=enclosing(std::move(values));
            }
            const bool constant_register=source>=0 && operands[1].size==32 && original.registers[source].count==1;
            if((mnemonic==ZYDIS_MNEMONIC_CMP || mnemonic==ZYDIS_MNEMONIC_SUB) && (immediate || constant_register)) {
                state.compared=target;state.immediate=immediate?value:original.registers[source].first;
                state.subtract=mnemonic==ZYDIS_MNEMONIC_SUB;state.test=false;
            } else if(mnemonic==ZYDIS_MNEMONIC_TEST && (immediate || source==target)) {
                state.compared=target;state.immediate=immediate?value:UINT32_MAX;state.subtract=false;state.test=true;
            }
        }
        const auto category=ins.meta.category; const auto destination=instruction.target();const auto next=pc+ins.length;
        if(category==ZYDIS_CATEGORY_CALL) {update(next,FlowState{});continue;}
        if(category==ZYDIS_CATEGORY_RET || category==ZYDIS_CATEGORY_INTERRUPT || mnemonic==ZYDIS_MNEMONIC_INT3 || mnemonic==ZYDIS_MNEMONIC_HLT) continue;
        if(category==ZYDIS_CATEGORY_COND_BR) {
            for(bool taken:{false,true}) {
                auto edge_state=state;
                if(state.compared>=0) {
                    const bool carry=mnemonic==ZYDIS_MNEMONIC_JB || mnemonic==ZYDIS_MNEMONIC_JNB;
                    if(state.test) {
                        if((mnemonic==ZYDIS_MNEMONIC_JNZ && taken) || (mnemonic==ZYDIS_MNEMONIC_JZ && !taken))
                            edge_state.nonzero_masks[state.compared]=state.immediate;
                    } else if(state.subtract && carry && ((mnemonic==ZYDIS_MNEMONIC_JB)==taken) && state.immediate && state.immediate<=256)
                        edge_state.registers[state.compared]={0U-state.immediate,state.immediate};
                    else if(!state.subtract) edge_state.registers[state.compared]=filter_range(state.registers[state.compared],mnemonic,state.immediate,taken);
                }
                if(!taken) update(next,edge_state);else if(destination) update(*destination,edge_state);
            }
        } else if(category==ZYDIS_CATEGORY_UNCOND_BR) {
            if(destination) update(*destination,state);
            else for(auto target_address:instruction.jump_targets) update(target_address,state);
        } else update(next,state);
    }
    std::map<std::uint32_t,std::vector<std::uint32_t>> result;
    if(!pending.empty()) return result;
    for(const auto& [pc,instruction]:function.instructions) {
        const auto& operand=instruction.operands[0];
        if(instruction.decoded.meta.category!=ZYDIS_CATEGORY_UNCOND_BR || instruction.target() || instruction.kernel_ordinal ||
            !instruction.jump_targets.empty() || !states.contains(pc) || operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || operand.size!=32 ||
            operand.mem.base!=ZYDIS_REGISTER_NONE || operand.mem.index==ZYDIS_REGISTER_NONE || operand.mem.scale!=4 || operand.mem.segment!=ZYDIS_REGISTER_DS) continue;
        const auto index=general(operand.mem.index); if(index<0) continue;
        const auto range=states.at(pc).registers[index]; if(!range.count) continue;
        std::vector<std::uint32_t> targets;
        try {
            for(unsigned i=0;i<range.count;++i) {
                const auto address=static_cast<std::uint32_t>(operand.mem.disp.value)+(range.first+i)*4;
                const auto data=image.data(address,4);const auto target_address=u32(data,0);image.code_section(target_address);targets.push_back(target_address);
            }
        } catch(const std::exception&) {continue;}
        result.emplace(pc,std::move(targets));
    }
    return result;
}
std::optional<TableGuard> table_guard(Xbe& image, const Function& function, const Instruction& jump) {
    const auto& operand = jump.operands[0];
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.size != 32 ||
        operand.mem.base != ZYDIS_REGISTER_NONE || operand.mem.index == ZYDIS_REGISTER_NONE ||
        operand.mem.scale != 4 || operand.mem.segment != ZYDIS_REGISTER_DS || !operand.mem.disp.has_displacement)
        return {};
    const auto* branch = previous(function, jump.address);
    const ZydisDecodedOperand* lookup = nullptr;
    if (branch && branch->decoded.mnemonic == ZYDIS_MNEMONIC_MOVZX &&
        branch->operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && branch->operands[0].reg.value == operand.mem.index &&
        branch->operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && branch->operands[1].size == 8 &&
        branch->operands[1].mem.base == operand.mem.index && branch->operands[1].mem.index == ZYDIS_REGISTER_NONE &&
        branch->operands[1].mem.segment == ZYDIS_REGISTER_DS && branch->operands[1].mem.disp.has_displacement) {
        lookup = &branch->operands[1];
        branch = previous(function, branch->address);
    }
    if (!branch || (branch->decoded.mnemonic != ZYDIS_MNEMONIC_JNBE && branch->decoded.mnemonic != ZYDIS_MNEMONIC_JNB)) return {};
    const auto* compare = previous(function, branch->address);
    // Compilers may save registers between CMP and its branch. Only permit
    // instructions proven to preserve flags and the complete index register.
    for (unsigned skipped = 0; compare && compare->decoded.mnemonic != ZYDIS_MNEMONIC_CMP && skipped < 8; ++skipped) {
        const auto& decoded = compare->decoded;
        if (decoded.mnemonic != ZYDIS_MNEMONIC_PUSH && decoded.mnemonic != ZYDIS_MNEMONIC_MOV && decoded.mnemonic != ZYDIS_MNEMONIC_LEA)
            return {};
        if (decoded.cpu_flags && (decoded.cpu_flags->modified || decoded.cpu_flags->set_0 || decoded.cpu_flags->set_1 || decoded.cpu_flags->undefined)) return {};
        for (unsigned i = 0; i < decoded.operand_count; ++i) {
            const auto& changed = compare->operands[i];
            if (changed.type == ZYDIS_OPERAND_TYPE_REGISTER && (changed.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
                ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, changed.reg.value) == operand.mem.index) return {};
        }
        compare = previous(function, compare->address);
    }
    if (!compare || compare->decoded.mnemonic != ZYDIS_MNEMONIC_CMP ||
        compare->operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER || compare->operands[0].size != 32 ||
        compare->operands[0].reg.value != operand.mem.index || compare->operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
        return {};
    const auto bound = compare->operands[1].imm.value.u;
    auto count = bound + (branch->decoded.mnemonic == ZYDIS_MNEMONIC_JNBE ? 1 : 0);
    if (!count || count > 256 || (function.entry > compare->address && function.entry <= jump.address)) return {};
    if (lookup) {
        const auto remap = image.data(static_cast<std::uint32_t>(lookup->mem.disp.value), static_cast<std::size_t>(count));
        count = std::to_integer<unsigned char>(*std::ranges::max_element(remap)) + 1U;
    }
    return TableGuard{compare->address, branch->address, static_cast<std::uint32_t>(count)};
}
}
Decoder::Decoder() {
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32)) ||
        !ZYAN_SUCCESS(ZydisFormatterInit(&formatter_, ZYDIS_FORMATTER_STYLE_INTEL)))
        throw std::runtime_error("Cannot initialise the x86 decoder");
}

Instruction Decoder::decode(Xbe& image, std::uint32_t address) {
    Instruction result{};
    result.address = address;
    if (cache_image_ != &image || address < cache_address_ ||
        static_cast<std::uint64_t>(address) - cache_address_ + result.bytes.size() > cache_size_) {
        cache_size_ = image.code_bytes(address, cache_);
        cache_address_ = address;
        cache_image_ = &image;
    }
    const auto offset = address - cache_address_;
    const auto count = std::min(result.bytes.size(), cache_size_ - offset);
    std::copy_n(cache_.begin() + offset, count, result.bytes.begin());
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, result.bytes.data(), count,
                                           &result.decoded, result.operands.data())))
        throw std::runtime_error("Invalid or truncated x86 instruction at " + hex32(address) +
                                 " [" + hex_bytes(Bytes(result.bytes).first(count)) + ']');
    ++decoded_count_;
    std::array<char, 256> text{};
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter_, &result.decoded, result.operands.data(),
                        result.decoded.operand_count_visible, text.data(), text.size(), address, nullptr)))
        throw std::runtime_error("Cannot format instruction at " + hex32(address));
    result.text = text.data();
    return result;
}

std::optional<std::uint32_t> Instruction::target() const {
    for (unsigned i = 0; i < decoded.operand_count_visible; ++i) {
        const auto& operand = operands[i];
        if(operand.type==ZYDIS_OPERAND_TYPE_POINTER) return operand.ptr.offset;
        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
            ZyanU64 target;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded, &operand, address, &target)))
                return static_cast<std::uint32_t>(target);
        }
    }
    return std::nullopt;
}

std::string Instruction::report() const {
    std::ostringstream out;
    out << "{\"address\":" << json(hex32(address)) << ",\"bytes\":"
        << json(hex_bytes(Bytes(bytes).first(decoded.length))) << ",\"text\":" << json(text);
    if (const auto dest = target()) out << ",\"target\":" << json(hex32(*dest));
    if (kernel_ordinal) out << ",\"kernel_ordinal\":" << *kernel_ordinal;
    if (!jump_targets.empty()) {
        out << ",\"jump_targets\":[";
        for (std::size_t i = 0; i < jump_targets.size(); ++i) {
            if (i) out << ',';
            out << json(hex32(jump_targets[i]));
        }
        out << ']';
    }
    return out.str() + '}';
}

std::string disassemble(Xbe& image, std::uint32_t address, std::uint32_t byte_count) {
    if (!byte_count || byte_count > 64 * 1024) throw std::runtime_error("Decode range must be 1..65536 bytes");
    const auto& section = image.code_section(address);
    if (static_cast<std::uint64_t>(address) - section.address + byte_count > std::min(section.size, section.raw_size))
        throw std::runtime_error("Decode range leaves the code section");
    Decoder decoder;
    std::ostringstream out;
    out << "{\"format\":\"smbd-decode-v1\",\"sha256\":" << json(image.sha256) << ",\"instructions\":[";
    bool first = true;
    std::uint32_t consumed = 0;
    while (consumed < byte_count) {
        const auto instruction = decoder.decode(image, address + consumed);
        if (instruction.decoded.length > byte_count - consumed)
            throw std::runtime_error("Range ends inside an instruction at " + hex32(address + consumed));
        if (!first) out << ',';
        first = false;
        out << instruction.report();
        consumed += instruction.decoded.length;
    }
    return out.str() + "]}";
}

Function recover(Xbe& image, std::uint32_t address, std::uint32_t instruction_budget) {
    Decoder decoder;
    return recover(image, address, instruction_budget, decoder);
}

Function recover(Xbe& image, std::uint32_t address, std::uint32_t instruction_budget, Decoder& decoder) {
    if (!instruction_budget || instruction_budget > 8192) throw std::runtime_error("Analysis budget must be 1..8192 instructions");
    std::deque<std::uint32_t> pending{address};
    std::unordered_set<std::uint32_t> scheduled{address};
    Function function{address};
    std::map<std::uint32_t, TableGuard> guards;
    const auto schedule = [&](std::uint32_t target) {
        image.code_section(target);
        if (scheduled.insert(target).second) pending.push_back(target);
    };
    do {
    while (!pending.empty() && function.instructions.size() < instruction_budget) {
        const auto pc = pending.front();
        pending.pop_front();
        auto instruction = decoder.decode(image, pc);
        const auto next = pc + instruction.decoded.length;
        const auto after = function.instructions.lower_bound(pc);
        if ((after != function.instructions.end() && after->first < next) ||
            (after != function.instructions.begin() &&
             std::prev(after)->first + std::prev(after)->second.decoded.length > pc))
            throw std::runtime_error("Overlapping instruction boundaries at " + hex32(pc));
        const auto category = instruction.decoded.meta.category;
        const auto destination = instruction.target();
        if ((category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_UNCOND_BR) && !destination) {
            const auto& operand = instruction.operands[0];
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.size == 32 &&
                operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE &&
                operand.mem.segment == ZYDIS_REGISTER_DS)
                if (const auto* import = image.kernel_import(static_cast<std::uint32_t>(operand.mem.disp.value)))
                    instruction.kernel_ordinal = import->ordinal;
        }
        const auto edge = [&](std::string_view kind, std::optional<std::uint32_t> target) {
            function.edges.push_back({pc, std::string(kind), target});
        };
        if (category == ZYDIS_CATEGORY_CALL) {
            edge(instruction.kernel_ordinal ? "kernel-call" : "call", instruction.kernel_ordinal ? instruction.kernel_ordinal : destination);
            function.unresolved |= !destination && !instruction.kernel_ordinal;
            schedule(next);
        } else if (category == ZYDIS_CATEGORY_UNCOND_BR || category == ZYDIS_CATEGORY_COND_BR) {
            if (instruction.kernel_ordinal) edge("kernel-jump", instruction.kernel_ordinal);
            else if (destination) {
                edge(category == ZYDIS_CATEGORY_COND_BR ? "conditional" : "jump", destination);
                schedule(*destination);
            } else if (const auto guard = table_guard(image, function, instruction)) {
                const auto table = image.data(static_cast<std::uint32_t>(instruction.operands[0].mem.disp.value), guard->count * 4);
                for (std::uint32_t i = 0; i < guard->count; ++i) {
                    const auto target = u32(table, i * 4);
                    schedule(target);
                    instruction.jump_targets.push_back(target);
                    edge("table-jump", target);
                }
                guards.emplace(pc, *guard);
            } else { edge("jump", {}); function.unresolved = true; }
            if (category == ZYDIS_CATEGORY_COND_BR) schedule(next);
        } else if (category == ZYDIS_CATEGORY_RET) {
            edge("return", std::nullopt);
        } else if (instruction.decoded.mnemonic == ZYDIS_MNEMONIC_INT3 ||
                   instruction.decoded.mnemonic == ZYDIS_MNEMONIC_HLT ||
                   instruction.decoded.meta.category == ZYDIS_CATEGORY_INTERRUPT) {
            edge("trap", std::nullopt);
            function.unresolved |= instruction.decoded.mnemonic != ZYDIS_MNEMONIC_INT3 && instruction.decoded.mnemonic != ZYDIS_MNEMONIC_INT;
        } else {
            schedule(next);
        }
        function.instructions.emplace(pc, std::move(instruction));
    }
    if(!pending.empty()) break;
    const auto tables=bounded_tables(image,function);
    for(const auto& [pc,targets]:tables) {
        function.instructions.at(pc).jump_targets=targets;
        for(auto target:targets) {schedule(target);function.edges.push_back({pc,"table-jump",target});}
    }
    } while(!pending.empty());
    function.budget_exhausted = !pending.empty();
    // Every path to the table must execute its bounds comparison and branch.
    // Case entries or other branches into the middle invalidate that proof.
    for (const auto& [pc, guard] : guards) for (const auto& edge : function.edges)
        if (edge.to && (edge.kind == "jump" || edge.kind == "conditional" || edge.kind == "table-jump") &&
            *edge.to > guard.compare && *edge.to <= pc) {
            function.instructions.at(pc).jump_targets.clear();
            function.unresolved = true;
            break;
        }
    function.unresolved=std::ranges::any_of(function.instructions,[](const auto& pair) {
        const auto& instruction=pair.second;const auto category=instruction.decoded.meta.category;
        return (category==ZYDIS_CATEGORY_CALL || category==ZYDIS_CATEGORY_UNCOND_BR) && !instruction.target() &&
            !instruction.kernel_ordinal && instruction.jump_targets.empty();
    });
    return function;
}

std::string analyse(Xbe& image, std::uint32_t address, std::uint32_t instruction_budget) {
    const auto function = recover(image, address, instruction_budget);
    std::ostringstream out;
    out << "{\"format\":\"smbd-analysis-v1\",\"sha256\":" << json(image.sha256)
        << ",\"entry\":" << json(hex32(address)) << ",\"instruction_count\":" << function.instructions.size()
        << ",\"budget_exhausted\":" << (function.budget_exhausted ? "true" : "false")
        << ",\"unresolved_control_flow\":" << (function.unresolved ? "true" : "false") << ",\"instructions\":[";
    bool first = true;
    for (const auto& [pc, instruction] : function.instructions) {
        if (!first) out << ',';
        first = false;
        out << instruction.report();
    }
    out << "],\"edges\":[";
    first = true;
    for (const auto& edge : function.edges) {
        if (!first) out << ',';
        first = false;
        out << "{\"from\":" << json(hex32(edge.from)) << ",\"kind\":" << json(edge.kind)
            << ",\"to\":" << (edge.to ? json(hex32(*edge.to)) : "null") << '}';
    }
    out << "],\"static_callbacks\":[";
    for(std::size_t i=0;i<function.static_callbacks.size();++i) {if(i) out<<',';out<<json(hex32(function.static_callbacks[i]));}
    return out.str() + "]}";
}
}
