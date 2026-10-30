#include "recompiler.h"
#include "native_apis.h"

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

class Emitter {
public:
    std::set<unsigned> used, dirty;
    bool uses_flags = false, writes_flags = false;
    bool calls = false;
    bool restores = false;
    bool returns = false;
    bool floating = false;
    bool force_scratch = false;
    std::set<std::uint32_t> loop_headers;
    std::map<std::string,std::string> native_helpers;
    std::map<std::string,std::string> native_declarations;
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
    std::string vector(const ZydisDecodedOperand& operand, bool scalar = false) {
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER)
            return "smbd::xmm(cpu.floating,"+std::to_string(xmm_register(operand.reg.value))+')';
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) throw std::runtime_error("Expected SIMD register or memory");
        if (scalar) return "smbd::scalar_vector(memory.load<std::uint32_t>("+address(operand)+"))";
        return "smbd::load_vector(memory,"+address(operand)+')';
    }
    void vector_write(const ZydisDecodedOperand& operand, const std::string& value, bool scalar = false) {
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER)
            body << "    smbd::set_xmm(cpu.floating," << xmm_register(operand.reg.value) << ',' << value << ");\n";
        else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY)
            body << "    smbd::store_vector(memory," << address(operand) << ',' << value << ',' << (scalar?"true":"false") << ");\n";
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
            native_declarations[helper] = "extern \"C\" std::uint32_t " + helper + "();\n";
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
        native_declarations[helper]="extern \"C\" void "+helper+'('+(!pointer.empty()?"void*":"")+");\n";
        body << "    " << helper << '(' << pointer << ");\n";
        if(register_write) write(*register_operand,"mmx_word");
    }
    void scalar_compare(const Instruction& instruction) {
        const auto& operands = instruction.operands;
        const auto left = "cpu.floating.xmm[" + std::to_string(xmm_register(operands[0].reg.value)) + "].data()";
        const auto right = operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER ?
            "cpu.floating.xmm[" + std::to_string(xmm_register(operands[1].reg.value)) + "].data()" :
            "memory.access(" + address(operands[1]) + ",4)";
        const std::string mnemonic = ZydisMnemonicGetString(instruction.decoded.mnemonic);
        const auto helper = "smbd_" + mnemonic;
        native_helpers[helper] = helper + " PROC\n    movss xmm0,dword ptr [rcx]\n    " +
            mnemonic + " xmm0,dword ptr [rdx]\n    pushfq\n    pop rax\n    ret\n" + helper + " ENDP\n";
        native_declarations[helper] = "extern \"C\" std::uint32_t " + helper + "(const void*,const void*);\n";
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
        native_declarations[helper] = "extern \"C\" void " + helper + "(const void*,const void*,void*);\n";
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
        native_declarations[helper]="extern \"C\" "+std::string(status||compare_flags?"std::uint32_t ":"void ")+helper+'('+(memory?"void*":"")+");\n";
        const bool control=waiting || mnemonic=="fnstsw" || mnemonic=="fnstcw" || mnemonic=="fldcw" ||
            mnemonic=="fnsave" || mnemonic=="frstor" || mnemonic=="fnstenv" || mnemonic=="fldenv" ||
            mnemonic=="fnclex" || mnemonic=="fninit" || mnemonic=="fnop";
        if (memory) {
            if (!memory->size || memory->size%8) throw std::runtime_error("Unsupported x87 memory width");
            body << "    const auto fp_address=" << address(*memory) << ";\n"
                 << "    auto* fp_memory=memory.access(fp_address," << memory->size/8 << ");\n";
            if(instruction.address==0xB3E19 || instruction.address==0xB3E21) {
                const auto expected=instruction.address==0xB3E19?"DD05D8244000":"DD05D0244000";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=expected) throw std::runtime_error("Original camera response signature differs");
                native_declarations["frame_gain_double"]="double frame_gain_double(FloatingState&,double);\n";
                body << "    const double response=frame_gain_double(cpu.floating,std::bit_cast<double>(memory.load<std::uint64_t>(fp_address)));\n"
                        "    fp_memory=const_cast<double*>(&response);\n";
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
    void instruction(const Instruction& instruction) {
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
        body << label(instruction.address) << ": { // " << instruction.text << '\n';
        if(loop_headers.contains(instruction.address))
            body << "    if(smbd::checkpoint_due(cpu)) { commit(); smbd::checkpoint(cpu," << constant(instruction.address) << "); }\n";
        if(instruction.address==0x14F00) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="6A04") throw std::runtime_error("Original frame entry signature differs");
            calls=restores=true;native_declarations["frame_begin"]="void native_frame_begin(Cpu&,Memory&);\n";
            body << "    commit(); native_frame_begin(cpu,memory); restore();\n";
        }
        constexpr std::pair<std::uint32_t,std::uint32_t> rule_calls[]={{0x14F30,0xEBD40},{0x14F4D,0x23F590},{0x14F52,0x23ECC0},
            {0x14F6A,0x23FB50},{0x14F82,0x16E30},{0x14F9A,0x15A90},{0x14FB2,0x1ADA0},{0x150B5,0x23EEC0},
            {0x79652,0x775E0}};
        for(const auto& [site,target]:rule_calls) if(instruction.address==site) {
            if(ins.mnemonic!=ZYDIS_MNEMONIC_CALL || ins.length!=5 || instruction.target()!=target)
                throw std::runtime_error("Original rule-tick call signature differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(next) << ";\n";
            legacy_scope();
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
        constexpr std::pair<std::uint32_t,std::string_view> tick_counters[]={{0x15110,"41"},{0x1511B,"FF05D0926C00"},
            {0x81243,"FF0580FF7200"},{0x8D8DD,"40"},{0x8D98D,"40"},{0x8D522,"40"},{0x901BE,"40"},{0x8C71F,"48"},{0x9D076,"48"},{0x9D087,"48"},{0xBAEA4,"41"},{0xB3C32,"40"},{0x13791D,"48"}};
        for(const auto& [site,bytes]:tick_counters) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original timed counter signature differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(next) << ";\n";
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
        if(instruction.address==0x811A8) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F2A0580FF7200") throw std::runtime_error("Original stage clock signature differs");
            calls=restores=floating=true;native_declarations["stage_time"]="void native_stage_time(Cpu&,Memory&);\n";
            body << "    commit(); native_stage_time(cpu,memory); restore();\n}\n";return;
        }
        if(instruction.address==0x750FA) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="A1B0936C00") throw std::runtime_error("Original sprite update boundary differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(0x75161) << ";\n";
        }
        // Update-only callbacks keep their original rule cadence. Drawing and
        // transform construction following these calls still run every frame.
        constexpr std::pair<std::uint32_t,std::string_view> update_callbacks[]={{0x75148,"FFD0"},{0x8795B,"FF148DC4575500"},
            {0x25860A,"FF148DC0486900"},{0x4DC3D,"FF1485F02F4B00"},{0x9D9E4,"E8475B0000"},{0x811C6,"FFD0"}};
        for(const auto& [site,bytes]:update_callbacks) if(instruction.address==site) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes) throw std::runtime_error("Original update callback signature differs at "+hex32(site));
            body << "    if(!smbd::frame_step.logic) goto " << label(next) << ";\n";legacy_scope();
        }
        if(instruction.address==0xB75A2 || instruction.address==0xB75B1) {
            const auto expected=instruction.address==0xB75A2?"FF1598A05600":"FF149590A05600";
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=expected)
                throw std::runtime_error("Original camera dispatch signature differs");
            body << "    const bool continuous_camera=(" << read(operands[0]) << ")==0xB3BA0U;\n"
                    "    if(!continuous_camera && !smbd::frame_step.logic) goto " << label(next) << ";\n"
                    "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
                    "    if(!continuous_camera) smbd::frame_step.delta=1;\n";
        }
        if(instruction.address==0x8C406) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="FF148570745500") throw std::runtime_error("Original ball-state dispatch differs");
            used.insert(0);
            body << "    const bool continuous_ball=eax==4 || eax==6 || eax==20 || eax==28 || eax==32;\n"
                    "    if(!continuous_ball && !smbd::frame_step.logic) goto " << label(next) << ";\n";
            body << "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
                    "    if(!continuous_ball) smbd::frame_step.delta=1;\n";
        }
        if(instruction.address==0xBAF14) {
            if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="FF2485E8565700") throw std::runtime_error("Original background update dispatch differs");
            body << "    if(!smbd::frame_step.logic) goto " << label(0xBAF1B) << ";\n";legacy_scope();
        }
        // Original 8A2C0: transformed acceleration -> velocity -> position.
        // Moving-stage transforms add the change in frame displacement to
        // velocity. Convert that change back to the game's 1/60-second units.
        constexpr std::pair<std::uint32_t,std::string_view> surface_velocity[]={{0x7B3C9,"F30F584500"},{0x7B3E3,"F30F584620"},{0x7B3FD,"F30F584624"},
            {0x7B49C,"F30F58461C"},{0x7B4BA,"F30F584620"},{0x7B4D4,"F30F584624"}};
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
            const auto storage="cpu.floating.xmm["+std::to_string(xmm_register(register_operand.reg.value))+"].data()+"+
                (ins.mnemonic==ZYDIS_MNEMONIC_MOVHPS?"2":"0");
            const auto mapped="memory.access("+address(memory_operand)+",8)";
            body << "    std::memcpy(" << (store?mapped:storage) << ',' << (store?storage:mapped) << ",8);\n";
            break;
        }
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVSS: {
            floating=true;
            const bool scalar=ins.mnemonic==ZYDIS_MNEMONIC_MOVSS;
            if (ins.mnemonic==ZYDIS_MNEMONIC_MOVAPS) for(unsigned i=0;i<2;++i)
                if(operands[i].type==ZYDIS_OPERAND_TYPE_MEMORY)
                    body << "    if(" << address(operands[i]) << "&15U) throw std::runtime_error(\"Unaligned guest MOVAPS\");\n";
            auto value=vector(operands[1],scalar);
            if(instruction.address==0x8B42D) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F114058")
                    throw std::runtime_error("Original contact friction signature differs");
                native_declarations["frame_gain"]="__m128 frame_gain(FloatingState&,__m128);\n";
                used.insert(4);
                value="((memory.load<std::uint32_t>(memory.load<std::uint32_t>(esp+4)+0x128)&1)?smbd::frame_gain(cpu.floating,"+value+"):"+value+')';
            }
            if(instruction.address==0x31563 || instruction.address==0x3157B) {
                const auto expected=instruction.address==0x31563?"F30F110528DF6D00":"F30F110D34DF6D00";
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=expected)
                    throw std::runtime_error("Original orthographic projection signature changed");
                // Mode 1 stores the orthographic X scale and offset. Preserve
                // the native 16:9 canvas while widening its projection to 21:9.
                // Perspective cameras use the game's aspect setting instead.
                body << "    if(cpu.ultrawide) { const auto status=_mm_getcsr();\n";
                vector_write(operands[0],"_mm_mul_ss("+value+",_mm_set_ss(16.0f/21))",true);
                body << "    _mm_setcsr(status); } else {\n";
                vector_write(operands[0],value,true);
                body << "    }\n";
                break;
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
            vector_write(operands[0],"_mm_cvtsi32_ss("+vector(operands[0])+",std::bit_cast<std::int32_t>("+read(operands[1])+"))");
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
            constexpr std::pair<std::uint32_t,std::string_view> time_adds[]={{0xBAEA7,"F30F58C1"},{0xBAECB,"F30F58D9"},{0xBAEF2,"F30F58D1"},
                {0x8FE43,"F30F58442414"},{0x8FE53,"F30F58442418"},{0x8FE63,"F30F5844241C"},
                {0x9CB35,"F30F5C0D847B3400"},{0x9CB3F,"F30F580D847B3400"},{0x9CBBC,"F30F5C0D847B3400"},{0x9CBC6,"F30F580D847B3400"},
                {0x9CDAD,"F30F5C0D847B3400"},{0x9CDB7,"F30F580D847B3400"},{0x9CE34,"F30F5C0D847B3400"},{0x9CE3E,"F30F580D847B3400"}};
            for(const auto& [site,bytes]:time_adds) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes || (ins.mnemonic!=ZYDIS_MNEMONIC_ADDSS && ins.mnemonic!=ZYDIS_MNEMONIC_SUBSS))
                    throw std::runtime_error("Original background clock signature differs");
                value=std::string(ins.mnemonic==ZYDIS_MNEMONIC_ADDSS?"_mm_add_ss(":"_mm_sub_ss(")+vector(operands[0])+",smbd::frame_increment("+vector(operands[1],true)+"))";
            }
            if(instruction.address==0x97D37) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F59542410") throw std::runtime_error("Original animation advance signature differs");
                value="smbd::frame_increment("+value+')';
            }
            if(instruction.address==0x9C58B) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F584048") throw std::runtime_error("Original animation end check signature differs");
                value="_mm_add_ss("+vector(operands[0])+",smbd::frame_increment("+vector(operands[1],true)+"))";
            }
            constexpr std::pair<std::uint32_t,std::string_view> responses[]={{0xA7C14,"F30F59C8"},{0xA7C2E,"F30F59C8"},{0xA7C4C,"F30F59C8"},
                {0xA7D1B,"F30F59D1"},{0xA7D2E,"F30F59D1"},{0xA9DD0,"F30F59C8"},{0xA9DEA,"F30F59C8"},{0xA9E08,"F30F59C8"},{0x8B58A,"F30F59C8"}};
            for(const auto& [site,bytes]:responses) if(instruction.address==site) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!=bytes || ins.mnemonic!=ZYDIS_MNEMONIC_MULSS)
                    throw std::runtime_error("Original motion response signature differs");
                native_declarations["frame_gain"]="__m128 frame_gain(FloatingState&,__m128);\n";
                value="_mm_mul_ss("+vector(operands[0])+",frame_gain(cpu.floating,"+vector(operands[1],true)+"))";
            }
            if(instruction.address==0xB3E3C) {
                if(hex_bytes(Bytes(instruction.bytes).first(ins.length))!="F30F5E05487F6C00") throw std::runtime_error("Original camera time interval signature differs");
                value="smbd::frame_increment("+value+')';
            }
            if (unary && scalar) value="_mm_move_ss("+vector(operands[0])+','+value+')';
            vector_write(operands[0],value); break;
        }
        case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX:
            body << "    const auto value = " << read(operands[1]) << ";\n";
            write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_MOVSX:
            body << "    const auto value = smbd::sign_extend<" << operands[1].size << ">(" << read(operands[1]) << ");\n";
            type(operands[1].size); write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_LEA:
            body << "    const auto value = " << address(operands[1], true) << ";\n";
            write(operands[0], "value"); break;
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP:
        case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB:
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST: {
            body << "    const std::uint32_t a = " << read(operands[0]) << ";\n"
                 << "    const std::uint32_t b = " << read(operands[1]) << ";\n";
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
            used.insert(4); dirty.insert(4);
            body << "    const auto value = " << read(operands[0]) << ";\n    esp -= " << width / 8
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
            const bool indirect=!instruction.target() && !instruction.kernel_ordinal;
            if(indirect) body << "    const auto target=" << read(operands[0]) << ";\n";
            used.insert(4); dirty.insert(4);
            body << "    esp -= 4U;\n    memory.store<std::uint32_t>(esp, " << constant(next)
                 << ");\n    commit();\n    cpu.eip = " << constant(instruction.target().value_or(instruction.address)) << ";\n    ";
            if (instruction.kernel_ordinal) body << "smbd::call_kernel<" << *instruction.kernel_ordinal << ">(cpu,memory);\n";
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
            write(operands[0], "result.value"); break;
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
        if (!terminator) body << "    goto " << label(next) << ";\n";
        body << "}\n";
    }
};
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
    cpu.flags=smbd::logic_flags<32>(cpu.flags,0);
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
    for(const auto& edge : function.edges)
        if(edge.to && (edge.kind=="jump" || edge.kind=="conditional" || edge.kind=="table-jump") && *edge.to<=edge.from)
            emitter.loop_headers.insert(*edge.to);
    emitter.calls=!emitter.loop_headers.empty();
    for (const auto& [pc, instruction] : function.instructions) {
        try { emitter.instruction(instruction); }
        catch (const std::exception& error) {
            result.issues.push_back({pc, hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length)),
                                     instruction.text, error.what()});
        }
    }
    if (!result.issues.empty()) return result;
    std::ostringstream out;
    for(const auto& [name,declaration] : emitter.native_declarations) out << declaration;
    if(function.entry==0x12F30 || function.entry==0x13180) out << "bool native_goal_query(Cpu&,Memory&,std::uint32_t);\n";
    if(function.entry==0x13C80) out << "void native_sample_goals(Cpu&,Memory&);\n";
    out << "void guest_" << std::format("{:08X}", function.entry) << "(Cpu& cpu, [[maybe_unused]] Memory& memory) {\n";
    out << "    if(smbd::checkpoint_due(cpu)) smbd::checkpoint(cpu," << constant(function.entry) << ");\n";
    if(function.entry==0x12F30 || function.entry==0x13180) {
        if(hex_bytes(Bytes(function.instructions.at(function.entry).bytes).first(6))!="81EC8C000000") throw std::runtime_error("Original goal query signature differs");
        out << "    if(native_goal_query(cpu,memory," << constant(function.entry) << ")) return;\n";
    }
    if(function.entry==0x13C80) out << "    native_sample_goals(cpu,memory);\n";
    // Legacy rule/controller services have no render submissions of their own.
    // Their registered drawing callbacks remain in the per-render-frame path.
    constexpr std::uint32_t legacy_updates[]={0x27C220,0x260CF0,0x13C80,0x11F60,0x23F8D0,0xF8F90,0x67F00,0x58690,0x994B0,0xEA950,0x610E0,0xA3530,
        0x8BD80,0x89450,0x10E350,0x115B80,0x127270,0x1A6350,0x1A5070,0x22CC50};
    if(std::ranges::find(legacy_updates,function.entry)!=std::end(legacy_updates))
        out << "    if(!smbd::frame_step.logic) { cpu.eip=memory.load<std::uint32_t>(cpu.registers[smbd::esp]);cpu.registers[smbd::esp]+=4;return; }\n"
               "    struct FrameDeltaRestore { float previous; ~FrameDeltaRestore() { smbd::frame_step.delta=previous; } } frame_delta_restore{smbd::frame_step.delta};\n"
               "    smbd::frame_step.delta=1;\n";
    for (const auto index : emitter.used)
        out << "    auto " << registers[index] << " = cpu.registers[" << index << "];\n";
    if (emitter.uses_flags) out << "    auto flags = cpu.flags;\n";
    if (emitter.force_scratch) out << "    std::array<std::uint32_t,3> frame_force{};\n";
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
