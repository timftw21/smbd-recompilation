#include "decoder.h"
#include "callback_abis.h"
#include "static_calls.h"
#include "native_apis.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <bit>
#include <bitset>

namespace smbd {
namespace {
const Instruction* previous(const Function& function, std::uint32_t address) {
    const auto found = function.instructions.lower_bound(address);
    if (found == function.instructions.begin()) return nullptr;
    const auto& instruction = std::prev(found)->second;
    return instruction.address + instruction.decoded.length == address ? &instruction : nullptr;
}
struct TableGuard { std::uint32_t begin, branch, count; };
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
std::optional<std::uint32_t> constant_before(const Function& function,std::uint32_t pc,const ZydisDecodedOperand& value,bool load_address=false) {
    if(value.type==ZYDIS_OPERAND_TYPE_IMMEDIATE) return load_address?std::nullopt:std::optional{static_cast<std::uint32_t>(value.imm.value.u)};
    if(value.type!=ZYDIS_OPERAND_TYPE_REGISTER || value.size!=32) return {};
    auto wanted=general(value.reg.value);
    for(unsigned steps=0;wanted>=0 && steps<64;++steps) {
        if(std::ranges::any_of(function.edges,[&](const Edge& edge){return edge.to==pc &&
            (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump");})) return {};
        const auto* instruction=previous(function,pc);
        if(!instruction) return {};
        const auto& ins=instruction->decoded;
        if(ins.meta.category==ZYDIS_CATEGORY_CALL || (!load_address && ins.meta.category==ZYDIS_CATEGORY_COND_BR) ||
           ins.meta.category==ZYDIS_CATEGORY_UNCOND_BR || ins.meta.category==ZYDIS_CATEGORY_RET) return {};
        const auto& operands=instruction->operands;
        bool written=false;
        for(unsigned i=0;i<ins.operand_count;++i) written |= operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER &&
            general(operands[i].reg.value)==wanted && (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE);
        if(written) {
            if(operands[0].type!=ZYDIS_OPERAND_TYPE_REGISTER || general(operands[0].reg.value)!=wanted || operands[0].size!=32) return {};
            if(ins.mnemonic==ZYDIS_MNEMONIC_MOV) {
                if(operands[1].type==ZYDIS_OPERAND_TYPE_IMMEDIATE)
                    return load_address?std::nullopt:std::optional{static_cast<std::uint32_t>(operands[1].imm.value.u)};
                if(load_address && operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY && operands[1].size==32 &&
                   operands[1].mem.base==ZYDIS_REGISTER_NONE && operands[1].mem.index==ZYDIS_REGISTER_NONE && operands[1].mem.segment==ZYDIS_REGISTER_DS)
                    return static_cast<std::uint32_t>(operands[1].mem.disp.value);
                if(operands[1].type!=ZYDIS_OPERAND_TYPE_REGISTER || operands[1].size!=32) return {};
                wanted=general(operands[1].reg.value);
            } else if(ins.mnemonic==ZYDIS_MNEMONIC_LEA && operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[1].mem.base==ZYDIS_REGISTER_NONE && operands[1].mem.index==ZYDIS_REGISTER_NONE)
                return load_address?std::nullopt:std::optional{static_cast<std::uint32_t>(operands[1].mem.disp.value)};
            else if(ins.mnemonic==ZYDIS_MNEMONIC_XOR && operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                    operands[1].size==32 && general(operands[1].reg.value)==wanted) return load_address?std::nullopt:std::optional{0U};
            else return {};
        }
        pc=instruction->address;
    }
    return {};
}
// A callback argument can be prepared on several branches before a shared call.
// Keep each proven PUSH origin; stop on calls, stack changes or possible aliases.
std::vector<const Instruction*> callback_pushes(const Function& function,std::uint32_t call,unsigned argument) {
    struct State {std::uint32_t pc;unsigned remaining;};
    std::vector<State> pending;
    std::set<std::pair<std::uint32_t,unsigned>> seen;
    std::vector<const Instruction*> result;
    const auto queue=[&](std::uint32_t pc,unsigned remaining) {
        if(seen.size()<128 && seen.emplace(pc,remaining).second) pending.push_back({pc,remaining});
    };
    queue(call,argument);
    while(!pending.empty()) {
        const auto state=pending.back();pending.pop_back();
        const auto visit=[&](const Instruction& instruction) {
            const auto& ins=instruction.decoded;
            if(ins.meta.category==ZYDIS_CATEGORY_CALL || ins.meta.category==ZYDIS_CATEGORY_RET ||
               ins.meta.category==ZYDIS_CATEGORY_INTERRUPT || ins.mnemonic==ZYDIS_MNEMONIC_HLT) return;
            if(ins.mnemonic==ZYDIS_MNEMONIC_PUSH && ins.operand_width==32) {
                if(!state.remaining) result.push_back(&instruction);
                else queue(instruction.address,state.remaining-1);
                return;
            }
            for(unsigned i=0;i<ins.operand_count;++i) {
                const auto& operand=instruction.operands[i];
                if(!(operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) continue;
                if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER && general(operand.reg.value)==4) return;
                if(operand.type==ZYDIS_OPERAND_TYPE_MEMORY) {
                    if(operand.mem.base!=ZYDIS_REGISTER_ESP || operand.mem.index!=ZYDIS_REGISTER_NONE) return;
                    const auto start=operand.mem.disp.value;
                    const auto slot=static_cast<std::int64_t>(state.remaining)*4;
                    if(start<slot+4 && start+(operand.size+7)/8>slot) return;
                }
            }
            queue(instruction.address,state.remaining);
        };
        if(const auto* before=previous(function,state.pc); before &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_UNCOND_BR &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_RET &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_INTERRUPT) visit(*before);
        for(const auto& edge:function.edges) if(edge.to==state.pc &&
            (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump"))
            visit(function.instructions.at(edge.from));
    }
    return result;
}

bool callback_preserves_target(Xbe& image,std::uint32_t target,int reg) {
    const auto proof=std::ranges::find(callback_preservers,target,&CallbackPreserver::address);
    if(proof==callback_preservers.end() || !(proof->mask&(1U<<reg))) return false;
    if(hex_bytes(image.data(proof->address,proof->prefix.size()/2))!=proof->prefix)
        throw std::runtime_error("Callback register preservation differs at "+hex32(proof->address));
    return true;
}
bool callback_preserves(Xbe& image,const Instruction& call,int reg) {
    if(const auto target=call.target()) return callback_preserves_target(image,*target,reg);
    return !call.jump_targets.empty() && std::ranges::all_of(call.jump_targets,[&](auto target) {return callback_preserves_target(image,target,reg);});
}
constexpr std::uint32_t kernel_preserved_registers=232; // EBX/EBP/ESI/EDI; native normal-return ABI.
bool value_preserved(Xbe& image,const Instruction& call,unsigned reg) {
    return (call.kernel_ordinal && (kernel_preserved_registers&(1U<<reg))) || callback_preserves(image,call,reg);
}
// Discover code values along bounded predecessor paths. Internal calls are
// barriers unless their original code proves that this register is preserved.
std::vector<std::uint32_t> callback_values(Xbe& image,const Function& function,std::uint32_t pc,
    const ZydisDecodedOperand& value) {
    if(value.type==ZYDIS_OPERAND_TYPE_IMMEDIATE) return {static_cast<std::uint32_t>(value.imm.value.u)};
    if(value.type!=ZYDIS_OPERAND_TYPE_REGISTER || value.size!=32 || general(value.reg.value)<0) return {};
    struct State {std::uint32_t pc;int reg;};
    std::vector<State> pending;
    std::set<std::pair<std::uint32_t,int>> seen;
    std::vector<std::uint32_t> result;
    const auto queue=[&](std::uint32_t at,int reg) {
        if(reg>=0 && seen.size()<128 && seen.emplace(at,reg).second) pending.push_back({at,reg});
    };
    queue(pc,general(value.reg.value));
    while(!pending.empty()) {
        const auto state=pending.back();pending.pop_back();
        const auto visit=[&](const Instruction& instruction) {
            const auto& ins=instruction.decoded;const auto& operands=instruction.operands;
            if(ins.meta.category==ZYDIS_CATEGORY_RET || ins.meta.category==ZYDIS_CATEGORY_INTERRUPT ||
               ins.mnemonic==ZYDIS_MNEMONIC_HLT) return;
            if(ins.meta.category==ZYDIS_CATEGORY_CALL) {
                if(!callback_preserves(image,instruction,state.reg)) return;
                queue(instruction.address,state.reg);return;
            }
            bool written=false;
            for(unsigned i=0;i<ins.operand_count;++i) written |= operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                general(operands[i].reg.value)==state.reg && (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE);
            if(!written) {queue(instruction.address,state.reg);return;}
            if(operands[0].type!=ZYDIS_OPERAND_TYPE_REGISTER || operands[0].size!=32 ||
               general(operands[0].reg.value)!=state.reg) return;
            if(ins.mnemonic==ZYDIS_MNEMONIC_MOV) {
                if(operands[1].type==ZYDIS_OPERAND_TYPE_IMMEDIATE) result.push_back(static_cast<std::uint32_t>(operands[1].imm.value.u));
                else if(operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[1].size==32)
                    queue(instruction.address,general(operands[1].reg.value));
            } else if(ins.mnemonic==ZYDIS_MNEMONIC_LEA && operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[1].mem.base==ZYDIS_REGISTER_NONE && operands[1].mem.index==ZYDIS_REGISTER_NONE)
                result.push_back(static_cast<std::uint32_t>(operands[1].mem.disp.value));
            else if(ins.mnemonic==ZYDIS_MNEMONIC_XOR && operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                    operands[1].size==32 && general(operands[1].reg.value)==state.reg) result.push_back(0);
        };
        if(const auto* before=previous(function,state.pc); before &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_UNCOND_BR &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_RET &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_INTERRUPT) visit(*before);
        for(const auto& edge:function.edges) if(edge.to==state.pc &&
            (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump"))
            visit(function.instructions.at(edge.from));
    }
    return result;
}
// A verified consumer identifies the callback field of a submitted record.
// Follow that exact record register back to its field installation, keeping
// unknown calls and register changes as barriers. This does not type the arena.
std::vector<std::uint32_t> record_callback_values(Xbe& image,const Function& function,
    const Instruction& argument,std::uint32_t field) {
    const auto& pointer=argument.operands[0];
    if(pointer.type!=ZYDIS_OPERAND_TYPE_REGISTER || pointer.size!=32 || general(pointer.reg.value)<0) return {};
    struct State {std::uint32_t pc;int reg;};
    std::vector<State> pending;
    std::set<std::pair<std::uint32_t,int>> seen;
    std::vector<std::uint32_t> result;
    const auto queue=[&](std::uint32_t pc,int reg) {
        if(reg>=0 && seen.size()<128 && seen.emplace(pc,reg).second) pending.push_back({pc,reg});
    };
    queue(argument.address,general(pointer.reg.value));
    while(!pending.empty()) {
        const auto state=pending.back();pending.pop_back();
        const auto visit=[&](const Instruction& instruction) {
            const auto& ins=instruction.decoded;const auto& operands=instruction.operands;
            if(ins.meta.category==ZYDIS_CATEGORY_RET || ins.meta.category==ZYDIS_CATEGORY_INTERRUPT ||
               ins.mnemonic==ZYDIS_MNEMONIC_HLT) return;
            if(ins.meta.category==ZYDIS_CATEGORY_CALL) {
                if(callback_preserves(image,instruction,state.reg)) queue(instruction.address,state.reg);
                return;
            }
            for(unsigned i=0;i<ins.operand_count;++i) {
                const auto& operand=operands[i];
                if(!(operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) continue;
                if(operand.type==ZYDIS_OPERAND_TYPE_MEMORY && general(operand.mem.base)==state.reg) {
                    if(operand.mem.index!=ZYDIS_REGISTER_NONE || operand.mem.segment!=ZYDIS_REGISTER_DS) return;
                    const auto start=operand.mem.disp.value;
                    if(start<static_cast<std::int64_t>(field)+4 && start+(operand.size+7)/8>field) {
                        if(i==0 && ins.mnemonic==ZYDIS_MNEMONIC_MOV && start==field && operand.size==32) {
                            const auto values=callback_values(image,function,instruction.address,operands[1]);
                            result.insert(result.end(),values.begin(),values.end());
                        }
                        return;
                    }
                }
                if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER && general(operand.reg.value)==state.reg) {
                    if(i==0 && operand.size==32 && ins.mnemonic==ZYDIS_MNEMONIC_MOV &&
                       operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[1].size==32)
                        queue(instruction.address,general(operands[1].reg.value));
                    return;
                }
            }
            queue(instruction.address,state.reg);
        };
        if(const auto* before=previous(function,state.pc); before &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_UNCOND_BR &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_RET &&
           before->decoded.meta.category!=ZYDIS_CATEGORY_INTERRUPT) visit(*before);
        for(const auto& edge:function.edges) if(edge.to==state.pc &&
            (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump"))
            visit(function.instructions.at(edge.from));
    }
    return result;
}
std::optional<std::uint32_t> stack_callback_before(const Function& function,std::uint32_t call,
    const Instruction& argument,std::uint32_t field) {
    const auto& pointer=argument.operands[0];
    if(pointer.type!=ZYDIS_OPERAND_TYPE_REGISTER || pointer.size!=32) return {};
    struct Store {std::int64_t first,last;std::optional<std::uint32_t> value;};
    std::vector<Store> stores;
    std::int64_t delta=0;
    std::optional<std::int64_t> slot;
    int wanted=-1;
    auto pc=call;
    for(unsigned steps=0;steps<128;++steps) {
        if(std::ranges::any_of(function.edges,[&](const Edge& edge){return edge.to==pc &&
            (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump");})) return {};
        const auto* instruction=previous(function,pc);
        if(!instruction) return {};
        const auto& ins=instruction->decoded;
        const auto& operands=instruction->operands;
        if(ins.meta.category==ZYDIS_CATEGORY_CALL || ins.meta.category==ZYDIS_CATEGORY_COND_BR ||
           ins.meta.category==ZYDIS_CATEGORY_UNCOND_BR || ins.meta.category==ZYDIS_CATEGORY_RET) return {};
        bool changes_stack=false;
        for(unsigned i=0;i<ins.operand_count;++i) changes_stack |= operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER &&
            general(operands[i].reg.value)==4 && (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE);
        if(ins.mnemonic==ZYDIS_MNEMONIC_PUSH && ins.operand_width==32) {
            delta+=4;
            stores.push_back({delta-4,delta,constant_before(function,instruction->address,operands[0])});
        }
        else if(ins.mnemonic==ZYDIS_MNEMONIC_POP && ins.operand_width==32) delta-=4;
        else if(changes_stack) {
            if((ins.mnemonic!=ZYDIS_MNEMONIC_ADD && ins.mnemonic!=ZYDIS_MNEMONIC_SUB) ||
               operands[0].type!=ZYDIS_OPERAND_TYPE_REGISTER || operands[0].reg.value!=ZYDIS_REGISTER_ESP ||
               operands[1].type!=ZYDIS_OPERAND_TYPE_IMMEDIATE || operands[1].imm.value.u>65536) return {};
            const auto amount=static_cast<std::int64_t>(operands[1].imm.value.u);
            delta+=ins.mnemonic==ZYDIS_MNEMONIC_SUB?amount:-amount;
        }
        if(delta < -65536 || delta>65536) return {};
        for(unsigned i=0;i<ins.operand_count_visible;++i) {
            const auto& operand=operands[i];
            if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || !(operand.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) continue;
            // Unknown aliases or partial writes cannot prove a record field.
            if(operand.mem.base!=ZYDIS_REGISTER_ESP || operand.mem.index!=ZYDIS_REGISTER_NONE) return {};
            const auto start=delta+operand.mem.disp.value;
            const auto value=ins.mnemonic==ZYDIS_MNEMONIC_MOV && i==0 && operand.size==32?
                constant_before(function,instruction->address,operands[1]):std::nullopt;
            stores.push_back({start,start+(operand.size+7)/8,value});
        }
        if(instruction->address==argument.address) wanted=general(pointer.reg.value);
        else if(wanted>=0) {
            bool written=false;
            for(unsigned i=0;i<ins.operand_count;++i) written |= operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                general(operands[i].reg.value)==wanted && (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE);
            if(written) {
                if(operands[0].type!=ZYDIS_OPERAND_TYPE_REGISTER || general(operands[0].reg.value)!=wanted || operands[0].size!=32) return {};
                if(ins.mnemonic==ZYDIS_MNEMONIC_MOV && operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[1].size==32)
                    wanted=general(operands[1].reg.value);
                else if(ins.mnemonic==ZYDIS_MNEMONIC_LEA && operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY &&
                        operands[1].mem.base==ZYDIS_REGISTER_ESP && operands[1].mem.index==ZYDIS_REGISTER_NONE) {
                    slot=delta+operands[1].mem.disp.value+field;
                    wanted=-1;
                } else return {};
            }
        }
        if(slot) for(const auto& store:stores) if(store.first<*slot+4 && store.last>*slot)
            return store.first==*slot && store.last==*slot+4?store.value:std::nullopt;
        pc=instruction->address;
    }
    return {};
}
void factory_callbacks(Xbe& image,Function& function) {
    const auto factory_at=[](const Instruction& instruction) {
        if(instruction.decoded.meta.category==ZYDIS_CATEGORY_CALL)
            for(unsigned i=0;i<callback_factories.size();++i)
                if(instruction.target()==callback_factories[i].address) return i+1;
        return 0U;
    };
    bool used=false;
    for(const auto& [pc,instruction]:function.instructions) if(const auto index=factory_at(instruction)) {
        const auto& factory=callback_factories[index-1];
        if(factory.count>factory.fields.size() || hex_bytes(image.data(factory.address,factory.prefix.size()/2))!=factory.prefix)
            throw std::runtime_error("Callback object factory differs at "+hex32(factory.address));
        used=true;
    }
    if(!used) return;
    // Track only exact returned-object aliases. Calls retain only registers
    // covered by verified preservation contracts; joins require agreement.
    using State=std::array<unsigned,8>;
    std::map<std::uint32_t,State> states;
    std::deque<std::uint32_t> pending;
    const auto update=[&](std::uint32_t pc,const State& value) {
        if(!function.instructions.contains(pc)) return;
        const auto [found,inserted]=states.emplace(pc,value);
        bool changed=inserted;
        if(!inserted) for(unsigned i=0;i<value.size();++i)
            if(found->second[i] && found->second[i]!=value[i]) {found->second[i]=0;changed=true;}
        if(changed) pending.push_back(pc);
    };
    update(function.entry,State{});
    unsigned iterations=0;
    while(!pending.empty() && ++iterations<=function.instructions.size()*16) {
        const auto pc=pending.front();pending.pop_front();
        const auto& instruction=function.instructions.at(pc);
        const auto& ins=instruction.decoded;const auto& operands=instruction.operands;
        auto state=states.at(pc);const auto original=state;
        for(unsigned i=0;i<ins.operand_count;++i) if(operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER &&
            (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            const auto reg=general(operands[i].reg.value);if(reg>=0) state[reg]=0;
        }
        if(ins.mnemonic==ZYDIS_MNEMONIC_MOV && operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[0].size==32 &&
           operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[1].size==32) {
            const auto target=general(operands[0].reg.value),source=general(operands[1].reg.value);
            if(target>=0 && source>=0) state[target]=original[source];
        }
        const auto next=pc+ins.length;const auto target=instruction.target();
        if(ins.meta.category==ZYDIS_CATEGORY_CALL) {
            for(unsigned reg=0;reg<state.size();++reg)
                if(state[reg] && !callback_preserves(image,instruction,reg)) state[reg]=0;
            state[0]=factory_at(instruction);update(next,state);
        }
        else if(ins.meta.category==ZYDIS_CATEGORY_COND_BR) {update(next,state);if(target) update(*target,state);}
        else if(ins.meta.category==ZYDIS_CATEGORY_UNCOND_BR) {
            if(target) update(*target,state);else for(auto address:instruction.jump_targets) update(address,state);
        } else if(ins.meta.category!=ZYDIS_CATEGORY_RET && ins.meta.category!=ZYDIS_CATEGORY_INTERRUPT &&
                  ins.mnemonic!=ZYDIS_MNEMONIC_INT3 && ins.mnemonic!=ZYDIS_MNEMONIC_HLT) update(next,state);
    }
    if(!pending.empty()) return;
    for(const auto& [pc,instruction]:function.instructions) {
        if(!states.contains(pc) || instruction.decoded.mnemonic!=ZYDIS_MNEMONIC_MOV) continue;
        const auto& field=instruction.operands[0];
        if(field.type!=ZYDIS_OPERAND_TYPE_MEMORY || field.size!=32 || field.mem.index!=ZYDIS_REGISTER_NONE ||
           field.mem.segment!=ZYDIS_REGISTER_DS) continue;
        const auto base=general(field.mem.base);
        if(base<0 || !states.at(pc)[base]) continue;
        const auto& factory=callback_factories[states.at(pc)[base]-1];
        if(!std::ranges::contains(std::span(factory.fields).first(factory.count),field.mem.disp.value)) continue;
        for(const auto target:callback_values(image,function,pc,instruction.operands[1]))
            if(target) {image.code_section(target);function.static_callbacks.push_back(target);}
    }
}
void resolve_import_aliases(Xbe& image,Function& function) {
    // A tag is an exact imported function value, not an arbitrary address.
    // Joins retain it only when all decoded incoming paths agree.
    using State=std::array<std::uint32_t,8>;
    std::map<std::uint32_t,State> states;
    std::deque<std::uint32_t> pending;
    const auto update=[&](std::uint32_t pc,const State& value) {
        if(!function.instructions.contains(pc)) return;
        const auto [found,inserted]=states.emplace(pc,value);
        bool changed=inserted;
        if(!inserted) for(unsigned reg=0;reg<value.size();++reg)
            if(found->second[reg] && found->second[reg]!=value[reg]) {found->second[reg]=0;changed=true;}
        if(changed) pending.push_back(pc);
    };
    update(function.entry,State{});
    unsigned iterations=0;
    while(!pending.empty() && ++iterations<=function.instructions.size()*16) {
        const auto pc=pending.front();pending.pop_front();
        const auto& instruction=function.instructions.at(pc);
        const auto& ins=instruction.decoded;const auto& operands=instruction.operands;
        auto state=states.at(pc);const auto original=state;
        for(unsigned i=0;i<ins.operand_count;++i) if(operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER &&
            (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            const auto reg=general(operands[i].reg.value);if(reg>=0) state[reg]=0;
        }
        if(ins.mnemonic==ZYDIS_MNEMONIC_MOV && operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[0].size==32) {
            const auto target=general(operands[0].reg.value);
            if(target>=0 && operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[1].size==32) {
                const auto source=general(operands[1].reg.value);if(source>=0) state[target]=original[source];
            } else if(target>=0 && ins.address_width==32 && operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY && operands[1].size==32 &&
                operands[1].mem.segment==ZYDIS_REGISTER_DS && operands[1].mem.base==ZYDIS_REGISTER_NONE &&
                operands[1].mem.index==ZYDIS_REGISTER_NONE)
                if(const auto* import=image.kernel_import(static_cast<std::uint32_t>(operands[1].mem.disp.value))) state[target]=import->ordinal;
        }
        const auto category=ins.meta.category;const auto destination=instruction.target();const auto next=pc+ins.length;
        if(category==ZYDIS_CATEGORY_CALL) {
            const auto target=operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[0].size==32?general(operands[0].reg.value):-1;
            const auto ordinal=instruction.kernel_ordinal.value_or(target>=0?original[target]:0);
            for(unsigned reg=0;reg<state.size();++reg)
                if(state[reg] && !((ordinal && (kernel_preserved_registers&(1U<<reg))) || callback_preserves(image,instruction,reg))) state[reg]=0;
            update(next,state);
        } else if(category==ZYDIS_CATEGORY_COND_BR) {update(next,state);if(destination) update(*destination,state);}
        else if(category==ZYDIS_CATEGORY_UNCOND_BR) {
            if(destination) update(*destination,state);else for(auto target:instruction.jump_targets) update(target,state);
        } else if(category!=ZYDIS_CATEGORY_RET && category!=ZYDIS_CATEGORY_INTERRUPT &&
                  ins.mnemonic!=ZYDIS_MNEMONIC_INT3 && ins.mnemonic!=ZYDIS_MNEMONIC_HLT) update(next,state);
    }
    if(!pending.empty()) return;
    for(auto& [pc,instruction]:function.instructions) {
        const auto category=instruction.decoded.meta.category;const auto& operand=instruction.operands[0];
        if((category!=ZYDIS_CATEGORY_CALL && category!=ZYDIS_CATEGORY_UNCOND_BR) || instruction.target() || instruction.kernel_ordinal ||
            !instruction.jump_targets.empty() || !states.contains(pc) || operand.type!=ZYDIS_OPERAND_TYPE_REGISTER || operand.size!=32) continue;
        const auto reg=general(operand.reg.value);if(reg<0 || !states.at(pc)[reg]) continue;
        instruction.kernel_ordinal=states.at(pc)[reg];
        function.edges.push_back({pc,category==ZYDIS_CATEGORY_CALL?"kernel-call":"kernel-jump",instruction.kernel_ordinal});
    }
}
void resolve_table_pointers(Xbe& image,Function& function,std::span<const Decoder::PointerSource> sources) {
    static_assert(std::size(entry_tables)<=128);
    using Tables=std::bitset<128>;
    Tables allowed;
    for(const auto& source:sources) allowed.set(source.table);
    struct Value {
        Tables tables;
        std::uint32_t offset=0;
        unsigned kind=0; // Unknown, table address, object, method cell, null.
        bool operator==(const Value&) const = default;
    };
    const auto source_value=[&](std::uint32_t address,bool global) {
        Value result;
        for(const auto& source:sources) if(source.source==address && source.global==global) {
            const auto kind=source.object?2U:1U;
            if(result.kind && result.kind!=kind) return Value{};
            result.kind=kind;result.tables.set(source.table);
        }
        return result;
    };
    const auto literal=[&](std::uint32_t address) {
        Value result;if(!address) {result.kind=4;return result;}
        for(unsigned i=0;i<std::size(entry_tables);++i) if(allowed[i] && entry_tables[i].start==address) {result.kind=1;result.tables.set(i);}
        return result;
    };
    const auto field=[&](Value base,std::uint32_t displacement) {
        if(base.kind==2 && !displacement) {base.kind=1;return base;}
        if(base.kind!=1) return Value{};
        base.offset+=displacement;
        for(unsigned i=0;i<std::size(entry_tables);++i) if(base.tables[i]) {
            const auto& table=entry_tables[i];
            if(base.offset>=table.end-table.start || base.offset%table.stride) return Value{};
        }
        base.kind=3;return base;
    };
    const auto targets=[&](const Value& value) {
        std::vector<std::uint32_t> result;
        if(value.kind!=3) return result;
        for(unsigned i=0;i<std::size(entry_tables);++i) if(value.tables[i]) {
            const auto target=u32(image.data(entry_tables[i].start+value.offset,4),0);
            if(!target) continue;
            image.code_section(target);result.push_back(target);
        }
        std::ranges::sort(result);result.erase(std::unique(result.begin(),result.end()),result.end());return result;
    };
    using State=std::array<Value,8>;
    const auto operand_value=[&](const ZydisDecodedOperand& operand,const State& state) {
        if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER && operand.size==32) {
            const auto reg=general(operand.reg.value);return reg>=0?state[reg]:Value{};
        }
        if(operand.type==ZYDIS_OPERAND_TYPE_IMMEDIATE) return literal(static_cast<std::uint32_t>(operand.imm.value.u));
        if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || operand.size!=32 || operand.mem.segment!=ZYDIS_REGISTER_DS || operand.mem.index!=ZYDIS_REGISTER_NONE) return Value{};
        if(operand.mem.base==ZYDIS_REGISTER_NONE) return source_value(static_cast<std::uint32_t>(operand.mem.disp.value),true);
        const auto reg=general(operand.mem.base);return reg>=0?field(state[reg],static_cast<std::uint32_t>(operand.mem.disp.value)):Value{};
    };
    bool used=false;
    for(const auto& [pc,instruction]:function.instructions) {
        if(instruction.decoded.meta.category==ZYDIS_CATEGORY_CALL) {
            auto callees=instruction.jump_targets;if(const auto target=instruction.target()) callees.push_back(*target);
            for(auto target:callees) used |= source_value(target,false).kind!=0;
        }
        if(instruction.decoded.mnemonic==ZYDIS_MNEMONIC_MOV && instruction.operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto value=operand_value(instruction.operands[1],State{});
            used |= value.kind==1 || value.kind==2;
        }
    }
    if(!used) return;
    const auto merge=[](Value a,const Value& b) {
        if(a==b) return a;
        if(a.kind==4 && b.kind) return b;
        if(b.kind==4 && a.kind) return a;
        if(!a.kind || a.kind!=b.kind || a.offset!=b.offset) return Value{};
        a.tables|=b.tables;return a;
    };
    std::map<std::uint32_t,State> states;
    std::deque<std::uint32_t> pending;
    const auto update=[&](std::uint32_t pc,const State& value) {
        if(!function.instructions.contains(pc)) return;
        const auto [found,inserted]=states.emplace(pc,value);bool changed=inserted;
        if(!inserted) for(unsigned reg=0;reg<value.size();++reg) {
            const auto merged=merge(found->second[reg],value[reg]);
            if(merged!=found->second[reg]) {found->second[reg]=merged;changed=true;}
        }
        if(changed) pending.push_back(pc);
    };
    update(function.entry,State{});unsigned iterations=0;
    while(!pending.empty() && ++iterations<=function.instructions.size()*32) {
        const auto pc=pending.front();pending.pop_front();const auto& instruction=function.instructions.at(pc);
        const auto& ins=instruction.decoded;const auto& operands=instruction.operands;
        auto state=states.at(pc);const auto original=state;
        // An untracked alias may replace a vptr. Keep object proofs only across reads.
        for(unsigned i=0;i<ins.operand_count;++i) if(operands[i].type==ZYDIS_OPERAND_TYPE_MEMORY &&
            (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            for(auto& value:state) if(value.kind==2) value={};
        }
        for(unsigned i=0;i<ins.operand_count;++i) if(operands[i].type==ZYDIS_OPERAND_TYPE_REGISTER && (operands[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            const auto reg=general(operands[i].reg.value);if(reg>=0) state[reg]={};
        }
        if(ins.mnemonic==ZYDIS_MNEMONIC_MOV && ins.address_width==32 && operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[0].size==32) {
            const auto reg=general(operands[0].reg.value);if(reg>=0) state[reg]=operand_value(operands[1],original);
        }
        if(ins.mnemonic==ZYDIS_MNEMONIC_XOR && operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[0].size==32 &&
           operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value==operands[1].reg.value) {
            const auto reg=general(operands[0].reg.value);if(reg>=0) state[reg].kind=4;
        }
        const auto next=pc+ins.length;const auto destination=instruction.target();
        if(ins.meta.category==ZYDIS_CATEGORY_CALL) {
            auto callees=instruction.jump_targets;
            if(destination) callees={*destination};else if(callees.empty()) callees=targets(operand_value(operands[0],original));
            const bool leaf=!callees.empty() && std::ranges::all_of(callees,[&](auto target) {
                return std::ranges::any_of(sources,[&](const auto& source) {return source.source==target && source.leaf;});
            });
            for(unsigned reg=0;reg<state.size();++reg) if(state[reg].kind &&
                !(leaf && reg!=0) &&
                !value_preserved(image,instruction,reg) && (callees.empty() || !std::ranges::all_of(callees,[&](auto target) {return callback_preserves_target(image,target,reg);}))) state[reg]={};
            // Register preservation alone does not prove an object's vptr survived.
            if(!leaf) for(auto& value:state) if(value.kind==2) value={};
            Value returned;bool first=true;
            for(auto target:callees) {const auto value=source_value(target,false);returned=first?value:merge(returned,value);first=false;}
            state[0]=returned;update(next,state);
        } else if(ins.meta.category==ZYDIS_CATEGORY_COND_BR) {update(next,state);if(destination) update(*destination,state);}
        else if(ins.meta.category==ZYDIS_CATEGORY_UNCOND_BR) {
            if(destination) update(*destination,state);else if(!instruction.tail_dispatch) for(auto target:instruction.jump_targets) update(target,state);
        } else if(ins.meta.category!=ZYDIS_CATEGORY_RET && ins.meta.category!=ZYDIS_CATEGORY_INTERRUPT && ins.mnemonic!=ZYDIS_MNEMONIC_INT3 && ins.mnemonic!=ZYDIS_MNEMONIC_HLT) update(next,state);
    }
    if(!pending.empty()) return;
    for(auto& [pc,instruction]:function.instructions) {
        const auto category=instruction.decoded.meta.category;
        if((category!=ZYDIS_CATEGORY_CALL && category!=ZYDIS_CATEGORY_UNCOND_BR) || instruction.target() || instruction.kernel_ordinal ||
           !instruction.jump_targets.empty() || !states.contains(pc)) continue;
        auto proven=targets(operand_value(instruction.operands[0],states.at(pc)));if(proven.empty()) continue;
        instruction.jump_targets=proven;instruction.tail_dispatch=category==ZYDIS_CATEGORY_UNCOND_BR;
        for(auto target:proven) {function.static_callbacks.push_back(target);function.edges.push_back({pc,instruction.tail_dispatch?"tail-call":"call",target});}
    }
}
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
std::map<std::uint32_t,std::vector<std::uint32_t>> bounded_tables(Xbe& image,const Function& function,
    const std::set<std::uint32_t>& reconsider={}) {
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
                const auto& lookup=operands[1];
                if(ins.address_width==32 && lookup.type==ZYDIS_OPERAND_TYPE_MEMORY && lookup.mem.segment==ZYDIS_REGISTER_DS) {
                    const auto index=lookup.mem.index==ZYDIS_REGISTER_NONE?general(lookup.mem.base):
                        lookup.mem.base==ZYDIS_REGISTER_NONE && lookup.mem.scale==1?general(lookup.mem.index):-1;
                    if(index>=0 && original.registers[index].count) {
                        const auto range=original.registers[index];
                        const auto address=static_cast<std::uint32_t>(lookup.mem.disp.value)+range.first;
                        const auto section=std::ranges::find_if(image.sections,[&](const Section& value) {
                            return !(value.flags&1) && address>=value.address &&
                                static_cast<std::uint64_t>(address)-value.address+range.count<=std::min(value.size,value.raw_size);
                        });
                        if(section!=image.sections.end()) {
                            const auto bytes=image.data(address,range.count);
                            std::vector<std::uint32_t> values;values.reserve(range.count);
                            for(const auto value:bytes) values.push_back(std::to_integer<unsigned char>(value));
                            state.registers[target]=enclosing(std::move(values));
                        }
                    }
                }
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
        if(category==ZYDIS_CATEGORY_CALL) {
            FlowState preserved;
            for(unsigned reg=0;reg<state.registers.size();++reg) if((state.registers[reg].count || state.nonzero_masks[reg]) && value_preserved(image,instruction,reg)) {
                preserved.registers[reg]=state.registers[reg];preserved.nonzero_masks[reg]=state.nonzero_masks[reg];
            }
            update(next,preserved);continue;
        }
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
            (!instruction.jump_targets.empty() && !reconsider.contains(pc)) || !states.contains(pc) || operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || operand.size!=32 ||
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
    // Scheduling can place independent saves, loads and SIMD operations anywhere
    // between CMP, its branch, remapping and dispatch. Check their actual flag
    // and register writes rather than requiring adjacent instruction spellings.
    const auto preserves=[](const Instruction& instruction,ZydisRegister index) {
        const auto& decoded=instruction.decoded;
        const auto category=decoded.meta.category;
        if(decoded.mnemonic!=ZYDIS_MNEMONIC_PUSH && decoded.mnemonic!=ZYDIS_MNEMONIC_MOV &&
           decoded.mnemonic!=ZYDIS_MNEMONIC_LEA && category!=ZYDIS_CATEGORY_DATAXFER &&
           category!=ZYDIS_CATEGORY_SSE && category!=ZYDIS_CATEGORY_LOGICAL_FP &&
           category!=ZYDIS_CATEGORY_CONVERT && category!=ZYDIS_CATEGORY_MMX && category!=ZYDIS_CATEGORY_NOP) return false;
        if(decoded.cpu_flags && (decoded.cpu_flags->modified || decoded.cpu_flags->set_0 ||
           decoded.cpu_flags->set_1 || decoded.cpu_flags->undefined)) return false;
        for(unsigned i=0;i<decoded.operand_count;++i) {
            const auto& changed=instruction.operands[i];
            if(changed.type==ZYDIS_OPERAND_TYPE_REGISTER && (changed.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
               ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,changed.reg.value)==index) return false;
        }
        return true;
    };
    unsigned saved=0;
    const auto skip_saves=[&](const Instruction* instruction,ZydisRegister index) {
        unsigned skipped=0;
        while(instruction && preserves(*instruction,index) && skipped++<8) {
            ++saved;instruction=previous(function,instruction->address);
        }
        return instruction;
    };
    branch=skip_saves(branch,operand.mem.index);
    const ZydisDecodedOperand* lookup = nullptr;
    auto compared_register=operand.mem.index;
    if (branch && branch->decoded.mnemonic == ZYDIS_MNEMONIC_MOVZX &&
        branch->operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && branch->operands[0].reg.value == operand.mem.index &&
        branch->operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && branch->operands[1].size == 8 &&
        branch->operands[1].mem.base != ZYDIS_REGISTER_NONE && branch->operands[1].mem.index == ZYDIS_REGISTER_NONE &&
        branch->operands[1].mem.segment == ZYDIS_REGISTER_DS && branch->operands[1].mem.disp.has_displacement) {
        lookup = &branch->operands[1];
        compared_register=lookup->mem.base;
        branch = skip_saves(previous(function, branch->address),compared_register);
    }
    if(saved && compared_register==ZYDIS_REGISTER_ESP) return {};
    if (!branch || (branch->decoded.mnemonic != ZYDIS_MNEMONIC_JNBE && branch->decoded.mnemonic != ZYDIS_MNEMONIC_JNB)) return {};
    const auto* compare = skip_saves(previous(function, branch->address),compared_register);
    if (!compare || compare->decoded.mnemonic != ZYDIS_MNEMONIC_CMP ||
        compare->operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER || compare->operands[0].size != 32 ||
        compare->operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
        return {};
    auto proof_begin=compare->address;
    std::uint32_t lookup_bias=0;
    if(compare->operands[0].reg.value!=compared_register) {
        // A compiler may check temp = index - low while using the original
        // index for its byte remap. The LEA must also dominate the dispatch.
        const auto* adjust=previous(function,compare->address);
        if(!lookup || !adjust || adjust->decoded.mnemonic!=ZYDIS_MNEMONIC_LEA || adjust->decoded.address_width!=32 ||
            adjust->operands[0].type!=ZYDIS_OPERAND_TYPE_REGISTER || adjust->operands[0].size!=32 ||
            adjust->operands[0].reg.value!=compare->operands[0].reg.value ||
            adjust->operands[1].type!=ZYDIS_OPERAND_TYPE_MEMORY || adjust->operands[1].mem.base!=compared_register ||
            adjust->operands[1].mem.index!=ZYDIS_REGISTER_NONE) return {};
        lookup_bias=static_cast<std::uint32_t>(adjust->operands[1].mem.disp.value);
        proof_begin=adjust->address;
    }
    const auto bound = compare->operands[1].imm.value.u;
    auto count = bound + (branch->decoded.mnemonic == ZYDIS_MNEMONIC_JNBE ? 1 : 0);
    // A byte remap may consume a wider guarded range while still producing at
    // most 256 dispatch slots (the video VLC tables use 512 input entries).
    if (!count || count > (lookup ? 65536U : 256U) ||
        (function.entry > proof_begin && function.entry <= jump.address)) return {};
    if (lookup) {
        const auto remap = image.data(static_cast<std::uint32_t>(lookup->mem.disp.value)-lookup_bias, static_cast<std::size_t>(count));
        count = std::to_integer<unsigned char>(*std::ranges::max_element(remap)) + 1U;
    }
    return TableGuard{proof_begin, branch->address, static_cast<std::uint32_t>(count)};
}
}
std::optional<std::uint32_t> indirect_slot(const Function& function,const Instruction& instruction) {
    const auto& operand=instruction.operands[0];
    if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER) return constant_before(function,instruction.address,operand,true);
    if(operand.type==ZYDIS_OPERAND_TYPE_MEMORY && operand.size==32 && instruction.decoded.address_width==32 &&
       operand.mem.base==ZYDIS_REGISTER_NONE && operand.mem.index==ZYDIS_REGISTER_NONE && operand.mem.segment==ZYDIS_REGISTER_DS)
        return static_cast<std::uint32_t>(operand.mem.disp.value);
    return {};
}
Decoder::Decoder() {
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32)) ||
        !ZYAN_SUCCESS(ZydisFormatterInit(&formatter_, ZYDIS_FORMATTER_STYLE_INTEL)))
        throw std::runtime_error("Cannot initialise the x86 decoder");
}
const std::vector<Decoder::PointerSource>& Decoder::pointer_sources(Xbe& image) {
    if(pointer_image_==&image) return pointer_sources_;
    std::vector<PointerSource> result;
    for(const auto& source:table_pointer_sources) {
        if(source.prefix.empty() || hex_bytes(image.data(source.proof_address,source.prefix.size()/2))!=source.prefix)
            throw std::runtime_error("Table pointer source differs at "+hex32(source.proof_address));
        const auto found=std::ranges::find(entry_tables,source.table,&EntryTable::start);
        if(found==std::end(entry_tables) || found->end<=found->start || found->end-found->start>128*1024 || found->stride<4 || found->stride%4)
            throw std::runtime_error("Table pointer source has no bounded interface at "+hex32(source.table));
        bool leaf=false;
        if(!source.global && !source.object && source.proof_address==source.source) {
            const auto code=image.data(source.source,11);
            leaf=(code[0]==std::byte{0xB8} && u32(code,1)==source.table && code[5]==std::byte{0xC3}) ||
                (code[0]==std::byte{0xA1} && code[5]==std::byte{0xB8} && u32(code,6)==source.table && code[10]==std::byte{0xC3});
        }
        result.push_back({source.source,static_cast<unsigned>(found-std::begin(entry_tables)),source.global,source.object,leaf});
    }
    pointer_sources_=std::move(result);pointer_image_=&image;return pointer_sources_;
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
                if(*destination!=function.entry && std::ranges::any_of(native_apis,[&](const NativeApi& api){return api.address==*destination;})) {
                    instruction.tail_target=*destination;
                    edge("tail-call",destination);
                } else {
                    edge(category == ZYDIS_CATEGORY_COND_BR ? "conditional" : "jump", destination);
                    schedule(*destination);
                }
            } else if(const auto table=std::ranges::find_if(static_call_tables,[&](const StaticCallTable& item){return item.jump && item.site==pc;});table!=std::end(static_call_tables)) {
                if(hex_bytes(image.data(table->proof_address,table->prefix.size()/2))!=table->prefix ||
                   table->end<=table->start || table->end-table->start>128*1024 || table->stride!=4)
                    throw std::runtime_error("Static jump proof differs at "+hex32(pc));
                const auto bytes=image.data(table->start,table->end-table->start);
                for(std::uint32_t offset=0;offset<bytes.size();offset+=4) {
                    const auto target=u32(bytes,offset);schedule(target);
                    instruction.jump_targets.push_back(target);edge("table-jump",target);
                }
            } else if (const auto guard = table_guard(image, function, instruction)) {
                const auto entries = image.data(static_cast<std::uint32_t>(instruction.operands[0].mem.disp.value), guard->count * 4);
                for (std::uint32_t i = 0; i < guard->count; ++i) {
                    const auto target = u32(entries, i * 4);
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
    std::set<std::uint32_t> reconsider;
    for (const auto& [pc, guard] : guards) for (const auto& edge : function.edges)
        if (edge.to && (edge.kind == "jump" || edge.kind == "conditional" || edge.kind == "table-jump") &&
            *edge.to > guard.begin && *edge.to <= pc) {reconsider.insert(pc);break;}
    // Some bypasses explicitly set an in-range constant. Check all decoded
    // candidate cases together, including backedges, before retaining a table.
    // A range proof may only reuse cases already present in that complete CFG.
    const auto rechecked=reconsider.empty()?std::map<std::uint32_t,std::vector<std::uint32_t>>{}:bounded_tables(image,function,reconsider);
    for(auto pc:reconsider) {
        auto& targets=function.instructions.at(pc).jump_targets;
        const auto proof=rechecked.find(pc);
        if(proof==rechecked.end() || !std::ranges::all_of(proof->second,[&](auto target) {
            return function.instructions.contains(target) && std::ranges::find(targets,target)!=targets.end();
        })) {targets.clear();function.unresolved=true;}
    }
    // Address-specific research describes bounded immutable pointer tables. The
    // proof bytes and all targets are checked against the supported executable.
    for (const auto& table : static_call_tables) {
        if(table.jump) continue;
        auto found = function.instructions.find(table.site);
        if (found == function.instructions.end()) continue;
        auto& call = found->second;
        if (call.decoded.meta.category != (table.tail?ZYDIS_CATEGORY_UNCOND_BR:ZYDIS_CATEGORY_CALL) || call.target() || call.kernel_ordinal ||
            hex_bytes(image.data(table.proof_address,table.prefix.size()/2)) != table.prefix ||
            (table.slot && indirect_slot(function,call)!=table.slot))
            throw std::runtime_error("Static call proof differs at " + hex32(table.site));
        if (table.end <= table.start || table.end-table.start > 128*1024 || table.stride < 4 || table.stride % 4)
            throw std::runtime_error("Invalid static call table bounds");
        for (auto slot=table.start;slot<table.end;slot+=table.stride) {
            const auto target=u32(image.data(slot,4),0);
            if (!target || target==UINT32_MAX) continue;
            image.code_section(target);
            call.jump_targets.push_back(target);
            function.static_callbacks.push_back(target);
        }
        if(call.jump_targets.empty()) throw std::runtime_error("Static call table has no targets");
        call.tail_dispatch=table.tail;
        std::erase_if(function.edges,[&](const Edge& edge) {return edge.from==table.site &&
            (edge.kind=="call" || edge.kind=="jump") && !edge.to;});
        for(auto target:call.jump_targets) function.edges.push_back({table.site,table.tail?"tail-call":"call",target});
    }
    for(const auto& table:entry_tables) if(function.entry==table.owner) {
        if(hex_bytes(image.data(table.owner,table.prefix.size()/2))!=table.prefix ||
           table.end<=table.start || table.end-table.start>128*1024 || (table.end-table.start)%4 ||
           table.stride<4 || table.stride>128*1024 || table.stride%4)
            throw std::runtime_error("Callback table proof differs at "+hex32(table.owner));
        const auto bytes=image.data(table.start,table.end-table.start);
        for(std::uint32_t offset=0;offset<bytes.size();offset+=table.stride) {
            const auto target=u32(bytes,offset);
            if(!target) continue;
            image.code_section(target);function.static_callbacks.push_back(target);
        }
    }
    resolve_import_aliases(image,function);
    if(!function.budget_exhausted) resolve_table_pointers(image,function,decoder.pointer_sources(image));
    // Bounds propagation and guard invalidation can both change a dispatch.
    // Report unresolved edges from the final instruction state.
    std::erase_if(function.edges,[&](const auto& edge) {
        return (!edge.to && (edge.kind=="call" || edge.kind=="jump")) ||
            (edge.kind=="table-jump" && function.instructions.at(edge.from).jump_targets.empty());
    });
    function.unresolved=false;
    for(const auto& [pc,instruction]:function.instructions) {
        const auto category=instruction.decoded.meta.category;
        if((category==ZYDIS_CATEGORY_CALL || category==ZYDIS_CATEGORY_UNCOND_BR) && !instruction.target() &&
           !instruction.kernel_ordinal && instruction.jump_targets.empty()) {
            function.unresolved=true;
            function.edges.push_back({pc,category==ZYDIS_CATEGORY_CALL?"call":"jump",std::nullopt});
        }
    }
    // Recover code-valued callback arguments from verified registration ABIs.
    // Writes to the outgoing arguments and control transfers stop the proof;
    // unrelated local stack writes cannot change those callback arguments.
    for (const auto& [pc, call] : function.instructions) {
        if (call.decoded.meta.category != ZYDIS_CATEGORY_CALL) continue;
        for (const auto& abi : callback_abis) {
            if(abi.site ? pc!=abi.site : abi.ordinal ? call.kernel_ordinal!=abi.ordinal : call.target()!=abi.address) continue;
            const auto proof=abi.site?abi.site:abi.address;
            if(!abi.prefix.empty() && hex_bytes(image.data(proof,abi.prefix.size()/2))!=abi.prefix)
                throw std::runtime_error("Callback registration ABI differs at "+hex32(proof));
            // Direct callback arguments can be prepared on separate branches.
            // Record-field recovery below retains its stricter linear proof.
            for(unsigned i=0;i<abi.count;++i) if(abi.offsets[i]<0)
                for(const auto* argument:callback_pushes(function,pc,abi.callbacks[i])) {
                    const auto target=constant_before(function,argument->address,argument->operands[0]);
                    if(target && *target) {image.code_section(*target);function.static_callbacks.push_back(*target);}
                }
            std::vector<const Instruction*> arguments;
            const auto* cursor = previous(function,pc);
            unsigned steps = 0;
            while (cursor && arguments.size() < abi.argument_count && ++steps <= 128) {
                const auto& decoded = cursor->decoded;
                if (decoded.meta.category == ZYDIS_CATEGORY_CALL || decoded.meta.category == ZYDIS_CATEGORY_RET ||
                    decoded.meta.category == ZYDIS_CATEGORY_COND_BR || decoded.meta.category == ZYDIS_CATEGORY_UNCOND_BR) break;
                if (decoded.mnemonic == ZYDIS_MNEMONIC_PUSH && decoded.operand_width == 32) arguments.push_back(cursor);
                else {
                    bool stack_write = false;
                    for (unsigned i=0;i<decoded.operand_count;++i) {
                        const auto& operand = cursor->operands[i];
                        stack_write |= operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.reg.value == ZYDIS_REGISTER_ESP &&
                            (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE);
                        if(operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_ESP &&
                            (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
                            const auto start=static_cast<std::int64_t>(arguments.size()*4)+operand.mem.disp.value;
                            const auto end=start+(operand.size+7)/8;
                            stack_write |= operand.mem.index!=ZYDIS_REGISTER_NONE ||
                                (start<static_cast<std::int64_t>(abi.argument_count*4) && end>0);
                        }
                    }
                    if (stack_write) break;
                }
                cursor = previous(function,cursor->address);
            }
            if (arguments.size() != abi.argument_count) continue;
            for (unsigned i=0;i<abi.count;++i) {
                const auto* argument=arguments.at(abi.callbacks[i]);
                auto target=constant_before(function,argument->address,argument->operands[0]);
                if(abi.offsets[i]>=0) {
                    if(!target) {
                        const auto field=static_cast<std::uint32_t>(abi.offsets[i]);
                        target=stack_callback_before(function,pc,*argument,field);
                        if(!target) for(const auto candidate:record_callback_values(image,function,*argument,field))
                            if(candidate) {image.code_section(candidate);function.static_callbacks.push_back(candidate);}
                    }
                    else if(*target) {
                        const auto slot=*target+static_cast<std::uint32_t>(abi.offsets[i]);
                        if(slot<*target || slot<image.base || slot-image.base>=image.image_size)
                            throw std::runtime_error("Callback record leaves the supported image");
                        function.indirect_slots.push_back(slot);
                        // A zero-filled record can be populated by another original
                        // function. The batch joins this use with proven stores.
                        try {target=u32(image.data(slot,4),0);} catch(const std::exception&) {continue;}
                    }
                }
                if(!target || !*target) continue;
                image.code_section(*target);
                function.static_callbacks.push_back(*target);
            }
        }
    }
    for(const auto& [pc,instruction]:function.instructions) {
        const auto& operand=instruction.operands[0];
        const auto category=instruction.decoded.meta.category;
        if((category==ZYDIS_CATEGORY_CALL || category==ZYDIS_CATEGORY_UNCOND_BR) && !instruction.kernel_ordinal &&
            operand.type==ZYDIS_OPERAND_TYPE_REGISTER) {
            // Follow a dominating absolute load through a null check/register
            // copy. Incoming branches that could bypass that load invalidate it.
            const auto source=constant_before(function,pc,operand,true);
            if(source && *source>=image.base && *source-image.base<image.image_size) function.indirect_slots.push_back(*source);
        }
        if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY || operand.size!=32 || operand.mem.base!=ZYDIS_REGISTER_NONE ||
            operand.mem.index!=ZYDIS_REGISTER_NONE || operand.mem.segment!=ZYDIS_REGISTER_DS) continue;
        const auto slot=static_cast<std::uint32_t>(operand.mem.disp.value);
        if(slot<image.base || slot-image.base>=image.image_size) continue;
        if((category==ZYDIS_CATEGORY_CALL || category==ZYDIS_CATEGORY_UNCOND_BR) && !instruction.kernel_ordinal)
            function.indirect_slots.push_back(slot);
        if(instruction.decoded.mnemonic!=ZYDIS_MNEMONIC_MOV) continue;
        const auto value=constant_before(function,pc,instruction.operands[1]);
        if(!value || !*value) continue;
        try {image.code_section(*value);function.callback_stores.emplace_back(slot,*value);} catch(const std::exception&) {}
    }
    factory_callbacks(image,function);
    std::ranges::sort(function.static_callbacks);
    function.static_callbacks.erase(std::unique(function.static_callbacks.begin(),function.static_callbacks.end()),function.static_callbacks.end());
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
