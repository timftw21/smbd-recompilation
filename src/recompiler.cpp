#include "recompiler.h"
#include "native_apis.h"
#include "native_guest_roots.h"

#include <algorithm>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>

namespace smbd {
namespace {
constexpr const char* registers[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
struct RegisterPart { unsigned index, bits, shift; };
RegisterPart reg(ZydisRegister value) {
    constexpr ZydisRegister full[] = {ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX, ZYDIS_REGISTER_EBX,
                                      ZYDIS_REGISTER_ESP, ZYDIS_REGISTER_EBP, ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI};
    constexpr ZydisRegister word[] = {ZYDIS_REGISTER_AX, ZYDIS_REGISTER_CX, ZYDIS_REGISTER_DX, ZYDIS_REGISTER_BX,
                                      ZYDIS_REGISTER_SP, ZYDIS_REGISTER_BP, ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI};
    constexpr ZydisRegister low[] = {ZYDIS_REGISTER_AL, ZYDIS_REGISTER_CL, ZYDIS_REGISTER_DL, ZYDIS_REGISTER_BL};
    constexpr ZydisRegister high[] = {ZYDIS_REGISTER_AH, ZYDIS_REGISTER_CH, ZYDIS_REGISTER_DH, ZYDIS_REGISTER_BH};
    for (unsigned i = 0; i < 8; ++i) {
        if (value == full[i]) return {i, 32, 0};
        if (value == word[i]) return {i, 16, 0};
        if (i < 4 && value == low[i]) return {i, 8, 0};
        if (i < 4 && value == high[i]) return {i, 8, 8};
    }
    throw std::runtime_error("Unsupported CPU register");
}
std::string constant(std::uint32_t value) { return std::format("0x{:08X}U", value); }
std::string label(std::uint32_t address) { return std::format("i_{:08X}", address); }
std::string type(unsigned bits) {
    if (bits != 8 && bits != 16 && bits != 32) throw std::runtime_error("Unsupported operand width");
    return std::format("std::uint{}_t", bits);
}

struct CheckedRange {
    ZydisDecodedOperand origin;
    unsigned bytes;
    bool shares_address(const ZydisDecodedOperand& operand) const {
        return origin.mem.base==operand.mem.base && origin.mem.index==operand.mem.index &&
            origin.mem.scale==operand.mem.scale;
    }
};

class Emitter {
public:
    std::set<unsigned> used, dirty;
    bool uses_flags = false, writes_flags = false;
    bool calls = false;
    bool restores = false;
    bool returns = false;
    bool floating = false;
    bool force_scratch = false;
    bool tennis_replay_fade = false;
    std::set<std::uint32_t> loop_headers;
    std::map<std::string,std::string> native_helpers;
    std::map<std::string,std::string> native_declarations;
    std::vector<CheckedRange> checked_ranges;
    std::array<bool,8> checked_xmm_inputs{},checked_xmm_writes{};
    std::ostringstream body;
    void legacy_scope() {
        body << "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
                "    smbd::frame_step.delta=1;\n";
    }

    std::string read_reg(ZydisRegister value) {
        const auto part = reg(value);
        used.insert(part.index);
        std::string result = registers[part.index];
        if (part.shift) result = "(" + result + " >> 8)";
        if (part.bits < 32) result = "(" + result + " & " + constant((1U << part.bits) - 1) + ')';
        return result;
    }
    std::string address(const ZydisDecodedOperand& operand, bool lea = false) {
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) throw std::runtime_error("Expected a memory operand");
        if (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM && !(lea && operand.mem.type == ZYDIS_MEMOP_TYPE_AGEN))
            throw std::runtime_error("Special memory operand is unsupported");
        std::string value = constant(operand.mem.disp.has_displacement ? static_cast<std::uint32_t>(operand.mem.disp.value) : 0);
        if (operand.mem.base != ZYDIS_REGISTER_NONE) value += " + " + read_reg(operand.mem.base);
        if (operand.mem.index != ZYDIS_REGISTER_NONE)
            value += " + " + read_reg(operand.mem.index) + " * " + std::to_string(operand.mem.scale) + 'U';
        if (!lea && operand.mem.segment == ZYDIS_REGISTER_FS) value += " + cpu.fs_base";
        if (!lea && operand.mem.segment == ZYDIS_REGISTER_GS) value += " + cpu.gs_base";
        return "std::uint32_t(" + value + ')';
    }
    std::string read(const ZydisDecodedOperand& operand) {
        switch (operand.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: return read_reg(operand.reg.value);
        case ZYDIS_OPERAND_TYPE_IMMEDIATE: return constant(static_cast<std::uint32_t>(operand.imm.value.u));
        case ZYDIS_OPERAND_TYPE_MEMORY: return "memory.load<" + type(operand.size) + ">(" + address(operand) + ')';
        default: throw std::runtime_error("Unsupported operand type");
        }
    }
    void write(const ZydisDecodedOperand& operand, const std::string& value) {
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            body << "    memory.store<" << type(operand.size) << ">(" << address(operand)
                 << ", static_cast<" << type(operand.size) << ">(" << value << "));\n";
        } else if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto part = reg(operand.reg.value);
            used.insert(part.index); dirty.insert(part.index);
            body << "    " << registers[part.index] << " = ";
            if (part.bits == 32) body << value;
            else {
                const auto field = ((1U << part.bits) - 1) << part.shift;
                body << '(' << registers[part.index] << " & " << constant(~field) << ") | ((("
                     << value << ") << " << part.shift << ") & " << constant(field) << ')';
            }
            body << ";\n";
        } else throw std::runtime_error("Unsupported destination operand");
    }
    std::string condition(ZydisMnemonic mnemonic) {
        std::string_view code = ZydisMnemonicGetString(mnemonic);
        if (code.starts_with("cmov")) code.remove_prefix(4);
        else if (code.starts_with("set")) code.remove_prefix(3);
        else if (code.starts_with('j')) code.remove_prefix(1);
        if (code == "ecxz") { used.insert(1); return "ecx == 0"; }
        uses_flags = true;
        if (code == "z") return "(flags & smbd::zero) != 0";
        if (code == "nz") return "(flags & smbd::zero) == 0";
        if (code == "b") return "(flags & smbd::carry) != 0";
        if (code == "nb") return "(flags & smbd::carry) == 0";
        if (code == "be") return "(flags & (smbd::carry | smbd::zero)) != 0";
        if (code == "nbe") return "(flags & (smbd::carry | smbd::zero)) == 0";
        if (code == "l") return "((flags >> 7) ^ (flags >> 11)) & 1U";
        if (code == "nl") return "(((flags >> 7) ^ (flags >> 11)) & 1U) == 0";
        if (code == "le") return "(flags & smbd::zero) || (((flags >> 7) ^ (flags >> 11)) & 1U)";
        if (code == "nle") return "!(flags & smbd::zero) && !(((flags >> 7) ^ (flags >> 11)) & 1U)";
        for (const auto& [name, flag] : {std::pair{"o", "overflow"}, {"s", "sign"}, {"p", "parity"}}) {
            if (code == name) return std::string("(flags & smbd::") + flag + ") != 0";
            if (code.starts_with('n') && code.substr(1) == name) return std::string("(flags & smbd::") + flag + ") == 0";
        }
        throw std::runtime_error("Unsupported condition code");
    }
    void flags(std::string_view operation, unsigned bits) {
        type(bits);
        uses_flags = writes_flags = true;
        body << "    flags = smbd::" << operation << '<' << bits << ">(flags, a";
        if (operation != "logic_flags") body << ", b";
        body << ");\n";
    }
    unsigned xmm_register(ZydisRegister value) {
        if (value < ZYDIS_REGISTER_XMM0 || value > ZYDIS_REGISTER_XMM7)
            throw std::runtime_error("Expected an Xbox XMM register");
        return value-ZYDIS_REGISTER_XMM0;
    }
    std::string checked_pointer(const ZydisDecodedOperand& operand,unsigned width) {
        const auto range=std::ranges::find_if(checked_ranges,[&](const auto& candidate) {return candidate.shares_address(operand);});
        const auto offset=operand.mem.disp.has_displacement?operand.mem.disp.value:0;
        if(range==checked_ranges.end() || offset<0 || offset+width>range->bytes)
            throw std::runtime_error("SIMD access leaves its checked range");
        return std::format("checked_{}+{}U",range-checked_ranges.begin(),offset);
    }
    std::string vector(const ZydisDecodedOperand& operand, bool scalar = false) {
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto index=xmm_register(operand.reg.value);
            if(checked_ranges.empty()) return "smbd::xmm(cpu.floating,"+std::to_string(index)+')';
            if(!checked_xmm_writes[index]) checked_xmm_inputs[index]=true;
            return "checked_xmm_"+std::to_string(index);
        }
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) throw std::runtime_error("Expected SIMD register or memory");
        if(!checked_ranges.empty())
            return std::string(scalar?"smbd::load_scalar_vector(":"smbd::load_vector(")+checked_pointer(operand,scalar?4:16)+')';
        if (scalar) return "smbd::scalar_vector(memory.load<std::uint32_t>("+address(operand)+"))";
        return "smbd::load_vector(memory,"+address(operand)+')';
    }
    void vector_write(const ZydisDecodedOperand& operand, const std::string& value, bool scalar = false) {
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto index=xmm_register(operand.reg.value);
            if(checked_ranges.empty()) body << "    smbd::set_xmm(cpu.floating," << index << ',' << value << ");\n";
            else {
                checked_xmm_writes[index]=true;
                body << "    checked_xmm_" << index << " = " << value << ";\n";
            }
        } else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const auto target=checked_ranges.empty()?"memory,"+address(operand):checked_pointer(operand,scalar?4:16);
            body << "    smbd::store_vector(" << target << ',' << value << ',' << (scalar?"true":"false") << ");\n";
        }
        else throw std::runtime_error("Unsupported SIMD destination");
    }
    void mmx(const Instruction& instruction) {
        const auto& ins=instruction.decoded;
        if (ins.mnemonic == ZYDIS_MNEMONIC_PEXTRW || ins.mnemonic == ZYDIS_MNEMONIC_PMOVMSKB) {
            const auto& source = instruction.operands[1];
            if (source.type != ZYDIS_OPERAND_TYPE_REGISTER || source.reg.value < ZYDIS_REGISTER_MM0 ||
                source.reg.value > ZYDIS_REGISTER_MM7) throw std::runtime_error("Expected an MMX source register");
            const bool extract = ins.mnemonic == ZYDIS_MNEMONIC_PEXTRW;
            const auto immediate = extract ? instruction.operands[2].imm.value.u : 0;
            const auto modrm = 0xC0U | (source.reg.value - ZYDIS_REGISTER_MM0);
            const auto helper = std::format("smbd_mmx_result_{:02X}_{:02X}_{:02X}", ins.opcode, modrm, immediate);
            native_helpers[helper] = helper + " PROC\n    db 00Fh," +
                std::format("0{:02X}h,0{:02X}h", ins.opcode, modrm) +
                (extract ? std::format(",0{:02X}h", immediate) : "") + "\n    ret\n" + helper + " ENDP\n";
            native_declarations[helper] = "extern \"C\" SMBD_HELPER_ABI std::uint32_t " + helper + "();\n";
            floating = true;
            write(instruction.operands[0], helper + "()");
            return;
        }
        const ZydisDecodedOperand* memory_operand=nullptr;
        const ZydisDecodedOperand* immediate=nullptr;
        const ZydisDecodedOperand* register_operand=nullptr;
        std::string pointer;
        for(unsigned i=0;i<ins.operand_count_visible;++i) {
            const auto& operand=instruction.operands[i];
            if(operand.type==ZYDIS_OPERAND_TYPE_MEMORY) memory_operand=&operand;
            else if(operand.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.size==8) immediate=&operand;
            else if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER && ins.mnemonic==ZYDIS_MNEMONIC_MOVD &&
                operand.reg.value>=ZYDIS_REGISTER_EAX && operand.reg.value<=ZYDIS_REGISTER_EDI) register_operand=&operand;
            else if(i==1 && operand.type==ZYDIS_OPERAND_TYPE_REGISTER && ins.mnemonic==ZYDIS_MNEMONIC_CVTPS2PI)
                pointer="cpu.floating.xmm["+std::to_string(xmm_register(operand.reg.value))+"].data()";
            else if(operand.type!=ZYDIS_OPERAND_TYPE_REGISTER || operand.reg.value<ZYDIS_REGISTER_MM0 || operand.reg.value>ZYDIS_REGISTER_MM7)
                throw std::runtime_error("Expected an MMX register or memory operand");
        }
        const auto width=(ins.mnemonic==ZYDIS_MNEMONIC_MOVD || ins.mnemonic==ZYDIS_MNEMONIC_PUNPCKLBW ||
            ins.mnemonic==ZYDIS_MNEMONIC_PUNPCKLWD || ins.mnemonic==ZYDIS_MNEMONIC_PUNPCKLDQ)?4U:8U;
        if(memory_operand) {
            if(memory_operand->size!=width*8) throw std::runtime_error("Unexpected MMX memory width");
            pointer="memory.access("+address(*memory_operand)+','+std::to_string(width)+')';
        }
        const bool register_write=register_operand==&instruction.operands[0];
        if(register_operand) {
            body << "    std::uint32_t mmx_word=" << (register_write?"0U":read(*register_operand)) << ";\n";
            pointer="&mmx_word";
        }
        floating=true;
        // Guest XMM/GPR operands live in C++ state; present their original
        // bits as memory while MMX continues to share the resident x87 state.
        const unsigned modrm=!pointer.empty()?(ins.raw.modrm.reg<<3)|1:(3U<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm;
        const bool empty=ins.mnemonic==ZYDIS_MNEMONIC_EMMS;
        const auto helper=(empty?std::string("smbd_mmx_77"):std::format("smbd_mmx_{:02X}{:02X}",ins.opcode,modrm))+
            (immediate?std::format("_{:02X}",immediate->imm.value.u):"");
        native_helpers[helper]=helper+" PROC\n    db 00Fh,"+std::format("0{:02X}h",ins.opcode)+
            (empty?"":std::format(",0{:02X}h",modrm))+(immediate?std::format(",0{:02X}h",immediate->imm.value.u):"")+"\n    ret\n"+helper+" ENDP\n";
        native_declarations[helper]="extern \"C\" SMBD_HELPER_ABI void "+helper+'('+(!pointer.empty()?"void*":"")+");\n";
        body << "    " << helper << '(' << pointer << ");\n";
        if(register_write) write(*register_operand,"mmx_word");
    }
    void scalar_compare(const Instruction& instruction) {
        const auto& operands = instruction.operands;
        const auto left = "cpu.floating.xmm[" + std::to_string(xmm_register(operands[0].reg.value)) + "].data()";
        auto right = operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER ?
            "cpu.floating.xmm[" + std::to_string(xmm_register(operands[1].reg.value)) + "].data()" :
            "memory.access(" + address(operands[1]) + ",4)";
        if(instruction.address==0x88F58) {
            if(hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length))!="0F2F15C87B3400")
                throw std::runtime_error("Original character upright snap threshold differs");
            native_declarations["frame_rotation_snap"]="float frame_rotation_snap(FloatingState&,Memory&,float);\n";
            used.insert(6);
            body << "    const float frame_snap=frame_rotation_snap(cpu.floating,memory,smbd::frame_step.delta);\n";
            right="&frame_snap";
        }
        const std::string mnemonic = ZydisMnemonicGetString(instruction.decoded.mnemonic);
        const auto helper = "smbd_" + mnemonic;
        native_helpers[helper] = helper + " PROC\n    movss xmm0,dword ptr [rcx]\n    " +
            mnemonic + " xmm0,dword ptr [rdx]\n    pushfq\n    pop rax\n    ret\n" + helper + " ENDP\n";
        native_declarations[helper] = "extern \"C\" SMBD_HELPER_ABI std::uint32_t " + helper + "(const void*,const void*);\n";
        floating = uses_flags = writes_flags = true;
        body << "    const auto comparison=" << helper << '(' << left << ',' << right << ");\n"
             << "    flags=(flags&~smbd::arithmetic_flags)|(comparison&(smbd::carry|smbd::parity|smbd::zero));\n";
    }
    void vector_compare(const Instruction& instruction) {
        const auto& operands = instruction.operands;
        const bool scalar = instruction.decoded.mnemonic == ZYDIS_MNEMONIC_CMPSS;
        const auto index = xmm_register(operands[0].reg.value);
        const auto left = "cpu.floating.xmm[" + std::to_string(index) + "].data()";
        const auto right = operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER ?
            "cpu.floating.xmm[" + std::to_string(xmm_register(operands[1].reg.value)) + "].data()" :
            "memory.access(" + address(operands[1]) + (scalar ? ",4)" : ",16)");
        const auto predicate = operands[2].imm.value.u & 7U;
        const auto helper = std::format("smbd_{}_{}", scalar ? "cmpss" : "cmpps", predicate);
        native_helpers[helper] = helper + " PROC\n    movups xmm0,xmmword ptr [rcx]\n    " +
            (scalar ? "cmpss xmm0,dword ptr [rdx]," : "cmpps xmm0,xmmword ptr [rdx],") +
            std::to_string(predicate) + "\n    movups xmmword ptr [r8],xmm0\n    ret\n" + helper + " ENDP\n";
        native_declarations[helper] = "extern \"C\" SMBD_HELPER_ABI void " + helper + "(const void*,const void*,void*);\n";
        floating = true;
        body << "    " << helper << '(' << left << ',' << right << ',' << left << ");\n";
    }
    void x87(const Instruction& instruction) {
        const auto& ins=instruction.decoded;
        const std::string_view mnemonic=ZydisMnemonicGetString(ins.mnemonic);
        if (mnemonic.starts_with("fcmov")) throw std::runtime_error("Floating conditional move needs guest flags");
        floating=true;
        const bool waiting=mnemonic=="fwait";
        const bool compare_flags=mnemonic.starts_with("fcomi") || mnemonic.starts_with("fucomi");
        const bool status=mnemonic=="fnstsw" && ins.raw.modrm.mod==3;
        const ZydisDecodedOperand* memory=nullptr;
        for (unsigned i=0;i<ins.operand_count_visible;++i)
            if (instruction.operands[i].type==ZYDIS_OPERAND_TYPE_MEMORY) memory=&instruction.operands[i];
        const bool environment=mnemonic=="fnsave" || mnemonic=="frstor" || mnemonic=="fnstenv" || mnemonic=="fldenv";
        if (environment && ins.operand_width!=32) throw std::runtime_error("Only 32-bit x87 environments are supported");
        const unsigned modrm=memory ? (ins.raw.modrm.reg<<3)|1 : (3U<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm;
        const auto helper=waiting?std::string("smbd_x87_9B"):std::format("smbd_x87_{:02X}{:02X}",ins.opcode,modrm);
        std::string assembly=helper+" PROC\n    db "+std::format("0{:02X}h",ins.opcode);
        if (!waiting) assembly+=std::format(",0{:02X}h",modrm);
        assembly+='\n';
        if (compare_flags) assembly+="    pushfq\n    pop rax\n";
        if (status) assembly+="    movzx eax,ax\n";
        assembly+="    ret\n"+helper+" ENDP\n";
        native_helpers[helper]=assembly;
        native_declarations[helper]="extern \"C\" SMBD_HELPER_ABI "+std::string(status||compare_flags?"std::uint32_t ":"void ")+helper+'('+(memory?"void*":"")+");\n";
        const bool control=waiting || mnemonic=="fnstsw" || mnemonic=="fnstcw" || mnemonic=="fldcw" ||
            mnemonic=="fnsave" || mnemonic=="frstor" || mnemonic=="fnstenv" || mnemonic=="fldenv" ||
            mnemonic=="fnclex" || mnemonic=="fninit" || mnemonic=="fnop";
        if (memory) {
            if (!memory->size || memory->size%8) throw std::runtime_error("Unsupported x87 memory width");
            body << "    const auto fp_address=" << address(*memory) << ";\n"
                 << "    auto* fp_memory=memory.access(fp_address," << memory->size/8 << ");\n";
            constexpr std::uint32_t blend_snap_limits[]={0x9541D,0x954B7,0x956B4,0x9574D,0x9CB1C,0x9CBA3,0x9CD94,0x9CE1B};
            if(std::ranges::find(blend_snap_limits,instruction.address)!=std::end(blend_snap_limits)) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="D905847B3400")
                    throw std::runtime_error("Original character blend snap limit differs");
                // The snap threshold and the step must cover the same distance.
                body << "    float frame_blend_limit;\n"
                        "    if(smbd::frame_step.delta!=1) {\n"
                        "        frame_blend_limit=_mm_cvtss_f32(smbd::frame_increment(smbd::scalar_vector(memory.load<std::uint32_t>(fp_address))));\n"
                        "        fp_memory=&frame_blend_limit;\n"
                        "    }\n";
            }
        }
        if (!control) {
            body << "    cpu.floating.instruction=" << constant(instruction.address) << ";\n"
                 << "    cpu.floating.opcode=" << (((ins.opcode&7U)<<8)|((3U<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm)) << "U;\n"
                 << "    cpu.floating.code_selector=cpu.cs_selector;\n";
            if (memory) {
                body << "    cpu.floating.data=fp_address;\n    cpu.floating.data_selector=cpu."
                    << (memory->mem.segment==ZYDIS_REGISTER_SS?"ss":memory->mem.segment==ZYDIS_REGISTER_FS?"fs":memory->mem.segment==ZYDIS_REGISTER_GS?"gs":"ds") << "_selector;\n";
                // Record the original ModRM, not the RCX addressing used by the host helper.
                body << "    cpu.floating.opcode=" << (((ins.opcode&7U)<<8)|((ins.raw.modrm.mod<<6)|(ins.raw.modrm.reg<<3)|ins.raw.modrm.rm)) << "U;\n";
            }
        }
        if (status || compare_flags) body << "    const auto fp_value=";
        else body << "    ";
        body << helper << '(' << (memory?"fp_memory":"") << ");\n";
        if (status) write(instruction.operands[0],"fp_value");
        if (compare_flags) {
            uses_flags=writes_flags=true;
            body << "    flags=(flags&~smbd::arithmetic_flags)|(fp_value&(smbd::carry|smbd::parity|smbd::zero));\n";
        }
        if (mnemonic=="fnsave" || mnemonic=="fnstenv") body << "    cpu.floating.environment(fp_memory);\n";
        if (mnemonic=="frstor" || mnemonic=="fldenv") body << "    cpu.floating.restore_environment(fp_memory);\n";
        if (mnemonic=="fnsave" || mnemonic=="fninit")
            body << "    cpu.floating.instruction=cpu.floating.data=0;\n    cpu.floating.opcode=cpu.floating.code_selector=cpu.floating.data_selector=0;\n";
    }
    void instruction(const Instruction& instruction,std::string_view prologue={},bool ordered_sse=false) {
        const auto& ins = instruction.decoded;
        const auto& operands = instruction.operands;
        const auto next = instruction.address + ins.length;
        const auto repeats = ins.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
        const bool string_move = ins.meta.category == ZYDIS_CATEGORY_STRINGOP &&
            (ins.mnemonic == ZYDIS_MNEMONIC_MOVSB || ins.mnemonic == ZYDIS_MNEMONIC_MOVSW || ins.mnemonic == ZYDIS_MNEMONIC_MOVSD);
        const bool string_fill = ins.mnemonic == ZYDIS_MNEMONIC_STOSB || ins.mnemonic == ZYDIS_MNEMONIC_STOSW || ins.mnemonic == ZYDIS_MNEMONIC_STOSD;
        const bool string_compare=ins.meta.category==ZYDIS_CATEGORY_STRINGOP &&
            (ins.mnemonic==ZYDIS_MNEMONIC_CMPSB || ins.mnemonic==ZYDIS_MNEMONIC_CMPSW || ins.mnemonic==ZYDIS_MNEMONIC_CMPSD);
        const bool string_scan=ins.mnemonic==ZYDIS_MNEMONIC_SCASB || ins.mnemonic==ZYDIS_MNEMONIC_SCASW || ins.mnemonic==ZYDIS_MNEMONIC_SCASD;
        const bool locked=ins.attributes&ZYDIS_ATTRIB_HAS_LOCK;
        if (ins.address_width != 32 || (locked && ins.mnemonic!=ZYDIS_MNEMONIC_XCHG && ins.mnemonic!=ZYDIS_MNEMONIC_XADD && ins.mnemonic!=ZYDIS_MNEMONIC_CMPXCHG) ||
            (repeats && !string_compare && !string_scan && !string_move && !string_fill) ||
            ((string_compare || string_scan || string_move || string_fill) &&
             (ins.attributes & (ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS))))
            throw std::runtime_error("Instruction prefixes need an implementation");
        if(checked_ranges.empty()) body << label(instruction.address) << ": ";
        body << "{ // " << instruction.text << '\n' << prologue;
        if(loop_headers.contains(instruction.address))
            body << "    if(smbd::checkpoint_due(cpu)) { commit(); smbd::checkpoint(cpu," << constant(instruction.address) << "); }\n";
        if(instruction.address==0xB785F) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F605142189000A")
                throw std::runtime_error("Original dynamic FOV entry differs");
            native_declarations["frame_camera_fov_begin"]="void frame_camera_fov_begin(Memory&,std::uint32_t);\n";
            body << "    frame_camera_fov_begin(memory," << read_reg(ZYDIS_REGISTER_ESI) << "-0xC4U);\n";
        }
        if(instruction.address==0x65C46 || instruction.address==0x66B25) {
            const bool portal_view=instruction.address==0x65C46;
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=(portal_view?"0F87A0000000":"764D"))
                throw std::runtime_error("Original portal projected-size cutoff differs");
            // Keep the portal view and its blue surface at a distance. The
            // frustum checks and the separate close-up fade still apply.
            body << "    if(cpu.unlimited_render_distance) goto " << label(portal_view?next:0x66B74) << ";\n";
        }
        if(instruction.address==0x668BA) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="A120D56D00")
                throw std::runtime_error("Original teleport overlay history copy differs");
            // The overlay blends the previous image before copying the result
            // back into its history. Keep that feedback on the rule clock.
            // A new effect still needs its first image before it can draw.
            body << "    if(!smbd::frame_step.logic && memory.load<std::uint32_t>(" << read_reg(ZYDIS_REGISTER_EBP)
                << "+0x108U)) goto " << label(0x6696F) << ";\n";
        }
        if(instruction.address==0x1A4C54) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="66891560937E00")
                throw std::runtime_error("Original bowling HUD rotation differs");
            used.insert(1);used.insert(2);dirty.insert(2);
            body << "    if(smbd::frame_step.delta!=1) {\n"
                    "        const auto before=static_cast<std::int32_t>(static_cast<std::int16_t>(ecx));\n"
                    "        edx=before+smbd::frame_integer_increment(static_cast<std::int32_t>(edx)-before);\n    }\n";
        }
        if(instruction.address==0xAE13D || instruction.address==0xAE150 ||
           instruction.address==0xAE208 || instruction.address==0xAE251) {
            const auto expected=instruction.address==0xAE13D?"0F8431010000":
                instruction.address==0xAE150?"F30F101520BD3600":instruction.address==0xAE208?"55":"8B86F8000000";
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=expected)
                throw std::runtime_error("Original camera shake boundary differs");
            if(instruction.address==0xAE13D) body << "    if(!frame_shake_step) goto " << label(next) << ";\n";
            else body << "    if(!" << (instruction.address==0xAE208?"frame_shake_draw":"frame_shake_step")
                << ") goto " << label(instruction.address==0xAE150?0xAE208:0xAE274) << ";\n";
        }
        if(instruction.address==0x313B0) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="A174EF6D00")
                throw std::runtime_error("Original immediate primitive completion differs");
            // Complete geometry once for every immediate frontend producer:
            // sprites, fonts, outlined text and overlays, including tail calls.
            native_declarations["screen_primitives"]="void complete_screen_primitives(Memory&,bool);\n";
            body << "    complete_screen_primitives(memory,cpu.ultrawide);\n";
        }
        // Keep the minimap's local 16:9 camera and map its entire viewport to
        // the wider display, anchored at the right edge. Its queued sprites
        // must use the same mapping as its 3D image and stencil frame.
        const bool map_view=instruction.address==0x58C3A || instruction.address==0x58F1C || instruction.address==0x59636;
        const bool map_clip=instruction.address==0x58C52 || instruction.address==0x58F2D || instruction.address==0x5964C;
        const bool map_sprite=instruction.address==0x59350 || instruction.address==0x594FA;
        const bool hud_scale=instruction.address==0x74EC6 || instruction.address==0x3D707 || instruction.address==0x3DBA7 ||
            instruction.address==0x3DE9D || instruction.address==0x3E286;
        if(map_view || map_clip || map_sprite || hud_scale) {
            const auto target=map_view?0x31790U:map_clip?0x23940U:map_sprite?0x75400U:0x28E40U;
            if(ins.mnemonic!=ZYDIS_MNEMONIC_CALL || ins.length!=5 || instruction.target()!=target)
                throw std::runtime_error("Original HUD layout call differs");
            used.insert(4);
            body << "    if(cpu.ultrawide) { const auto status=_mm_getcsr();\n";
            if(hud_scale) {
                body << "    float scale=4.0f/7;\n";
                if(instruction.address==0x74EC6) body <<
                    // 74E70 saves the sprite caller's ESI below its XYZ scale
                    // arguments. Minimap arrows already include the original
                    // 16:9 artwork scale; apply only the extra viewport scale,
                    // after their rotation, through the native anchor matrix.
                    "    if(memory.load<std::uint32_t>(esp+16)==0x7550FU) {\n"
                    "        const auto sprite=memory.load<std::uint32_t>(esp+12);\n"
                    "        if(memory.load<std::uint32_t>(sprite)==0x500U && memory.load<std::uint32_t>(sprite+0x34)==0x102000AU) scale=16.0f/21;\n"
                    "    }\n";
                body << "    memory.store<std::uint32_t>(esp,std::bit_cast<std::uint32_t>(scale));\n";
            }
            else if(map_sprite) body <<
                "    const auto sprite=memory.load<std::uint32_t>(esp);\n"
                "    const auto x=640.0f+(std::bit_cast<float>(memory.load<std::uint32_t>(sprite+4))-640.0f)*(16.0f/21);\n"
                "    memory.store<std::uint32_t>(sprite+4,std::bit_cast<std::uint32_t>(x));\n"
                "    memory.store<std::uint32_t>(sprite+0x34,memory.load<std::uint32_t>(sprite+0x34)|0x1000000U);\n"
                "    memory.store<std::uint16_t>(sprite+0x40,static_cast<std::uint16_t>(static_cast<std::int32_t>(x)));\n";
            else if(map_view) body <<
                "    const float width=memory.load<std::uint16_t>(memory.load<std::uint32_t>(0x6DD520U)+4);\n"
                "    memory.store<std::uint32_t>(esp,std::bit_cast<std::uint32_t>(std::round(width+\n"
                "        (std::bit_cast<float>(memory.load<std::uint32_t>(esp))-width)*(16.0f/21))));\n"
                "    memory.store<std::uint32_t>(esp+8,std::bit_cast<std::uint32_t>(\n"
                "        std::round(std::bit_cast<float>(memory.load<std::uint32_t>(esp+8))*(16.0f/21))));\n";
            else body <<
                "    const float width=memory.load<std::uint16_t>(memory.load<std::uint32_t>(0x6DD520U)+4);\n"
                "    const auto left=memory.load<std::uint32_t>(esp),size=memory.load<std::uint32_t>(esp+8);\n"
                "    const auto x=static_cast<std::uint32_t>(width+(float(left)-width)*(16.0f/21));\n"
                "    const auto right=static_cast<std::uint32_t>(std::ceil(width+(float(left)+size-width)*(16.0f/21)));\n"
                "    memory.store<std::uint32_t>(esp,x);memory.store<std::uint32_t>(esp+8,right-x);\n";
            body << "    _mm_setcsr(status); }\n";
        }
        if(instruction.address==0x594FA) {
            used.insert(4);
            // The deferred icon's widescreen matrix scales about a signed
            // integer pivot. Keep its center at the projected floating X.
            body << "    if(memory.load<std::uint8_t>(0x6C934CU) &&\n"
                    "       (!memory.load<std::uint8_t>(0x891E8CU) || memory.load<std::uint32_t>(0x71B30CU))) {\n"
                    "        const auto sprite=memory.load<std::uint32_t>(esp);\n"
                    "        const auto x=std::bit_cast<float>(memory.load<std::uint32_t>(sprite+4));\n"
                    "        const auto pivot=static_cast<std::int16_t>(memory.load<std::uint16_t>(sprite+0x40));\n"
                    "        const auto center=pivot+(x-pivot)*(cpu.ultrawide?7.0f/4:4.0f/3);\n"
                    "        memory.store<std::uint32_t>(sprite+4,std::bit_cast<std::uint32_t>(center));\n"
                    "    }\n";
        }
        if(instruction.address==0x7550A && (ins.mnemonic!=ZYDIS_MNEMONIC_CALL || ins.length!=5 || instruction.target()!=0x74E70U))
            throw std::runtime_error("Original sprite anchor ABI differs");
        if(instruction.address==0x14F00 || instruction.address==0x1E1287 || instruction.address==0x233DE8) {
            const auto signature=instruction.address==0x14F00?"6A04":instruction.address==0x1E1287?"E8A4FDE4FF":"E843D2DFFF";
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original frame entry signature differs");
            calls=restores=true;native_declarations["frame_begin"]="void native_frame_begin(Cpu&,Memory&,std::uint32_t);\n";
            body << "    commit(); native_frame_begin(cpu,memory," << constant(instruction.address) << "); restore();\n";
        }
        constexpr std::pair<std::uint32_t,std::uint32_t> rule_calls[]={{0x14F30,0xEBD40},{0x14F4D,0x23F590},{0x14F52,0x23ECC0},
            {0x14F6A,0x23FB50},{0x14F82,0x16E30},{0x14F9A,0x15A90},{0x14FB2,0x1ADA0},{0x150B5,0x23EEC0},
            {0x79652,0x775E0},
            {0x1E129F,0xEBD40},{0x1E12A4,0x23F590},{0x1E12A9,0x23ECC0},{0x1E12AE,0x1EF4D0},
            {0x1E12B3,0x1EB370},{0x1E12B8,0x16E30},{0x1E1355,0x1EDD80},{0x1E1385,0x23EEC0},{0x1E139A,0xFB4D0},
            {0x233DFE,0xEBD40},{0x233E03,0x23F590},{0x233E08,0x23ECC0},{0x233E1C,0x232BF0},{0x233E35,0x23EEC0}};
        for(const auto& [site,target]:rule_calls) if(instruction.address==site) {
            if(ins.mnemonic!=ZYDIS_MNEMONIC_CALL || ins.length!=5 || instruction.target()!=target)
                throw std::runtime_error("Original rule-tick call signature differs");
            if(site==0x14F9A) {
                calls=restores=true;native_declarations["menu_layout"]="void native_menu_layout(Cpu&,Memory&,std::uint32_t,bool);\n";
                body << "    if(!smbd::frame_step.logic) { commit(); native_menu_layout(cpu,memory,0,false); restore(); goto " << label(next) << "; }\n";
                body << "    struct PresentationScope { bool previous; ~PresentationScope() { smbd::frame_step.capture_presentation=previous; } } presentation_scope{smbd::frame_step.capture_presentation};\n"
                        "    smbd::frame_step.capture_presentation=true;\n";
            } else {
                // Tennis's rule section also owns pause transitions and counters
                // between calls. Resume at the shared scene update on draw frames.
                body << "    if(!smbd::frame_step.logic) goto " << label(site==0x1E129F?0x1E135A:next) << ";\n";
            }
            legacy_scope();
        }
        for(const auto& layout:frame_presentations) if(layout.prologue_end) {
            if(instruction.address==layout.prologue_end) {
                const auto signature=layout.entry==0x2A13B0?"8BC2":layout.entry==0x2A5D50?"BE00000030":"C1E107";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original presentation prologue differs");
                body << "    if(!smbd::frame_step.logic) goto " << label(layout.draw) << ";\n";
            }
            if(instruction.address==layout.draw) {
                const auto signature=layout.entry==0x2A13B0?"A18C268B00":"6840010000";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original presentation boundary differs");
                calls=restores=true;native_declarations["menu_layout"]="void native_menu_layout(Cpu&,Memory&,std::uint32_t,bool);\n";
                body << "    if(smbd::frame_step.capture_presentation || !smbd::frame_step.logic) { commit(); native_menu_layout(cpu,memory," << constant(layout.entry) << ",smbd::frame_step.logic); restore(); }\n"
                        "    smbd::frame_step.delta=smbd::frame_step.interval;\n";
            }
        }
        if(instruction.address==0x794E1) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F6C601")
                throw std::runtime_error("Original window input signature differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(0x7954A) << ";\n";
        }
        if(instruction.address==0x8FF4C) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="E81FFDFFFF") throw std::runtime_error("Original main-game force call differs");
            force_scratch=true;used.insert(6);
            body << "    for(unsigned axis=0;axis<3;++axis) frame_force[axis]=memory.load<std::uint32_t>(esi+0x1CU+axis*4);\n";
        }
        if(instruction.address==0x8FF51) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F1007") throw std::runtime_error("Original force/position boundary differs");
            force_scratch=true;used.insert(6);
            native_declarations["frame_ball_step"]="void frame_ball_step(FloatingState&,Memory&,std::uint32_t,std::array<std::uint32_t,3>&);\n";
            body << "    smbd::frame_ball_step(cpu.floating,memory,esi,frame_force);\n";
        }
        constexpr std::pair<std::uint32_t,std::string_view> main_positions[]={{0x8FF55,"F30F584604"},{0x8FF64,"F30F584608"},{0x8FF73,"F30F58460C"}};
        for(unsigned axis=0;axis<3;++axis) if(instruction.address==main_positions[axis].first) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=main_positions[axis].second) throw std::runtime_error("Original main-game position integration differs");
            force_scratch=floating=true;
            const auto left=vector(operands[0]),right=vector(operands[1],true);
            body << "    if(smbd::frame_step.delta!=1) {\n"
                    "    smbd::set_xmm(cpu.floating,0,_mm_move_ss(" << left << ",_mm_add_ss(smbd::scalar_vector(frame_force[" << axis << "])," << right << ")));\n"
                    "    } else {\n";
            vector_write(operands[0],"_mm_add_ss("+left+','+right+')');body << "    }\n}\n";return;
        }
        // These counters measure game time; all following flag consumers are
        // independent TESTs or loop arithmetic, not the omitted INC/DEC flags.
        if(instruction.address==0x586B4) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="48") throw std::runtime_error("Original minimap state dispatch differs");
            // Opening, closing and integer border rotation retain their
            // rule steps. The center below follows the ball on each scene.
            body << "    if(!smbd::frame_step.logic) goto " << label(0x58738) << ";\n";
        }
        constexpr std::pair<std::uint32_t,std::string_view> tick_counters[]={{0x58767,"6601153E374B00"},{0x1E1394,"FF05D0926C00"},{0x15110,"41"},{0x1511B,"FF05D0926C00"},
            {0x81243,"FF0580FF7200"},{0x8D8DD,"40"},{0x8D98D,"40"},{0x8D522,"40"},{0x8CCC7,"40"},{0x901BE,"40"},{0x8C71F,"48"},{0xBAEA4,"41"},{0xB3C32,"40"},{0x13791D,"48"},
            {0x7F569,"41"},{0x7F56C,"49"},{0x7F56F,"83C102"},{0x7F574,"83E902"},{0x76ECB,"49"},{0x2B8E16,"FF05BC288B00"},
            {0x2B77D9,"40"},{0x897A2,"41"},{0x897D8,"C7827C02000000000000"},{0xA80E2,"48"},{0x6BC95,"40"}};
        for(const auto& [site,bytes]:tick_counters) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original timed counter signature differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(next) << ";\n";
        }
        if(instruction.address==0x6BCB6) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="8B8398000000")
                throw std::runtime_error("Original text reveal boundary differs");
            // Once the delay has elapsed, the original consumes one token per
            // tick. Keep the layout and positioning work below this call live.
            body << "    if(!smbd::frame_step.logic) goto " << label(0x6BCCC) << ";\n";
        }
        constexpr std::tuple<std::uint32_t,std::string_view,std::uint32_t> text_pose_boundaries[]={
            {0x6BBC2,"F6430802",0x6BC58},{0x6BC60,"F30F11454C",0x6BC65},{0x6BC91,"7517",0x6BCCC}};
        for(const auto& [site,bytes,resume]:text_pose_boundaries) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes)
                throw std::runtime_error("Original text style boundary differs");
            body << "    if(!frame_text_style) goto " << label(resume) << ";\n";
        }
        if(instruction.address==0x6BCCC) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="53")
                throw std::runtime_error("Original text placement boundary differs");
            body << "    if(!frame_text_placement) goto " << label(0x6C304) << ";\n";
        }
        // Keep the cue rolls, state changes and both directions of their
        // countdowns on the same tick. The original stack cleanup and model
        // sampling remain reachable from intermediate scenes.
        constexpr std::tuple<std::uint32_t,std::string_view,std::uint32_t> character_cues[]={
            {0x9CBF0,"E817EA2600",0x9CE99},{0x9CE60,"E8A7E72600",0x9CE99},
            {0x9CE99,"8B86A0020000",0x9D018},{0x9D01A,"753B",0x9D08E},
            {0x954FE,"E809612700",0x95536},{0x95792,"E8755E2700",0x957C6},
            {0x957CC,"B901000000",0x95908},{0x9590A,"7E06",0x95912}};
        for(const auto& [site,bytes,resume]:character_cues) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes)
                throw std::runtime_error("Original character cue boundary differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(resume) << ";\n";
        }
        if(instruction.address==0x1DD6D || instruction.address==0x1DD86) {
            const bool closing=instruction.address==0x1DD6D;
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=(closing?"48":"40"))
                throw std::runtime_error("Original storage-dialog fade counter differs");
            used.insert(0);uses_flags=writes_flags=true;
            body << "    frame_dialog_direction=" << (closing?-1:1) << ";\n"
                    "    if(!smbd::frame_step.logic) { flags=(smbd::sub_flags<32>(flags,eax,0)&~smbd::carry)|(flags&smbd::carry);goto " << label(next) << "; }\n";
        }
        if(instruction.address==0x1DD9F) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="7E2D" || instruction.target()!=0x1DDCE)
                throw std::runtime_error("Original storage-dialog fade visibility differs");
            // The first fraction of an opening fade is already visible even
            // while its integer duration remains zero. Keep drawing per scene.
            body << "    if(frame_dialog_direction>0 && smbd::frame_step.phase>0) goto " << label(next) << ";\n";
        }
        if(instruction.address==0xB3C3B) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="48") throw std::runtime_error("Original camera duration counter differs");
            uses_flags=writes_flags=true;used.insert(0);
            body << "    if(!smbd::frame_step.logic) { flags=(smbd::sub_flags<32>(flags,eax,0)&~smbd::carry)|(flags&smbd::carry);goto " << label(next) << "; }\n";
        }
        if(instruction.address==0x8DCFD) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="40") throw std::runtime_error("Original ball reset timer differs");
            // The preceding physics and transforms run per scene. Age, reset
            // countdown and the one-shot respawn below retain rule cadence.
            body << "    if(!smbd::frame_step.logic) goto " << label(0x8DE4B) << ";\n";
        }
        if(instruction.address==0x8D910) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="7546" || instruction.target()!=0x8D958)
                throw std::runtime_error("Original goal celebration boundary differs");
            // The mode controller must observe the ape's completion flag before
            // the ball consumes it. Physics below this event still runs each scene.
            body << "    if(!smbd::frame_step.logic) goto " << label(0x8D958) << ";\n";
        }
        if(instruction.address==0x1B12CE) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="0F8508020000" || instruction.target()!=0x1B14DC)
                throw std::runtime_error("Original bowling trail update/draw boundary differs");
            // Append and age the 32 trail samples with the bowling rules. The
            // original ribbon drawing below this boundary still runs each scene.
            body << "    if(!smbd::frame_step.logic) goto " << label(0x1B14DC) << ";\n";
        }
        constexpr std::tuple<std::uint32_t,std::string_view,std::uint32_t> hud_rules[]={
            {0x4D0BE,"FF04B5F8B66E00",0x4D0C5},{0x4D0DA,"0F8E98010000",0x4D278},
            {0x4D2D8,"48",0x4D2D9},{0x4D31B,"48",0x4D31C},{0x4D35D,"40",0x4D35E},
            {0x4D3B7,"A020B76E00",0x4D3EA},{0x4D438,"0FBFCA",0x4D46A},{0x4D448,"0FBFC2",0x4D46A}};
        for(const auto& [site,bytes,resume]:hud_rules) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes)
                throw std::runtime_error("Original character HUD rule boundary differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(resume) << ";\n";
        }
        if(instruction.address==0x4D434) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="2BD0") throw std::runtime_error("Original fallout HUD turn differs");
            native_declarations["frame_hud_fall_turn"]="std::uint32_t frame_hud_fall_turn(FloatingState&,std::uint32_t,std::uint32_t);\n";
            used.insert(0);used.insert(4);dirty.insert(0);floating=true;
            body << "    eax=frame_hud_fall_turn(cpu.floating,memory.load<std::uint32_t>(memory.load<std::uint32_t>(esp+0x14U)),eax);\n";
        }
        constexpr std::tuple<std::uint32_t,std::string_view,std::uint32_t> blend_completion[]={{0x97E0C,"761D",0x97E2B},
            {0x9C5B5,"7622",0x9C5D9},{0x9AEE0,"7615",0x9AEF7}};
        for(const auto& [site,bytes,target]:blend_completion) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes || instruction.target()!=target)
                throw std::runtime_error("Original animation blend completion differs");
            // The blend, selector and completion query share this boundary.
            floating=true;
            body << "    if(!smbd::frame_step.logic && smbd::frame_step.delta!=1 && "
                    "_mm_cvtss_f32(smbd::xmm(cpu.floating,0))>0) goto " << label(next) << ";\n";
        }
        if(instruction.address==0x97E1C) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F5805BC7B3400")
                throw std::runtime_error("Original animation blend clock differs");
            body << "    if(!smbd::frame_step.logic && smbd::frame_step.delta!=1) goto " << label(next) << ";\n";
        }
        if(instruction.address==0x97D28 || instruction.address==0x9C59E || instruction.address==0x9AEC3) {
            const auto site=instruction.address;
            const auto signature=site==0x97D28?"0F85C9000000":site==0x9C59E?"81C900200000":"B801000000";
            const auto target=site==0x97D28?0x97DF7U:site==0x9C59E?0x9C5AAU:0x9AEC9U;
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature)
                throw std::runtime_error("Original clip rule boundary differs");
            body << "    if(!smbd::frame_step.logic && smbd::frame_step.delta!=1) goto " << label(target) << ";\n";
        }
        if(instruction.address==0x97D46) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F58CA") throw std::runtime_error("Original clip advance differs");
            used.insert(6);
            body << "    frame_clip_advanced=true;\n"
                    "    frame_clip_bounds=memory.load<std::uint32_t>(esi+2U);\n"
                    "    frame_clip_before=_mm_cvtss_f32(smbd::xmm(cpu.floating,1));\n"
                    "    frame_clip_increment=_mm_cvtss_f32(smbd::xmm(cpu.floating,2));\n";
        }
        if(instruction.address==0x99319 || instruction.address==0x99385) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="33DB") throw std::runtime_error("Original pose loop entry differs");
            native_declarations["frame_clip_sample"]="std::uint32_t frame_clip_sample(Memory&,std::uint32_t,bool);\n";
            used.insert(7);
            body << "    frame_pose_time=frame_clip_sample(memory,edi," << (instruction.address==0x99319?"true":"false") << ");\n";
        }
        if(instruction.address==0x98962) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="66895658") throw std::runtime_error("Original blend clip copy differs");
            native_declarations["frame_clip_copy"]="void frame_clip_copy(std::uint32_t);\n";
            used.insert(6);
            body << "    frame_clip_copy(esi);\n";
        }
        constexpr std::tuple<std::uint32_t,std::string_view,unsigned,std::uint32_t> clip_replacements[]={
            {0x97977,"F30F11404C",0,0x40},{0x986B1,"F30F11464C",6,0x40},{0x986D0,"F30F114664",6,0x58},
            {0x94784,"F30F11424C",2,0x40},{0x9ADED,"F30F11414C",1,0x40},{0x9C704,"89514C",1,0x40}};
        for(const auto& [site,bytes,model,offset]:clip_replacements) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original clip replacement differs");
            native_declarations["frame_clip_reset"]="void frame_clip_reset(std::uint32_t);\n";
            used.insert(model);
            body << "    frame_clip_reset(" << registers[model] << '+' << constant(offset) << ");\n";
        }
        if(instruction.address==0x89634) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="81A798000000FFBFFFFF") throw std::runtime_error("Original reaction completion event differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(0x8964A) << ";\n";
        }
        if(instruction.address==0x8BC90) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="FF8948010000") throw std::runtime_error("Original ball size timer differs");
            uses_flags=writes_flags=true;used.insert(1);
            body << "    if(!smbd::frame_step.logic) { const auto count=memory.load<std::uint32_t>(ecx+0x148U);flags=(smbd::sub_flags<32>(flags,count,0)&~smbd::carry)|(flags&smbd::carry);goto " << label(next) << "; }\n";
        }
        if(instruction.address==0x8BCA7) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="EB3A" || instruction.target()!=0x8BCE3)
                throw std::runtime_error("Original ball-size expiry boundary differs");
            // The original expiry tick holds the radius. If this scene crosses
            // that boundary, only its trailing fraction starts normal recovery.
            body << "    if(smbd::frame_step.phase>0) { frame_size_expired=true; goto " << label(0x8BC6F) << "; }\n";
        }
        if(instruction.address==0x8B6A1 || instruction.address==0x8B8DA) {
            const auto signature=instruction.address==0x8B6A1?"0F84EE000000":"833D54936C0054";
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original ball audio timing boundary differs");
            // Periodic vocalizations and looping-sound updates use rule time.
            // Impact flags at 8B86D are transient physics events; consume them
            // each scene so a collision between rule ticks cannot be lost.
            body << "    if(!smbd::frame_step.logic) goto " << label(instruction.address==0x8B6A1?0x8B86D:0x8B9C2) << ";\n";
        }
        if(instruction.address==0x1E05D8 || instruction.address==0x1E07DC) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="69C0B0010000") throw std::runtime_error("Original party HUD animation event differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(instruction.address==0x1E05D8?0x1E065B:0x1E085F) << ";\n";
        }
        if(instruction.address==0x811A8) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F2A0580FF7200") throw std::runtime_error("Original stage clock signature differs");
            calls=restores=floating=true;native_declarations["stage_time"]="void native_stage_time(Cpu&,Memory&);\n";
            body << "    commit(); native_stage_time(cpu,memory); restore();\n}\n";return;
        }
        if(instruction.address==0x7F5E2) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F6471201") throw std::runtime_error("Original stage group sampling boundary differs");
            calls=restores=floating=true;native_declarations["stage_group_time"]="void native_stage_group_time(Cpu&,Memory&);\n";
            body << "    commit(); native_stage_group_time(cpu,memory); restore();\n";
        }
        if(instruction.address==0x2584F9 || instruction.address==0x258F08) {
            const auto expected=instruction.address==0x2584F9?"33C0":"668B460E";
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=expected) throw std::runtime_error("Original item timer boundary differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(instruction.address==0x2584F9?0x258517:0x258F85) << ";\n";
        }
        if(instruction.address==0x750FA) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="A1B0936C00") throw std::runtime_error("Original sprite update boundary differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(0x75161) << ";\n";
        }
        if(instruction.address==0xEC366) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="8B86B4000000") throw std::runtime_error("Original stream cue clock differs");
            // This completion callback also counts original frames for menu cues.
            // Keep servicing the voice; only its cue clock follows rule ticks.
            used.insert(3);
            body << "    if(!smbd::frame_step.logic) { ebx=0; goto " << label(0xEC3DA) << "; }\n";
        }
        // Update-only callbacks keep their original rule cadence. Drawing and
        // transform construction following these calls still run every frame.
        constexpr std::pair<std::uint32_t,std::string_view> update_callbacks[]={{0x75148,"FFD0"},{0x8795B,"FF148DC4575500"},
            {0x25860A,"FF148DC0486900"},{0x811C6,"FFD0"}};
        for(const auto& [site,bytes]:update_callbacks) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original update callback signature differs at "+hex32(site));
            body << "    if(!smbd::frame_step.logic) goto " << label(next) << ";\n";legacy_scope();
        }
        if(instruction.address==0x88CFD || instruction.address==0x88D64) {
            const auto signature=instruction.address==0x88CFD?"F30F109EBC000000":"57";
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original ape rolling response differs");
            native_declarations["frame_quaternion_step"]="void frame_quaternion_step(FloatingState&,Memory&,std::uint32_t,float,bool);\n";
            used.insert(6);
            // Apply the original response to one tick's angular motion, then
            // integrate its result over this scene interval.
            body << "    smbd::frame_quaternion_step(cpu.floating,memory,esi+0xB0U,smbd::frame_ball_interval(memory.load<std::uint8_t>(esi+3))," << (instruction.address==0x88CFD?"true":"false") << ");\n";
        }
        if(instruction.address==0x8C5A4) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="E867CBF9FF") throw std::runtime_error("Original rolling velocity boundary differs");
            used.insert(7);used.insert(6);floating=true;
            // C0 is a displacement measured from successive ball transforms.
            // Consumers (including replay apes) require original per-tick units.
            body << "    const auto interval=smbd::frame_ball_interval(memory.load<std::uint8_t>(esi+3));\n"
                    "    if(interval!=1) for(unsigned axis=0;axis<3;++axis) {\n"
                    "        const auto address=edi+0x28U+4*axis;\n"
                    "        memory.store<std::uint32_t>(address,std::bit_cast<std::uint32_t>(_mm_cvtss_f32(_mm_mul_ss(smbd::scalar_vector(memory.load<std::uint32_t>(address)),\n"
                    "            _mm_set_ss(interval>0?1/interval:0)))));\n    }\n";
        }
        if(instruction.address==0x88FF1) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="50") throw std::runtime_error("Original ape recovery rotation differs");
            used.insert(0);dirty.insert(0);
            body << "    eax=static_cast<std::uint32_t>(smbd::frame_integer_increment(std::bit_cast<std::int32_t>(eax)));\n";
        }
        if(instruction.address==0x893C0) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="E8EB04FAFF") throw std::runtime_error("Original reaction spin differs");
            used.insert(1);dirty.insert(1);
            body << "    ecx=static_cast<std::uint32_t>(smbd::frame_integer_increment(std::bit_cast<std::int32_t>(ecx)));\n";
        }
        if(instruction.address==0xB75A2 || instruction.address==0xB75B1) {
            const auto expected=instruction.address==0xB75A2?"FF1598A05600":"FF149590A05600";
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=expected)
                throw std::runtime_error("Original camera dispatch signature differs");
            body << "    const auto camera_update=" << read(operands[0]) << ";\n"
                    "    const bool continuous_camera=camera_update==0xAFCA0U || camera_update==0xB3BA0U || camera_update==0xB00D0U || camera_update==0xAFF40U || camera_update==0xB03F0U || camera_update==0xB0580U || camera_update==0xB0800U || camera_update==0xB0A50U;\n"
                    "    if(!continuous_camera && !smbd::frame_step.logic) goto " << label(next) << ";\n"
                    "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
                    "    if(!continuous_camera) smbd::frame_step.delta=1;\n";
        }
        if(instruction.address==0x8C406) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="FF148570745500") throw std::runtime_error("Original ball-state dispatch differs");
            used.insert(0);
            body << "    const bool continuous_ball=smbd::frame_continuous_ball(eax);\n"
                    "    if(!continuous_ball && !smbd::frame_step.logic) goto " << label(next) << ";\n";
            body << "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
                    "    if(!continuous_ball) smbd::frame_step.delta=1;\n";
        }
        if(instruction.address==0x258532) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="FF1485A0476900") throw std::runtime_error("Original item dispatch differs");
            body << "    const bool continuous_item=(" << read(operands[0]) << ")==0x258F00U;\n"
                    "    if(!continuous_item && !smbd::frame_step.logic) goto " << label(next) << ";\n"
                    "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
                    "    if(!continuous_item) smbd::frame_step.delta=1;\n";
        }
        if(instruction.address==0xBAF14) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="FF2485E8565700") throw std::runtime_error("Original background update dispatch differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(0xBAF1B) << ";\n";legacy_scope();
        }
        // Original 8A2C0: transformed acceleration -> velocity -> position.
        // Moving-stage transforms add the change in frame displacement to
        // velocity. Convert that change back to the game's 1/60-second units.
        constexpr std::pair<std::uint32_t,std::string_view> surface_velocity[]={{0x7B3C9,"F30F584500"},{0x7B3E3,"F30F584620"},{0x7B3FD,"F30F584624"},
            {0x7B49C,"F30F58461C"},{0x7B4BA,"F30F584620"},{0x7B4D4,"F30F584624"},
            {0x7EB1B,"F30F5803"},{0x7EB33,"F30F584620"},{0x7EB4D,"F30F584624"}};
        for(const auto& [site,bytes]:surface_velocity) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original moving-surface velocity signature differs");
            floating=true;
            const auto left=vector(operands[0]),right=vector(operands[1],true);
            const auto correction="(smbd::frame_step.delta==1?"+left+":_mm_mul_ss("+left+",_mm_set_ss(smbd::frame_step.delta>0?1/smbd::frame_step.delta:0)))";
            vector_write(operands[0],"_mm_add_ss("+correction+','+right+')');body << "}\n";return;
        }
        // Restrict time scaling to the six proved integration operations.
        constexpr std::pair<std::uint32_t,std::string_view> integration[]={{0x8A4FE,"F30F58442410"},{0x8A50E,"F30F584C2414"},{0x8A514,"F30F58542418"},
            {0x8A51E,"F30F584604"},{0x8A532,"F30F584E08"},{0x8A538,"F30F58C2"}};
        for(const auto& [site,bytes]:integration) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes || ins.mnemonic!=ZYDIS_MNEMONIC_ADDSS)
                throw std::runtime_error("Original ball integration signature differs");
            floating=true;
            const auto left=vector(operands[0]),right=vector(operands[1],true);
            std::string value;
            if(site<=0x8A514) {
                if(site==0x8A4FE) {
                    force_scratch=true;used.insert(6);
                    body << "    for(unsigned axis=0;axis<3;++axis) frame_force[axis]=memory.load<std::uint32_t>(esi+0x1CU+axis*4);\n";
                }
                value="_mm_add_ss("+left+",smbd::frame_increment("+right+"))";
            }
            else {
                force_scratch=true;used.insert(6);
                if(site==0x8A51E) {
                    native_declarations["frame_ball_step"]="void frame_ball_step(FloatingState&,Memory&,std::uint32_t,std::array<std::uint32_t,3>&);\n";
                    body << "    if(smbd::frame_step.delta!=1) {\n"
                            "        for(unsigned axis=0;axis<3;++axis) memory.store<std::uint32_t>(esi+0x1CU+axis*4,cpu.floating.xmm[axis][0]);\n"
                            "        smbd::frame_ball_step(cpu.floating,memory,esi,frame_force);\n"
                            "        for(unsigned axis=0;axis<3;++axis) cpu.floating.xmm[axis][0]=memory.load<std::uint32_t>(esi+0x1CU+axis*4);\n    }\n";
                }
                const auto axis=site==0x8A51E?0:site==0x8A532?1:2;
                const auto position=site==0x8A538?left:right;
                value="(smbd::frame_step.delta==1?_mm_add_ss("+left+','+right+"):_mm_move_ss("+left+",_mm_add_ss(smbd::scalar_vector(frame_force["+std::to_string(axis)+"]),"+position+")))";
            }
            vector_write(operands[0],value);body << "}\n";return;
        }
        if(instruction.address==0x2A647) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="E834BD0000")
                throw std::runtime_error("Original frame wait call signature differs");
            calls=restores=true;
            native_declarations["frame_wait"]="void native_frame_wait(Cpu&,Memory&);\n";
            body << "    commit(); cpu.eip=0x0002A647U; native_frame_wait(cpu,memory); restore();\n";
        }
        if(instruction.address==0x2A65B) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="72EA" || instruction.target()!=0x2A647)
                throw std::runtime_error("Original frame wait branch signature differs");
            // The independent producer clock replaces this display-counter
            // spin. Keep the original counter read, arithmetic and frame end.
            body << "    goto " << label(next) << ";\n}\n";
            return;
        }
        if(instruction.address==0xE59A2) {
            if(ins.mnemonic!=ZYDIS_MNEMONIC_JMP || ins.length!=5 || instruction.target()!=0x31D960U)
                throw std::runtime_error("Original movie frame synchronization differs");
            // The shared frame deadline replaces this second synchronization
            // gate. Keep E5990's original tests/flags and the SDK event reset.
            body << "    memory.store<std::uint32_t>(memory.load<std::uint32_t>(0x32B6A8U)+0x1DC0U,0);\n"
                    "    goto i_000E59A7;\n}\n";
            return;
        }
        bool terminator = false;
        switch (ins.mnemonic) {
        case ZYDIS_MNEMONIC_WBINVD:
            // Guest storage is coherent host RAM. The host publishes pending
            // native graphics uploads; no privileged device instruction runs.
            calls = true;
            body << "    commit();\n    memory.publish_writes();\n"
                 << "    if(!cpu.kernel || !cpu.kernel->publish_memory) throw smbd::NativeBoundary("
                 << constant(instruction.address) << ",\"native memory publication service is unbound\");\n"
                 << "    { smbd::HostFloatingScope host(cpu.floating); cpu.kernel->publish_memory(cpu.kernel->context); }\n";
            break;
        case ZYDIS_MNEMONIC_SAHF:
            uses_flags=writes_flags=true;
            body << "    flags=(flags&~0xD5U)|((" << read_reg(ZYDIS_REGISTER_EAX) << ">>8)&0xD5U);\n";
            break;
        case ZYDIS_MNEMONIC_LAHF:
            uses_flags=true;
            write(ZydisDecodedOperand{.type=ZYDIS_OPERAND_TYPE_REGISTER,.reg={ZYDIS_REGISTER_AH}},"(flags&0xD5U)|2U");
            break;
        case ZYDIS_MNEMONIC_XCHG: {
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY || operands[1].type==ZYDIS_OPERAND_TYPE_MEMORY) {
                const auto& memory_operand=operands[operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY?0:1];
                const auto& register_operand=operands[operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY?1:0];
                body << "    const auto location=" << address(memory_operand) << ";\n"
                    << "    const auto value=memory.exchange<" << type(memory_operand.size) << ">(location,static_cast<"
                    << type(memory_operand.size) << ">(" << read(register_operand) << "));\n";
                write(register_operand,"value");
            } else {
                body << "    const auto a=" << read(operands[0]) << ";\n    const auto b=" << read(operands[1]) << ";\n";
                write(operands[0],"b"); write(operands[1],"a");
            }
            break;
        }
        case ZYDIS_MNEMONIC_XADD: {
            uses_flags=writes_flags=true;
            const auto bits=operands[0].size;
            body << "    const auto b=" << read(operands[1]) << ";\n";
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY) {
                body << "    const auto location=" << address(operands[0]) << ";\n    const auto a=memory."
                    << (locked?"fetch_add":"load") << '<' << type(bits) << ">(location";
                if(locked) body << ",static_cast<" << type(bits) << ">(b)";
                body << ");\n";
            } else body << "    const auto a=" << read(operands[0]) << ";\n";
            body << "    flags=smbd::add_flags<" << bits << ">(flags,a,b);\n";
            write(operands[1],"a");
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY) {
                if(!locked) body << "    memory.store<" << type(bits) << ">(location,static_cast<" << type(bits) << ">(a+b));\n";
            } else write(operands[0],"a+b");
            break;
        }
        case ZYDIS_MNEMONIC_CMPXCHG: {
            const auto bits=operands[0].size;
            type(bits); uses_flags=writes_flags=true;
            const auto accumulator=bits==8?ZYDIS_REGISTER_AL:bits==16?ZYDIS_REGISTER_AX:ZYDIS_REGISTER_EAX;
            body << "    const auto expected=" << read_reg(accumulator) << ";\n"
                << "    const auto desired=" << read(operands[1]) << ";\n";
            if(operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY && locked)
                body << "    const auto prior=memory.compare_exchange<" << type(bits) << ">(" << address(operands[0])
                    << ",static_cast<" << type(bits) << ">(expected),static_cast<" << type(bits) << ">(desired));\n";
            else body << "    const auto prior=" << read(operands[0]) << ";\n";
            body << "    flags=smbd::sub_flags<" << bits << ">(flags,expected,prior);\n    if(expected==prior) {\n";
            if(!locked) write(operands[0],"desired");
            body << "    } else {\n";
            write(ZydisDecodedOperand{.type=ZYDIS_OPERAND_TYPE_REGISTER,.reg={accumulator}},"prior");
            body << "    }\n";
            break;
        }
        case ZYDIS_MNEMONIC_BSF: {
            uses_flags=writes_flags=true;
            body << "    const std::uint32_t value=" << read(operands[1]) << ";\n"
                << "    flags=(flags&~smbd::zero)|(value?0U:smbd::zero);\n    if(value) {\n";
            write(operands[0],"static_cast<std::uint32_t>(std::countr_zero(value))");
            body << "    }\n";
            break;
        }
        case ZYDIS_MNEMONIC_CLI: case ZYDIS_MNEMONIC_STI:
            uses_flags=writes_flags=true;
            body << "    flags " << (ins.mnemonic==ZYDIS_MNEMONIC_CLI?"&= ~":"|= ") << "0x200U;\n";
            break;
        case ZYDIS_MNEMONIC_SGDT:
            body << "    const auto location=" << address(operands[0]) << ";\n"
                << "    memory.access(location,6);\n    memory.store<std::uint16_t>(location,cpu.gdt_limit);\n"
                << "    memory.store<std::uint32_t>(location+2,cpu.gdt_base);\n";
            break;
        case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHT0:
        case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2: {
            const auto hint = ins.mnemonic == ZYDIS_MNEMONIC_PREFETCHT0 ? "_MM_HINT_T0" :
                ins.mnemonic == ZYDIS_MNEMONIC_PREFETCHT1 ? "_MM_HINT_T1" :
                ins.mnemonic == ZYDIS_MNEMONIC_PREFETCHT2 ? "_MM_HINT_T2" : "_MM_HINT_NTA";
            body << "    memory.prefetch<" << hint << ">(" << address(operands[0]) << ");\n";
            break;
        }
        case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS:
            scalar_compare(instruction); break;
        case ZYDIS_MNEMONIC_CMPPS: case ZYDIS_MNEMONIC_CMPSS:
            vector_compare(instruction); break;
        case ZYDIS_MNEMONIC_POPAD: {
            if (ins.operand_width != 32) throw std::runtime_error("POPAD needs 32-bit operands");
            used.insert(4); dirty.insert(4);
            constexpr unsigned order[] = {7,6,5,4,3,2,1,0};
            for (unsigned i = 0; i < 8; ++i) if (order[i] != 4) {
                used.insert(order[i]); dirty.insert(order[i]);
                body << "    " << registers[order[i]] << "=memory.load<std::uint32_t>(esp+" << i * 4 << "U);\n";
            }
            body << "    esp+=32U;\n"; break;
        }
        case ZYDIS_MNEMONIC_XLAT:
            used.insert(0); used.insert(3); dirty.insert(0);
            body << "    eax=(eax&0xFFFFFF00U)|memory.load<std::uint8_t>(std::uint32_t(ebx+(eax&0xFFU)));\n";
            break;
        case ZYDIS_MNEMONIC_SFENCE:
            body << "    _mm_sfence();\n";
            break;
        case ZYDIS_MNEMONIC_STMXCSR:
            floating=true;
            write(operands[0],"_mm_getcsr()");
            break;
        case ZYDIS_MNEMONIC_LDMXCSR:
            floating=true;
            body << "    const auto control=" << read(operands[0]) << ";\n"
                << "    if(control&~0xFFFFU) throw std::runtime_error(\"Guest MXCSR has reserved bits set\");\n"
                << "    cpu.floating.mxcsr=control;\n"
                << "    _mm_setcsr(control);\n";
            break;
        case ZYDIS_MNEMONIC_MOVQ: case ZYDIS_MNEMONIC_MOVNTQ: case ZYDIS_MNEMONIC_EMMS:
        case ZYDIS_MNEMONIC_PSHUFW: case ZYDIS_MNEMONIC_PUNPCKLWD: case ZYDIS_MNEMONIC_PUNPCKHWD:
        case ZYDIS_MNEMONIC_PUNPCKLDQ: case ZYDIS_MNEMONIC_PUNPCKHDQ:
        case ZYDIS_MNEMONIC_MOVD:
        case ZYDIS_MNEMONIC_CVTPS2PI:
        case ZYDIS_MNEMONIC_PACKSSDW:
        case ZYDIS_MNEMONIC_PACKUSWB:
        case ZYDIS_MNEMONIC_PADDUSW:
        case ZYDIS_MNEMONIC_PADDW:
        case ZYDIS_MNEMONIC_PAND:
        case ZYDIS_MNEMONIC_PANDN:
        case ZYDIS_MNEMONIC_PAVGB:
        case ZYDIS_MNEMONIC_PAVGW:
        case ZYDIS_MNEMONIC_PCMPEQB:
        case ZYDIS_MNEMONIC_PCMPEQW:
        case ZYDIS_MNEMONIC_PEXTRW:
        case ZYDIS_MNEMONIC_PMOVMSKB:
        case ZYDIS_MNEMONIC_PMULLW:
        case ZYDIS_MNEMONIC_POR:
        case ZYDIS_MNEMONIC_PSLLQ:
        case ZYDIS_MNEMONIC_PSLLW:
        case ZYDIS_MNEMONIC_PSRAW:
        case ZYDIS_MNEMONIC_PSRLQ:
        case ZYDIS_MNEMONIC_PSRLW:
        case ZYDIS_MNEMONIC_PUNPCKHBW:
        case ZYDIS_MNEMONIC_PUNPCKLBW:
        case ZYDIS_MNEMONIC_PXOR:
            mmx(instruction); break;
        case ZYDIS_MNEMONIC_SHUFPS: case ZYDIS_MNEMONIC_UNPCKLPS: case ZYDIS_MNEMONIC_UNPCKHPS:
        case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_MOVLHPS: {
            floating=true;
            const auto name=ins.mnemonic==ZYDIS_MNEMONIC_SHUFPS?"shuffle_ps":
                ins.mnemonic==ZYDIS_MNEMONIC_UNPCKLPS?"unpacklo_ps":ins.mnemonic==ZYDIS_MNEMONIC_UNPCKHPS?"unpackhi_ps":
                ins.mnemonic==ZYDIS_MNEMONIC_MOVHLPS?"movehl_ps":"movelh_ps";
            auto value="_mm_"+std::string(name)+'('+vector(operands[0])+','+vector(operands[1]);
            if(ins.mnemonic==ZYDIS_MNEMONIC_SHUFPS) value+=','+read(operands[2]);
            vector_write(operands[0],value+')');
            break;
        }
        case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVHPS: {
            floating=true;
            const bool store=operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY;
            const auto& memory_operand=operands[store?0:1];
            const auto& register_operand=operands[store?1:0];
            const bool high=ins.mnemonic==ZYDIS_MNEMONIC_MOVHPS;
            if(!checked_ranges.empty()) {
                const auto value=vector(register_operand),mapped=checked_pointer(memory_operand,8);
                if(store) {
                    const auto source=high?"reinterpret_cast<const std::byte*>(&"+value+")+8":"&"+value;
                    body << "    std::memcpy(" << mapped << ',' << source << ",8);\n";
                }
                else {
                    const auto target=high?"reinterpret_cast<std::byte*>(&value)+8":"&value";
                    body << "    auto value = " << value << ";\n"
                         << "    std::memcpy(" << target << ',' << mapped << ",8);\n";
                    vector_write(register_operand,"value");
                }
                break;
            }
            const auto storage="cpu.floating.xmm["+std::to_string(xmm_register(register_operand.reg.value))+"].data()+"+
                (high?"2":"0");
            const auto mapped="memory.access("+address(memory_operand)+",8)";
            body << "    std::memcpy(" << (store?mapped:storage) << ',' << (store?storage:mapped) << ",8);\n";
            break;
        }
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVSS: {
            floating=true;
            const bool scalar=ins.mnemonic==ZYDIS_MNEMONIC_MOVSS;
            if (ins.mnemonic==ZYDIS_MNEMONIC_MOVAPS && checked_ranges.empty()) for(unsigned i=0;i<2;++i)
                if(operands[i].type==ZYDIS_OPERAND_TYPE_MEMORY)
                    body << "    if(" << address(operands[i]) << "&15U) throw std::runtime_error(\"Unaligned guest MOVAPS\");\n";
            auto value=vector(operands[1],scalar);
            if(instruction.address==0x98B62 || instruction.address==0x98B71) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=(instruction.address==0x98B62?"F30F104564":"F30F10454C"))
                    throw std::runtime_error("Original clip pose clock read differs");
                value="(frame_sampled_pose?smbd::scalar_vector(*frame_sampled_pose):"+value+")";
            }
            if(instruction.address==0x88ED0 || instruction.address==0x8C697) {
                const auto signature=instruction.address==0x88ED0?"F30F10053CAE3F00":"F30F100D90AE3F00";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original angular threshold differs");
                used.insert(6);
                native_declarations["frame_rotation_threshold"]="__m128 frame_rotation_threshold(FloatingState&,__m128,float);\n";
                value="smbd::frame_rotation_threshold(cpu.floating,"+value+",smbd::frame_ball_interval(memory.load<std::uint8_t>(esi+3)))";
            }
            constexpr std::pair<std::uint32_t,std::string_view> replay_velocity[]={{0x8CB9A,"F30F114E20"},{0x8CBBC,"F30F11461C"},{0x8CC40,"F30F115624"}};
            for(const auto& [site,bytes]:replay_velocity) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original replay velocity store differs");
                value="(smbd::frame_step.delta==1?"+value+":_mm_mul_ss("+value+",_mm_set_ss(smbd::frame_step.delta>0?1/smbd::frame_step.delta:0)))";
            }
            constexpr std::pair<std::uint32_t,std::string_view> response_gains[]={{0xAFF5C,"F30F103534903700"},{0xB040C,"F30F103534903700"},
                {0xB0660,"F30F103534903700"},{0xB06AF,"F30F101DB88D3700"},{0xB08C4,"F30F101DB88D3700"},
                {0xB08E5,"F30F102D34903700"},{0xB0B04,"F30F101D4C923700"},{0xB0B43,"F30F102D34903700"},
                {0x6BBB3,"F30F1015B87B3400"},{0x6BBF8,"F30F100D34903700"},{0x58865,"F30F1015B87B3400"}};
            for(const auto& [site,bytes]:response_gains) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original response gain differs");
                native_declarations["frame_gain"]="__m128 frame_gain(FloatingState&,__m128);\n";
                value="smbd::frame_gain(cpu.floating,"+value+')';
            }
            if(instruction.address==0x588D5 || instruction.address==0x588F4) {
                const auto bytes=instruction.address==0x588D5?"F30F1015108E3700":"F30F100DC48D3700";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original minimap fade step differs");
                value="smbd::frame_increment("+value+')';
            }
            if(instruction.address==0xAFF50 || instruction.address==0xB0400) {
                const auto bytes=instruction.address==0xAFF50?"F30F101D081A3A00":"F30F101D4C6F3900";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original replay camera damping differs");
                native_declarations["frame_decay"]="__m128 frame_decay(FloatingState&,__m128);\n";
                value="smbd::frame_decay(cpu.floating,"+value+')';
            }
            if(instruction.address==0x8B42D) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F114058")
                    throw std::runtime_error("Original contact friction signature differs");
                native_declarations["frame_gain"]="__m128 frame_gain(FloatingState&,__m128);\n";
                used.insert(4);
                value="((memory.load<std::uint32_t>(memory.load<std::uint32_t>(esp+4)+0x128)&1)?smbd::frame_gain(cpu.floating,"+value+"):"+value+')';
            }
            if (scalar && operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER && operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER)
                value="_mm_move_ss("+vector(operands[0])+','+value+')';
            vector_write(operands[0],value,scalar); break;
        }
        case ZYDIS_MNEMONIC_MOVNTPS:
            floating=true;
            body << "    smbd::stream_vector(memory," << address(operands[0]) << ',' << vector(operands[1]) << ");\n";
            break;
        case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI:
            floating=true;
            write(operands[0],std::string("static_cast<std::uint32_t>(")+
                (ins.mnemonic==ZYDIS_MNEMONIC_CVTTSS2SI?"_mm_cvtt_ss2si(":"_mm_cvt_ss2si(")+vector(operands[1],true)+"))");
            break;
        case ZYDIS_MNEMONIC_CVTSI2SS:
            floating=true;
            if(instruction.address==0x1DDA1) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F2AC0") throw std::runtime_error("Original storage-dialog alpha conversion differs");
                body << "    const float sample=float(std::bit_cast<std::int32_t>(" << read(operands[1]) << "))+frame_dialog_direction*smbd::frame_step.phase;\n";
                vector_write(operands[0],"_mm_move_ss("+vector(operands[0])+",_mm_set_ss(sample<0?0:sample>8?8:sample))");
            } else vector_write(operands[0],"_mm_cvtsi32_ss("+vector(operands[0])+",std::bit_cast<std::int32_t>("+read(operands[1])+"))");
            break;
        case ZYDIS_MNEMONIC_ADDSS: case ZYDIS_MNEMONIC_SUBSS: case ZYDIS_MNEMONIC_MULSS: case ZYDIS_MNEMONIC_DIVSS:
        case ZYDIS_MNEMONIC_MINSS: case ZYDIS_MNEMONIC_MAXSS: case ZYDIS_MNEMONIC_SQRTSS: case ZYDIS_MNEMONIC_RSQRTSS:
        case ZYDIS_MNEMONIC_RCPSS: case ZYDIS_MNEMONIC_ADDPS: case ZYDIS_MNEMONIC_SUBPS: case ZYDIS_MNEMONIC_MULPS:
        case ZYDIS_MNEMONIC_DIVPS: case ZYDIS_MNEMONIC_MINPS: case ZYDIS_MNEMONIC_MAXPS: case ZYDIS_MNEMONIC_SQRTPS:
        case ZYDIS_MNEMONIC_RSQRTPS: case ZYDIS_MNEMONIC_RCPPS: case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_ANDPS:
        case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ANDNPS: {
            floating=true;
            std::string name=ZydisMnemonicGetString(ins.mnemonic);
            const bool scalar=name.ends_with("ss");
            name.insert(name.size()-2,"_");
            const bool unary=name.starts_with("sqrt_") || name.starts_with("rsqrt_") || name.starts_with("rcp_");
            std::string value="_mm_"+name+'(';
            if (!unary) value+=vector(operands[0])+',';
            value+=vector(operands[1],scalar)+')';
            if(instruction.address==0x3484A || instruction.address==0x34853) {
                const auto expected=instruction.address==0x3484A?"F30F5ECA":"F30F5EE2";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=expected)
                    throw std::runtime_error("Original perspective depth calculation differs");
                // The far->infinity limits of near*far/(far-near) and
                // far/(far-near) are near and 1. Keep the original near
                // plane, FOV and side culling without infinity/NaN arithmetic.
                const auto depth=instruction.address==0x3484A?
                    "smbd::scalar_vector(memory.load<std::uint32_t>("+read_reg(ZYDIS_REGISTER_ESP)+"+0x14U))":
                    std::string("_mm_set_ss(1.0f)");
                value="(cpu.unlimited_render_distance ? _mm_move_ss("+vector(operands[0])+','+depth+") : "+value+')';
            }
            if(instruction.address==0x66080) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F5EC1")
                    throw std::runtime_error("Original teleport overlay placement differs");
                // The original quad sits halfway to the camera's far plane.
                // With infinite clipping, q+1 is zero. Use the camera's
                // original far distance for placement, keeping its projection.
                value="(cpu.unlimited_render_distance && _mm_cvtss_f32("+vector(operands[1],true)+")==0.0f ? _mm_move_ss("+
                    vector(operands[0])+",smbd::scalar_vector(memory.load<std::uint32_t>(memory.load<std::uint32_t>(0x73F19CU)+0x44U))) : "+value+')';
            }
            constexpr std::pair<std::uint32_t,std::string_view> time_adds[]={{0xBAEA7,"F30F58C1"},{0xBAECB,"F30F58D9"},{0xBAEF2,"F30F58D1"},
                {0xB7799,"F30F5805C0713900"},{0xB77B3,"F30F5C05C0713900"},{0xB77DD,"F30F5805C0713900"},{0xB7817,"F30F5C05C0713900"},
                {0x8CB22,"F30F5C05BC7B3400"},{0x7F67E,"F30F5C05BC7B3400"},
                {0x8FE43,"F30F58442414"},{0x8FE53,"F30F58442418"},{0x8FE63,"F30F5844241C"},
                {0x95436,"F30F5C0D847B3400"},{0x95440,"F30F580D847B3400"},{0x954D0,"F30F5C0D847B3400"},{0x954DA,"F30F580D847B3400"},
                {0x956CD,"F30F5C0D847B3400"},{0x956D7,"F30F580D847B3400"},{0x95766,"F30F5C0D847B3400"},{0x95770,"F30F580D847B3400"},
                {0x9CB35,"F30F5C0D847B3400"},{0x9CB3F,"F30F580D847B3400"},{0x9CBBC,"F30F5C0D847B3400"},{0x9CBC6,"F30F580D847B3400"},
                {0x9CDAD,"F30F5C0D847B3400"},{0x9CDB7,"F30F580D847B3400"},{0x9CE34,"F30F5C0D847B3400"},{0x9CE3E,"F30F580D847B3400"}};
            for(const auto& [site,bytes]:time_adds) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes || (ins.mnemonic!=ZYDIS_MNEMONIC_ADDSS && ins.mnemonic!=ZYDIS_MNEMONIC_SUBSS))
                    throw std::runtime_error("Original continuous increment signature differs");
                value=std::string(ins.mnemonic==ZYDIS_MNEMONIC_ADDSS?"_mm_add_ss(":"_mm_sub_ss(")+vector(operands[0])+",smbd::frame_increment("+vector(operands[1],true)+"))";
            }
            constexpr std::pair<std::uint32_t,std::string_view> replay_camera_motion[]={{0xAFFA7,"F30F58D8"},{0xAFFBB,"F30F58E1"},{0xAFFBF,"F30F58EA"},
                {0xB0453,"F30F58D8"},{0xB0467,"F30F58E1"},{0xB046B,"F30F58EA"}};
            for(const auto& [site,bytes]:replay_camera_motion) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original replay camera motion differs");
                native_declarations["frame_damped_increment"]="__m128 frame_damped_increment(FloatingState&,__m128,float);\n";
                const auto retention=site==0xAFFBB?0x402458U:site<0xB0000?0x3A1A08U:0x396F4CU;
                value="_mm_add_ss("+vector(operands[0])+",smbd::frame_damped_increment(cpu.floating,"+vector(operands[1],true)+",std::bit_cast<float>(memory.load<std::uint32_t>("+constant(retention)+"))))";
            }
            if(instruction.address==0xAFF81) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F590D58244000") throw std::runtime_error("Original fallout camera vertical damping differs");
                native_declarations["frame_decay"]="__m128 frame_decay(FloatingState&,__m128);\n";
                value="_mm_mul_ss("+vector(operands[0])+",smbd::frame_decay(cpu.floating,"+vector(operands[1],true)+"))";
            }
            if(instruction.address==0x27A8B7 || instruction.address==0x24980A) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F5CC1")
                    throw std::runtime_error("Original recorded stage fractional sample differs");
                used.insert(0);
                // Integer-only source frames hid this forward-only assumption.
                // Reconstruction supplies the recorded stop/direction/speed.
                body << "    constexpr float speeds[]={1,0,-1,2,-2,0,0,0};\n";
                value="_mm_sub_ss("+vector(operands[0])+",_mm_mul_ss("+vector(operands[1],true)+",_mm_set_ss(speeds[memory.load<std::uint32_t>(eax)&7U])))";
            }
            constexpr std::pair<std::uint32_t,std::string_view> item_motion[]={{0x258F8A,"F30F584620"},{0x258FD0,"F30F584624"},{0x258FE7,"F30F584628"}};
            for(const auto& [site,bytes]:item_motion) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original item motion differs");
                value="_mm_add_ss(smbd::frame_increment("+vector(operands[0])+"),"+vector(operands[1],true)+")";
            }
            if(instruction.address==0x97E13) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F5ED8")
                    throw std::runtime_error("Original animation blend weight differs");
                // The original samples before incrementing. Between ticks the
                // stored clock is already one tick ahead of that sample.
                const auto elapsed=vector(operands[0]),duration=vector(operands[1]);
                body << "    if(smbd::frame_step.delta==smbd::frame_step.interval && "
                        "(smbd::frame_step.phase>0 || !smbd::frame_step.logic)) {\n"
                        "        const auto phase=smbd::frame_step.phase-(smbd::frame_step.logic?0.0f:1.0f);\n"
                        "        const auto sample=_mm_add_ss(" << elapsed << ",_mm_set_ss(phase));\n";
                vector_write(operands[0],"_mm_div_ss(_mm_min_ss(_mm_max_ss(sample,_mm_setzero_ps()),"+duration+"),"+duration+")");
                body << "    } else {\n";
                vector_write(operands[0],value);
                body << "    }\n}\n";
                return;
            }
            if(instruction.address==0x7F9AC || instruction.address==0x7F9EA || instruction.address==0x7FA28) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F5905C47C3400") throw std::runtime_error("Original conveyor displacement differs");
                value="smbd::frame_increment("+value+')';
            }
            constexpr std::pair<std::uint32_t,std::string_view> responses[]={{0xA7C14,"F30F59C8"},{0xA7C2E,"F30F59C8"},{0xA7C4C,"F30F59C8"},
                {0xA9DD0,"F30F59C8"},{0xA9DEA,"F30F59C8"},{0xA9E08,"F30F59C8"},{0x8B58A,"F30F59C8"}};
            for(const auto& [site,bytes]:responses) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes || ins.mnemonic!=ZYDIS_MNEMONIC_MULSS)
                    throw std::runtime_error("Original motion response signature differs");
                native_declarations["frame_gain"]="__m128 frame_gain(FloatingState&,__m128);\n";
                value="_mm_mul_ss("+vector(operands[0])+",frame_gain(cpu.floating,"+vector(operands[1],true)+"))";
            }
            if(instruction.address==0x8BC7C || instruction.address==0x8BCD1) {
                const auto signature=instruction.address==0x8BC7C?"F30F5905407C3400":"F30F59050C7C3400";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original ball-size response differs");
                native_declarations["frame_gain_interval"]="__m128 frame_gain_interval(FloatingState&,__m128,float);\n";
                std::string interval="(frame_size_expired?smbd::frame_step.phase:smbd::frame_step.delta)";
                if(instruction.address==0x8BCD1) {
                    used.insert(1);
                    body << "    const float remaining=float(std::bit_cast<std::int32_t>(memory.load<std::uint32_t>(ecx+0x148U)))+(smbd::frame_step.logic?1.0f:0.0f)-smbd::frame_step.previous_phase;\n"
                            "    const float active_interval=remaining<0?0:remaining>smbd::frame_step.delta?smbd::frame_step.delta:remaining;\n";
                    interval="active_interval";
                }
                value="_mm_mul_ss("+vector(operands[0])+",frame_gain_interval(cpu.floating,"+vector(operands[1],true)+','+interval+"))";
            }
            if (unary && scalar) value="_mm_move_ss("+vector(operands[0])+','+value+')';
            if(ordered_sse) {
                const auto left=vector(operands[0]),right=vector(operands[1],scalar);
                if(value!="_mm_"+name+'('+left+','+right+')')
                    throw std::runtime_error("Checked SIMD arithmetic has a custom lowering");
                // Native intrinsics can commute add and multiply. Execute the
                // x86 opcode in guest order to preserve NaN payload selection.
                body << "#if (defined(__clang__) || defined(__GNUC__)) && (defined(__x86_64__) || defined(_M_X64))\n"
                     << "    auto result = " << left << ";\n"
                     << "    const auto source = " << right << ";\n"
                     << "    __asm__ volatile(\"" << ZydisMnemonicGetString(ins.mnemonic)
                     << " {%1,%0|%0,%1}\" : \"+x\"(result) : \"x\"(source));\n"
                     << "#else\n    const auto result = " << value << ";\n#endif\n";
                value="result";
            }
            vector_write(operands[0],value); break;
        }
        case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX: {
            auto value=read(operands[1]);
            if(instruction.address==0xB78A3) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="66C7866CFFFFFF5555")
                    throw std::runtime_error("Original dynamic FOV maximum differs");
                native_declarations["frame_camera_fov_max"]="std::uint32_t frame_camera_fov_max(Memory&,std::uint32_t);\n";
                value="frame_camera_fov_max(memory,"+read_reg(ZYDIS_REGISTER_ESI)+"-0xC4U)";
            }
            if(instruction.address==0xA7D45 || instruction.address==0xA7D48) {
                const bool second=instruction.address==0xA7D48;
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=(second?"66894602":"66890E"))
                    throw std::runtime_error("Original WORLD angle decay store differs");
                native_declarations["frame_world_decay"]="std::uint32_t frame_world_decay(FloatingState&,Memory&,std::uint32_t,std::uint32_t);\n";
                used.insert(6);
                value="frame_world_decay(cpu.floating,memory,esi+"+std::string(second?"2":"0")+"U,"+value+")";
            }
            if(instruction.address==0xA78C9 || instruction.address==0xA7AFD) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=(instruction.address==0xA78C9?"8B0DA87E6C00":"668B06"))
                    throw std::runtime_error("Original WORLD controller transition differs");
                native_declarations["frame_reset_world"]="void frame_reset_world(std::uint32_t);\n";
                used.insert(6);body << "    frame_reset_world(esi);\n";
            }
            if(instruction.address==0xA7FC1) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="66894602") throw std::runtime_error("Original WORLD steering store differs");
                native_declarations["frame_world_steering"]="std::uint32_t frame_world_steering(FloatingState&,Memory&,std::uint32_t,std::uint32_t,std::uint32_t);\n";
                used.insert(6);
                value="frame_world_steering(cpu.floating,memory,esi+2U,frame_world_difference,"+value+")";
            }
            if(instruction.address==0xA3059 || instruction.address==0x895B8 || instruction.address==0x10E4FB) {
                const auto signature=instruction.address==0x10E4FB?"B900080000":"B9380E0000";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original continuous character rotation differs");
                value="static_cast<std::uint32_t>(smbd::frame_integer_increment("+value+"))";
            }
            body << "    const auto value = " << value << ";\n";
            write(operands[0], "value"); break;
        }
        case ZYDIS_MNEMONIC_MOVSX:
            body << "    const auto value = smbd::sign_extend<" << operands[1].size << ">(" << read(operands[1]) << ");\n";
            if(instruction.address==0xA7F94) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="0FBFC0") throw std::runtime_error("Original WORLD steering difference differs");
                body << "    frame_world_difference=value;\n";
            }
            type(operands[1].size); write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_LEA: {
            auto value=address(operands[1],true);
            if(instruction.address==0xAE217) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="8DAEFC000000")
                    throw std::runtime_error("Original camera shake vector pointer differs");
                value="(frame_shake_step?"+value+":frame_shake_sample)";
            }
            body << "    const auto value = " << value << ";\n";
            write(operands[0],"value"); break;
        }
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP:
        case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB:
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST: {
            auto left=read(operands[0]);
            auto increment=read(operands[1]);
            constexpr std::pair<std::uint32_t,std::string_view> hud_turns[]={
                {0x4D407,"81C280020000"},{0x4D40F,"81C200010000"},
                {0x4D417,"81C280000000"},{0x4D464,"81C280010000"}};
            for(const auto& [site,bytes]:hud_turns) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes)
                    throw std::runtime_error("Original character HUD spin differs");
                increment="static_cast<std::uint32_t>(smbd::frame_integer_increment("+increment+"))";
            }
            constexpr std::pair<std::uint32_t,std::string_view> item_angles[]={{0x258FB9,"6603D8"},{0x258FC4,"6603C1"},{0x258FF3,"6603CA"}};
            for(const auto& [site,bytes]:item_angles) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original item rotation differs");
                left="static_cast<std::uint32_t>(smbd::frame_integer_increment(static_cast<std::int16_t>("+left+")))";
            }
            body << "    const std::uint32_t a = " << left << ";\n"
                 << "    const std::uint32_t b = " << increment << ";\n";
            const auto bits = operands[0].size;
            const auto m = ins.mnemonic;
            if (m == ZYDIS_MNEMONIC_ADD || m == ZYDIS_MNEMONIC_SUB || m == ZYDIS_MNEMONIC_CMP ||
                m == ZYDIS_MNEMONIC_ADC || m == ZYDIS_MNEMONIC_SBB) {
                const bool addition = m == ZYDIS_MNEMONIC_ADD || m == ZYDIS_MNEMONIC_ADC;
                const bool with_carry = m == ZYDIS_MNEMONIC_ADC || m == ZYDIS_MNEMONIC_SBB;
                if (with_carry) {
                    type(bits);
                    uses_flags = writes_flags = true;
                    body << "    const auto input_carry = flags & smbd::carry;\n"
                         << "    flags = smbd::" << (addition ? "add_flags" : "sub_flags") << '<' << bits
                         << ">(flags, a, b, input_carry);\n";
                } else flags(addition ? "add_flags" : "sub_flags", bits);
                if (m != ZYDIS_MNEMONIC_CMP)
                    write(operands[0], std::string(addition ? "a + b" : "a - b") +
                                      (with_carry ? (addition ? " + input_carry" : " - input_carry") : ""));
            } else {
                const char operation = m == ZYDIS_MNEMONIC_OR ? '|' : m == ZYDIS_MNEMONIC_XOR ? '^' : '&';
                body << "    const auto value = a " << operation << " b;\n";
                uses_flags = writes_flags = true;
                type(bits);
                body << "    flags = smbd::logic_flags<" << bits << ">(flags, value);\n";
                if (m != ZYDIS_MNEMONIC_TEST) write(operands[0], "value");
            }
            break;
        }
        case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_SETNB:
        case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETNL:
        case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_SETNLE:
            body << "    const std::uint32_t value = (" << condition(ins.mnemonic) << ") ? 1U : 0U;\n";
            write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JNB:
        case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JNL:
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_JNLE:
            if(instruction.tail_target) {
                calls=restores=returns=true;
                body << "    if (" << condition(ins.mnemonic) << ") { commit(); guest_" << std::format("{:08X}",*instruction.tail_target)
                    << "(cpu,memory); restore(); goto done; }\n";
            } else body << "    if (" << condition(ins.mnemonic) << ") goto " << label(*instruction.target()) << ";\n";
            break;
        case ZYDIS_MNEMONIC_JMP:
            if(operands[0].type==ZYDIS_OPERAND_TYPE_POINTER) {
                calls=true;
                body << "    commit();\n    cpu.eip=" << constant(instruction.address) << ";\n"
                    << "    smbd::load_code_selector(cpu,memory," << operands[0].ptr.segment << ',' << constant(operands[0].ptr.offset)
                    << ");\n    goto " << label(*instruction.target()) << ";\n";
            } else if (instruction.kernel_ordinal) {
                calls=restores=returns=true;
                body << "    commit();\n    cpu.eip=" << constant(instruction.address) << ";\n"
                    << "    smbd::call_kernel<" << *instruction.kernel_ordinal << ">(cpu,memory);\n    restore();\n    goto done;\n";
            } else if(instruction.zero_guard) {
                calls=true;
                body << "    const auto target=" << read(operands[0]) << ";\n    commit();\n"
                    << "    cpu.eip=" << constant(instruction.address) << ";\n"
                    << "    throw smbd::NativeBoundary(target,\"callback was proved unreachable\");\n";
            } else if(instruction.invalid_target) {
                calls=true;
                body << "    const auto target=" << read(operands[0]) << ";\n    commit();\n"
                    << "    cpu.eip=" << constant(instruction.address) << ";\n"
                    << "    throw smbd::NativeBoundary(target,\"invalid transfer through an immutable zero slot\");\n";
            } else if(instruction.tail_dispatch) {
                calls=restores=returns=true;
                body << "    const auto target=" << read(operands[0]) << ";\n    commit();\n    switch(target) {\n";
                const std::set<std::uint32_t> targets(instruction.jump_targets.begin(),instruction.jump_targets.end());
                for(auto target:targets) body << "    case " << constant(target) << ": guest_" << std::format("{:08X}",target) << "(cpu,memory); break;\n";
                if(!instruction.null_sources.empty()) body << "    case 0U: cpu.eip=" << constant(instruction.address) << "; throw smbd::NativeBoundary(target,\"null callback from a verified table\");\n";
                body << "    default: throw smbd::NativeBoundary(target,\"tail-call target is outside its verified table\");\n    }\n    restore();\n    goto done;\n";
            } else if (!instruction.jump_targets.empty()) {
                calls=true;
                body << "    switch(" << read(operands[0]) << ") {\n";
                const std::set<std::uint32_t> targets(instruction.jump_targets.begin(),instruction.jump_targets.end());
                for(const auto target : targets) body << "    case " << constant(target) << ": goto " << label(target) << ";\n";
                body << "    default: commit(); cpu.eip=" << constant(instruction.address)
                    << "; throw std::runtime_error(\"Guest jump table target is outside its recovered set\");\n    }\n";
            } else if(instruction.tail_target) {
                calls=restores=returns=true;
                body << "    commit();\n    guest_" << std::format("{:08X}",*instruction.tail_target) << "(cpu,memory);\n    restore();\n    goto done;\n";
            } else if (instruction.target()) body << "    goto " << label(*instruction.target()) << ";\n";
            else {
                calls=restores=returns=true;
                body << "    const auto target=" << read(operands[0]) << ";\n    commit();\n"
                    << "    cpu.eip=" << constant(instruction.address) << ";\n"
                    << "    smbd::invoke_native(cpu,memory,target);\n    restore();\n    goto done;\n";
            }
            terminator = true; break;
        case ZYDIS_MNEMONIC_PUSH: {
            const auto width = ins.operand_width;
            if (width != 16 && width != 32) throw std::runtime_error("Unsupported push width");
            auto value=read(operands[0]);
            if(instruction.address==0x891C0 || instruction.address==0x898DC || instruction.address==0x906C0) {
                const auto signature=instruction.address==0x891C0?"68D8020000":instruction.address==0x898DC?"688E030000":"52";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=signature) throw std::runtime_error("Original character recovery turn differs");
                value="static_cast<std::uint32_t>(smbd::frame_integer_increment("+value+"))";
            }
            if(instruction.address==0x6A206) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="68ABAAAA3F")
                    throw std::runtime_error("Original perspective UI aspect differs");
                // These UI sprites already contain the native 16:9 layout.
                // Widen their separate 4:3 perspective by 21/16, matching
                // the orthographic HUD without changing scene cameras.
                value="(cpu.ultrawide?0x3FE00000U:"+value+")";
            }
            used.insert(4); dirty.insert(4);
            body << "    const auto value = " << value << ";\n    esp -= " << width / 8
                 << "U;\n    memory.store<" << type(width) << ">(esp, static_cast<" << type(width) << ">(value));\n";
            break;
        }
        case ZYDIS_MNEMONIC_PUSHF: case ZYDIS_MNEMONIC_PUSHFD: {
            const unsigned width=ins.operand_width;
            if(width!=16 && width!=32) throw std::runtime_error("Unsupported flags stack width");
            used.insert(4); dirty.insert(4); uses_flags=true;
            body << "    esp-=" << width/8 << "U;\n    memory.store<" << type(width)
                << ">(esp,static_cast<" << type(width) << ">((flags&~0x30000U)|2U));\n";
            break;
        }
        case ZYDIS_MNEMONIC_POPF: case ZYDIS_MNEMONIC_POPFD: {
            const unsigned width=ins.operand_width;
            if(width!=16 && width!=32) throw std::runtime_error("Unsupported flags stack width");
            used.insert(4); dirty.insert(4); uses_flags=writes_flags=true;
            body << "    const auto value=memory.load<" << type(width) << ">(esp);\n    esp+=" << width/8
                << "U;\n    flags=smbd::pop_flags(flags,value," << width << ",cpu.cs_selector&3U);\n";
            break;
        }
        case ZYDIS_MNEMONIC_POP: {
            const auto width = ins.operand_width;
            if (width != 16 && width != 32) throw std::runtime_error("Unsupported pop width");
            used.insert(4); dirty.insert(4);
            body << "    const auto value = memory.load<" << type(width) << ">(esp);\n    esp += " << width / 8 << "U;\n";
            write(operands[0], "value"); break;
        }
        case ZYDIS_MNEMONIC_CALL: {
            if (ins.operand_width != 32 || operands[0].type==ZYDIS_OPERAND_TYPE_POINTER)
                throw std::runtime_error("Only 32-bit near native calls are supported");
            calls = restores = true;
            const bool map_aspect=instruction.address==0x58BDF || instruction.address==0x58D3A || instruction.address==0x58D55 ||
                instruction.address==0x58F5B || instruction.address==0x58F80;
            if(map_aspect) {
                if(ins.length!=5 || instruction.target()!=0xADDC0U) throw std::runtime_error("Original minimap aspect call differs");
                body << "    const auto aspect=memory.load<std::uint32_t>(0x73F0ECU);\n"
                    "    if(cpu.ultrawide) memory.store<std::uint32_t>(0x73F0ECU,0x3FE38E39U);\n";
            }
            const bool indirect=!instruction.target() && !instruction.kernel_ordinal;
            if(indirect) body << "    const auto target=" << read(operands[0]) << ";\n";
            used.insert(4); dirty.insert(4);
            body << "    esp -= 4U;\n    memory.store<std::uint32_t>(esp, " << constant(next)
                 << ");\n    commit();\n    cpu.eip = " << constant(instruction.target().value_or(instruction.address)) << ";\n    ";
            if(instruction.address==0x9932B || instruction.address==0x9938F) {
                if(instruction.target()!=0x98B50) throw std::runtime_error("Original sampled pose call differs");
                native_declarations["frame_sampled_pose"]="void guest_00098B50_sampled(Cpu&,Memory&,const std::uint32_t*);\n";
                body << "guest_00098B50_sampled(cpu,memory,&frame_pose_time);\n";
            } else if(instruction.address==0x8902F) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="E8DC0EFAFF" || instruction.target()!=0x29F10)
                    throw std::runtime_error("Original character upright quaternion call differs");
                native_declarations["frame_upright_rotation"]="bool frame_upright_rotation(Cpu&,Memory&,float);\n";
                used.insert(6);
                body << "if(!frame_upright_rotation(cpu,memory,smbd::frame_step.delta)) guest_00029F10(cpu,memory);\n";
            } else if (instruction.kernel_ordinal) body << "smbd::call_kernel<" << *instruction.kernel_ordinal << ">(cpu,memory);\n";
            else if (instruction.target()) body << "guest_" << std::format("{:08X}", *instruction.target()) << "(cpu, memory);\n";
            else if(instruction.zero_guard) body << "throw smbd::NativeBoundary(target,\"callback was proved unreachable\");\n";
            else if(instruction.invalid_target) body << "throw smbd::NativeBoundary(target,\"invalid transfer through an immutable zero slot\");\n";
            else if(!instruction.jump_targets.empty()) {
                body << "switch(target) {\n";
                const std::set<std::uint32_t> targets(instruction.jump_targets.begin(),instruction.jump_targets.end());
                for(auto target:targets) body << "    case " << constant(target) << ": guest_" << std::format("{:08X}",target) << "(cpu,memory); break;\n";
                if(!instruction.null_sources.empty()) body << "    case 0U: throw smbd::NativeBoundary(target,\"null callback from a verified table\");\n";
                body << "    default: throw smbd::NativeBoundary(target,\"call target is outside its verified table\");\n    }\n";
            } else body << "smbd::invoke_native(cpu,memory,target);\n";
            body << "    if (cpu.eip != " << constant(next)
                 << ") throw std::runtime_error(\"Unexpected return from call at " << hex32(instruction.address) << "\");\n"
                 << "    restore();\n";
            if(map_aspect) body << "    if(cpu.ultrawide) memory.store<std::uint32_t>(0x73F0ECU,aspect);\n";
            break;
        }
        case ZYDIS_MNEMONIC_RDTSC:
            calls=true;
            body << "    commit();\n    cpu.eip=" << constant(instruction.address) << ";\n"
                 << "    const auto timestamp=smbd::read_timestamp(cpu);\n";
            write(ZydisDecodedOperand{.type=ZYDIS_OPERAND_TYPE_REGISTER,.reg={ZYDIS_REGISTER_EAX}},"static_cast<std::uint32_t>(timestamp)");
            write(ZydisDecodedOperand{.type=ZYDIS_OPERAND_TYPE_REGISTER,.reg={ZYDIS_REGISTER_EDX}},"static_cast<std::uint32_t>(timestamp>>32)");
            break;
        case ZYDIS_MNEMONIC_IN: case ZYDIS_MNEMONIC_OUT: {
            calls=restores=true;
            const bool input=ins.mnemonic==ZYDIS_MNEMONIC_IN;
            const auto& port=operands[input?1:0];
            const auto& value=operands[input?0:1];
            body << "    const auto port=static_cast<std::uint16_t>(" << read(port) << ");\n";
            if(!input) body << "    const auto value=" << read(value) << ";\n";
            body << "    commit();\n    cpu.eip=" << constant(instruction.address) << ";\n";
            if(input) body << "    const auto value=smbd::read_port(cpu,port," << value.size << ");\n";
            else body << "    smbd::write_port(cpu,port," << value.size << ",value);\n";
            body << "    restore();\n";
            if(input) write(value,"value");
            break;
        }
        case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_INT:
            calls=true;
            body << "    commit();\n    cpu.eip=" << constant(next) << ";\n"
                 << "    throw smbd::GuestTrap(" << (ins.mnemonic==ZYDIS_MNEMONIC_INT3?3:operands[0].imm.value.u)
                 << ',' << constant(instruction.address) << ',' << constant(next) << ");\n";
            terminator=true; break;
        case ZYDIS_MNEMONIC_LEAVE:
            if (ins.operand_width != 32) throw std::runtime_error("Unsupported leave width");
            used.insert(4); used.insert(5); dirty.insert(4); dirty.insert(5);
            body << "    esp = ebp;\n    ebp = memory.load<std::uint32_t>(esp);\n    esp += 4U;\n";
            break;
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: {
            const bool increment = ins.mnemonic == ZYDIS_MNEMONIC_INC;
            type(operands[0].size);
            uses_flags = writes_flags = true;
            body << "    const auto a = " << read(operands[0]) << ";\n    flags = (smbd::"
                 << (increment ? "add_flags" : "sub_flags") << '<' << operands[0].size
                 << ">(flags, a, 1U) & ~smbd::carry) | (flags & smbd::carry);\n";
            write(operands[0], increment ? "a + 1U" : "a - 1U"); break;
        }
        case ZYDIS_MNEMONIC_NOT: case ZYDIS_MNEMONIC_NEG:
            body << "    const auto a = " << read(operands[0]) << ";\n";
            if (ins.mnemonic == ZYDIS_MNEMONIC_NEG) {
                type(operands[0].size);
                uses_flags = writes_flags = true;
                body << "    flags = smbd::sub_flags<" << operands[0].size << ">(flags, 0U, a);\n";
            }
            write(operands[0], ins.mnemonic == ZYDIS_MNEMONIC_NOT ? "~a" : "0U - a"); break;
        case ZYDIS_MNEMONIC_CDQ:
            used.insert(0); used.insert(2); dirty.insert(2);
            body << "    edx = 0U - (eax >> 31);\n"; break;
        case ZYDIS_MNEMONIC_CWDE:
            used.insert(0); dirty.insert(0);
            body << "    eax = smbd::sign_extend<16>(eax);\n"; break;
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: {
            type(operands[0].size);
            uses_flags = writes_flags = true;
            body << "    const auto result = smbd::shift<" << operands[0].size << ">(flags, " << read(operands[0])
                 << ", " << read(operands[1]) << ", smbd::Shift::"
                 << (ins.mnemonic == ZYDIS_MNEMONIC_SHL ? "left" : ins.mnemonic == ZYDIS_MNEMONIC_SHR ? "right" : "arithmetic_right")
                 << ");\n    flags = result.flags;\n";
            std::string value="result.value";
            if(instruction.address==0xB7945) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="C1F805")
                    throw std::runtime_error("Original dynamic FOV response differs");
                native_declarations["frame_camera_fov_response"]="std::uint32_t frame_camera_fov_response(FloatingState&,Memory&,std::uint32_t,std::uint32_t,std::uint32_t);\n";
                value="frame_camera_fov_response(cpu.floating,memory,"+read_reg(ZYDIS_REGISTER_ESI)+"-0xC4U,"+
                    read(operands[0])+",result.value)";
                floating=true;
            }
            if(instruction.address==0xA7DE8 || instruction.address==0xA7E11 || instruction.address==0xA7EB6 || instruction.address==0xA7EE1) {
                const bool second=instruction.address==0xA7E11 || instruction.address==0xA7EE1;
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=(second?"66C1FA02":"66C1F902"))
                    throw std::runtime_error("Original replay WORLD response differs");
                used.insert(6);
                native_declarations["frame_world_response"]="std::uint32_t frame_world_response(FloatingState&,Memory&,std::uint32_t,std::uint32_t,bool);\n";
                value="frame_world_response(cpu.floating,memory,esi+"+std::string(second?"2":"0")+"U,"+read(operands[0])+','+
                    (instruction.address>=0xA7E70?"true":"false")+')';
            }
            write(operands[0],value); break;
        }
        case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCR: case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_RCL: {
            type(operands[0].size);
            uses_flags=writes_flags=true;
            body << "    const auto result=smbd::rotate_" << (ins.mnemonic==ZYDIS_MNEMONIC_ROL || ins.mnemonic==ZYDIS_MNEMONIC_RCL?"left":"right")
                << '<' << operands[0].size << ','
                << (ins.mnemonic==ZYDIS_MNEMONIC_RCR || ins.mnemonic==ZYDIS_MNEMONIC_RCL?"true":"false") << ">(" << read(operands[0]) << ','
                << read(operands[1]) << ",flags);\n    flags=result.flags;\n";
            write(operands[0],"result.value"); break;
        }
        case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD: {
            if(operands[0].size!=16 && operands[0].size!=32) throw std::runtime_error("Invalid double shift width");
            uses_flags=writes_flags=true;
            body << "    const auto result=smbd::double_shift<" << operands[0].size << ','
                << (ins.mnemonic==ZYDIS_MNEMONIC_SHRD?"true":"false") << ">(" << read(operands[0]) << ','
                << read(operands[1]) << ',' << read(operands[2]) << ",flags);\n    flags=result.flags;\n";
            write(operands[0],"result.value"); break;
        }
        case ZYDIS_MNEMONIC_MUL: case ZYDIS_MNEMONIC_IMUL: {
            uses_flags = writes_flags = true;
            const auto bits = operands[0].size;
            type(bits);
            const bool implicit = ins.operand_count_visible == 1;
            const auto accumulator = bits == 8 ? ZYDIS_REGISTER_AL : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX;
            const auto a = implicit ? read_reg(accumulator) : read(operands[ins.operand_count_visible == 3 ? 1 : 0]);
            const auto b = read(operands[implicit ? 0 : ins.operand_count_visible == 3 ? 2 : 1]);
            body << "    const auto product = smbd::multiply<" << bits << ", "
                 << (ins.mnemonic == ZYDIS_MNEMONIC_IMUL ? "true" : "false") << ">(flags, " << a << ", " << b
                 << ");\n    flags = product.flags;\n";
            if (implicit) {
                ZydisDecodedOperand dest{};
                dest.type = ZYDIS_OPERAND_TYPE_REGISTER;
                dest.reg.value = bits == 8 ? ZYDIS_REGISTER_AX : accumulator;
                write(dest, "static_cast<std::uint32_t>(product.value)");
                if (bits != 8) {
                    dest.reg.value = bits == 16 ? ZYDIS_REGISTER_DX : ZYDIS_REGISTER_EDX;
                    write(dest, "static_cast<std::uint32_t>(product.value >> " + std::to_string(bits) + ')');
                }
            } else write(operands[0], "static_cast<std::uint32_t>(product.value)");
            break;
        }
        case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: {
            const auto bits = operands[0].size;
            type(bits);
            const auto accumulator = bits == 8 ? ZYDIS_REGISTER_AL : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX;
            const auto dividend = bits == 8 ? read_reg(ZYDIS_REGISTER_AX) :
                "(std::uint64_t(" + read_reg(bits == 16 ? ZYDIS_REGISTER_DX : ZYDIS_REGISTER_EDX) + ") << " +
                std::to_string(bits) + ") | " + read_reg(accumulator);
            body << "    const auto quotient = smbd::divide<" << bits << ", "
                 << (ins.mnemonic == ZYDIS_MNEMONIC_IDIV ? "true" : "false") << ">(" << dividend << ", " << read(operands[0]) << ");\n";
            ZydisDecodedOperand dest{};
            dest.type = ZYDIS_OPERAND_TYPE_REGISTER;
            dest.reg.value = accumulator;
            write(dest, "quotient.value");
            dest.reg.value = bits == 8 ? ZYDIS_REGISTER_AH : bits == 16 ? ZYDIS_REGISTER_DX : ZYDIS_REGISTER_EDX;
            write(dest, "quotient.remainder"); break;
        }
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD: {
            if (!string_move && !string_fill) throw std::runtime_error("SIMD move needs its own lowering");
            const auto bits = (ins.mnemonic == ZYDIS_MNEMONIC_MOVSB || ins.mnemonic == ZYDIS_MNEMONIC_STOSB) ? 8 :
                              (ins.mnemonic == ZYDIS_MNEMONIC_MOVSW || ins.mnemonic == ZYDIS_MNEMONIC_STOSW) ? 16 : 32;
            uses_flags = true;
            used.insert(7); dirty.insert(7);
            if (repeats) { used.insert(1); dirty.insert(1); }
            body << "    const auto count = " << (repeats ? "ecx" : "1U") << ";\n"
                 << "    const bool descending = (flags & smbd::direction) != 0;\n";
            if (string_move) {
                used.insert(6); dirty.insert(6);
                body << "    memory.move_elements<" << type(bits) << ">(edi, esi, count, descending);\n";
            } else {
                used.insert(0);
                body << "    memory.fill_elements<" << type(bits) << ">(edi, static_cast<" << type(bits) << ">(eax), count, descending);\n";
            }
            body << "    const auto step = descending ? 0U - count * " << bits / 8 << "U : count * " << bits / 8 << "U;\n"
                 << "    edi += step;\n";
            if (string_move) body << "    esi += step;\n";
            if (repeats) body << "    ecx = 0;\n";
            break;
        }
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: {
            if(!string_compare && !string_scan) throw std::runtime_error("SIMD compare needs its own lowering");
            const auto bits=ins.mnemonic==ZYDIS_MNEMONIC_CMPSB || ins.mnemonic==ZYDIS_MNEMONIC_SCASB?8:
                ins.mnemonic==ZYDIS_MNEMONIC_CMPSW || ins.mnemonic==ZYDIS_MNEMONIC_SCASW?16:32;
            used.insert(7); dirty.insert(7);
            if(string_compare) { used.insert(6); dirty.insert(6); }
            else used.insert(0);
            uses_flags = writes_flags = true;
            if (repeats) { used.insert(1); dirty.insert(1); body << "    while (ecx != 0) {\n"; }
            body << "    const auto a = ";
            if(string_compare) body << "memory.load<" << type(bits) << ">(esi)";
            else body << "eax";
            body << ";\n    const auto b = memory.load<" << type(bits) << ">(edi);\n"
                 << "    flags = smbd::sub_flags<" << bits << ">(flags, a, b);\n"
                 << "    const auto step = (flags & smbd::direction) ? 0U-" << bits/8 << "U : " << bits/8 << "U;\n";
            if(string_compare) body << "    esi += step;\n";
            body << "    edi += step;\n";
            if (repeats) {
                body << "    --ecx;\n    if ((flags & smbd::zero) "
                     << ((ins.attributes & ZYDIS_ATTRIB_HAS_REPNE) ? "!= 0" : "== 0") << ") break;\n    }\n";
            }
            break;
        }
        case ZYDIS_MNEMONIC_CLD: case ZYDIS_MNEMONIC_STD:
            uses_flags = writes_flags = true;
            body << "    flags " << (ins.mnemonic == ZYDIS_MNEMONIC_CLD ? "&= ~" : "|= ") << "smbd::direction;\n";
            break;
        case ZYDIS_MNEMONIC_RET: {
            if(instruction.address==0x97D80 || instruction.address==0x97DC1 || instruction.address==0x97DD9 || instruction.address==0x97DFA) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="C3") throw std::runtime_error("Original clip advance return differs");
                native_declarations["frame_clip_commit"]="void frame_clip_commit(Memory&,std::uint32_t,std::uint32_t,float,float);\n";
                used.insert(6);
                body << "    if(frame_clip_advanced) frame_clip_commit(memory,esi,frame_clip_bounds,frame_clip_before,frame_clip_increment);\n";
            }
            if(tennis_replay_fade && (instruction.address==0x1EB108 || instruction.address==0x1EB10E)) {
                // The fade ignores the cursor helper's end result. Retain the
                // last recorded frame until the normal playback phase handles
                // it; a full circular history must still advance and wrap.
                used.insert(0);
                body << "    if(eax) {\n"
                        "        const auto replay=memory.load<std::uint32_t>(0x00807DDCU);\n"
                        "        if(!memory.load<std::uint32_t>(replay+0x21CU)) {\n"
                        "            const auto cursor=memory.load<std::uint32_t>(replay+0x210U);\n"
                        "            memory.store<std::uint32_t>(replay+0x210U,cursor-1);\n"
                        "        }\n    }\n";
            }
            returns=true;
            if (ins.operand_width != 32) throw std::runtime_error("Unsupported return width");
            used.insert(4); dirty.insert(4);
            body << "    const auto return_address = memory.load<std::uint32_t>(esp);\n"
                 << "    esp += " << (4U + (ins.operand_count_visible ? static_cast<unsigned>(operands[0].imm.value.u) : 0U))
                 << "U;\n    cpu.eip = return_address;\n    goto done;\n";
            terminator = true; break;
        }
        case ZYDIS_MNEMONIC_NOP: break;
        default:
            if (ins.meta.category == ZYDIS_CATEGORY_COND_BR && instruction.target()) {
                body << "    if (" << condition(ins.mnemonic) << ") goto " << label(*instruction.target()) << ";\n";
            } else if (ins.meta.category == ZYDIS_CATEGORY_SETCC) {
                body << "    const std::uint32_t value = (" << condition(ins.mnemonic) << ") ? 1U : 0U;\n";
                write(operands[0], "value");
            } else if (ins.meta.category == ZYDIS_CATEGORY_CMOV) {
                body << "    const auto value = " << read(operands[1]) << ";\n    if (" << condition(ins.mnemonic) << ") {\n";
                write(operands[0], "value");
                body << "    }\n";
            } else if ((ins.opcode>=0xD8 && ins.opcode<=0xDF && ins.opcode_map==ZYDIS_OPCODE_MAP_DEFAULT) || ins.mnemonic==ZYDIS_MNEMONIC_FWAIT) {
                x87(instruction);
            } else throw std::runtime_error("Instruction has no native lowering yet");
        }
        if (!terminator && checked_ranges.empty()) body << "    goto " << label(next) << ";\n";
        body << "}\n";
    }
};

// The measured loops repeatedly access fields of the same matrix or vertex
// record. The ordinary lowering handles split or unmapped ranges and retains
// every original branch target.
std::string checked_simd_block(const Function& function,const Emitter& original,std::uint32_t start,std::uint32_t end) {
    constexpr ZydisMnemonic allowed[]={ZYDIS_MNEMONIC_MOVSS,ZYDIS_MNEMONIC_ADDSS,ZYDIS_MNEMONIC_MULSS,
        ZYDIS_MNEMONIC_MOVAPS,ZYDIS_MNEMONIC_MOVUPS,ZYDIS_MNEMONIC_MOVLPS,ZYDIS_MNEMONIC_MOVLHPS,
        ZYDIS_MNEMONIC_SHUFPS,ZYDIS_MNEMONIC_MULPS,ZYDIS_MNEMONIC_ADDPS};
    Emitter fast;
    std::set<std::string> aligned_addresses;
    const auto first=function.instructions.find(start),last=function.instructions.find(end);
    if(first==function.instructions.end() || last==function.instructions.end() || start>=end)
        throw std::runtime_error("Missing checked SIMD block boundary");
    auto next=start;
    for(auto at=first;at!=last;++at) {
        const auto& [pc,instruction]=*at;
        const auto& ins=instruction.decoded;
        if(pc!=next || original.loop_headers.contains(pc) || std::ranges::find(allowed,ins.mnemonic)==std::end(allowed))
            throw std::runtime_error("Unexpected instruction in checked SIMD block at "+hex32(pc));
        next+=ins.length;
        for(unsigned i=0;i<ins.operand_count_visible;++i) {
            const auto& operand=instruction.operands[i];
            if(operand.type==ZYDIS_OPERAND_TYPE_REGISTER) fast.xmm_register(operand.reg.value);
            if(operand.type!=ZYDIS_OPERAND_TYPE_MEMORY) continue;
            const auto offset=operand.mem.disp.has_displacement?operand.mem.disp.value:0;
            if(operand.mem.type!=ZYDIS_MEMOP_TYPE_MEM || (operand.mem.segment!=ZYDIS_REGISTER_DS && operand.mem.segment!=ZYDIS_REGISTER_SS) ||
                offset<0 || offset>64 || !operand.size || operand.size>128 || operand.size%8)
                throw std::runtime_error("Unexpected operand in checked SIMD block at "+hex32(pc));
            const auto bytes=static_cast<unsigned>(offset)+operand.size/8;
            const auto range=std::ranges::find_if(fast.checked_ranges,[&](const auto& candidate){return candidate.shares_address(operand);});
            if(range!=fast.checked_ranges.end()) range->bytes=std::max(range->bytes,bytes);
            else {
                auto origin=operand;
                origin.mem.disp.has_displacement=false;
                fast.checked_ranges.push_back({origin,bytes});
            }
            if(ins.mnemonic==ZYDIS_MNEMONIC_MOVAPS) {
                auto aligned=operand;
                aligned.mem.disp.has_displacement=offset%16!=0;
                aligned.mem.disp.value=offset%16;
                aligned_addresses.insert(fast.address(aligned));
            }
        }
    }
    if(next!=end || fast.checked_ranges.empty()) throw std::runtime_error("Incomplete checked SIMD block");
    std::ostringstream out;
    for(unsigned i=0;i<fast.checked_ranges.size();++i) {
        const auto& range=fast.checked_ranges[i];
        out << "    auto* checked_" << i << "=memory.try_access(" << fast.address(range.origin) << ',' << range.bytes << ");\n";
    }
    out << "    if(checked_0";
    for(unsigned i=1;i<fast.checked_ranges.size();++i) out << " && checked_" << i;
    for(const auto& address:aligned_addresses) out << " && (" << address << " &15U)==0";
    // Keep intermediate registers local only when memory and SSE operations
    // cannot fault. The ordinary path retains instruction-level state.
    // Floating scopes and LDMXCSR track the control bits in guest state.
    // Arithmetic changes sticky status flags, but never these exception masks.
    out << " && (cpu.floating.mxcsr & 0x1F80U)==0x1F80U) {\n";
    for(auto at=first;at!=last;++at) fast.instruction(at->second,{},true);
    if(fast.calls || fast.returns || !fast.native_declarations.empty())
        throw std::runtime_error("Checked SIMD block calls a runtime service");
    for(unsigned i=0;i<fast.checked_xmm_inputs.size();++i) if(fast.checked_xmm_inputs[i] || fast.checked_xmm_writes[i]) {
        out << "    __m128 checked_xmm_" << i;
        if(fast.checked_xmm_inputs[i]) out << " = smbd::xmm(cpu.floating," << i << ')';
        out << ";\n";
    }
    out << fast.body.view();
    for(unsigned i=0;i<fast.checked_xmm_writes.size();++i) if(fast.checked_xmm_writes[i])
        out << "    smbd::set_xmm(cpu.floating," << i << ",checked_xmm_" << i << ");\n";
    out << "    goto " << label(end) << ";\n    }\n";
    return out.str();
}
}

LoweredFunction lower(const Function& function) {
    LoweredFunction result;
    if(function.entry==0x73900) {
        // The HUD repeatedly searches this 80-entry sprite pool. Lower the
        // verified loop directly, retaining its stack writes and register ABI.
        std::string bytes;
        for(const auto& [pc,instruction]:function.instructions) bytes+=hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length));
        if(bytes!="8B15B4936C00568B74240857B8F889720033C9803A0074080FBE780F3BFE740E4105D00000004283F9507CE733C05F5EC3")
            throw std::runtime_error("Original sprite search signature differs");
        result.source=R"(void guest_00073900(Cpu& cpu, Memory& memory) {
    if(smbd::checkpoint_due(cpu)) smbd::checkpoint(cpu,0x73900);
    const auto stack=cpu.registers[smbd::esp];
    const auto needle=memory.load<std::uint32_t>(stack+4);
    const auto status=memory.load<std::uint32_t>(0x6C93B4);
    memory.store<std::uint32_t>(stack-4,cpu.registers[smbd::esi]);
    memory.store<std::uint32_t>(stack-8,cpu.registers[smbd::edi]);
    const auto* active=static_cast<const std::byte*>(memory.access(status,80));
    unsigned index=0;
    for(;index<80;++index) if(active[index]!=std::byte{} &&
        smbd::sign_extend<8>(memory.load<std::uint8_t>(0x7289F8+index*0xD0+15))==needle) break;
    cpu.registers[smbd::eax]=index<80?0x7289F8+index*0xD0:0;
    cpu.registers[smbd::ecx]=index;cpu.registers[smbd::edx]=status+index;
    cpu.flags=index<80?smbd::sub_flags<32>(cpu.flags,needle,needle):smbd::logic_flags<32>(cpu.flags,0);
    cpu.eip=memory.load<std::uint32_t>(stack);cpu.registers[smbd::esp]=stack+4;
}
)";
        return result;
    }
    const auto api=std::ranges::find(native_apis,function.entry,&NativeApi::address);
    if(api!=native_apis.end()) {
        std::string prefix;
        auto pc=function.entry;
        while(prefix.size()<api->prefix.size() && function.instructions.contains(pc)) {
            const auto& instruction=function.instructions.at(pc);
            prefix+=hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length));
            pc+=instruction.decoded.length;
        }
        if(!prefix.starts_with(api->prefix)) throw std::runtime_error("Native API signature differs at "+hex32(function.entry));
        unsigned returns=0;
        for(const auto& [address,instruction]:function.instructions) if(instruction.decoded.mnemonic==ZYDIS_MNEMONIC_RET) {
            const auto cleanup=instruction.decoded.operand_count_visible?instruction.operands[0].imm.value.u:0;
            if(cleanup!=api->cleanup) throw std::runtime_error("Native API return ABI differs at "+hex32(address));
            ++returns;
        }
        if(!returns) throw std::runtime_error("Native API lacks a verified return ABI");
        result.native_api=api->name;
        result.source=std::format("void native_platform(Cpu&,Memory&,std::uint32_t);\nvoid guest_{:08X}(Cpu& cpu,Memory& memory) {{\n"
            "    if(smbd::checkpoint_due(cpu)) smbd::checkpoint(cpu,0x{:08X}U);\n"
            "    native_platform(cpu,memory,0x{:08X}U);\n}}\n",function.entry,function.entry,function.entry);
        return result;
    }
    Emitter emitter;
    if(function.instructions.contains(0x1F0AD6)) {
        constexpr std::pair<std::uint32_t,std::string_view> replay_fade[]={{0x1F0AD6,"E9E5A5FFFF"},
            {0x1EB0C5,"FF8010020000"},{0x1EB0FD,"2B8118020000"},{0x1EB108,"C3"},{0x1EB10E,"C3"}};
        for(const auto& [site,bytes]:replay_fade) {
            const auto at=function.instructions.find(site);
            if(at==function.instructions.end() || hex_bytes(Bytes(at->second.bytes).first(at->second.decoded.length))!=bytes)
                throw std::runtime_error("Original tennis replay fade signature differs at "+hex32(site));
        }
        emitter.tennis_replay_fade=true;
    }
    for(const auto& edge : function.edges)
        if(edge.to && (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump") && *edge.to<=edge.from)
            emitter.loop_headers.insert(*edge.to);
    emitter.calls=!emitter.loop_headers.empty();
    constexpr std::uint32_t checked_blocks[][3]={{0x21BE0,0x21C35,0x21D54},
        {0x21BE0,0x21D7A,0x21E94},{0x2F4A0,0x2F557,0x2F584}};
    for (const auto& [pc, instruction] : function.instructions) {
        try {
            const auto begin=emitter.body.view().size();
            std::string prologue;
            bool ordered_sse=false;
            for(const auto& [entry,start,end]:checked_blocks)
                if(function.entry==entry && pc>=start && pc<end) {
                    ordered_sse=true;
                    if(pc==start) prologue=checked_simd_block(function,emitter,start,end);
                }
            emitter.instruction(instruction,prologue,ordered_sse);
            const auto code=emitter.body.view().substr(begin);
            if(code.contains("frame_") || code.contains("native_stage_") || code.contains("native_menu_layout"))
                result.timing_sites.emplace_back(pc,code);
        }
        catch (const std::exception& error) {
            result.issues.push_back({pc, hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length)),
                                     instruction.text, error.what()});
        }
    }
    if (!result.issues.empty()) return result;
    std::ostringstream out;
    if(const auto layout=std::ranges::find(frame_presentations,function.entry,&FramePresentation::entry);
        layout!=frame_presentations.end() && !layout->prologue_end)
        emitter.native_declarations["menu_layout"]="void native_menu_layout(Cpu&,Memory&,std::uint32_t,bool);\n";
    for(const auto& [name,declaration] : emitter.native_declarations) out << declaration;
    if(function.entry==0x12F30 || function.entry==0x13180) out << "bool native_goal_query(Cpu&,Memory&,std::uint32_t);\n";
    if(function.entry==0x13C80) out << "void native_sample_goals(Cpu&,Memory&);\n";
    if(function.entry==0xA8000) out << "void frame_reset_world();\n";
    if(function.entry==0xA7820) out << "void frame_reset_world(std::uint32_t);\n";
    if(function.entry==0xB7350) out << "void frame_reset_camera();\n";
    if(function.entry==0xADFC0) out << "void frame_reset_camera(std::uint32_t);\n";
    if(function.entry==0xADE60) out << "void frame_reset_camera_shake(std::uint32_t);\n";
    if(function.entry==0x27C220) {
        if(hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(5))!="A014218900")
            throw std::runtime_error("Original replay recorder entry differs");
        out << "void frame_record_sample(FloatingState&,Memory&,std::array<std::uint32_t,24>&,bool);\n";
    }
    const bool pose_sampler=function.entry==0x98B50;
    const bool text_update=function.entry==0x6BB60,text_draw=function.entry==0x6B520;
    const bool camera_update=function.entry==0xB3BA0 || function.entry==0xB00D0;
    const bool camera_shake=function.entry==0xAE130;
    const char* ranking_camera=function.entry==0x264DF0?"frame_ranking_camera_step":function.entry==0xB74C0?"frame_ranking_camera_view":nullptr;
    if(text_update) out << "void frame_text_box_update(Cpu&,Memory&,void (*)(Cpu&,Memory&,bool,bool));\n";
    if(text_draw) out << "void frame_text_box_draw(Cpu&,Memory&,void (*)(Cpu&,Memory&));\n";
    if(camera_update) out << "void frame_camera_update(Cpu&,Memory&,void (*)(Cpu&,Memory&),bool);\n";
    if(camera_shake) out << "void frame_camera_shake_update(Cpu&,Memory&,void (*)(Cpu&,Memory&,bool,bool,std::uint32_t));\n";
    if(ranking_camera) out << "void " << ranking_camera << "(Cpu&,Memory&,void (*)(Cpu&,Memory&));\n";
    out << "void guest_" << std::format("{:08X}", function.entry);
    if(pose_sampler) out << "_sampled";
    if(text_update || text_draw || camera_update || camera_shake || ranking_camera) out << "_source";
    out << "(Cpu& cpu, [[maybe_unused]] Memory& memory";
    if(pose_sampler) out << ", const std::uint32_t* frame_sampled_pose";
    if(text_update) out << ", bool frame_text_style, bool frame_text_placement";
    if(camera_shake) out << ", bool frame_shake_step, bool frame_shake_draw, std::uint32_t frame_shake_sample";
    out << ") {\n";
    out << "    if(smbd::checkpoint_due(cpu)) smbd::checkpoint(cpu," << constant(function.entry) << ");\n";
    const auto entry_begin=out.view().size();
    if(function.entry==0xB7350) {
        if(hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(8))!="F30F1005347C3400")
            throw std::runtime_error("Original camera array initializer differs");
        out << "    frame_reset_camera();\n";
    }
    if(function.entry==0xADFC0) {
        if(hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(6))!="81EC8C020000")
            throw std::runtime_error("Original camera record initializer differs");
        out << "    frame_reset_camera(memory.load<std::uint32_t>(cpu.registers[smbd::esp]+4));\n";
    }
    if(function.entry==0xADE60) {
        if(hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(4))!="8B442404")
            throw std::runtime_error("Original camera shake reset differs");
        out << "    frame_reset_camera_shake(0x73F1A0U+memory.load<std::uint32_t>(cpu.registers[smbd::esp]+4)*0x28CU);\n";
    }
    if(function.entry==0x2A13B0) out << "    smbd::trace_ui_draw(cpu,memory);\n";
    if(function.entry==0x2A13B0 && hex_bytes(Bytes(function.instructions.at(0x2A13B7).bytes).first(6))!="81ECA4000000")
        throw std::runtime_error("Original menu local frame differs");
    if(function.entry==0x2A5D80 && hex_bytes(Bytes(function.instructions.at(0x2B7860).bytes).first(6))!="81ECF8020000")
        throw std::runtime_error("Original Gift local frame differs");
    if(function.entry==0x2A5D50 && hex_bytes(Bytes(function.instructions.at(0x2B71A5).bytes).first(3))!="83EC50")
        throw std::runtime_error("Original replay browser local frame differs");
    if(const auto layout=std::ranges::find(frame_presentations,function.entry,&FramePresentation::entry);layout!=frame_presentations.end()) {
        if(!layout->prologue_end) {
            const auto signature=function.entry==0x2A9970?"83EC20":function.entry==0x1D3890?"558BEC83E4F8":"83EC50";
            const auto length=std::string_view(signature).size()/2;
            // 1D3890's signature spans three short prologue instructions.
            std::string actual;for(const auto& [pc,instruction]:function.instructions) {
                if(pc<function.entry || actual.size()>=length*2) continue;
                actual+=hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length));
            }
            if(!actual.starts_with(signature)) throw std::runtime_error("Original presentation entry differs");
            out << "    if(smbd::frame_step.logic && smbd::frame_step.capture_presentation) native_menu_layout(cpu,memory," << constant(function.entry) << ",true);\n";
        }
        out << "    struct PresentationDeltaRestore { float previous=smbd::frame_step.delta; ~PresentationDeltaRestore() { smbd::frame_step.delta=previous; } } presentation_delta_restore;\n";
        if(!layout->prologue_end) out << "    smbd::frame_step.delta=smbd::frame_step.interval;\n";
    }
    if(function.entry==0x12F30 || function.entry==0x13180) {
        if(hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(6))!="81EC8C000000") throw std::runtime_error("Original goal query signature differs");
        out << "    if(native_goal_query(cpu,memory," << constant(function.entry) << ")) return;\n";
    }
    if(function.entry==0x13C80) out << "    native_sample_goals(cpu,memory);\n";
    // Legacy rule/controller services have no render submissions of their own.
    // Their registered drawing callbacks remain in the per-render-frame path.
    constexpr std::uint32_t legacy_updates[]={0x27C220,0x260CF0,0x13C80,0x11F60,0x23F8D0,0xF8F90,0x67F00,0x994B0,0xEA950,0x610E0,0x72350,
        0x4DB90,0x8B9D0,0x8B5B0,0x9ABE0,
        0x8BD80,0x115B80,0x127270,0x1A6350,0x1A5070,0x22CC50};
    if(std::ranges::find(legacy_updates,function.entry)!=std::end(legacy_updates)) {
        out << "    if(!smbd::frame_step.logic) { cpu.eip=memory.load<std::uint32_t>(cpu.registers[smbd::esp]);cpu.registers[smbd::esp]+=4;return; }\n";
        if(function.entry==0x27C220)
            out << "    struct RecordingSample { FloatingState& state; Memory& memory; std::array<std::uint32_t,24> saved{}; bool active;\n"
                   "        ~RecordingSample() { if(active) frame_record_sample(state,memory,saved,true); }\n"
                   "    } recording{cpu.floating,memory,{},smbd::frame_step.phase>0 && !(memory.load<std::uint8_t>(0x892114U)&0xA)};\n"
                   "    if(recording.active) frame_record_sample(cpu.floating,memory,recording.saved,false);\n";
        out << "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
               "    smbd::frame_step.delta=1;\n";
    }
    if(function.entry==0xA3530) {
        if(hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(4))!="8B442404") throw std::runtime_error("Original ape controller dispatcher differs");
        out << "    const auto ape=memory.load<std::uint32_t>(cpu.registers[smbd::esp]+4);\n"
               "    const bool continuous=memory.load<std::uint8_t>(ape+0x86)==3 && memory.load<std::uint8_t>(ape+0x87)==3;\n"
               "    if(!smbd::frame_step.logic && !continuous) { cpu.eip=memory.load<std::uint32_t>(cpu.registers[smbd::esp]);cpu.registers[smbd::esp]+=4;return; }\n"
               "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
               "    if(!continuous) smbd::frame_step.delta=1;\n";
    }
    if(function.entry==0xA8000) {
        if(hex_bytes(Bytes(function.instructions.at(0xA8000).bytes).first(8))!="F30F100558B63400") throw std::runtime_error("Original WORLD initializer differs");
        out << "    frame_reset_world();\n";
    }
    if(function.entry==0xA7820) {
        if(hex_bytes(Bytes(function.instructions.at(0xA7820).bytes).first(8))!="F30F100558B63400") throw std::runtime_error("Original individual WORLD initializer differs");
        out << "    frame_reset_world(cpu.registers[smbd::edx]);\n";
    }
    const auto entry_code=out.view().substr(entry_begin);
    if(entry_code.contains("frame_") || entry_code.contains("native_goal_") || entry_code.contains("native_sample_goals"))
        result.timing_entry=entry_code;
    for (const auto index : emitter.used)
        out << "    auto " << registers[index] << " = cpu.registers[" << index << "];\n";
    if (emitter.uses_flags) out << "    auto flags = cpu.flags;\n";
    if (emitter.force_scratch) out << "    std::array<std::uint32_t,3> frame_force{};\n";
    if(function.instructions.contains(0x8BCA7)) out << "    bool frame_size_expired=false;\n";
    if(function.instructions.contains(0xA7F94)) out << "    std::uint32_t frame_world_difference=0;\n";
    if(function.instructions.contains(0x1DD6D)) out << "    int frame_dialog_direction=0;\n";
    if(function.instructions.contains(0x99319)) out << "    std::uint32_t frame_pose_time=0;\n";
    if(function.instructions.contains(0x97D46)) out << "    bool frame_clip_advanced=false;\n"
        "    std::uint32_t frame_clip_bounds=0;\n    float frame_clip_before=0,frame_clip_increment=0;\n";
    if (emitter.calls) {
        // Outlining these reference captures pins every guest register to the
        // host stack and prevents promotion between calls and checkpoints.
        out << "    const auto commit = [&] [[gnu::always_inline]] {\n";
        for (const auto index : emitter.dirty) out << "        cpu.registers[" << index << "] = " << registers[index] << ";\n";
        if (emitter.writes_flags) out << "        cpu.flags = flags;\n";
        out << "    };\n";
        if (emitter.restores) {
            out << "    const auto restore = [&] [[gnu::always_inline]] {\n";
            for (const auto index : emitter.used) out << "        " << registers[index] << " = cpu.registers[" << index << "];\n";
            if (emitter.uses_flags) out << "        flags = cpu.flags;\n";
            out << "    };\n";
        }
    }
    out << "    goto " << label(function.entry) << ";\n";
    out << emitter.body.str();
    if(emitter.returns) {
        out << "done:\n";
        for (const auto index : emitter.dirty)
            out << "    cpu.registers[" << index << "] = " << registers[index] << ";\n";
        if (emitter.writes_flags) out << "    cpu.flags = flags;\n";
    }
    out << "}\n";
    if(pose_sampler) out << "void guest_00098B50(Cpu& cpu,Memory& memory) { guest_00098B50_sampled(cpu,memory,nullptr); }\n";
    if(text_update) {
        result.timing_entry="    frame_text_box_update(cpu,memory,guest_0006BB60_source);\n";
        out << "void guest_0006BB60(Cpu& cpu,Memory& memory) {\n" << result.timing_entry << "}\n";
    }
    if(text_draw) {
        result.timing_entry="    frame_text_box_draw(cpu,memory,guest_0006B520_source);\n";
        out << "void guest_0006B520(Cpu& cpu,Memory& memory) {\n" << result.timing_entry << "}\n";
    }
    if(camera_update) {
        const auto guest="guest_"+std::format("{:08X}",function.entry);
        if(function.entry==0xB00D0 && hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(5))!="A014218900")
            throw std::runtime_error("Original goal camera entry differs");
        result.timing_entry="    frame_camera_update(cpu,memory,"+guest+"_source,"+(function.entry==0xB00D0?"true":"false")+");\n";
        out << "void " << guest << "(Cpu& cpu,Memory& memory) {\n" << result.timing_entry << "}\n";
    }
    if(camera_shake) {
        result.timing_entry="    frame_camera_shake_update(cpu,memory,guest_000AE130_source);\n";
        out << "void guest_000AE130(Cpu& cpu,Memory& memory) {\n" << result.timing_entry << "}\n";
    }
    if(ranking_camera) {
        const auto& first=function.instructions.at(function.entry);
        if(hex_bytes(Bytes(first.bytes).first(first.decoded.length))!=(function.entry==0x264DF0?"6A03":"83EC24"))
            throw std::runtime_error("Original ranking camera entry differs");
        const auto guest="guest_"+std::format("{:08X}",function.entry);
        result.timing_entry=std::format("    {}(cpu,memory,{}_source);\n",ranking_camera,guest);
        out << "void " << guest << "(Cpu& cpu,Memory& memory) {\n" << result.timing_entry << "}\n";
    }
    result.source = out.str();
    result.floating=emitter.floating;
    result.native_helpers=std::move(emitter.native_helpers);
    return result;
}

std::string recompile(Xbe& image, std::uint32_t address, std::uint32_t instruction_budget) {
    if (!image.supported()) throw std::runtime_error("Native generation requires the verified target executable");
    const auto function = recover(image, address, instruction_budget);
    if (function.budget_exhausted || function.unresolved)
        throw std::runtime_error("Function discovery is incomplete; inspect the analysis report");
    const auto generated = lower(function);
    if (!generated.issues.empty()) {
        const auto& issue = generated.issues.front();
        throw std::runtime_error(hex32(issue.address) + " [" + issue.bytes + "]: " + issue.instruction + ": " + issue.reason);
    }
    if (std::ranges::any_of(function.edges, [](const Edge& edge) { return edge.kind == "call" || edge.kind=="tail-call"; }))
        throw std::runtime_error("This function calls other functions; use batch recompilation to recover its dependencies");
    if (generated.floating) throw std::runtime_error("Floating-point functions require batch generation of their native helpers");
    return "// Generated from XBE SHA256 " + image.sha256 + "\n#include \"cpu.h\"\nnamespace smbd {\n" + generated.source +
        "extern const CompiledFunction " + std::format("compiled_{:#x}", address) + '{' + constant(address) + ", \"" + image.sha256 +
        "\", guest_" + std::format("{:08X}", address) + "};\n}\n";
}
}
