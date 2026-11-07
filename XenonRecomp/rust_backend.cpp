#include "pch.h"
#include "rust_backend.h"
#include <xex_patcher.h>

// The cases of Emit follow Recompiler::Recompile (recompiler.cpp) in order, each
// the same operation in Rust. Where they look different it is one of these:
//
// - Arithmetic wraps by name. C++ wraps by being unsigned or by -fwrapv-free luck
//   with signed registers; Rust's plain operators trap in a debug build.
// - A load or store goes through a helper of ppc_rt, which swaps the bytes.
//   An address is always a u32: `r1.u32.wrapping_add(0xFFFFFFF8)` for C++'s
//   `r1.u32 + -8`, which is what the unsigned arithmetic there comes to.
// - A vector register is read as .i or .ps, one 128-bit value, rather than loaded
//   and stored through a pointer to its bytes.
// - An instruction that branches says so through `flow` and ends the block.

bool RustRecompiler::NeedsCppFallback(const Function& fn) const
{
    const auto* data = static_cast<const uint32_t*>(image.Find(fn.base));
    bool callsSetjmp = false;

    for (size_t address = fn.base; address < fn.base + fn.size; address += 4)
    {
        if (config.midAsmHooks.find(uint32_t(address)) != config.midAsmHooks.end())
            return true;

        const uint32_t instruction = ByteSwap(data[(address - fn.base) / 4]);
        const size_t op = PPC_OP(instruction);
        size_t target = 0;
        if (op == PPC_OP_B)
            target = address + PPC_BI(instruction);
        else if (op == PPC_OP_BC)
            target = address + PPC_BD(instruction);

        if (target != 0 && target == config.setJmpAddress)
        {
            // Only a plain bl has somewhere to continue.
            if (op != PPC_OP_B || !PPC_BL(instruction))
                return true;
            callsSetjmp = true;
        }
    }

    if (callsSetjmp)
    {
        const bool anyLocals = config.ctrAsLocalVariable || config.xerAsLocalVariable || config.reservedRegisterAsLocalVariable ||
            config.crRegistersAsLocalVariables || config.nonArgumentRegistersAsLocalVariables || config.nonVolatileRegistersAsLocalVariables;
        return anyLocals && functionsKeepingRegistersInContext.count(fn.base) == 0;
    }

    return false;
}

bool RustRecompiler::Emit(
    const Function& fn,
    uint32_t base,
    const ppc_insn& insn,
    const uint32_t* data,
    std::unordered_map<uint32_t, RecompilerSwitchTable>::iterator& switchTable,
    RecompilerLocalVariables& localVariables,
    CSRState& csrState,
    std::set<std::string>& externs,
    PendingFlow& flow)
{
    println("\t// {} {}", insn.opcode->name, insn.op_str);

    auto r = [&](size_t index) -> std::string
        {
            if (!keepRegistersInContext &&
                ((config.nonArgumentRegistersAsLocalVariables && (index == 0 || index == 2 || index == 11 || index == 12)) ||
                (config.nonVolatileRegistersAsLocalVariables && index >= 14)))
            {
                localVariables.r[index] = true;
                return fmt::format("r{}", index);
            }
            return fmt::format("ctx.r{}", index);
        };

    auto f = [&](size_t index) -> std::string
        {
            if (!keepRegistersInContext &&
                ((config.nonArgumentRegistersAsLocalVariables && index == 0) ||
                (config.nonVolatileRegistersAsLocalVariables && index >= 14)))
            {
                localVariables.f[index] = true;
                return fmt::format("f{}", index);
            }
            return fmt::format("ctx.f{}", index);
        };

    auto v = [&](size_t index) -> std::string
        {
            if (!keepRegistersInContext &&
                ((config.nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
                (config.nonVolatileRegistersAsLocalVariables && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))))
            {
                localVariables.v[index] = true;
                return fmt::format("v{}", index);
            }
            return fmt::format("ctx.v{}", index);
        };

    auto cr = [&](size_t index) -> std::string
        {
            if (config.crRegistersAsLocalVariables && !keepRegistersInContext)
            {
                localVariables.cr[index] = true;
                return fmt::format("cr{}", index);
            }
            return fmt::format("ctx.cr{}", index);
        };

    auto ctr = [&]() -> std::string
        {
            if (config.ctrAsLocalVariable && !keepRegistersInContext)
            {
                localVariables.ctr = true;
                return "ctr";
            }
            return "ctx.ctr";
        };

    auto xer = [&]() -> std::string
        {
            if (config.xerAsLocalVariable && !keepRegistersInContext)
            {
                localVariables.xer = true;
                return "xer";
            }
            return "ctx.xer";
        };

    auto reserved = [&]() -> std::string
        {
            if (config.reservedRegisterAsLocalVariable && !keepRegistersInContext)
            {
                localVariables.reserved = true;
                return "reserved";
            }
            return "ctx.reserved";
        };

    auto temp = [&]() -> std::string
        {
            localVariables.temp = true;
            return "temp";
        };

    auto vTemp = [&]() -> std::string
        {
            localVariables.vTemp = true;
            return "vTemp";
        };

    auto ea = [&]() -> std::string
        {
            localVariables.ea = true;
            return "ea";
        };

    // The operands by position, the way nearly every case below wants them.
    auto R = [&](size_t i) { return r(insn.operands[i]); };
    auto F = [&](size_t i) { return f(insn.operands[i]); };
    auto V = [&](size_t i) { return v(insn.operands[i]); };

    // rA|0 + rB, the address of an indexed form.
    auto indexed = [&](size_t a, size_t b) -> std::string
        {
            if (insn.operands[a] != 0)
                return fmt::format("{}.u32.wrapping_add({}.u32)", r(insn.operands[a]), r(insn.operands[b]));
            return fmt::format("{}.u32", r(insn.operands[b]));
        };

    // rA|0 + d, the address of a D form: disp and rA are operands 1 and 2.
    auto displaced = [&]() -> std::string
        {
            const uint32_t disp = insn.operands[1];
            if (insn.operands[2] != 0)
                return fmt::format("{}.u32.wrapping_add(0x{:X})", r(insn.operands[2]), disp);
            return fmt::format("0x{:X}", disp);
        };

    // The update forms: ea = d + rA and ea = rA + rB.
    auto eaDisplaced = [&]() { println("\t{} = {}.u32.wrapping_add(0x{:X});", ea(), R(2), insn.operands[1]); };
    auto eaIndexed = [&]() { println("\t{} = {}.u32.wrapping_add({}.u32);", ea(), R(1), R(2)); };

    auto rc = [&]()
        {
            if (strchr(insn.opcode->name, '.'))
                println("\t{}.cmp({}.s32, 0, {}.so);", cr(0), R(0), xer());
        };

    auto mmioStore = [&]() -> bool
        {
            return *(data + 1) == c_eieio;
        };

    auto store = [&](const char* width) -> std::string
        {
            return mmioStore() ? fmt::format("ppc_mm_store_u{}", width) : fmt::format("st{}", width);
        };

    auto printFunctionCall = [&](uint32_t address)
        {
            if (address == config.longJmpAddress)
            {
                println("\tppc_longjmp(base.add({}.u32 as usize), {}.s32);", r(3), r(4));
                return;
            }

            if (address == config.setJmpAddress)
            {
                // The body stops here, and something calls it again to go on.
                setjmpResumes.push_back(base + 4);
                println("\tppc_setjmp_site(ctx, __frame, {});", setjmpResumes.size());
                flow.kind = Flow::Return;
                return;
            }

            auto targetSymbol = image.symbols.find(address);

            if (targetSymbol != image.symbols.end() && targetSymbol->address == address && targetSymbol->type == Symbol_Function)
            {
                if (config.nonVolatileRegistersAsLocalVariables && !keepRegistersInContext && (targetSymbol->name.find("__rest") == 0 || targetSymbol->name.find("__save") == 0))
                {
                    // print nothing
                }
                else
                {
                    externs.insert(targetSymbol->name);
                    println("\t{}(ctx, base);", targetSymbol->name);
                }
            }
            else
            {
                println("\t// ERROR {:X}", address);
            }
        };

    auto indirectCall = [&]()
        {
            println("\tppc_call_indirect(ctx, base, {}.u32);", ctr());
        };

    auto inside = [&](uint32_t address)
        {
            return address >= fn.base && address < fn.base + fn.size;
        };

    auto jump = [&](uint32_t target)
        {
            if (inside(target))
            {
                flow.kind = Flow::Jump;
                flow.target = target;
            }
            else
            {
                printFunctionCall(target);
                flow.kind = Flow::Return;
            }
        };

    auto branchIf = [&](const std::string& cond, uint32_t target)
        {
            if (inside(target))
            {
                flow.kind = Flow::Cond;
                flow.cond = cond;
                flow.target = target;
            }
            else
            {
                println("\tif {} {{", cond);
                print("\t");
                printFunctionCall(target);
                println("\t\treturn;");
                println("\t}}");
            }
        };

    auto printConditionalBranch = [&](bool not_, const std::string_view& field)
        {
            branchIf(fmt::format("{}.{} {} 0", cr(insn.operands[0]), field, not_ ? "==" : "!="), insn.operands[1]);
        };

    auto returnIf = [&](const std::string& cond)
        {
            println("\tif {} {{ return; }}", cond);
        };

    auto printSetFlushMode = [&](bool enable)
        {
            auto newState = enable ? CSRState::VMX : CSRState::FPU;
            if (csrState != newState)
            {
                auto prefix = enable ? "enable" : "disable";
                auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
                println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

                csrState = newState;
            }
        };

    int id = insn.opcode->id;

    // Handling instructions that don't disassemble correctly for some reason here
    if (id == PPC_INST_VUPKHSB128 && insn.operands[2] == 0x60) id = PPC_INST_VUPKHSH128;
    else if (id == PPC_INST_VUPKLSB128 && insn.operands[2] == 0x60) id = PPC_INST_VUPKLSH128;

    switch (id)
    {
    case PPC_INST_ADD:
        println("\t{}.u64 = {}.u64.wrapping_add({}.u64);", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_ADDE:
        println("\t{}.u8 = carry3({}.u32, {}.u32, {}.ca);", temp(), R(1), R(2), xer());
        println("\t{}.u64 = {}.u64.wrapping_add({}.u64).wrapping_add({}.ca as u64);", R(0), R(1), R(2), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        rc();
        break;

    case PPC_INST_ADDI:
        if (insn.operands[1] != 0)
            println("\t{}.s64 = {}.s64.wrapping_add({});", R(0), R(1), int32_t(insn.operands[2]));
        else
            println("\t{}.s64 = {};", R(0), int32_t(insn.operands[2]));
        break;

    case PPC_INST_ADDIC:
        println("\t{}.ca = ({}.u32 > {}) as u8;", xer(), R(1), ~insn.operands[2]);
        println("\t{}.s64 = {}.s64.wrapping_add({});", R(0), R(1), int32_t(insn.operands[2]));
        rc();
        break;

    case PPC_INST_ADDIS:
        if (insn.operands[1] != 0)
            println("\t{}.s64 = {}.s64.wrapping_add({});", R(0), R(1), static_cast<int32_t>(insn.operands[2] << 16));
        else
            println("\t{}.s64 = {};", R(0), static_cast<int32_t>(insn.operands[2] << 16));
        break;

    case PPC_INST_ADDZE:
        println("\t{}.s64 = {}.s64.wrapping_add({}.ca as i64);", temp(), R(1), xer());
        println("\t{}.ca = ({}.u32 < {}.u32) as u8;", xer(), temp(), R(1));
        println("\t{}.s64 = {}.s64;", R(0), temp());
        rc();
        break;

    case PPC_INST_AND:
        println("\t{}.u64 = {}.u64 & {}.u64;", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_ANDC:
        println("\t{}.u64 = {}.u64 & !{}.u64;", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_ANDI:
        println("\t{}.u64 = {}.u64 & {};", R(0), R(1), insn.operands[2]);
        println("\t{}.cmp({}.s32, 0, {}.so);", cr(0), R(0), xer());
        break;

    case PPC_INST_ANDIS:
        println("\t{}.u64 = {}.u64 & {};", R(0), R(1), insn.operands[2] << 16);
        println("\t{}.cmp({}.s32, 0, {}.so);", cr(0), R(0), xer());
        break;

    case PPC_INST_ATTN:
        // undefined instruction
        break;

    case PPC_INST_B:
        jump(insn.operands[0]);
        break;

    case PPC_INST_BCTR:
        if (switchTable != config.switchTables.end())
        {
            flow.kind = Flow::Switch;
            flow.cond = fmt::format("{}.u32", r(switchTable->second.r));
            flow.targets.clear();

            for (size_t i = 0; i < switchTable->second.labels.size(); i++)
            {
                auto label = switchTable->second.labels[i];
                if (!inside(label))
                {
                    fmt::println("ERROR: Switch case at {:X} is trying to jump outside function: {:X}", base, label);
                    flow.targets.push_back(0);
                }
                else
                {
                    flow.targets.push_back(label);
                }
            }

            switchTable = config.switchTables.end();
        }
        else
        {
            indirectCall();
            flow.kind = Flow::Return;
        }
        break;

    case PPC_INST_BCTRL:
        if (!config.skipLr)
            println("\tctx.lr = 0x{:X};", base + 4);
        indirectCall();
        csrState = CSRState::Unknown; // the call could change it
        break;

    case PPC_INST_BDZ:
        println("\t{0}.u64 = {0}.u64.wrapping_sub(1);", ctr());
        branchIf(fmt::format("{}.u32 == 0", ctr()), insn.operands[0]);
        break;

    case PPC_INST_BDZLR:
        println("\t{0}.u64 = {0}.u64.wrapping_sub(1);", ctr());
        returnIf(fmt::format("{}.u32 == 0", ctr()));
        break;

    case PPC_INST_BDNZ:
        println("\t{0}.u64 = {0}.u64.wrapping_sub(1);", ctr());
        branchIf(fmt::format("{}.u32 != 0", ctr()), insn.operands[0]);
        break;

    case PPC_INST_BDNZF:
        // NOTE: assuming eq here as a shortcut because all the instructions in the game do that
        println("\t{0}.u64 = {0}.u64.wrapping_sub(1);", ctr());
        branchIf(fmt::format("{}.u32 != 0 && {}.eq == 0", ctr(), cr(insn.operands[0] / 4)), insn.operands[1]);
        break;

    case PPC_INST_BEQ:
        printConditionalBranch(false, "eq");
        break;

    case PPC_INST_BEQLR:
        returnIf(fmt::format("{}.eq != 0", cr(insn.operands[0])));
        break;

    case PPC_INST_BGE:
        printConditionalBranch(true, "lt");
        break;

    case PPC_INST_BGELR:
        returnIf(fmt::format("{}.lt == 0", cr(insn.operands[0])));
        break;

    case PPC_INST_BGT:
        printConditionalBranch(false, "gt");
        break;

    case PPC_INST_BGTLR:
        returnIf(fmt::format("{}.gt != 0", cr(insn.operands[0])));
        break;

    case PPC_INST_BL:
        if (!config.skipLr)
            println("\tctx.lr = 0x{:X};", base + 4);
        printFunctionCall(insn.operands[0]);
        csrState = CSRState::Unknown; // the call could change it
        break;

    case PPC_INST_BLE:
        printConditionalBranch(true, "gt");
        break;

    case PPC_INST_BLELR:
        returnIf(fmt::format("{}.gt == 0", cr(insn.operands[0])));
        break;

    case PPC_INST_BLR:
        flow.kind = Flow::Return;
        break;

    case PPC_INST_BLRL:
        println("\tppc_debugtrap();");
        break;

    case PPC_INST_BLT:
        printConditionalBranch(false, "lt");
        break;

    case PPC_INST_BLTLR:
        returnIf(fmt::format("{}.lt != 0", cr(insn.operands[0])));
        break;

    case PPC_INST_BNE:
        printConditionalBranch(true, "eq");
        break;

    case PPC_INST_BNECTR:
        println("\tif {}.eq == 0 {{", cr(insn.operands[0]));
        print("\t");
        indirectCall();
        println("\t\treturn;");
        println("\t}}");
        break;

    case PPC_INST_BNELR:
        returnIf(fmt::format("{}.eq == 0", cr(insn.operands[0])));
        break;

    case PPC_INST_CCTPL:
    case PPC_INST_CCTPM:
        // no op
        break;

    case PPC_INST_CLRLDI:
        println("\t{}.u64 = {}.u64 & 0x{:X};", R(0), R(1), (1ull << (64 - insn.operands[2])) - 1);
        break;

    case PPC_INST_CLRLWI:
        println("\t{}.u64 = {}.u32 as u64 & 0x{:X};", R(0), R(1), (1ull << (32 - insn.operands[2])) - 1);
        rc();
        break;

    case PPC_INST_CMPD:
        println("\t{}.cmp({}.s64, {}.s64, {}.so);", cr(insn.operands[0]), R(1), R(2), xer());
        break;

    case PPC_INST_CMPDI:
        println("\t{}.cmp({}.s64, {}, {}.so);", cr(insn.operands[0]), R(1), int32_t(insn.operands[2]), xer());
        break;

    case PPC_INST_CMPLD:
        println("\t{}.cmp({}.u64, {}.u64, {}.so);", cr(insn.operands[0]), R(1), R(2), xer());
        break;

    case PPC_INST_CMPLDI:
        println("\t{}.cmp({}.u64, {}, {}.so);", cr(insn.operands[0]), R(1), insn.operands[2], xer());
        break;

    case PPC_INST_CMPLW:
        println("\t{}.cmp({}.u32, {}.u32, {}.so);", cr(insn.operands[0]), R(1), R(2), xer());
        break;

    case PPC_INST_CMPLWI:
        println("\t{}.cmp({}.u32, {}, {}.so);", cr(insn.operands[0]), R(1), insn.operands[2], xer());
        break;

    case PPC_INST_CMPW:
        println("\t{}.cmp({}.s32, {}.s32, {}.so);", cr(insn.operands[0]), R(1), R(2), xer());
        break;

    case PPC_INST_CMPWI:
        println("\t{}.cmp({}.s32, {}, {}.so);", cr(insn.operands[0]), R(1), int32_t(insn.operands[2]), xer());
        break;

    case PPC_INST_CNTLZD:
        println("\t{}.u64 = {}.u64.leading_zeros() as u64;", R(0), R(1));
        break;

    case PPC_INST_CNTLZW:
        println("\t{}.u64 = {}.u32.leading_zeros() as u64;", R(0), R(1));
        break;

    case PPC_INST_DB16CYC:
    case PPC_INST_DCBF:
    case PPC_INST_DCBT:
    case PPC_INST_DCBTST:
        // no op
        break;

    case PPC_INST_DCBZ:
        println("\tdcbz(base, {});", indexed(0, 1));
        break;

    case PPC_INST_DCBZL:
        println("\tdcbzl(base, {});", indexed(0, 1));
        break;

    case PPC_INST_DIVD:
        println("\t{}.s64 = ppc_divide_s64({}.s64, {}.s64);", R(0), R(1), R(2));
        break;

    case PPC_INST_DIVDU:
        println("\t{}.u64 = ppc_divide_u64({}.u64, {}.u64);", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_DIVW:
        println("\t{}.s32 = ppc_divide_s32({}.s32, {}.s32);", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_DIVWU:
        println("\t{}.u32 = ppc_divide_u32({}.u32, {}.u32);", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_EIEIO:
        println("\tppc_eieio();");
        break;

    case PPC_INST_EXTSB:
        println("\t{}.s64 = {}.s8 as i64;", R(0), R(1));
        rc();
        break;

    case PPC_INST_EXTSH:
        println("\t{}.s64 = {}.s16 as i64;", R(0), R(1));
        rc();
        break;

    case PPC_INST_EXTSW:
        println("\t{}.s64 = {}.s32 as i64;", R(0), R(1));
        rc();
        break;

    case PPC_INST_FABS:
        printSetFlushMode(false);
        println("\t{}.u64 = {}.u64 & !0x8000000000000000;", F(0), F(1));
        break;

    case PPC_INST_FADD:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 + {}.f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_FADDS:
        printSetFlushMode(false);
        println("\t{}.f64 = ({}.f64 + {}.f64) as f32 as f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_FCFID:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.s64 as f64;", F(0), F(1));
        break;

    case PPC_INST_FCMPU:
        printSetFlushMode(false);
        println("\t{}.cmpf({}.f64, {}.f64);", cr(insn.operands[0]), F(1), F(2));
        break;

    case PPC_INST_FCTID:
        printSetFlushMode(false);
        println("\t{}.s64 = fctid({}.f64);", F(0), F(1));
        break;

    case PPC_INST_FCTIDZ:
        printSetFlushMode(false);
        println("\t{}.s64 = fctidz({}.f64);", F(0), F(1));
        break;

    case PPC_INST_FCTIWZ:
        printSetFlushMode(false);
        println("\t{}.s64 = fctiwz({}.f64);", F(0), F(1));
        break;

    case PPC_INST_FDIV:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 / {}.f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_FDIVS:
        printSetFlushMode(false);
        println("\t{}.f64 = ({}.f64 / {}.f64) as f32 as f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_FMADD:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 * {}.f64 + {}.f64;", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FMADDS:
        printSetFlushMode(false);
        println("\t{}.f64 = ({}.f64 * {}.f64 + {}.f64) as f32 as f64;", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FMR:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64;", F(0), F(1));
        break;

    case PPC_INST_FMSUB:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 * {}.f64 - {}.f64;", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FMSUBS:
        printSetFlushMode(false);
        println("\t{}.f64 = ({}.f64 * {}.f64 - {}.f64) as f32 as f64;", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FMUL:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 * {}.f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_FMULS:
        printSetFlushMode(false);
        println("\t{}.f64 = ({}.f64 * {}.f64) as f32 as f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_FNABS:
        printSetFlushMode(false);
        println("\t{}.u64 = {}.u64 | 0x8000000000000000;", F(0), F(1));
        break;

    case PPC_INST_FNEG:
        printSetFlushMode(false);
        println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", F(0), F(1));
        break;

    case PPC_INST_FNMADDS:
        printSetFlushMode(false);
        println("\t{}.f64 = (-({}.f64 * {}.f64 + {}.f64)) as f32 as f64;", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FNMSUB:
        printSetFlushMode(false);
        println("\t{}.f64 = -({}.f64 * {}.f64 - {}.f64);", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FNMSUBS:
        printSetFlushMode(false);
        println("\t{}.f64 = (-({}.f64 * {}.f64 - {}.f64)) as f32 as f64;", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FRES:
        printSetFlushMode(false);
        println("\t{}.f64 = (1.0 / {}.f64) as f32 as f64;", F(0), F(1));
        break;

    case PPC_INST_FRSP:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 as f32 as f64;", F(0), F(1));
        break;

    case PPC_INST_FSEL:
        printSetFlushMode(false);
        println("\t{}.f64 = if {}.f64 >= 0.0 {{ {}.f64 }} else {{ {}.f64 }};", F(0), F(1), F(2), F(3));
        break;

    case PPC_INST_FSQRT:
        printSetFlushMode(false);
        println("\t{}.f64 = fsqrt({}.f64);", F(0), F(1));
        break;

    case PPC_INST_FSQRTS:
        printSetFlushMode(false);
        println("\t{}.f64 = fsqrt({}.f64) as f32 as f64;", F(0), F(1));
        break;

    case PPC_INST_FSUB:
        printSetFlushMode(false);
        println("\t{}.f64 = {}.f64 - {}.f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_FSUBS:
        printSetFlushMode(false);
        println("\t{}.f64 = ({}.f64 - {}.f64) as f32 as f64;", F(0), F(1), F(2));
        break;

    case PPC_INST_LBZ:
        println("\t{}.u64 = ld8(base, {}) as u64;", R(0), displaced());
        break;

    case PPC_INST_LBZU:
        eaDisplaced();
        println("\t{}.u64 = ld8(base, {}) as u64;", R(0), ea());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_LBZX:
        println("\t{}.u64 = ld8(base, {}) as u64;", R(0), indexed(1, 2));
        break;

    case PPC_INST_LD:
        println("\t{}.u64 = ld64(base, {});", R(0), displaced());
        break;

    case PPC_INST_LDARX:
        println("\t{}.u64 = ldarx(base, {});", reserved(), indexed(1, 2));
        println("\t{}.u64 = {}.u64.swap_bytes();", R(0), reserved());
        break;

    case PPC_INST_LDU:
        eaDisplaced();
        println("\t{}.u64 = ld64(base, {});", R(0), ea());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_LDX:
        println("\t{}.u64 = ld64(base, {});", R(0), indexed(1, 2));
        break;

    case PPC_INST_LFD:
        printSetFlushMode(false);
        println("\t{}.u64 = ld64(base, {});", F(0), displaced());
        break;

    case PPC_INST_LFDX:
        printSetFlushMode(false);
        println("\t{}.u64 = ld64(base, {});", F(0), indexed(1, 2));
        break;

    case PPC_INST_LFS:
        printSetFlushMode(false);
        println("\t{}.u32 = ld32(base, {});", temp(), displaced());
        println("\t{}.f64 = {}.f32 as f64;", F(0), temp());
        break;

    case PPC_INST_LFSX:
        printSetFlushMode(false);
        println("\t{}.u32 = ld32(base, {});", temp(), indexed(1, 2));
        println("\t{}.f64 = {}.f32 as f64;", F(0), temp());
        break;

    case PPC_INST_LHA:
        println("\t{}.s64 = ld16(base, {}) as i16 as i64;", R(0), displaced());
        break;

    case PPC_INST_LHAX:
        println("\t{}.s64 = ld16(base, {}) as i16 as i64;", R(0), indexed(1, 2));
        break;

    case PPC_INST_LHZ:
        println("\t{}.u64 = ld16(base, {}) as u64;", R(0), displaced());
        break;

    case PPC_INST_LHZX:
        println("\t{}.u64 = ld16(base, {}) as u64;", R(0), indexed(1, 2));
        break;

    case PPC_INST_LI:
        println("\t{}.s64 = {};", R(0), int32_t(insn.operands[1]));
        break;

    case PPC_INST_LIS:
        println("\t{}.s64 = {};", R(0), int32_t(insn.operands[1] << 16));
        break;

    case PPC_INST_LVEWX:
    case PPC_INST_LVEWX128:
    case PPC_INST_LVX:
    case PPC_INST_LVX128:
        // NOTE: for endian swapping, we reverse the whole vector instead of individual elements.
        // this is accounted for in every instruction (eg. dp3 sums yzw instead of xyz)
        println("\t{}.i = lvx(base, {});", V(0), indexed(1, 2));
        break;

    case PPC_INST_LVLX:
    case PPC_INST_LVLX128:
        println("\t{}.u32 = {};", temp(), indexed(1, 2));
        println("\t{}.i = lvlx(base, {}.u32);", V(0), temp());
        break;

    case PPC_INST_LVRX:
    case PPC_INST_LVRX128:
        println("\t{}.u32 = {};", temp(), indexed(1, 2));
        println("\t{}.i = lvrx(base, {}.u32);", V(0), temp());
        break;

    case PPC_INST_LVSL:
        println("\t{}.u32 = {};", temp(), indexed(1, 2));
        println("\t{}.i = lvsl({}.u32);", V(0), temp());
        break;

    case PPC_INST_LVSR:
        println("\t{}.u32 = {};", temp(), indexed(1, 2));
        println("\t{}.i = lvsr({}.u32);", V(0), temp());
        break;

    case PPC_INST_LWA:
        println("\t{}.s64 = ld32(base, {}) as i32 as i64;", R(0), displaced());
        break;

    case PPC_INST_LWARX:
        println("\t{}.u32 = lwarx(base, {});", reserved(), indexed(1, 2));
        println("\t{}.u64 = {}.u32.swap_bytes() as u64;", R(0), reserved());
        break;

    case PPC_INST_LWAX:
        println("\t{}.s64 = ld32(base, {}) as i32 as i64;", R(0), indexed(1, 2));
        break;

    case PPC_INST_LWBRX:
        println("\t{}.u64 = ld32(base, {}).swap_bytes() as u64;", R(0), indexed(1, 2));
        break;

    case PPC_INST_LWSYNC:
        println("\tppc_lwsync();");
        break;

    case PPC_INST_LWZ:
        println("\t{}.u64 = ld32(base, {}) as u64;", R(0), displaced());
        break;

    case PPC_INST_LWZU:
        eaDisplaced();
        println("\t{}.u64 = ld32(base, {}) as u64;", R(0), ea());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_LWZX:
        println("\t{}.u64 = ld32(base, {}) as u64;", R(0), indexed(1, 2));
        break;

    case PPC_INST_MFCR:
        for (size_t i = 0; i < 32; i++)
        {
            constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
            println("\t{}.u64 {}= crbit({}.{}, 0x{:X});", R(0), i == 0 ? "" : "|", cr(i / 4), fields[i % 4], 1u << (31 - i));
        }
        break;

    case PPC_INST_MFFS:
        println("\t{}.u64 = ctx.fpscr.loadFromHost() as u64;", F(0));
        break;

    case PPC_INST_MFLR:
        if (!config.skipLr)
            println("\t{}.u64 = ctx.lr;", R(0));
        break;

    case PPC_INST_MFMSR:
        if (!config.skipMsr)
            println("\t{}.u64 = ctx.msr as u64;", R(0));
        break;

    case PPC_INST_MFOCRF:
        // TODO: don't hardcode to cr6
        println("\t{0}.u64 = (({1}.lt as u64) << 7) | (({1}.gt as u64) << 6) | (({1}.eq as u64) << 5) | (({1}.so as u64) << 4);", R(0), cr(6));
        break;

    case PPC_INST_MFTB:
        println("\t{}.u64 = PPC_QueryTimebase();", R(0));
        break;

    case PPC_INST_MR:
        println("\t{}.u64 = {}.u64;", R(0), R(1));
        rc();
        break;

    case PPC_INST_MTCR:
        for (size_t i = 0; i < 32; i++)
        {
            constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
            println("\t{}.{} = (({}.u32 & 0x{:X}) != 0) as u8;", cr(i / 4), fields[i % 4], R(0), 1u << (31 - i));
        }
        break;

    case PPC_INST_MTCTR:
        println("\t{}.u64 = {}.u64;", ctr(), R(0));
        break;

    case PPC_INST_MTFSF:
        println("\tctx.fpscr.storeFromGuest({}.u32);", F(1));
        break;

    case PPC_INST_MTLR:
        if (!config.skipLr)
            println("\tctx.lr = {}.u64;", R(0));
        break;

    case PPC_INST_MTMSRD:
        if (!config.skipMsr)
            println("\tctx.msr = ({}.u32 & 0x8020) | (ctx.msr & !0x8020);", R(0));
        break;

    case PPC_INST_MTXER:
        println("\t{}.so = (({}.u64 & 0x80000000) != 0) as u8;", xer(), R(0));
        println("\t{}.ov = (({}.u64 & 0x40000000) != 0) as u8;", xer(), R(0));
        println("\t{}.ca = (({}.u64 & 0x20000000) != 0) as u8;", xer(), R(0));
        break;

    case PPC_INST_MULHW:
        println("\t{}.s64 = (({}.s32 as i64) * ({}.s32 as i64)) >> 32;", R(0), R(1), R(2));
        break;

    case PPC_INST_MULHWU:
        println("\t{}.u64 = (({}.u32 as u64) * ({}.u32 as u64)) >> 32;", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_MULLD:
        println("\t{}.s64 = {}.s64.wrapping_mul({}.s64);", R(0), R(1), R(2));
        break;

    case PPC_INST_MULLI:
        println("\t{}.s64 = {}.s64.wrapping_mul({});", R(0), R(1), int32_t(insn.operands[2]));
        break;

    case PPC_INST_MULLW:
        println("\t{}.s64 = ({}.s32 as i64) * ({}.s32 as i64);", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_NAND:
        println("\t{}.u64 = !({}.u64 & {}.u64);", R(0), R(1), R(2));
        break;

    case PPC_INST_NEG:
        println("\t{}.s64 = {}.s64.wrapping_neg();", R(0), R(1));
        rc();
        break;

    case PPC_INST_NOP:
        // no op
        break;

    case PPC_INST_NOR:
        println("\t{}.u64 = !({}.u64 | {}.u64);", R(0), R(1), R(2));
        break;

    case PPC_INST_NOT:
        println("\t{}.u64 = !{}.u64;", R(0), R(1));
        rc();
        break;

    case PPC_INST_OR:
        println("\t{}.u64 = {}.u64 | {}.u64;", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_ORC:
        println("\t{}.u64 = {}.u64 | !{}.u64;", R(0), R(1), R(2));
        break;

    case PPC_INST_ORI:
        println("\t{}.u64 = {}.u64 | {};", R(0), R(1), insn.operands[2]);
        break;

    case PPC_INST_ORIS:
        println("\t{}.u64 = {}.u64 | {};", R(0), R(1), insn.operands[2] << 16);
        break;

    case PPC_INST_RLDICL:
        println("\t{}.u64 = {}.u64.rotate_left({}) & 0x{:X};", R(0), R(1), insn.operands[2], ComputeMask(insn.operands[3], 63));
        break;

    case PPC_INST_RLDICR:
        println("\t{}.u64 = {}.u64.rotate_left({}) & 0x{:X};", R(0), R(1), insn.operands[2], ComputeMask(0, insn.operands[3]));
        break;

    case PPC_INST_RLDIMI:
    {
        const uint64_t mask = ComputeMask(insn.operands[3], ~insn.operands[2]);
        println("\t{0}.u64 = ({1}.u64.rotate_left({2}) & 0x{3:X}) | ({0}.u64 & 0x{4:X});", R(0), R(1), insn.operands[2], mask, ~mask);
        break;
    }

    case PPC_INST_RLWIMI:
    {
        const uint64_t mask = ComputeMask(insn.operands[3] + 32, insn.operands[4] + 32);
        println("\t{0}.u64 = (({1}.u32.rotate_left({2}) as u64) & 0x{3:X}) | ({0}.u64 & 0x{4:X});", R(0), R(1), insn.operands[2], mask, ~mask);
        break;
    }

    case PPC_INST_RLWINM:
        println("\t{0}.u64 = (({1}.u32 as u64) | ({1}.u64 << 32)).rotate_left({2}) & 0x{3:X};", R(0), R(1), insn.operands[2], ComputeMask(insn.operands[3] + 32, insn.operands[4] + 32));
        rc();
        break;

    case PPC_INST_ROTLDI:
        println("\t{}.u64 = {}.u64.rotate_left({});", R(0), R(1), insn.operands[2]);
        break;

    case PPC_INST_ROTLW:
        println("\t{}.u64 = {}.u32.rotate_left(({}.u8 & 0x1F) as u32) as u64;", R(0), R(1), R(2));
        break;

    case PPC_INST_ROTLWI:
        println("\t{}.u64 = {}.u32.rotate_left({}) as u64;", R(0), R(1), insn.operands[2]);
        rc();
        break;

    case PPC_INST_SLD:
        println("\t{0}.u64 = if {2}.u8 & 0x40 != 0 {{ 0 }} else {{ {1}.u64 << ({2}.u8 & 0x3F) }};", R(0), R(1), R(2));
        break;

    case PPC_INST_SLW:
        println("\t{0}.u64 = if {2}.u8 & 0x20 != 0 {{ 0 }} else {{ ({1}.u32 << ({2}.u8 & 0x1F)) as u64 }};", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_SRAD:
        println("\t{}.u64 = {}.u64 & 0x7F;", temp(), R(2));
        println("\tif {0}.u64 > 0x3F {{ {0}.u64 = 0x3F; }}", temp());
        println("\t{0}.ca = (({1}.s64 < 0) & ((({1}.s64 >> {2}.u64) << {2}.u64) != {1}.s64)) as u8;", xer(), R(1), temp());
        println("\t{}.s64 = {}.s64 >> {}.u64;", R(0), R(1), temp());
        break;

    case PPC_INST_SRADI:
        if (insn.operands[2] != 0)
        {
            println("\t{0}.ca = (({1}.s64 < 0) & (({1}.u64 & 0x{2:X}) != 0)) as u8;", xer(), R(1), ComputeMask(64 - insn.operands[2], 63));
            println("\t{}.s64 = {}.s64 >> {};", R(0), R(1), insn.operands[2]);
        }
        else
        {
            println("\t{}.ca = 0;", xer());
            println("\t{}.s64 = {}.s64;", R(0), R(1));
        }
        break;

    case PPC_INST_SRAW:
        println("\t{}.u32 = {}.u32 & 0x3F;", temp(), R(2));
        println("\tif {0}.u32 > 0x1F {{ {0}.u32 = 0x1F; }}", temp());
        println("\t{0}.ca = (({1}.s32 < 0) & ((({1}.s32 >> {2}.u32) << {2}.u32) != {1}.s32)) as u8;", xer(), R(1), temp());
        println("\t{}.s64 = ({}.s32 >> {}.u32) as i64;", R(0), R(1), temp());
        rc();
        break;

    case PPC_INST_SRAWI:
        if (insn.operands[2] != 0)
        {
            println("\t{0}.ca = (({1}.s32 < 0) & ((({1}.u32 as u64) & 0x{2:X}) != 0)) as u8;", xer(), R(1), ComputeMask(64 - insn.operands[2], 63));
            println("\t{}.s64 = ({}.s32 >> {}) as i64;", R(0), R(1), insn.operands[2]);
        }
        else
        {
            println("\t{}.ca = 0;", xer());
            println("\t{}.s64 = {}.s32 as i64;", R(0), R(1));
        }
        rc();
        break;

    case PPC_INST_SRD:
        println("\t{0}.u64 = if {2}.u8 & 0x40 != 0 {{ 0 }} else {{ {1}.u64 >> ({2}.u8 & 0x3F) }};", R(0), R(1), R(2));
        break;

    case PPC_INST_SRW:
        println("\t{0}.u64 = if {2}.u8 & 0x20 != 0 {{ 0 }} else {{ ({1}.u32 >> ({2}.u8 & 0x1F)) as u64 }};", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_STB:
        println("\t{}(base, {}, {}.u8);", store("8"), displaced(), R(0));
        break;

    case PPC_INST_STBU:
        eaDisplaced();
        println("\tst8(base, {}, {}.u8);", ea(), R(0));
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_STBUX:
        eaIndexed();
        println("\tst8(base, {}, {}.u8);", ea(), R(0));
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_STBX:
        println("\t{}(base, {}, {}.u8);", store("8"), indexed(1, 2), R(0));
        break;

    case PPC_INST_STD:
        println("\t{}(base, {}, {}.u64);", store("64"), displaced(), R(0));
        break;

    case PPC_INST_STDCX:
        println("\t{}.lt = 0;", cr(0));
        println("\t{}.gt = 0;", cr(0));
        println("\t{}.eq = stdcx(base, {}, {}.u64, {}.u64.swap_bytes()) as u8;", cr(0), indexed(1, 2), reserved(), R(0));
        println("\t{}.so = {}.so;", cr(0), xer());
        break;

    case PPC_INST_STDU:
        eaDisplaced();
        println("\tst64(base, {}, {}.u64);", ea(), R(0));
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_STDX:
        println("\t{}(base, {}, {}.u64);", store("64"), indexed(1, 2), R(0));
        break;

    case PPC_INST_STFD:
        printSetFlushMode(false);
        println("\t{}(base, {}, {}.u64);", store("64"), displaced(), F(0));
        break;

    case PPC_INST_STFDX:
        printSetFlushMode(false);
        println("\t{}(base, {}, {}.u64);", store("64"), indexed(1, 2), F(0));
        break;

    case PPC_INST_STFIWX:
        printSetFlushMode(false);
        println("\t{}(base, {}, {}.u32);", store("32"), indexed(1, 2), F(0));
        break;

    case PPC_INST_STFS:
        printSetFlushMode(false);
        println("\t{}.f32 = {}.f64 as f32;", temp(), F(0));
        println("\t{}(base, {}, {}.u32);", store("32"), displaced(), temp());
        break;

    case PPC_INST_STFSX:
        printSetFlushMode(false);
        println("\t{}.f32 = {}.f64 as f32;", temp(), F(0));
        println("\t{}(base, {}, {}.u32);", store("32"), indexed(1, 2), temp());
        break;

    case PPC_INST_STH:
        println("\t{}(base, {}, {}.u16);", store("16"), displaced(), R(0));
        break;

    case PPC_INST_STHBRX:
        println("\t{}(base, {}, {}.u16.swap_bytes());", store("16"), indexed(1, 2), R(0));
        break;

    case PPC_INST_STHX:
        println("\t{}(base, {}, {}.u16);", store("16"), indexed(1, 2), R(0));
        break;

    case PPC_INST_STVEHX:
        // NOTE: accounting for the full vector reversal here
        println("\t{} = {} & !0x1;", ea(), indexed(1, 2));
        println("\tstvehx(base, {}, &{});", ea(), V(0));
        break;

    case PPC_INST_STVEWX:
    case PPC_INST_STVEWX128:
        // NOTE: accounting for the full vector reversal here
        println("\t{} = {} & !0x3;", ea(), indexed(1, 2));
        println("\tstvewx(base, {}, &{});", ea(), V(0));
        break;

    case PPC_INST_STVLX:
    case PPC_INST_STVLX128:
        // NOTE: accounting for the full vector reversal here
        println("\t{} = {};", ea(), indexed(1, 2));
        println("\tstvlx(base, {}, &{});", ea(), V(0));
        break;

    case PPC_INST_STVRX:
    case PPC_INST_STVRX128:
        // NOTE: accounting for the full vector reversal here
        println("\t{} = {};", ea(), indexed(1, 2));
        println("\tstvrx(base, {}, &{});", ea(), V(0));
        break;

    case PPC_INST_STVX:
    case PPC_INST_STVX128:
        println("\tstvx(base, {}, {}.i);", indexed(1, 2), V(0));
        break;

    case PPC_INST_STW:
        println("\t{}(base, {}, {}.u32);", store("32"), displaced(), R(0));
        break;

    case PPC_INST_STWBRX:
        println("\t{}(base, {}, {}.u32.swap_bytes());", store("32"), indexed(1, 2), R(0));
        break;

    case PPC_INST_STWCX:
        println("\t{}.lt = 0;", cr(0));
        println("\t{}.gt = 0;", cr(0));
        println("\t{}.eq = stwcx(base, {}, {}.u32, {}.u32.swap_bytes()) as u8;", cr(0), indexed(1, 2), reserved(), R(0));
        println("\t{}.so = {}.so;", cr(0), xer());
        break;

    case PPC_INST_STWU:
        eaDisplaced();
        println("\tst32(base, {}, {}.u32);", ea(), R(0));
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_STWUX:
        eaIndexed();
        println("\tst32(base, {}, {}.u32);", ea(), R(0));
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_STWX:
        println("\t{}(base, {}, {}.u32);", store("32"), indexed(1, 2), R(0));
        break;

    case PPC_INST_SUBF:
        println("\t{}.s64 = {}.s64.wrapping_sub({}.s64);", R(0), R(2), R(1));
        rc();
        break;

    case PPC_INST_SUBFC:
        println("\t{}.ca = ({}.u32 >= {}.u32) as u8;", xer(), R(2), R(1));
        println("\t{}.s64 = {}.s64.wrapping_sub({}.s64);", R(0), R(2), R(1));
        rc();
        break;

    case PPC_INST_SUBFE:
        println("\t{}.u8 = carry3(!{}.u32, {}.u32, {}.ca);", temp(), R(1), R(2), xer());
        println("\t{}.u64 = (!{}.u64).wrapping_add({}.u64).wrapping_add({}.ca as u64);", R(0), R(1), R(2), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        rc();
        break;

    case PPC_INST_SUBFIC:
        println("\t{}.ca = ({}.u32 <= {}) as u8;", xer(), R(1), insn.operands[2]);
        println("\t{}.s64 = ({}i64).wrapping_sub({}.s64);", R(0), int32_t(insn.operands[2]), R(1));
        break;

    case PPC_INST_SYNC:
        println("\tppc_sync();");
        break;

    case PPC_INST_TDLGEI:
    case PPC_INST_TDLLEI:
    case PPC_INST_TWI:
    case PPC_INST_TWLGEI:
    case PPC_INST_TWLLEI:
        // no op
        break;

    case PPC_INST_VADDFP:
    case PPC_INST_VADDFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_add_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        break;

    case PPC_INST_VADDSHS:
        println("\t{}.i = _mm_adds_epi16({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VADDUBM:
        println("\t{}.i = _mm_add_epi8({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VADDUBS:
        println("\t{}.i = _mm_adds_epu8({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VADDUHM:
        println("\t{}.i = _mm_add_epi16({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VADDUWM:
        println("\t{}.i = _mm_add_epi32({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VADDUWS:
        println("\t{}.i = mm_adds_epu32({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VAND:
    case PPC_INST_VAND128:
        println("\t{}.i = _mm_and_si128({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VANDC128:
        println("\t{}.i = _mm_andnot_si128({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VAVGSB:
        println("\t{}.i = mm_avg_epi8({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VAVGSH:
        println("\t{}.i = mm_avg_epi16({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VAVGUB:
        println("\t{}.i = _mm_avg_epu8({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VCTSXS:
    case PPC_INST_VCFPSXWS128:
        printSetFlushMode(true);
        if (insn.operands[2] != 0)
            println("\t{}.i = mm_vctsxs(_mm_mul_ps({}.ps, _mm_set1_ps({}.0)));", V(0), V(1), 1u << insn.operands[2]);
        else
            println("\t{}.i = mm_vctsxs({}.ps);", V(0), V(1));
        break;

    case PPC_INST_VCFSX:
    case PPC_INST_VCSXWFP128:
    {
        printSetFlushMode(true);
        if (insn.operands[2] != 0)
        {
            const float value = ldexp(1.0f, -int32_t(insn.operands[2]));
            println("\t{}.ps = _mm_mul_ps(_mm_cvtepi32_ps({}.i), _mm_set1_ps(f32::from_bits(0x{:X})));", V(0), V(1), *reinterpret_cast<const uint32_t*>(&value));
        }
        else
        {
            println("\t{}.ps = _mm_cvtepi32_ps({}.i);", V(0), V(1));
        }
        break;
    }

    case PPC_INST_VCFUX:
    case PPC_INST_VCUXWFP128:
    {
        printSetFlushMode(true);
        if (insn.operands[2] != 0)
        {
            const float value = ldexp(1.0f, -int32_t(insn.operands[2]));
            println("\t{}.ps = _mm_mul_ps(mm_cvtepu32_ps({}.i), _mm_set1_ps(f32::from_bits(0x{:X})));", V(0), V(1), *reinterpret_cast<const uint32_t*>(&value));
        }
        else
        {
            println("\t{}.ps = mm_cvtepu32_ps({}.i);", V(0), V(1));
        }
        break;
    }

    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        println("\tppc_debugtrap();");
        break;

    case PPC_INST_VCMPEQFP:
    case PPC_INST_VCMPEQFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_cmpeq_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMaskPs({}.ps, 0xF);", cr(6), V(0));
        break;

    case PPC_INST_VCMPEQUB:
        println("\t{}.i = _mm_cmpeq_epi8({}.i, {}.i);", V(0), V(1), V(2));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMaskSi({}.i, 0xFFFF);", cr(6), V(0));
        break;

    case PPC_INST_VCMPEQUW:
    case PPC_INST_VCMPEQUW128:
        println("\t{}.i = _mm_cmpeq_epi32({}.i, {}.i);", V(0), V(1), V(2));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMaskPs({}.ps, 0xF);", cr(6), V(0));
        break;

    case PPC_INST_VCMPGEFP:
    case PPC_INST_VCMPGEFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_cmpge_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMaskPs({}.ps, 0xF);", cr(6), V(0));
        break;

    case PPC_INST_VCMPGTFP:
    case PPC_INST_VCMPGTFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_cmpgt_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMaskPs({}.ps, 0xF);", cr(6), V(0));
        break;

    case PPC_INST_VCMPGTUB:
        println("\t{}.i = mm_cmpgt_epu8({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VCMPGTUH:
        println("\t{}.i = mm_cmpgt_epu16({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VEXPTEFP:
    case PPC_INST_VEXPTEFP128:
        // TODO: vectorize
        printSetFlushMode(true);
        for (size_t i = 0; i < 4; i++)
            println("\t{}.f32[{}] = exp2f({}.f32[{}]);", V(0), i, V(1), i);
        break;

    case PPC_INST_VLOGEFP:
    case PPC_INST_VLOGEFP128:
        // TODO: vectorize
        printSetFlushMode(true);
        for (size_t i = 0; i < 4; i++)
            println("\t{}.f32[{}] = log2f({}.f32[{}]);", V(0), i, V(1), i);
        break;

    case PPC_INST_VMADDCFP128:
    case PPC_INST_VMADDFP:
    case PPC_INST_VMADDFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_add_ps(_mm_mul_ps({}.ps, {}.ps), {}.ps);", V(0), V(1), V(2), V(3));
        break;

    case PPC_INST_VMAXFP:
    case PPC_INST_VMAXFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_max_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        break;

    case PPC_INST_VMAXSW:
        println("\t{}.i = _mm_max_epi32({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VMINFP:
    case PPC_INST_VMINFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_min_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        break;

    case PPC_INST_VMRGHB:
        println("\t{}.i = _mm_unpackhi_epi8({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VMRGHH:
        println("\t{}.i = _mm_unpackhi_epi16({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VMRGHW:
    case PPC_INST_VMRGHW128:
        println("\t{}.i = _mm_unpackhi_epi32({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VMRGLB:
        println("\t{}.i = _mm_unpacklo_epi8({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VMRGLH:
        println("\t{}.i = _mm_unpacklo_epi16({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VMRGLW:
    case PPC_INST_VMRGLW128:
        println("\t{}.i = _mm_unpacklo_epi32({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VMSUM3FP128:
        // NOTE: accounting for full vector reversal here. should dot product yzw instead of xyz
        printSetFlushMode(true);
        println("\t{}.ps = _mm_dp_ps::<0xEF>({}.ps, {}.ps);", V(0), V(1), V(2));
        break;

    case PPC_INST_VMSUM4FP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_dp_ps::<0xFF>({}.ps, {}.ps);", V(0), V(1), V(2));
        break;

    case PPC_INST_VMULFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_mul_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        break;

    case PPC_INST_VNMSUBFP:
    case PPC_INST_VNMSUBFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_xor_ps(_mm_sub_ps(_mm_mul_ps({}.ps, {}.ps), {}.ps), _mm_set1_ps(f32::from_bits(0x80000000)));", V(0), V(1), V(2), V(3));
        break;

    case PPC_INST_VOR:
    case PPC_INST_VOR128:
        if (insn.operands[1] != insn.operands[2])
            println("\t{}.i = _mm_or_si128({}.i, {}.i);", V(0), V(1), V(2));
        else
            println("\t{}.i = {}.i;", V(0), V(1));
        break;

    case PPC_INST_VPERM:
    case PPC_INST_VPERM128:
        println("\t{}.i = mm_perm_epi8({}.i, {}.i, {}.i);", V(0), V(1), V(2), V(3));
        break;

    case PPC_INST_VPERMWI128:
    {
        // NOTE: accounting for full vector reversal here
        uint32_t x = 3 - (insn.operands[2] & 0x3);
        uint32_t y = 3 - ((insn.operands[2] >> 2) & 0x3);
        uint32_t z = 3 - ((insn.operands[2] >> 4) & 0x3);
        uint32_t w = 3 - ((insn.operands[2] >> 6) & 0x3);
        uint32_t perm = x | (y << 2) | (z << 4) | (w << 6);
        println("\t{}.i = _mm_shuffle_epi32::<0x{:X}>({}.i);", V(0), perm & 0xFF, V(1));
        break;
    }

    case PPC_INST_VPKD3D128:
    {
        // TODO: vectorize somehow?
        // NOTE: handling vector reversal here too
        printSetFlushMode(true);

        // float16_2 and float16_4 differ only in how many components they take
        // and where those land, so the conversion itself lives in one place.
        auto packHalf = [&](size_t dest, size_t source)
        {
            // C++ would write past the register. No such instruction is in use
            // ("Unexpected" above), and rustc rejects the index outright.
            if (dest >= 8)
            {
                println("\tppc_debugtrap();");
                return;
            }

            // Strip sign from source
            println("\t{}.u32 = {}.u32[{}] & 0x7FFFFFFF;", temp(), V(1), source);
            // If |source| is > 65504, clamp output to 0x7FFF, else save 8 exponent bits
            println("\t{0}.u8[0] = if {1}.f32 != {1}.f32 || {1}.f32 > 65504.0 {{ 0xFF }} else {{ (({2}.u32[{3}] & 0x7f800000) >> 23) as u8 }};", vTemp(), temp(), V(1), source);
            // If 8 exponent bits were saved, it can only be 0x8E at most
            // If saved, save first 10 bits of mantissa
            println("\t{}.u16 = if {}.u8[0] != 0xFF {{ (({}.u32[{}] & 0x7FE000) >> 13) as u16 }} else {{ 0 }};", temp(), vTemp(), V(1), source);
            // If saved and > 127-15, exponent is converted from 8 to 5-bit by subtracting 0x70
            // If saved but not > 127-15, clamp exponent at 0, add 0x400 to mantissa and shift right by (0x71-exponent)
            // If right shift is greater than 31 bits, manually clamp mantissa to 0 or else the output of the shift will be wrong
            println("\t{0}.u16[{1}] = if {2}.u8[0] != 0xFF {{ if {2}.u8[0] > 0x70 {{ ((({2}.u8[0] as i32 - 0x70) << 10) + {3}.u16 as i32) as u16 }} else if 0x71 - {2}.u8[0] as i32 > 31 {{ 0 }} else {{ ((0x400 + {3}.u16 as i32) >> (0x71 - {2}.u8[0] as i32)) as u16 }} }} else {{ 0x7FFF }};", V(0), dest, vTemp(), temp());
            // Add back original sign
            println("\t{}.u16[{}] |= (({}.u32[{}] & 0x80000000) >> 16) as u16;", V(0), dest, V(1), source);
        };

        switch (insn.operands[2])
        {
        case 0: // D3D color
            if (insn.operands[3] != 1)
                fmt::println("Unexpected D3D color pack instruction at {:X}", base);

            for (size_t i = 0; i < 4; i++)
            {
                constexpr size_t indices[] = { 3, 0, 1, 2 };
                println("\t{}.u32[{}] = 0x404000FF;", vTemp(), i);
                println("\t{0}.f32[{1}] = if {2}.f32[{1}] < 3.0 {{ 3.0 }} else if {2}.f32[{1}] > {0}.f32[{1}] {{ {0}.f32[{1}] }} else {{ {2}.f32[{1}] }};", vTemp(), i, V(1));
                println("\t{}.u32 {}= ({}.u8[{}] as u32) << {};", temp(), i == 0 ? "" : "|", vTemp(), i * 4, indices[i] * 8);
            }
            println("\t{}.u32[{}] = {}.u32;", V(0), insn.operands[4], temp());
            break;

        case 2: // three 10-bit signed normalised components, w in the top two bits
            // The inverse of the unpack below: the caller has already put each
            // component back in the mantissa of 3.0f, so the field is just the
            // low bits of the float. Clamped because one component overflowing
            // its field would corrupt the next one.
            {
                constexpr size_t lanes[]  = { 3, 2, 1, 0 };     // x, y, z, w
                constexpr size_t shifts[] = { 20, 10, 0, 30 };
                constexpr int    limits[] = { 511, 511, 511, 1 };
                constexpr uint32_t masks[]= { 0x3FF, 0x3FF, 0x3FF, 0x3 };
                for (size_t i = 0; i < 4; i++)
                {
                    println("\t{}.s32[{}] = ({}.u32[{}] as i32).wrapping_sub(0x40400000);", vTemp(), i, V(1), lanes[i]);
                    println("\t{0}.s32[{1}] = if {0}.s32[{1}] < {2} {{ {2} }} else if {0}.s32[{1}] > {3} {{ {3} }} else {{ {0}.s32[{1}] }};", vTemp(), i, -limits[i] - 1, limits[i]);
                    println("\t{}.u32 {}= (({}.s32[{}] as u32) & 0x{:X}) << {};", temp(), i == 0 ? "" : "|", vTemp(), i, masks[i], shifts[i]);
                }
                println("\t{}.u32[{}] = {}.u32;", V(0), insn.operands[4], temp());
            }
            break;

        case 3: // float16_2 -- x and y into the two halves of one word
            if (insn.operands[3] != 1)
                fmt::println("Unexpected float16_2 pack instruction at {:X}", base);

            for (size_t i = 2; i < 4; i++)
                packHalf((i - 2) + (2 * insn.operands[4]), i);
            break;

        case 5: // float16_4
            if (insn.operands[3] != 2 || insn.operands[4] > 2)
                fmt::println("Unexpected float16_4 pack instruction at {:X}", base);

            for (size_t i = 0; i < 4; i++)
                packHalf(i + (2 * insn.operands[4]), i);
            break;

        default:
            println("\tppc_debugtrap();");
            break;
        }
        break;
    }

    case PPC_INST_VPKSHUS:
    case PPC_INST_VPKSHUS128:
        println("\t{}.i = _mm_packus_epi16({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VREFP:
    case PPC_INST_VREFP128:
        // TODO: see if we can use rcp safely
        printSetFlushMode(true);
        println("\t{}.ps = _mm_div_ps(_mm_set1_ps(1.0), {}.ps);", V(0), V(1));
        break;

    case PPC_INST_VRFIM:
    case PPC_INST_VRFIM128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_round_ps::<{{ _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC }}>({}.ps);", V(0), V(1));
        break;

    case PPC_INST_VRFIN:
    case PPC_INST_VRFIN128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_round_ps::<{{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }}>({}.ps);", V(0), V(1));
        break;

    case PPC_INST_VRFIZ:
    case PPC_INST_VRFIZ128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_round_ps::<{{ _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC }}>({}.ps);", V(0), V(1));
        break;

    case PPC_INST_VRLIMI128:
    {
        constexpr size_t shuffles[] = { SIMDE_MM_SHUFFLE(3, 2, 1, 0), SIMDE_MM_SHUFFLE(2, 1, 0, 3), SIMDE_MM_SHUFFLE(1, 0, 3, 2), SIMDE_MM_SHUFFLE(0, 3, 2, 1) };
        println("\t{0}.ps = _mm_blend_ps::<{1}>({0}.ps, _mm_shuffle_ps::<{2}>({3}.ps, {3}.ps));", V(0), insn.operands[2] & 0xF, shuffles[insn.operands[3]], V(1));
        break;
    }

    case PPC_INST_VRSQRTEFP:
    case PPC_INST_VRSQRTEFP128:
        // TODO: see if we can use rsqrt safely
        // TODO: we can detect if the input is from a dot product and apply logic only on one value
        printSetFlushMode(true);
        println("\t{}.ps = _mm_div_ps(_mm_set1_ps(1.0), _mm_sqrt_ps({}.ps));", V(0), V(1));
        break;

    case PPC_INST_VSEL:
    case PPC_INST_VSEL128:
        println("\t{0}.i = _mm_or_si128(_mm_andnot_si128({1}.i, {2}.i), _mm_and_si128({1}.i, {3}.i));", V(0), V(3), V(1), V(2));
        break;

    case PPC_INST_VSLB:
        // TODO: vectorize
        for (size_t i = 0; i < 16; i++)
            println("\t{}.u8[{}] = {}.u8[{}] << ({}.u8[{}] & 0x7);", V(0), i, V(1), i, V(2), i);
        break;

    case PPC_INST_VSLDOI:
    case PPC_INST_VSLDOI128:
        println("\t{}.i = _mm_alignr_epi8::<{}>({}.i, {}.i);", V(0), 16 - insn.operands[3], V(1), V(2));
        break;

    case PPC_INST_VSLW:
    case PPC_INST_VSLW128:
        // TODO: vectorize, ensure endianness is correct
        for (size_t i = 0; i < 4; i++)
            println("\t{}.u32[{}] = {}.u32[{}] << ({}.u8[{}] & 0x1F);", V(0), i, V(1), i, V(2), i * 4);
        break;

    case PPC_INST_VSPLTB:
    {
        // NOTE: accounting for full vector reversal here
        uint32_t perm = 15 - insn.operands[2];
        println("\t{}.i = _mm_shuffle_epi8({}.i, _mm_set1_epi8({}));", V(0), V(1), int(int8_t(perm)));
        break;
    }

    case PPC_INST_VSPLTH:
    {
        // NOTE: accounting for full vector reversal here
        uint32_t perm = 7 - insn.operands[2];
        perm = (perm * 2) | ((perm * 2 + 1) << 8);
        println("\t{}.i = _mm_shuffle_epi8({}.i, _mm_set1_epi16({}));", V(0), V(1), int(int16_t(perm)));
        break;
    }

    case PPC_INST_VSPLTISB:
        println("\t{}.i = _mm_set1_epi8({});", V(0), int(int8_t(insn.operands[1])));
        break;

    case PPC_INST_VSPLTISW:
    case PPC_INST_VSPLTISW128:
        println("\t{}.i = _mm_set1_epi32({});", V(0), int32_t(insn.operands[1]));
        break;

    case PPC_INST_VSPLTW:
    case PPC_INST_VSPLTW128:
    {
        // NOTE: accounting for full vector reversal here
        uint32_t perm = 3 - insn.operands[2];
        perm |= (perm << 2) | (perm << 4) | (perm << 6);
        println("\t{}.i = _mm_shuffle_epi32::<0x{:X}>({}.i);", V(0), perm & 0xFF, V(1));
        break;
    }

    case PPC_INST_VSR:
        println("\t{}.i = mm_vsr({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VSRAW:
    case PPC_INST_VSRAW128:
        // TODO: vectorize, ensure endianness is correct
        for (size_t i = 0; i < 4; i++)
            println("\t{}.s32[{}] = {}.s32[{}] >> ({}.u8[{}] & 0x1F);", V(0), i, V(1), i, V(2), i * 4);
        break;

    case PPC_INST_VSRW:
    case PPC_INST_VSRW128:
        // TODO: vectorize, ensure endianness is correct
        for (size_t i = 0; i < 4; i++)
            println("\t{}.u32[{}] = {}.u32[{}] >> ({}.u8[{}] & 0x1F);", V(0), i, V(1), i, V(2), i * 4);
        break;

    case PPC_INST_VSUBFP:
    case PPC_INST_VSUBFP128:
        printSetFlushMode(true);
        println("\t{}.ps = _mm_sub_ps({}.ps, {}.ps);", V(0), V(1), V(2));
        break;

    case PPC_INST_VSUBSWS:
        // TODO: vectorize
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.s64 = ({}.s32[{}] as i64) - ({}.s32[{}] as i64);", temp(), V(1), i, V(2), i);
            println("\t{}.s32[{}] = sat_s32({}.s64);", V(0), i, temp());
        }
        break;

    case PPC_INST_VSUBUBS:
        println("\t{}.i = _mm_subs_epu8({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VSUBUHM:
        println("\t{}.i = _mm_sub_epi16({}.i, {}.i);", V(0), V(1), V(2));
        break;

    case PPC_INST_VUPKD3D128:
        // TODO: vectorize somehow?
        // NOTE: handling vector reversal here too
        switch (insn.operands[2] >> 2)
        {
        case 0: // D3D color
            for (size_t i = 0; i < 4; i++)
            {
                constexpr size_t indices[] = { 3, 0, 1, 2 };
                println("\t{}.u32[{}] = {}.u8[{}] as u32 | 0x3F800000;", vTemp(), i, V(1), indices[i]);
            }
            println("\t{} = {};", V(0), vTemp());
            break;

        case 1: // 2 shorts
            for (size_t i = 0; i < 2; i++)
            {
                println("\t{}.f32 = 3.0;", temp());
                println("\t{0}.s32 = {0}.s32.wrapping_add({1}.s16[{2}] as i32);", temp(), V(1), 1 - i);
                println("\t{}.f32[{}] = {}.f32;", vTemp(), 3 - i, temp());
            }
            println("\t{}.f32[1] = 0.0;", vTemp());
            println("\t{}.f32[0] = 1.0;", vTemp());
            println("\t{} = {};", V(0), vTemp());
            break;

        case 2: // three 10-bit signed normalised components, w in the top two bits
            // Like the D3D color case this does no arithmetic of its own: each
            // field is dropped into the mantissa of 3.0f, and the caller
            // finishes the job with a multiply-add by (511 * 2^-22, -3.0f),
            // which is why 511 rather than 512 is full scale.
            //
            // The packed word is w_z_y_x from the top down: see recompiler.cpp.
            //
            // x, y and z are signed; w is two bits and unsigned, so it is not
            // sign-extended along with them.
            {
                constexpr size_t shifts[] = { 22, 12, 2 };      // x, y, z
                println("\t{}.u32 = {}.u32[0];", temp(), V(1));
                for (size_t i = 0; i < 3; i++)
                    println("\t{}.u32[{}] = 0x40400000u32.wrapping_add((({}.u32 << {}) as i32 >> 22) as u32);", vTemp(), 3 - i, temp(), shifts[i]);
                println("\t{}.u32[0] = 0x40400000 + ({}.u32 >> 30);", vTemp(), temp());
                println("\t{} = {};", V(0), vTemp());
            }
            break;

        default:
            println("\tppc_debugtrap();");
            break;
        }
        break;

    case PPC_INST_VUPKHSB:
    case PPC_INST_VUPKHSB128:
        println("\t{0}.i = _mm_cvtepi8_epi16(_mm_unpackhi_epi64({1}.i, {1}.i));", V(0), V(1));
        break;

    case PPC_INST_VUPKHSH:
    case PPC_INST_VUPKHSH128:
        println("\t{0}.i = _mm_cvtepi16_epi32(_mm_unpackhi_epi64({1}.i, {1}.i));", V(0), V(1));
        break;

    case PPC_INST_VUPKLSB:
    case PPC_INST_VUPKLSB128:
        println("\t{}.i = _mm_cvtepi8_epi16({}.i);", V(0), V(1));
        break;

    case PPC_INST_VUPKLSH:
    case PPC_INST_VUPKLSH128:
        println("\t{}.i = _mm_cvtepi16_epi32({}.i);", V(0), V(1));
        break;

    case PPC_INST_VXOR:
    case PPC_INST_VXOR128:
        if (insn.operands[1] != insn.operands[2])
            println("\t{}.i = _mm_xor_si128({}.i, {}.i);", V(0), V(1), V(2));
        else
            println("\t{}.i = _mm_setzero_si128();", V(0));
        break;

    case PPC_INST_XOR:
        println("\t{}.u64 = {}.u64 ^ {}.u64;", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_XORI:
        println("\t{}.u64 = {}.u64 ^ {};", R(0), R(1), insn.operands[2]);
        break;

    case PPC_INST_XORIS:
        println("\t{}.u64 = {}.u64 ^ {};", R(0), R(1), insn.operands[2] << 16);
        break;

    // ---- Load/store "update" (base-writeback) forms ----

    case PPC_INST_LHZU:
        eaDisplaced();
        println("\t{}.u64 = ld16(base, {}) as u64;", R(0), ea());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_LHAU:
        eaDisplaced();
        println("\t{}.s64 = ld16(base, {}) as i16 as i64;", R(0), ea());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_LFSU:
        printSetFlushMode(false);
        eaDisplaced();
        println("\t{}.u32 = ld32(base, {});", temp(), ea());
        println("\t{}.f64 = {}.f32 as f64;", F(0), temp());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_LFDU:
        printSetFlushMode(false);
        eaDisplaced();
        println("\t{}.u64 = ld64(base, {});", F(0), ea());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_STHU:
        eaDisplaced();
        println("\tst16(base, {}, {}.u16);", ea(), R(0));
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_STFSU:
        printSetFlushMode(false);
        eaDisplaced();
        println("\t{}.f32 = {}.f64 as f32;", temp(), F(0));
        println("\tst32(base, {}, {}.u32);", ea(), temp());
        println("\t{}.u32 = {};", R(2), ea());
        break;

    case PPC_INST_STFDU:
        printSetFlushMode(false);
        eaDisplaced();
        println("\tst64(base, {}, {}.u64);", ea(), F(0));
        println("\t{}.u32 = {};", R(2), ea());
        break;

    // ---- Indexed "update" forms (EA = rA + rB, rA written back) ----

    case PPC_INST_LBZUX:
        eaIndexed();
        println("\t{}.u64 = ld8(base, {}) as u64;", R(0), ea());
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_LHZUX:
        eaIndexed();
        println("\t{}.u64 = ld16(base, {}) as u64;", R(0), ea());
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_LWZUX:
        eaIndexed();
        println("\t{}.u64 = ld32(base, {}) as u64;", R(0), ea());
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_LDUX:
        eaIndexed();
        println("\t{}.u64 = ld64(base, {});", R(0), ea());
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_LFSUX:
        printSetFlushMode(false);
        eaIndexed();
        println("\t{}.u32 = ld32(base, {});", temp(), ea());
        println("\t{}.f64 = {}.f32 as f64;", F(0), temp());
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_STHUX:
        eaIndexed();
        println("\tst16(base, {}, {}.u16);", ea(), R(0));
        println("\t{}.u32 = {};", R(1), ea());
        break;

    case PPC_INST_STDUX:
        eaIndexed();
        println("\tst64(base, {}, {}.u64);", ea(), R(0));
        println("\t{}.u32 = {};", R(1), ea());
        break;

    // ---- Byte-reversed halfword load ----

    case PPC_INST_LHBRX:
        println("\t{}.u64 = ld16(base, {}).swap_bytes() as u64;", R(0), indexed(1, 2));
        break;

    // ---- Cache hint, no observable effect under this memory model ----

    case PPC_INST_DCBST:
        // no op
        break;

    // ---- Branch on decremented CTR == 0 and condition false ----

    case PPC_INST_BDZF:
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        println("\t{0}.u64 = {0}.u64.wrapping_sub(1);", ctr());
        branchIf(fmt::format("{}.u32 == 0 && {}.{} == 0", ctr(), cr(insn.operands[0] / 4), fields[insn.operands[0] % 4]), insn.operands[1]);
        break;
    }

    // ---- Integer ALU ----

    case PPC_INST_EQV:
        println("\t{}.u64 = !({}.u64 ^ {}.u64);", R(0), R(1), R(2));
        rc();
        break;

    case PPC_INST_ADDC:
        println("\t{0}.u8 = ({1}.u32.wrapping_add({2}.u32) < {1}.u32) as u8;", temp(), R(1), R(2));
        println("\t{}.u64 = {}.u64.wrapping_add({}.u64);", R(0), R(1), R(2));
        println("\t{}.ca = {}.u8;", xer(), temp());
        rc();
        break;

    case PPC_INST_ADDME:
        // rD = rA + CA - 1; carry out iff rA + CA != 0
        println("\t{}.u8 = (({}.u32 != 0) | ({}.ca != 0)) as u8;", temp(), R(1), xer());
        println("\t{}.s64 = {}.s64.wrapping_add({}.ca as i64).wrapping_sub(1);", R(0), R(1), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        rc();
        break;

    case PPC_INST_SUBFZE:
        // rD = ~rA + CA; carry out iff ~rA + CA wraps
        println("\t{0}.u8 = ((!{1}.u32).wrapping_add({2}.ca as u32) < {2}.ca as u32) as u8;", temp(), R(1), xer());
        println("\t{}.u64 = (!{}.u64).wrapping_add({}.ca as u64);", R(0), R(1), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        rc();
        break;

    case PPC_INST_MULHDU:
        println("\t{}.u64 = ((({}.u64 as u128) * ({}.u64 as u128)) >> 64) as u64;", R(0), R(1), R(2));
        rc();
        break;

    // ---- Floating point ----

    case PPC_INST_FRSQRTE:
        printSetFlushMode(false);
        println("\t{}.f64 = 1.0 / fsqrt({}.f64);", F(0), F(1));
        break;

    // ---- VMX ----

    case PPC_INST_VSUBUWS:
        // TODO: vectorize
        for (size_t i = 0; i < 4; i++)
            println("\t{0}.u32[{3}] = if {1}.u32[{3}] > {2}.u32[{3}] {{ {1}.u32[{3}] - {2}.u32[{3}] }} else {{ 0 }};", V(0), V(1), V(2), i);
        break;

    case PPC_INST_VPKSWSS:
    case PPC_INST_VPKSWSS128:
        // NOTE: operands swapped to account for the full vector reversal
        println("\t{}.i = _mm_packs_epi32({}.i, {}.i);", V(0), V(2), V(1));
        break;

    case PPC_INST_VCFPUXWS128:
        printSetFlushMode(true);
        if (insn.operands[2] != 0)
            println("\t{}.i = mm_vctuxs(_mm_mul_ps({}.ps, _mm_set1_ps({}.0)));", V(0), V(1), 1u << insn.operands[2]);
        else
            println("\t{}.i = mm_vctuxs({}.ps);", V(0), V(1));
        break;

    default:
        return false;
    }

    return true;
}

std::string RustRecompiler::RecompileFunction(const Function& fn, std::set<std::string>& externs)
{
    auto base = fn.base;
    auto end = base + fn.size;
    auto* data = (uint32_t*)image.Find(base);

    // The same labels as Recompiler::Recompile finds: a block starts at each.
    std::unordered_set<size_t> labels;
    for (size_t addr = base; addr < end; addr += 4)
    {
        const uint32_t instruction = ByteSwap(*(uint32_t*)((char*)data + addr - base));
        if (!PPC_BL(instruction))
        {
            const size_t op = PPC_OP(instruction);
            if (op == PPC_OP_B)
                labels.emplace(addr + PPC_BI(instruction));
            else if (op == PPC_OP_BC)
                labels.emplace(addr + PPC_BD(instruction));
        }

        auto switchTable = config.switchTables.find(addr);
        if (switchTable != config.switchTables.end())
        {
            for (auto label : switchTable->second.labels)
                labels.emplace(label);
        }
    }

    auto symbol = image.symbols.find(fn.base);
    const std::string name = symbol != image.symbols.end() ? symbol->name : fmt::format("sub_{}", fn.base);

    keepRegistersInContext = name.find("__save") == 0 || name.find("__rest") == 0 ||
        functionsKeepingRegistersInContext.count(fn.base) != 0;

    auto switchTable = config.switchTables.end();
    CSRState csrState = CSRState::Unknown;
    RecompilerLocalVariables localVariables;
    setjmpResumes.clear();

    // Each block's flow is in guest addresses until every block exists.
    constexpr uint32_t FALL_OFF_THE_END = 0;
    std::vector<rustcfg::Block> blocks;
    std::vector<PendingFlow> flows;
    std::vector<uint32_t> fallsTo;
    std::unordered_map<uint32_t, int> blockAt;

    std::string savedOut;
    std::swap(out, savedOut);

    uint32_t blockStart = base;
    auto endBlock = [&](PendingFlow flow, uint32_t next)
        {
            auto& block = blocks.emplace_back();
            block.id = blockStart;
            block.body = std::move(out);
            out.clear();
            blockAt[blockStart] = int(blocks.size() - 1);
            flows.push_back(std::move(flow));
            fallsTo.push_back(next);
            blockStart = next;
        };

    ppc_insn insn;
    while (base < end)
    {
        if (labels.find(base) != labels.end())
        {
            if (blockStart != base)
                endBlock({ Flow::Jump, {}, uint32_t(base) }, uint32_t(base));

            // Anyone could jump to this label so we wouldn't know what the CSR state would be.
            csrState = CSRState::Unknown;
        }

        if (switchTable == config.switchTables.end())
            switchTable = config.switchTables.find(base);

        ppc::Disassemble(data, 4, base, insn);

        PendingFlow flow;
        if (insn.opcode == nullptr)
        {
            println("\t// {}", insn.op_str);
            if (*data != 0)
                fmt::println("Unable to decode instruction {:X} at {:X}", *data, base);
        }
        else
        {
            if (insn.opcode->id == PPC_INST_BCTR && (*(data - 1) == 0x07008038 || *(data - 1) == 0x00000060) && switchTable == config.switchTables.end())
                fmt::println("Found a switch jump table at {:X} with no switch table entry present", base);

            if (!Emit(fn, base, insn, data, switchTable, localVariables, csrState, externs, flow))
                fmt::println("Unrecognized instruction at 0x{:X}: {}", base, insn.opcode->name);
        }

        base += 4;
        ++data;

        if (flow.kind != Flow::None)
            endBlock(std::move(flow), uint32_t(base));
    }

    // What remains falls off the end, which is a return as it is in C++.
    endBlock({ Flow::Return }, FALL_OFF_THE_END);

    auto blockIndex = [&](uint32_t address)
        {
            auto it = blockAt.find(address);
            if (it != blockAt.end())
                return it->second;
            if (address >= fn.base && address < fn.base + fn.size)
                throw std::logic_error(fmt::format("no block at {:X}, in {}", address, name));
            return rustcfg::RETURN;
        };

    for (size_t i = 0; i < blocks.size(); i++)
    {
        auto& block = blocks[i];
        const PendingFlow& flow = flows[i];

        switch (flow.kind)
        {
        case Flow::Jump:
            block.term = rustcfg::Term::Jump;
            block.succ.push_back(blockIndex(flow.target));
            break;

        case Flow::Cond:
            block.term = rustcfg::Term::Cond;
            block.cond = flow.cond;
            block.succ.push_back(blockIndex(flow.target));
            block.succ.push_back(blockIndex(fallsTo[i]));
            break;

        case Flow::Switch:
            block.term = rustcfg::Term::Switch;
            block.cond = flow.cond;
            for (size_t j = 0; j < flow.targets.size(); j++)
            {
                block.values.push_back(uint32_t(j));
                block.succ.push_back(flow.targets[j] != 0 ? blockIndex(flow.targets[j]) : rustcfg::RETURN);
            }
            break;

        default:
            block.term = rustcfg::Term::Jump;
            break;
        }
    }

    // A body that can start after any of its setjmps: one more multi-entry graph.
    const bool hasSetjmp = !setjmpResumes.empty();
    if (hasSetjmp)
    {
        for (auto& block : blocks)
        {
            for (int& successor : block.succ)
            {
                if (successor != rustcfg::RETURN)
                    successor++;
            }
        }

        rustcfg::Block start;
        start.id = uint32_t(fn.base + fn.size + 4);
        start.term = rustcfg::Term::Switch;
        start.cond = "(*__frame).resume";
        start.values.push_back(0);
        start.succ.push_back(1);
        for (size_t i = 0; i < setjmpResumes.size(); i++)
        {
            start.values.push_back(uint32_t(i + 1));
            start.succ.push_back(blockIndex(setjmpResumes[i]) + 1);
        }
        blocks.insert(blocks.begin(), std::move(start));
    }

    rustcfg::Stats stats;
    std::string body = rustcfg::Structure(std::move(blocks), &stats);

    structureTotals.irreducibleRegions += stats.irreducibleRegions;
    structureTotals.dispatchEdges += stats.dispatchEdges;
    structureTotals.maxDepth = std::max(structureTotals.maxDepth, stats.maxDepth);

    println("#[unsafe(no_mangle)]");
    println("pub unsafe extern \"C\" fn __imp__{}(ctx: &mut PPCContext, base: *mut u8) {{", name);

    if (hasSetjmp)
    {
        println("\tppc_run_setjmp_function(ctx, base, __body_{});", name);
        println("}}\n");
        println("unsafe extern \"C\" fn __body_{}(ctx: &mut PPCContext, base: *mut u8, __frame: *mut SetjmpFrame) {{", name);
    }

    if (localVariables.ctr)
        println("\tlet mut ctr = PPCRegister {{ u64: 0 }};");
    if (localVariables.xer)
        println("\tlet mut xer = PPCXERRegister::ZERO;");
    if (localVariables.reserved)
        println("\tlet mut reserved = PPCRegister {{ u64: 0 }};");

    for (size_t i = 0; i < 8; i++)
    {
        if (localVariables.cr[i])
            println("\tlet mut cr{} = PPCCRRegister::ZERO;", i);
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (localVariables.r[i])
            println("\tlet mut r{} = PPCRegister {{ u64: 0 }};", i);
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (localVariables.f[i])
            println("\tlet mut f{} = PPCRegister {{ u64: 0 }};", i);
    }

    for (size_t i = 0; i < 128; i++)
    {
        if (localVariables.v[i])
            println("\tlet mut v{} = PPCVRegister {{ u64: [0, 0] }};", i);
    }

    if (localVariables.temp)
        println("\tlet mut temp = PPCRegister {{ u64: 0 }};");
    if (localVariables.vTemp)
        println("\tlet mut vTemp = PPCVRegister {{ u64: [0, 0] }};");
    if (localVariables.ea)
        println("\tlet mut ea: u32 = 0;");
    if (stats.dispatchEdges != 0)
        println("\tlet mut __state: u32 = 0;");

    out += body;
    println("}}\n");

    std::swap(out, savedOut);
    return savedOut;
}
