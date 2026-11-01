#include "register_flow.h"
#include "recompiler.h"

#include <array>
#include <bitset>
#include <optional>

// A local is private to the function that declares it, so the *_as_local
// options are only correct where no value crosses a function boundary in a
// register that becomes a local. The calling convention keeps to that: values
// go in and out in r3-r10, f1-f13 and v1-v13, which stay in the context, and a
// callee preserves the non-volatile registers, which is what a caller's
// locals do by themselves. MSVC's output does not always keep to it:
//
// - __chkstk takes the size to probe in r12;
// - an exception handler funclet gets its parent's frame in r12 and reads the
//   parent's non-volatile registers; the parent calls it directly on the way
//   out of a __finally, and a trampoline that sets r12 calls it indirectly;
// - XenonAnalyse splits some functions in two, and the second half, reached by
//   a branch it emits as a tail call, reads the first half's registers;
// - with link-time code generation a caller keeps a scratch register (f0,
//   v45-v63) live across a call to a function that does not write it, which
//   a local does too; it takes the context only if the callee does write it;
// - a function that calls setjmp resumes after a longjmp with the registers
//   the host's longjmp restores from the context (env).
//
// Those functions, and the ones on the other end of such a value, keep every
// register in the context. They are found by data flow over the instructions:
// what each one reads and writes is what Recompile emits for it, and what a
// call reads and writes comes from the callee, iterated to a fixed point over
// the call graph.
//
// Register numbering, for one bitset over all of them: r0-r31, f0-f31,
// v0-v127, cr0-cr7, ctr, xer's carry and summary overflow, the reservation.

namespace
{
    constexpr size_t GPR = 0;
    constexpr size_t FPR = 32;
    constexpr size_t VR = 64;
    constexpr size_t CR = 192;
    constexpr size_t CTR = 200;
    constexpr size_t CA = 201;
    constexpr size_t SO = 202;
    constexpr size_t RESERVED = 203;
    constexpr size_t REGISTER_COUNT = 204;

    using Registers = std::bitset<REGISTER_COUNT>;

    constexpr uint32_t NONE = ~0u;

    // A value the stack frame pass has not reached yet, or cannot know.
    constexpr int64_t UNSET = INT64_MIN;
    constexpr int64_t UNKNOWN = INT64_MAX;

    enum class Flow : uint8_t
    {
        Next,
        Goto,
        CondGoto,
        Switch,
        Return,
        CondReturn,
        Call,
        TailCall,
        CondTailCall,
        IndirectCall,
        IndirectTailCall,
        CondIndirectTailCall,
    };

    struct Instruction
    {
        Registers uses;
        Registers defs;
        Flow flow = Flow::Next;
        // The instruction a goto lands on, or the callee's index in the
        // function list (NONE for a call to an import or to nothing known).
        uint32_t target = NONE;
    };

    // The opcode and first operands, for the stack frame pass.
    struct Raw
    {
        int id = 0;
        uint32_t o[3]{};
    };

    struct FunctionFlow
    {
        size_t base{};
        std::vector<Instruction> instructions;
        std::vector<Raw> raw;
        std::unordered_map<size_t, std::vector<uint32_t>> switchCases;
        std::vector<uint32_t> callers;
        Registers liveIn;
        // Volatile registers a callee may write, its callees included (for
        // the constants of the stack frame pass).
        Registers mayDef;
        // Non-volatile registers the function saves and restores.
        Registers preserved;
        // Registers the function may return holding a value it or a callee
        // wrote.
        Registers exits;
        bool callsSetJmp = false;
        bool unhandled = false;
        bool hooked = false;
        // A save/restore helper, which Recompile keeps in the context anyway.
        bool helper = false;
    };

    bool IsDirectCall(Flow flow)
    {
        return flow == Flow::Call || flow == Flow::TailCall || flow == Flow::CondTailCall;
    }

    bool IsIndirectCall(Flow flow)
    {
        return flow == Flow::IndirectCall || flow == Flow::IndirectTailCall || flow == Flow::CondIndirectTailCall;
    }

    template<typename F>
    void ForEachSuccessor(const FunctionFlow& fn, size_t i, F&& f)
    {
        const auto& insn = fn.instructions[i];
        switch (insn.flow)
        {
        case Flow::Goto:
        case Flow::CondGoto:
            if (insn.target != NONE)
                f(insn.target);
            break;
        case Flow::Switch:
            for (auto target : fn.switchCases.at(i))
                f(target);
            break;
        default:
            break;
        }

        switch (insn.flow)
        {
        case Flow::Next:
        case Flow::CondGoto:
        case Flow::CondReturn:
        case Flow::Call:
        case Flow::CondTailCall:
        case Flow::IndirectCall:
        case Flow::CondIndirectTailCall:
            if (i + 1 < fn.instructions.size())
                f(i + 1);
            break;
        default:
            break;
        }
    }

    // What an instruction reads and writes, following what Recompile emits for
    // it. False for an instruction Recompile does not handle either.
    bool Describe(int id, const ppc_insn& insn, Instruction& out)
    {
        const auto& o = insn.operands;
        auto use = [&](size_t index) { out.uses.set(index); };
        auto def = [&](size_t index) { out.defs.set(index); };
        auto useR = [&](size_t i) { use(GPR + o[i]); };
        auto defR = [&](size_t i) { def(GPR + o[i]); };
        // rA|0: register 0 reads as zero, and Recompile reads no register.
        auto useR0 = [&](size_t i) { if (o[i] != 0) useR(i); };
        auto useF = [&](size_t i) { use(FPR + o[i]); };
        auto defF = [&](size_t i) { def(FPR + o[i]); };
        auto useV = [&](size_t i) { use(VR + o[i]); };
        auto defV = [&](size_t i) { def(VR + o[i]); };
        auto useAllCr = [&]() { for (size_t i = 0; i < 8; i++) use(CR + i); };
        auto defAllCr = [&]() { for (size_t i = 0; i < 8; i++) def(CR + i); };
        // A compare copies xer's summary overflow into the field it sets.
        auto compare = [&](size_t field) { def(CR + field); use(SO); };
        auto record = [&]() { if (strchr(insn.opcode->name, '.')) compare(0); };

        switch (id)
        {
        // rD from rA and rB
        case PPC_INST_ADD:
        case PPC_INST_AND:
        case PPC_INST_ANDC:
        case PPC_INST_DIVDU:
        case PPC_INST_DIVW:
        case PPC_INST_DIVWU:
        case PPC_INST_EQV:
        case PPC_INST_MULHDU:
        case PPC_INST_MULHWU:
        case PPC_INST_MULLW:
        case PPC_INST_OR:
        case PPC_INST_SLW:
        case PPC_INST_SRW:
        case PPC_INST_SUBF:
            record();
            [[fallthrough]];
        case PPC_INST_DIVD:
        case PPC_INST_MULHW:
        case PPC_INST_MULLD:
        case PPC_INST_NAND:
        case PPC_INST_NOR:
        case PPC_INST_ORC:
        case PPC_INST_ROTLW:
        case PPC_INST_SLD:
        case PPC_INST_SRD:
            defR(0); useR(1); useR(2);
            break;

        case PPC_INST_ADDC:
        case PPC_INST_SUBFC:
        case PPC_INST_SRAW:
            defR(0); useR(1); useR(2); def(CA); record();
            break;

        case PPC_INST_SRAD:
            defR(0); useR(1); useR(2); def(CA);
            break;

        case PPC_INST_ADDE:
            defR(0); useR(1); useR(2); use(CA); def(CA); record();
            break;

        // xor rD,rA,rA is zero whatever rA holds.
        case PPC_INST_XOR:
            defR(0); record();
            if (o[1] != o[2])
            {
                useR(1); useR(2);
            }
            break;

        case PPC_INST_SUBFE:
            // subfe rD,rA,rA gives CA - 1, whatever rA holds: ~a + a is all ones.
            defR(0); use(CA); def(CA); record();
            if (o[1] != o[2])
            {
                useR(1); useR(2);
            }
            break;

        // rD from rA
        case PPC_INST_CLRLWI:
        case PPC_INST_EXTSB:
        case PPC_INST_EXTSH:
        case PPC_INST_EXTSW:
        case PPC_INST_MR:
        case PPC_INST_NEG:
        case PPC_INST_NOT:
        case PPC_INST_RLWINM:
        case PPC_INST_ROTLWI:
            record();
            [[fallthrough]];
        case PPC_INST_CLRLDI:
        case PPC_INST_CNTLZD:
        case PPC_INST_CNTLZW:
        case PPC_INST_MULLI:
        case PPC_INST_ORI:
        case PPC_INST_ORIS:
        case PPC_INST_RLDICL:
        case PPC_INST_RLDICR:
        case PPC_INST_ROTLDI:
        case PPC_INST_XORI:
        case PPC_INST_XORIS:
            defR(0); useR(1);
            break;

        case PPC_INST_ANDI:
        case PPC_INST_ANDIS:
            defR(0); useR(1); compare(0);
            break;

        case PPC_INST_SRAWI:
            defR(0); useR(1); def(CA); record();
            break;

        case PPC_INST_ADDIC:
            defR(0); useR(1); def(CA); record();
            break;

        case PPC_INST_SUBFIC:
        case PPC_INST_SRADI:
            defR(0); useR(1); def(CA);
            break;

        case PPC_INST_ADDZE:
        case PPC_INST_ADDME:
        case PPC_INST_SUBFZE:
            defR(0); useR(1); use(CA); def(CA); record();
            break;

        // Insert into rD: the bits outside the mask are rD's own.
        case PPC_INST_RLDIMI:
        case PPC_INST_RLWIMI:
            useR(0); defR(0); useR(1);
            break;

        case PPC_INST_ADDI:
        case PPC_INST_ADDIS:
            defR(0); useR0(1);
            break;

        case PPC_INST_LI:
        case PPC_INST_LIS:
        case PPC_INST_MFLR:
        case PPC_INST_MFMSR:
        case PPC_INST_MFTB:
            defR(0);
            break;

        case PPC_INST_CMPD:
        case PPC_INST_CMPLD:
        case PPC_INST_CMPLW:
        case PPC_INST_CMPW:
            compare(o[0]); useR(1); useR(2);
            break;

        case PPC_INST_CMPDI:
        case PPC_INST_CMPLDI:
        case PPC_INST_CMPLWI:
        case PPC_INST_CMPWI:
            compare(o[0]); useR(1);
            break;

        case PPC_INST_FCMPU:
            def(CR + o[0]); useF(1); useF(2);
            break;

        // fD from fA..
        case PPC_INST_FMADD:
        case PPC_INST_FMADDS:
        case PPC_INST_FMSUB:
        case PPC_INST_FMSUBS:
        case PPC_INST_FNMADDS:
        case PPC_INST_FNMSUB:
        case PPC_INST_FNMSUBS:
        case PPC_INST_FSEL:
            useF(3);
            [[fallthrough]];
        case PPC_INST_FADD:
        case PPC_INST_FADDS:
        case PPC_INST_FDIV:
        case PPC_INST_FDIVS:
        case PPC_INST_FMUL:
        case PPC_INST_FMULS:
        case PPC_INST_FSUB:
        case PPC_INST_FSUBS:
            useF(2);
            [[fallthrough]];
        case PPC_INST_FABS:
        case PPC_INST_FCFID:
        case PPC_INST_FCTID:
        case PPC_INST_FCTIDZ:
        case PPC_INST_FCTIWZ:
        case PPC_INST_FMR:
        case PPC_INST_FNABS:
        case PPC_INST_FNEG:
        case PPC_INST_FRES:
        case PPC_INST_FRSP:
        case PPC_INST_FRSQRTE:
        case PPC_INST_FSQRT:
        case PPC_INST_FSQRTS:
            defF(0); useF(1);
            break;

        case PPC_INST_MFFS:
            defF(0);
            break;

        case PPC_INST_MTFSF:
            useF(1);
            break;

        // Loads, d(rA|0)
        case PPC_INST_LBZ:
        case PPC_INST_LD:
        case PPC_INST_LHA:
        case PPC_INST_LHZ:
        case PPC_INST_LWA:
        case PPC_INST_LWZ:
            defR(0); useR0(2);
            break;

        case PPC_INST_LFD:
        case PPC_INST_LFS:
            defF(0); useR0(2);
            break;

        // Loads, d(rA) with rA updated
        case PPC_INST_LBZU:
        case PPC_INST_LDU:
        case PPC_INST_LHAU:
        case PPC_INST_LHZU:
        case PPC_INST_LWZU:
            defR(0); useR(2); defR(2);
            break;

        case PPC_INST_LFDU:
        case PPC_INST_LFSU:
            defF(0); useR(2); defR(2);
            break;

        // Loads, (rA|0 + rB)
        case PPC_INST_LBZX:
        case PPC_INST_LDX:
        case PPC_INST_LHAX:
        case PPC_INST_LHBRX:
        case PPC_INST_LHZX:
        case PPC_INST_LWAX:
        case PPC_INST_LWBRX:
        case PPC_INST_LWZX:
            defR(0); useR0(1); useR(2);
            break;

        case PPC_INST_LDARX:
        case PPC_INST_LWARX:
            defR(0); useR0(1); useR(2); def(RESERVED);
            break;

        case PPC_INST_LFDX:
        case PPC_INST_LFSX:
            defF(0); useR0(1); useR(2);
            break;

        case PPC_INST_LVEWX:
        case PPC_INST_LVEWX128:
        case PPC_INST_LVX:
        case PPC_INST_LVX128:
        case PPC_INST_LVLX:
        case PPC_INST_LVLX128:
        case PPC_INST_LVRX:
        case PPC_INST_LVRX128:
        case PPC_INST_LVSL:
        case PPC_INST_LVSR:
            defV(0); useR0(1); useR(2);
            break;

        // Loads, (rA + rB) with rA updated
        case PPC_INST_LBZUX:
        case PPC_INST_LDUX:
        case PPC_INST_LHZUX:
        case PPC_INST_LWZUX:
            defR(0); useR(1); defR(1); useR(2);
            break;

        case PPC_INST_LFSUX:
            defF(0); useR(1); defR(1); useR(2);
            break;

        // Stores, d(rA|0)
        case PPC_INST_STB:
        case PPC_INST_STD:
        case PPC_INST_STH:
        case PPC_INST_STW:
            useR(0); useR0(2);
            break;

        case PPC_INST_STFD:
        case PPC_INST_STFS:
            useF(0); useR0(2);
            break;

        // Stores, d(rA) with rA updated
        case PPC_INST_STBU:
        case PPC_INST_STDU:
        case PPC_INST_STHU:
        case PPC_INST_STWU:
            useR(0); useR(2); defR(2);
            break;

        case PPC_INST_STFDU:
        case PPC_INST_STFSU:
            useF(0); useR(2); defR(2);
            break;

        // Stores, (rA|0 + rB)
        case PPC_INST_STBX:
        case PPC_INST_STDX:
        case PPC_INST_STHBRX:
        case PPC_INST_STHX:
        case PPC_INST_STWBRX:
        case PPC_INST_STWX:
            useR(0); useR0(1); useR(2);
            break;

        case PPC_INST_STFDX:
        case PPC_INST_STFIWX:
        case PPC_INST_STFSX:
            useF(0); useR0(1); useR(2);
            break;

        case PPC_INST_STVEHX:
        case PPC_INST_STVEWX:
        case PPC_INST_STVEWX128:
        case PPC_INST_STVLX:
        case PPC_INST_STVLX128:
        case PPC_INST_STVRX:
        case PPC_INST_STVRX128:
        case PPC_INST_STVX:
        case PPC_INST_STVX128:
            useV(0); useR0(1); useR(2);
            break;

        // Stores, (rA + rB) with rA updated
        case PPC_INST_STBUX:
        case PPC_INST_STDUX:
        case PPC_INST_STHUX:
        case PPC_INST_STWUX:
            useR(0); useR(1); defR(1); useR(2);
            break;

        // Store conditional: cr0 says whether it took, from the reservation.
        case PPC_INST_STDCX:
        case PPC_INST_STWCX:
            useR(0); useR0(1); useR(2); use(RESERVED); compare(0);
            break;

        case PPC_INST_DCBZ:
        case PPC_INST_DCBZL:
            useR0(0); useR(1);
            break;

        case PPC_INST_MFCR:
            defR(0); useAllCr();
            break;

        case PPC_INST_MTCR:
            useR(0); defAllCr();
            break;

        // Recompile reads cr6 whatever the field mask names.
        case PPC_INST_MFOCRF:
            defR(0); use(CR + 6);
            break;

        case PPC_INST_MTCTR:
            useR(0); def(CTR);
            break;

        case PPC_INST_MTLR:
        case PPC_INST_MTMSRD:
            useR(0);
            break;

        case PPC_INST_MTXER:
            useR(0); def(SO); def(CA);
            break;

        // vD from vA..
        case PPC_INST_VMADDCFP128:
        case PPC_INST_VMADDFP:
        case PPC_INST_VMADDFP128:
        case PPC_INST_VNMSUBFP:
        case PPC_INST_VNMSUBFP128:
        case PPC_INST_VPERM:
        case PPC_INST_VPERM128:
        case PPC_INST_VSEL:
        case PPC_INST_VSEL128:
            useV(3);
            [[fallthrough]];
        case PPC_INST_VADDFP:
        case PPC_INST_VADDFP128:
        case PPC_INST_VADDSHS:
        case PPC_INST_VADDUBM:
        case PPC_INST_VADDUBS:
        case PPC_INST_VADDUHM:
        case PPC_INST_VADDUWM:
        case PPC_INST_VADDUWS:
        case PPC_INST_VAND:
        case PPC_INST_VAND128:
        case PPC_INST_VANDC128:
        case PPC_INST_VAVGSB:
        case PPC_INST_VAVGSH:
        case PPC_INST_VAVGUB:
        case PPC_INST_VCMPGTUB:
        case PPC_INST_VCMPGTUH:
        case PPC_INST_VMAXFP:
        case PPC_INST_VMAXFP128:
        case PPC_INST_VMAXSW:
        case PPC_INST_VMINFP:
        case PPC_INST_VMINFP128:
        case PPC_INST_VMRGHB:
        case PPC_INST_VMRGHH:
        case PPC_INST_VMRGHW:
        case PPC_INST_VMRGHW128:
        case PPC_INST_VMRGLB:
        case PPC_INST_VMRGLH:
        case PPC_INST_VMRGLW:
        case PPC_INST_VMRGLW128:
        case PPC_INST_VMSUM3FP128:
        case PPC_INST_VMSUM4FP128:
        case PPC_INST_VMULFP128:
        case PPC_INST_VOR:
        case PPC_INST_VOR128:
        case PPC_INST_VPKSHUS:
        case PPC_INST_VPKSHUS128:
        case PPC_INST_VPKSWSS:
        case PPC_INST_VPKSWSS128:
        case PPC_INST_VSLB:
        case PPC_INST_VSLDOI:
        case PPC_INST_VSLDOI128:
        case PPC_INST_VSLW:
        case PPC_INST_VSLW128:
        case PPC_INST_VSR:
        case PPC_INST_VSRAW:
        case PPC_INST_VSRAW128:
        case PPC_INST_VSRW:
        case PPC_INST_VSRW128:
        case PPC_INST_VSUBFP:
        case PPC_INST_VSUBFP128:
        case PPC_INST_VSUBSWS:
        case PPC_INST_VSUBUBS:
        case PPC_INST_VSUBUHM:
        case PPC_INST_VSUBUWS:
            useV(2);
            [[fallthrough]];
        case PPC_INST_VCFPSXWS128:
        case PPC_INST_VCFPUXWS128:
        case PPC_INST_VCFSX:
        case PPC_INST_VCFUX:
        case PPC_INST_VCSXWFP128:
        case PPC_INST_VCTSXS:
        case PPC_INST_VCUXWFP128:
        case PPC_INST_VEXPTEFP:
        case PPC_INST_VEXPTEFP128:
        case PPC_INST_VLOGEFP:
        case PPC_INST_VLOGEFP128:
        case PPC_INST_VPERMWI128:
        case PPC_INST_VREFP:
        case PPC_INST_VREFP128:
        case PPC_INST_VRFIM:
        case PPC_INST_VRFIM128:
        case PPC_INST_VRFIN:
        case PPC_INST_VRFIN128:
        case PPC_INST_VRFIZ:
        case PPC_INST_VRFIZ128:
        case PPC_INST_VRSQRTEFP:
        case PPC_INST_VRSQRTEFP128:
        case PPC_INST_VSPLTB:
        case PPC_INST_VSPLTH:
        case PPC_INST_VSPLTW:
        case PPC_INST_VSPLTW128:
        case PPC_INST_VUPKD3D128:
        case PPC_INST_VUPKHSB:
        case PPC_INST_VUPKHSB128:
        case PPC_INST_VUPKHSH:
        case PPC_INST_VUPKHSH128:
        case PPC_INST_VUPKLSB:
        case PPC_INST_VUPKLSB128:
        case PPC_INST_VUPKLSH:
        case PPC_INST_VUPKLSH128:
            defV(0); useV(1);
            break;

        case PPC_INST_VCMPEQFP:
        case PPC_INST_VCMPEQFP128:
        case PPC_INST_VCMPEQUB:
        case PPC_INST_VCMPEQUW:
        case PPC_INST_VCMPEQUW128:
        case PPC_INST_VCMPGEFP:
        case PPC_INST_VCMPGEFP128:
        case PPC_INST_VCMPGTFP:
        case PPC_INST_VCMPGTFP128:
            defV(0); useV(1); useV(2);
            if (strchr(insn.opcode->name, '.'))
                def(CR + 6);
            break;

        // vxor vD,vA,vA is zero, and Recompile reads nothing for it.
        case PPC_INST_VXOR:
        case PPC_INST_VXOR128:
            defV(0);
            if (o[1] != o[2])
            {
                useV(1); useV(2);
            }
            break;

        case PPC_INST_VSPLTISB:
        case PPC_INST_VSPLTISW:
        case PPC_INST_VSPLTISW128:
            defV(0);
            break;

        // Into some of vD's elements, the others kept.
        case PPC_INST_VPKD3D128:
        case PPC_INST_VRLIMI128:
            useV(0); defV(0); useV(1);
            break;

        // Branches: where they go is for the caller to work out.
        case PPC_INST_B:
        case PPC_INST_BL:
        case PPC_INST_BLR:
        case PPC_INST_BLRL:
            break;

        case PPC_INST_BCTR:
        case PPC_INST_BCTRL:
            use(CTR);
            break;

        case PPC_INST_BDZ:
        case PPC_INST_BDNZ:
        case PPC_INST_BDZLR:
            use(CTR); def(CTR);
            break;

        case PPC_INST_BDNZF:
        case PPC_INST_BDZF:
            use(CTR); def(CTR); use(CR + o[0] / 4);
            break;

        case PPC_INST_BEQ:
        case PPC_INST_BGE:
        case PPC_INST_BGT:
        case PPC_INST_BLE:
        case PPC_INST_BLT:
        case PPC_INST_BNE:
        case PPC_INST_BEQLR:
        case PPC_INST_BGELR:
        case PPC_INST_BGTLR:
        case PPC_INST_BLELR:
        case PPC_INST_BLTLR:
        case PPC_INST_BNELR:
            use(CR + o[0]);
            break;

        case PPC_INST_BNECTR:
            use(CR + o[0]); use(CTR);
            break;

        // Recompile emits nothing that touches a register.
        case PPC_INST_ATTN:
        case PPC_INST_CCTPL:
        case PPC_INST_CCTPM:
        case PPC_INST_DB16CYC:
        case PPC_INST_DCBF:
        case PPC_INST_DCBST:
        case PPC_INST_DCBT:
        case PPC_INST_DCBTST:
        case PPC_INST_EIEIO:
        case PPC_INST_LWSYNC:
        case PPC_INST_NOP:
        case PPC_INST_SYNC:
        case PPC_INST_TDLGEI:
        case PPC_INST_TDLLEI:
        case PPC_INST_TWI:
        case PPC_INST_TWLGEI:
        case PPC_INST_TWLLEI:
        case PPC_INST_VCMPBFP:
        case PPC_INST_VCMPBFP128:
            break;

        default:
            return false;
        }

        return true;
    }

    // A prologue stores the non-volatile registers it is going to use below the
    // stack pointer it was called with, and the epilogue loads them back; the
    // caller's values are only kept, never used, so the store is no read of
    // them. A store counts as such a save when it writes a register the
    // function has not written yet to a slot at a known offset from the entry
    // stack pointer, and nothing the function loads from that slot goes
    // anywhere but back into the same register (a function that never returns
    // never loads it). Nothing reaches a save slot but its function's own r1.
    void DropSaves(FunctionFlow& fn, const std::vector<FunctionFlow>& functions, const Registers& volatileRegisters)
    {
        const auto& raw = fn.raw;
        const size_t n = fn.instructions.size();
        if (n == 0)
            return;

        // Where r1 is, and where the back chain word at 0(r1) points, as
        // offsets from r1 at entry; the general registers that hold a known
        // constant (a frame too big for stwu has its size in one); and which
        // registers may have been written, before each instruction.
        struct State
        {
            int64_t sp = UNSET;
            int64_t backChain = UNSET;
            std::array<int64_t, 32> constants;
            Registers written;
            State() { constants.fill(UNSET); }
        };
        std::vector<State> states(n);
        states[0].sp = 0;
        states[0].backChain = UNKNOWN;
        states[0].constants.fill(UNKNOWN);

        auto merge = [](int64_t& into, int64_t value)
            {
                int64_t merged = into == UNSET || into == value ? value : UNKNOWN;
                bool changed = merged != into;
                into = merged;
                return changed;
            };

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (size_t i = 0; i < n; i++)
            {
                if (states[i].sp == UNSET)
                    continue;

                const auto& insn = fn.instructions[i];
                const auto& r = raw[i];
                State after = states[i];
                after.written |= insn.defs;

                auto& c = after.constants;
                const auto before = c;
                for (size_t reg = 0; reg < 32; reg++)
                {
                    if (insn.defs.test(GPR + reg))
                        c[reg] = UNKNOWN;
                }
                if (IsDirectCall(insn.flow) || IsIndirectCall(insn.flow))
                {
                    const Registers clobbered = IsDirectCall(insn.flow) && insn.target != NONE ?
                        functions[insn.target].mayDef : volatileRegisters;
                    for (size_t reg = 0; reg < 32; reg++)
                    {
                        if (clobbered.test(GPR + reg))
                            c[reg] = UNKNOWN;
                    }
                }
                auto beforeKnown = [&](uint32_t reg) { return before[reg] != UNSET && before[reg] != UNKNOWN; };
                switch (r.id)
                {
                case PPC_INST_LI:
                    c[r.o[0]] = int32_t(r.o[1]);
                    break;
                case PPC_INST_LIS:
                    c[r.o[0]] = int32_t(r.o[1] << 16);
                    break;
                case PPC_INST_ADDI:
                case PPC_INST_ADDIS:
                {
                    const int64_t imm = r.id == PPC_INST_ADDI ? int32_t(r.o[2]) : int32_t(r.o[2] << 16);
                    if (r.o[1] == 0)
                        c[r.o[0]] = imm;
                    else if (beforeKnown(r.o[1]))
                        c[r.o[0]] = before[r.o[1]] + imm;
                    break;
                }
                case PPC_INST_ORI:
                    if (beforeKnown(r.o[1]))
                        c[r.o[0]] = before[r.o[1]] | int64_t(r.o[2]);
                    break;
                case PPC_INST_NEG:
                    if (beforeKnown(r.o[1]))
                        c[r.o[0]] = -before[r.o[1]];
                    break;
                }

                if (insn.defs.test(GPR + 1))
                {
                    const bool known = after.sp != UNKNOWN;
                    if ((r.id == PPC_INST_STWU || r.id == PPC_INST_STDU) && r.o[2] == 1)
                    {
                        // The old r1 goes to the new 0(r1).
                        after.backChain = after.sp;
                        after.sp = known ? after.sp + int32_t(r.o[1]) : UNKNOWN;
                    }
                    else if ((r.id == PPC_INST_STWUX || r.id == PPC_INST_STDUX) && r.o[0] == 1 && r.o[1] == 1)
                    {
                        after.backChain = after.sp;
                        after.sp = known && beforeKnown(r.o[2]) ? after.sp + before[r.o[2]] : UNKNOWN;
                    }
                    else if ((r.id == PPC_INST_LWZ || r.id == PPC_INST_LD) && r.o[1] == 0 && r.o[2] == 1)
                    {
                        after.sp = after.backChain;
                        after.backChain = UNKNOWN;
                    }
                    else if (r.id == PPC_INST_ADDI && r.o[0] == 1 && r.o[1] == 1)
                    {
                        after.sp = known ? after.sp + int32_t(r.o[2]) : UNKNOWN;
                    }
                    else
                    {
                        after.sp = UNKNOWN;
                        after.backChain = UNKNOWN;
                    }
                }

                ForEachSuccessor(fn, i, [&](size_t s)
                    {
                        auto& to = states[s];
                        bool c = merge(to.sp, after.sp);
                        c |= merge(to.backChain, after.backChain);
                        for (size_t reg = 0; reg < 32; reg++)
                            c |= merge(to.constants[reg], after.constants[reg]);
                        if ((after.written & ~to.written).any())
                        {
                            to.written |= after.written;
                            c = true;
                        }
                        changed |= c;
                    });
            }
        }

        // The frame bytes instruction i stores to or loads from, and the
        // register, when it addresses the frame at a known offset: d(r1), or
        // (r1 + rB) with rB holding a known constant.
        struct Access
        {
            int64_t slot;
            int width;
            size_t reg;
            bool store;
        };
        auto access = [&](size_t i) -> std::optional<Access>
            {
                const auto& r = raw[i];
                const int64_t sp = states[i].sp;
                if (sp == UNSET || sp == UNKNOWN)
                    return std::nullopt;

                int width = 0;
                size_t file = GPR;
                bool store = false;
                bool indexed = false;
                switch (r.id)
                {
                case PPC_INST_LBZ: width = 1; break;
                case PPC_INST_LHA: case PPC_INST_LHZ: width = 2; break;
                case PPC_INST_LWA: case PPC_INST_LWZ: width = 4; break;
                case PPC_INST_LD: width = 8; break;
                case PPC_INST_LFS: width = 4; file = FPR; break;
                case PPC_INST_LFD: width = 8; file = FPR; break;
                case PPC_INST_STW: width = 4; store = true; break;
                case PPC_INST_STD: width = 8; store = true; break;
                case PPC_INST_STFD: width = 8; file = FPR; store = true; break;
                case PPC_INST_LBZX: width = 1; indexed = true; break;
                case PPC_INST_LHAX: case PPC_INST_LHZX: case PPC_INST_LHBRX: width = 2; indexed = true; break;
                case PPC_INST_LWAX: case PPC_INST_LWZX: case PPC_INST_LWBRX: width = 4; indexed = true; break;
                case PPC_INST_LDX: width = 8; indexed = true; break;
                case PPC_INST_LFSX: width = 4; file = FPR; indexed = true; break;
                case PPC_INST_LFDX: width = 8; file = FPR; indexed = true; break;
                case PPC_INST_LVX: case PPC_INST_LVX128:
                case PPC_INST_LVLX: case PPC_INST_LVLX128:
                case PPC_INST_LVRX: case PPC_INST_LVRX128:
                case PPC_INST_LVEWX: case PPC_INST_LVEWX128:
                    width = 16; file = VR; indexed = true; break;
                // stvlx stores the register up to the end of its 16-byte
                // block; all of it when the function loads it back whole, or
                // the console's own code would hand its caller a broken one.
                case PPC_INST_STVX: case PPC_INST_STVX128:
                case PPC_INST_STVLX: case PPC_INST_STVLX128:
                    width = 16; file = VR; indexed = true; store = true; break;
                default:
                    return std::nullopt;
                }

                if (!indexed)
                {
                    if (r.o[2] != 1)
                        return std::nullopt;
                    return Access{ sp + int32_t(r.o[1]), width, file + r.o[0], store };
                }

                const int64_t offset = states[i].constants[r.o[2]];
                if (r.o[1] != 1 || offset == UNSET || offset == UNKNOWN)
                    return std::nullopt;
                return Access{ sp + offset, width, file + r.o[0], store };
            };

        auto nonVolatile = [](size_t r)
            {
                return (r >= GPR + 14 && r < GPR + 32) || (r >= FPR + 14 && r < FPR + 32) ||
                    (r >= VR + 14 && r < VR + 32) || (r >= VR + 64 && r < VR + 128);
            };

        std::vector<Access> loads;
        for (size_t i = 0; i < n; i++)
        {
            auto a = access(i);
            if (a && !a->store)
                loads.push_back(*a);
        }

        for (size_t i = 0; i < n; i++)
        {
            auto a = access(i);
            if (!a || !a->store || !nonVolatile(a->reg) || states[i].written.test(a->reg))
                continue;

            bool save = true;
            for (auto& load : loads)
            {
                const bool overlaps = load.slot < a->slot + a->width && a->slot < load.slot + load.width;
                if (overlaps && (load.slot != a->slot || load.width != a->width || load.reg != a->reg))
                    save = false;
            }
            if (save)
            {
                fn.instructions[i].uses.reset(a->reg);
                fn.preserved.set(a->reg);
            }
        }
    }

    FunctionFlow Build(const Recompiler& recompiler, const Function& function,
        const std::unordered_map<size_t, uint32_t>& indices, bool& writesSummaryOverflow)
    {
        const auto& config = recompiler.config;
        FunctionFlow fn;
        fn.base = function.base;
        auto symbol = recompiler.image.symbols.find(function.base);
        fn.helper = symbol != recompiler.image.symbols.end() && symbol->address == function.base &&
            (symbol->name.find("__rest") == 0 || symbol->name.find("__save") == 0);
        const size_t n = function.size / 4;
        fn.instructions.resize(n);
        auto& raw = fn.raw;
        raw.resize(n);

        auto* data = (const uint32_t*)recompiler.image.Find(function.base);
        auto switchTable = config.switchTables.end();
        auto inside = [&](size_t address) { return address >= function.base && address < function.base + function.size; };
        auto index = [&](size_t address) { return uint32_t((address - function.base) / 4); };

        for (size_t i = 0; i < n; i++)
        {
            const size_t address = function.base + i * 4;
            auto& out = fn.instructions[i];

            if (config.midAsmHooks.count(address) != 0)
                fn.hooked = true;

            if (switchTable == config.switchTables.end())
                switchTable = config.switchTables.find(address);

            ppc_insn insn;
            ppc::Disassemble(data + i, 4, address, insn);
            if (insn.opcode == nullptr)
                continue;

            int id = insn.opcode->id;
            if (id == PPC_INST_VUPKHSB128 && insn.operands[2] == 0x60) id = PPC_INST_VUPKHSH128;
            else if (id == PPC_INST_VUPKLSB128 && insn.operands[2] == 0x60) id = PPC_INST_VUPKLSH128;

            raw[i] = { id, { insn.operands[0], insn.operands[1], insn.operands[2] } };
            if (id == PPC_INST_MTXER)
                writesSummaryOverflow = true;

            if (!Describe(id, insn, out))
            {
                fn.unhandled = true;
                continue;
            }

            // A call as printFunctionCall emits it: setjmp and longjmp are the
            // host's, and the save/restore helpers are left out of functions
            // whose registers are locals.
            auto call = [&](uint32_t target, Flow callFlow, Flow noCallFlow)
                {
                    if (target == config.longJmpAddress)
                    {
                        out.flow = noCallFlow;
                        return;
                    }
                    if (target == config.setJmpAddress)
                    {
                        fn.callsSetJmp = true;
                        out.flow = noCallFlow;
                        return;
                    }
                    auto symbol = recompiler.image.symbols.find(target);
                    if (symbol == recompiler.image.symbols.end() || symbol->address != target || symbol->type != Symbol_Function)
                    {
                        out.flow = noCallFlow;
                        return;
                    }
                    // The helpers take the return address (__savegprlr) or
                    // the frame (the others) in r12; __restgprlr loads the
                    // return address from the frame.
                    if (symbol->name.find("__rest") == 0 || symbol->name.find("__save") == 0)
                    {
                        if (symbol->name.find("__restgprlr") != 0)
                            out.uses.set(GPR + 12);
                        out.flow = noCallFlow;

                        // __savegprlr_N saves rN-r31, __savefpr_N fN-f31,
                        // __savevmx_N vN-v31 or, from 64, vN-v127.
                        const size_t first = std::stoul(symbol->name.substr(symbol->name.rfind('_') + 1));
                        if (symbol->name.find("__savegprlr_") == 0)
                            for (size_t r = first; r < 32; r++) fn.preserved.set(GPR + r);
                        else if (symbol->name.find("__savefpr_") == 0)
                            for (size_t r = first; r < 32; r++) fn.preserved.set(FPR + r);
                        else if (symbol->name.find("__savevmx_") == 0)
                            for (size_t r = first; r < (first < 32 ? 32 : 128); r++) fn.preserved.set(VR + r);
                        return;
                    }
                    out.flow = callFlow;
                    auto callee = indices.find(target);
                    out.target = callee != indices.end() ? callee->second : NONE;
                };

            const auto& o = insn.operands;
            switch (id)
            {
            case PPC_INST_B:
                if (inside(o[0]))
                {
                    out.flow = Flow::Goto;
                    out.target = index(o[0]);
                }
                else
                {
                    call(o[0], Flow::TailCall, Flow::Return);
                }
                break;

            case PPC_INST_BL:
                call(o[0], Flow::Call, Flow::Next);
                break;

            case PPC_INST_BCTR:
                if (switchTable != config.switchTables.end())
                {
                    out.flow = Flow::Switch;
                    out.uses.reset(CTR);
                    out.uses.set(GPR + switchTable->second.r);
                    auto& cases = fn.switchCases[i];
                    for (auto label : switchTable->second.labels)
                    {
                        if (inside(label))
                            cases.push_back(index(label));
                    }
                    switchTable = config.switchTables.end();
                }
                else
                {
                    out.flow = Flow::IndirectTailCall;
                }
                break;

            case PPC_INST_BCTRL:
                out.flow = Flow::IndirectCall;
                break;

            case PPC_INST_BNECTR:
                out.flow = Flow::CondIndirectTailCall;
                break;

            case PPC_INST_BLR:
                out.flow = Flow::Return;
                break;

            case PPC_INST_BDZLR:
            case PPC_INST_BEQLR:
            case PPC_INST_BGELR:
            case PPC_INST_BGTLR:
            case PPC_INST_BLELR:
            case PPC_INST_BLTLR:
            case PPC_INST_BNELR:
                out.flow = Flow::CondReturn;
                break;

            case PPC_INST_BDZ:
            case PPC_INST_BDNZ:
                out.flow = Flow::CondGoto;
                out.target = inside(o[0]) ? index(o[0]) : NONE;
                break;

            case PPC_INST_BDNZF:
            case PPC_INST_BDZF:
                out.flow = Flow::CondGoto;
                out.target = inside(o[1]) ? index(o[1]) : NONE;
                break;

            case PPC_INST_BEQ:
            case PPC_INST_BGE:
            case PPC_INST_BGT:
            case PPC_INST_BLE:
            case PPC_INST_BLT:
            case PPC_INST_BNE:
                if (inside(o[1]))
                {
                    out.flow = Flow::CondGoto;
                    out.target = index(o[1]);
                }
                else
                {
                    call(o[1], Flow::CondTailCall, Flow::CondReturn);
                }
                break;
            }
        }

        // What the entry cannot reach is no code: a jump table's words decode
        // as loads, and Recompile emits them, but nothing runs them.
        std::vector<bool> reachable(n);
        std::vector<size_t> pending{ 0 };
        while (!pending.empty() && n != 0)
        {
            size_t i = pending.back();
            pending.pop_back();
            if (reachable[i])
                continue;
            reachable[i] = true;
            ForEachSuccessor(fn, i, [&](size_t s) { pending.push_back(s); });
        }
        for (size_t i = 0; i < n; i++)
        {
            if (!reachable[i])
            {
                fn.instructions[i] = Instruction();
                raw[i] = Raw();
            }
        }

        return fn;
    }

    // Backward liveness over one function, with what each call reads as found
    // so far for its callee. live[i] is what is live before instruction i.
    void Liveness(const FunctionFlow& fn, const std::vector<FunctionFlow>& functions, std::vector<Registers>& live)
    {
        const size_t n = fn.instructions.size();
        live.assign(n, Registers());

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (size_t i = n; i-- > 0;)
            {
                const auto& insn = fn.instructions[i];
                Registers out;
                ForEachSuccessor(fn, i, [&](size_t s) { out |= live[s]; });

                // A call writes a register on some paths at most, so what is
                // live after it is live before it too.
                Registers in = insn.uses;
                if (IsDirectCall(insn.flow) && insn.target != NONE)
                    in |= functions[insn.target].liveIn;
                in |= out & ~insn.defs;

                if (in != live[i])
                {
                    live[i] = in;
                    changed = true;
                }
            }
        }
    }

    // The registers whose value right after each instruction can still be in
    // them when the function returns: reach[i].
    void ReachExit(const FunctionFlow& fn, std::vector<Registers>& reach)
    {
        const size_t n = fn.instructions.size();
        Registers all;
        all.set();
        std::vector<Registers> before(n);
        reach.assign(n, Registers());

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (size_t i = n; i-- > 0;)
            {
                const auto& insn = fn.instructions[i];
                Registers after;
                ForEachSuccessor(fn, i, [&](size_t s) { after |= before[s]; });
                switch (insn.flow)
                {
                case Flow::Switch: // the default case returns
                case Flow::Return:
                case Flow::CondReturn:
                case Flow::TailCall:
                case Flow::CondTailCall:
                case Flow::IndirectTailCall:
                case Flow::CondIndirectTailCall:
                    after = all;
                    break;
                default:
                    if (i + 1 == n && insn.flow != Flow::Goto)
                        after = all;
                    break;
                }
                reach[i] = after;
                Registers in = after & ~insn.defs;
                if (in != before[i])
                {
                    before[i] = in;
                    changed = true;
                }
            }
        }
    }
}

std::unordered_set<size_t> FindFunctionsKeepingRegistersInContext(const Recompiler& recompiler)
{
    const auto& config = recompiler.config;

    std::unordered_map<size_t, uint32_t> indices;
    for (size_t i = 0; i < recompiler.functions.size(); i++)
        indices.emplace(recompiler.functions[i].base, uint32_t(i));

    std::vector<FunctionFlow> functions;
    functions.reserve(recompiler.functions.size());
    bool writesSummaryOverflow = false;
    for (auto& function : recompiler.functions)
        functions.push_back(Build(recompiler, function, indices, writesSummaryOverflow));

    // The registers that become locals, as Recompile's r, f, v, cr, ctr, xer
    // and reserved choose them. Summary overflow only counts if something
    // writes it: otherwise it is zero in the context and in a local alike.
    Registers local;
    if (config.nonArgumentRegistersAsLocalVariables)
    {
        for (size_t i : { 0, 2, 11, 12 })
            local.set(GPR + i);
        local.set(FPR + 0);
        for (size_t i = 32; i < 64; i++)
            local.set(VR + i);
    }
    if (config.nonVolatileRegistersAsLocalVariables)
    {
        for (size_t i = 14; i < 32; i++)
        {
            local.set(GPR + i);
            local.set(FPR + i);
            local.set(VR + i);
        }
        for (size_t i = 64; i < 128; i++)
            local.set(VR + i);
    }
    if (config.crRegistersAsLocalVariables)
    {
        for (size_t i = 0; i < 8; i++)
            local.set(CR + i);
    }
    if (config.ctrAsLocalVariable)
        local.set(CTR);
    if (config.xerAsLocalVariable)
    {
        local.set(CA);
        if (writesSummaryOverflow)
            local.set(SO);
    }
    if (config.reservedRegisterAsLocalVariable)
        local.set(RESERVED);

    // The registers a callee may leave changed: all but r1, r13-r31, f14-f31,
    // v14-v31, v64-v127 and cr2-cr4.
    Registers volatileRegisters;
    volatileRegisters.set();
    volatileRegisters.reset(GPR + 1);
    for (size_t i = 13; i < 32; i++)
        volatileRegisters.reset(GPR + i);
    for (size_t i = 14; i < 32; i++)
    {
        volatileRegisters.reset(FPR + i);
        volatileRegisters.reset(VR + i);
    }
    for (size_t i = 64; i < 128; i++)
        volatileRegisters.reset(VR + i);
    for (size_t i = 2; i <= 4; i++)
        volatileRegisters.reset(CR + i);
    const Registers volatileLocal = local & volatileRegisters;

    for (size_t f = 0; f < functions.size(); f++)
    {
        for (auto& insn : functions[f].instructions)
        {
            if (IsDirectCall(insn.flow) && insn.target != NONE)
                functions[insn.target].callers.push_back(uint32_t(f));
        }
    }

    // What each function may write in a volatile register, its callees' writes
    // included. An indirect call adds nothing: a caller cannot know what its
    // target writes, so it keeps nothing in a volatile register across one.
    std::vector<uint32_t> worklist;
    for (size_t f = 0; f < functions.size(); f++)
    {
        for (auto& insn : functions[f].instructions)
            functions[f].mayDef |= insn.defs & volatileRegisters;
        worklist.push_back(uint32_t(f));
    }
    while (!worklist.empty())
    {
        auto& fn = functions[worklist.back()];
        worklist.pop_back();
        Registers mayDef = fn.mayDef;
        for (auto& insn : fn.instructions)
        {
            if (IsDirectCall(insn.flow) && insn.target != NONE)
                mayDef |= functions[insn.target].mayDef;
        }
        if (mayDef != fn.mayDef)
        {
            fn.mayDef = mayDef;
            worklist.insert(worklist.end(), fn.callers.begin(), fn.callers.end());
        }
    }

    for (auto& fn : functions)
    {
        DropSaves(fn, functions, volatileRegisters);
        fn.raw = {};
        for (auto& insn : fn.instructions)
        {
            insn.uses &= local;
            insn.defs &= local;
        }
    }

    // What each function may return holding a value it wrote, or a callee
    // did. An indirect callee follows the calling convention, which returns
    // nothing in a register that becomes a local.
    std::vector<Registers> reach;
    for (size_t f = 0; f < functions.size(); f++)
        worklist.push_back(uint32_t(f));
    while (!worklist.empty())
    {
        auto& fn = functions[worklist.back()];
        worklist.pop_back();
        ReachExit(fn, reach);
        Registers exits;
        for (size_t i = 0; i < fn.instructions.size(); i++)
        {
            const auto& insn = fn.instructions[i];
            exits |= insn.defs & reach[i];
            if (IsDirectCall(insn.flow) && insn.target != NONE)
                exits |= functions[insn.target].exits & reach[i];
        }
        // A non-volatile register comes back changed only from a function
        // that writes it without saving it, which the calling convention
        // rules out but nothing here assumes.
        exits &= volatileLocal | (local & ~volatileRegisters & ~fn.preserved);
        if (exits != fn.exits)
        {
            fn.exits = exits;
            worklist.insert(worklist.end(), fn.callers.begin(), fn.callers.end());
        }
    }

    // What is live at each function's entry, through its calls.
    std::vector<Registers> live;
    for (size_t f = 0; f < functions.size(); f++)
        worklist.push_back(uint32_t(f));
    while (!worklist.empty())
    {
        auto& fn = functions[worklist.back()];
        worklist.pop_back();
        Liveness(fn, functions, live);
        Registers liveIn = live.empty() ? Registers() : live[0];
        if (liveIn != fn.liveIn)
        {
            fn.liveIn = liveIn;
            worklist.insert(worklist.end(), fn.callers.begin(), fn.callers.end());
        }
    }

    std::vector<bool> keep(functions.size());
    size_t readAtEntry = 0, passToCallee = 0, readFromCallee = 0, passIndirectly = 0, other = 0;
    // The callees a value comes back from, and in which registers.
    std::vector<std::pair<uint32_t, Registers>> returned;

    for (size_t f = 0; f < functions.size(); f++)
    {
        auto& fn = functions[f];
        const size_t n = fn.instructions.size();
        if (fn.helper)
            continue;
        Liveness(fn, functions, live);

        bool reads = fn.liveIn.any();
        bool passes = false, takes = false, passesIndirectly = false;
        for (size_t i = 0; i < n; i++)
        {
            const auto& insn = fn.instructions[i];
            if (IsDirectCall(insn.flow) && insn.target != NONE)
            {
                if (functions[insn.target].liveIn.any())
                    passes = true;

                if (insn.flow == Flow::Call && i + 1 < n)
                {
                    Registers back = live[i + 1] & functions[insn.target].exits;
                    if (back.any())
                    {
                        takes = true;
                        returned.emplace_back(insn.target, back);
                    }
                }
            }
            else if (insn.flow == Flow::IndirectCall && i + 1 < n && (live[i + 1] & volatileLocal).any())
            {
                takes = true;
            }
        }

        // A value an indirect call takes: one written to a general, floating
        // point or vector register and then read by nothing in the function
        // before the call. A compiler writes no such register it does not
        // read, so the target reads it. (The carry and a record form's cr0 are
        // written on the side, and often never read.)
        Registers explicitDestinations;
        for (size_t r = GPR; r < CR; r++)
            explicitDestinations.set(r);
        if (!reads && !passes)
        {
            std::vector<Registers> reaching(n);
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (size_t i = 0; i < n; i++)
                {
                    const auto& insn = fn.instructions[i];
                    Registers after;
                    ForEachSuccessor(fn, i, [&](size_t s) { after |= live[s]; });
                    Registers out = (insn.defs & explicitDestinations & ~after) | (reaching[i] & ~insn.defs);
                    ForEachSuccessor(fn, i, [&](size_t s)
                        {
                            if ((out & ~reaching[s]).any())
                            {
                                reaching[s] |= out;
                                changed = true;
                            }
                        });
                }
            }
            for (size_t i = 0; i < n; i++)
            {
                if (IsIndirectCall(fn.instructions[i].flow) && reaching[i].any())
                    passesIndirectly = true;
            }
        }

        if (reads) readAtEntry++;
        else if (passes) passToCallee++;
        else if (passesIndirectly) passIndirectly++;
        else if (takes) readFromCallee++;
        else if (fn.callsSetJmp || fn.unhandled || fn.hooked) other++;

        keep[f] = reads || passes || takes || passesIndirectly || fn.callsSetJmp || fn.unhandled || fn.hooked;
    }

    // A value read after a call has to be written to the context by the
    // function whose write it is: the callee, or one of its callees in turn.
    std::vector<Registers> visited(functions.size());
    while (!returned.empty())
    {
        auto [f, regs] = returned.back();
        returned.pop_back();
        if ((regs & ~visited[f]).none())
            continue;
        visited[f] |= regs;

        const auto& fn = functions[f];
        ReachExit(fn, reach);
        for (size_t i = 0; i < fn.instructions.size(); i++)
        {
            const auto& insn = fn.instructions[i];
            if ((insn.defs & reach[i] & regs).any())
                keep[f] = true;
            if (IsDirectCall(insn.flow) && insn.target != NONE)
            {
                Registers through = regs & functions[insn.target].exits & reach[i];
                if (through.any())
                    returned.emplace_back(insn.target, through);
            }
        }
    }

    std::unordered_set<size_t> result;
    for (size_t f = 0; f < functions.size(); f++)
    {
        if (keep[f])
            result.emplace(functions[f].base);
    }

    const size_t helpers = std::count_if(functions.begin(), functions.end(), [](auto& fn) { return fn.helper; });
    fmt::println("Registers stay in the context in {} of {} functions: {} read one at entry, {} pass one to a callee, "
        "{} to an indirect call, {} read one a callee wrote and {} write it, {} call setjmp or are unhandled or hooked",
        result.size(), functions.size() - helpers, readAtEntry, passToCallee, passIndirectly, readFromCallee,
        result.size() - readAtEntry - passToCallee - passIndirectly - readFromCallee - other, other);

    return result;
}
