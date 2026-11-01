#include "pch.h"
#include "rust_backend.h"
#include <random>

// Writes a synthetic image through both backends, for XenonTests/rust_backend to
// run against each other:
//
// - Every encoding family of the opcode table that the backends handle, several
//   random encodings of each, as a function of its own. Branches are aimed at
//   the end of the function or at a stub, so both ways out are taken.
// - lwarx/stwcx. and ldarx/stdcx. pairs, whose store can succeed.
// - Random control flow graphs, jump tables and multi-entry loops among them,
//   which end when a counter runs out. A hash of the path taken stays in r3.
//
// Everything stays in the context, where the harness can see it.

extern "C" const powerpc_opcode powerpc_opcodes[];
extern "C" const int powerpc_num_opcodes;

namespace
{
    constexpr uint32_t IMAGE_BASE = 0x82000000;
    constexpr uint32_t CODE_BASE = 0x82010000;
    constexpr uint32_t STUB_SPACING = 0x4000; // well within a conditional branch

    constexpr uint32_t NOP = 0x60000000;
    constexpr uint32_t EIEIO = 0x7C0006AC;
    constexpr uint32_t BLR = 0x4E800020;

    // CASE_TAME keeps inputs where the C++ output is what the guest would see, for
    // what Rust does better and a comparison would only report as a difference:
    // C++ adds `base + rA + rB` for the atomics, which does not wrap at 4 GB, and
    // overflows a signed int (undefined) packing a vector far from 3.0f.
    enum CaseClass { CASE_PLAIN = 0, CASE_INDIRECT = 1, CASE_GRAPH = 2, CASE_TAME = 3, CASE_JUMP = 4 };

    struct Case
    {
        uint32_t address;
        int kind;
        std::string text;
    };

    struct Builder
    {
        std::vector<uint32_t> words; // host order
        std::vector<std::pair<uint32_t, uint32_t>> ranges; // address, size
        std::vector<uint32_t> stubs;

        uint32_t Here() const { return CODE_BASE + uint32_t(words.size()) * 4; }

        uint32_t NearestStub(uint32_t address) const
        {
            return stubs[(address - CODE_BASE) / STUB_SPACING];
        }

        // One stub per STUB_SPACING, so that one is always in a conditional
        // branch's reach. Call before a function.
        void Align()
        {
            const size_t index = (Here() - CODE_BASE) / STUB_SPACING;
            if (index < stubs.size())
                return;

            const uint32_t stub = Here();
            ranges.emplace_back(stub, 8);
            words.push_back(0x63DE0001); // ori r30,r30,1
            words.push_back(BLR);
            stubs.resize(index + 1, stub);
        }

        void Add(const std::vector<uint32_t>& code)
        {
            ranges.emplace_back(Here(), uint32_t(code.size()) * 4);
            words.insert(words.end(), code.begin(), code.end());
        }
    };
}

void RustRecompiler::WriteSelfTest(const std::filesystem::path& directory, const std::filesystem::path& headerFilePath, uint32_t seed, size_t perOpcode, size_t graphs)
{
    std::mt19937 random(seed);
    Builder builder;
    std::vector<Case> cases;

    // Which ids Emit handles, found by asking it.
    std::set<int> handled;
    {
        Function dummy{ CODE_BASE, 16 };
        const uint32_t nothing[2] = { 0, 0 };
        auto noTable = config.switchTables.end();
        RecompilerLocalVariables locals;
        CSRState csr = CSRState::Unknown;
        std::set<std::string> externs;
        PendingFlow flow;
        for (int i = 0; i < powerpc_num_opcodes; i++)
        {
            ppc_insn insn{};
            insn.opcode = &powerpc_opcodes[i];
            if (Emit(dummy, CODE_BASE, insn, nothing, noTable, locals, csr, externs, flow))
                handled.insert(powerpc_opcodes[i].id);
            out.clear();
        }
    }

    std::set<uint32_t> seen;

    auto addCase = [&](const std::vector<uint32_t>& code, int kind, const std::string& text)
        {
            builder.Align();
            cases.push_back({ builder.Here(), kind, text });
            builder.Add(code);
        };

    for (int entry = 0; entry < powerpc_num_opcodes; entry++)
    {
        const powerpc_opcode& opcode = powerpc_opcodes[entry];
        if (handled.count(opcode.id) == 0)
            continue;

        size_t made = 0;
        for (size_t attempt = 0; attempt < perOpcode * 40 && made < perOpcode; attempt++)
        {
            builder.Align();
            const uint32_t address = builder.Here();

            uint32_t word = uint32_t(opcode.opcode) | (random() & ~uint32_t(opcode.mask));
            const uint32_t major = word >> 26;

            if (major == 16) // bc: to the end of the function, or a stub, not linking
            {
                // The C++ backend cannot leave the function with a branch that
                // also decrements the counter, which is BO's 0x04 clear.
                const bool decrements = (word & (1u << 23)) == 0;
                const uint32_t target = !decrements && (random() % 4) == 0 ? builder.NearestStub(address) : address + 12;
                word = (word & ~0xFFFFu) | ((target - address) & 0xFFFC);
            }
            else if (major == 18) // b and bl
            {
                const bool link = (word & 1) != 0;
                const uint32_t target = link || (random() % 2) == 0 ? builder.NearestStub(address) : address + 12;
                word = (word & 0xFC000000) | ((target - address) & 0x03FFFFFC) | (link ? 1 : 0);
            }

            // A negative displacement from no register is below `base` in C++ and
            // near the top of the 4 GB in Rust, which is what the guest would mean.
            if (major >= 32 && ((word >> 16) & 0x1F) == 0)
                word &= ~0x8000u;

            const uint32_t swapped = ByteSwap(word);
            ppc_insn insn;
            ppc::Disassemble(&swapped, 4, address, insn);
            if (insn.opcode == nullptr || handled.count(insn.opcode->id) == 0)
                continue;
            if (insn.opcode->id == PPC_INST_VPKD3D128 && insn.operands[2] == 5 && insn.operands[4] > 2)
                continue; // writes outside the register in C++, and traps in Rust
            if (insn.opcode->id == PPC_INST_CLRLDI && insn.operands[2] == 0)
                continue; // the generator's own shift by 64
            if (!seen.insert(word).second)
                continue;

            const int id = insn.opcode->id;
            int kind = CASE_PLAIN;
            if (id == PPC_INST_BCTR || id == PPC_INST_BCTRL || id == PPC_INST_BNECTR)
                kind = CASE_INDIRECT;
            else if (id == PPC_INST_LWARX || id == PPC_INST_LDARX || id == PPC_INST_STWCX || id == PPC_INST_STDCX || id == PPC_INST_VPKD3D128)
                kind = CASE_TAME;

            // addi r31,r31,1 shows the fall through was taken.
            addCase({ word, (random() % 4) == 0 ? EIEIO : NOP, 0x3BFF0001, BLR }, kind, fmt::format("{} {}", insn.opcode->name, insn.op_str));
            made++;
        }
    }

    // What random operands rarely reach: mr and not are or and nor with both
    // sources the same.
    for (size_t i = 0; i < perOpcode; i++)
    {
        const uint32_t rS = random() % 32, rA = random() % 32;
        addCase({ 0x7C000378 | (rS << 21) | (rA << 16) | (rS << 11) | uint32_t(i & 1), NOP, 0x3BFF0001, BLR }, CASE_PLAIN, "mr");
        addCase({ 0x7C0000F8 | (rS << 21) | (rA << 16) | (rS << 11) | uint32_t(i & 1), NOP, 0x3BFF0001, BLR }, CASE_PLAIN, "not");
    }
    // A reservation that can hold: rD = (rA), then a store of rS to the same place.
    for (size_t i = 0; i < perOpcode; i++)
    {
        const uint32_t rD = 3 + random() % 8, rS = 3 + random() % 8, rA = 3 + random() % 8;
        addCase({ 0x7C000028 | (rD << 21) | (rA << 11), 0x7C00012D | (rS << 21) | (rA << 11), NOP, BLR }, CASE_TAME, "lwarx + stwcx.");
        addCase({ 0x7C0000A8 | (rD << 21) | (rA << 11), 0x7C0001AD | (rS << 21) | (rA << 11), NOP, BLR }, CASE_TAME, "ldarx + stdcx.");
    }

    // setjmp and longjmp, with r3 a jmp_buf: the plain way there and back, from a
    // function further down, around a loop, and with two buffers in force.
    {
        const auto mr = [](uint32_t a, uint32_t s) { return 0x7C000378 | (s << 21) | (a << 16) | (s << 11); };
        const auto li = [](uint32_t d, uint32_t v) { return 0x38000000 | (d << 21) | v; };
        const auto bl = [](uint32_t from, uint32_t to) { return 0x48000001 | ((to - from) & 0x03FFFFFC); };
        const uint32_t ADD_R30 = 0x3BDE0001;  // addi r30,r30,1
        const uint32_t CMP_R3 = 0x2F030000;   // cmpwi cr6,r3,0
        const auto bne = [](uint32_t from, uint32_t to) { return 0x409A0000 | ((to - from) & 0xFFFC); };

        builder.Align();
        const uint32_t setjmpStub = builder.Here();
        builder.Add({ BLR });
        const uint32_t longjmpStub = builder.Here();
        builder.Add({ BLR });
        config.setJmpAddress = setjmpStub;
        config.longJmpAddress = longjmpStub;

        for (uint32_t value : { 0u, 1u, 7u, 0x7FFFu })
        {
            builder.Align();
            uint32_t a = builder.Here();
            auto at = [&](uint32_t i) { return a + i * 4; };
            addCase({ mr(31, 3), li(29, 0x11), bl(at(2), setjmpStub), ADD_R30, CMP_R3, bne(at(5), at(10)),
                li(29, 0x22), mr(3, 31), li(4, value), bl(at(9), longjmpStub), mr(28, 3), BLR }, CASE_JUMP, "setjmp, longjmp");

            builder.Align();
            const uint32_t helper = builder.Here();
            builder.Add({ mr(3, 31), li(4, value), bl(helper + 8, longjmpStub), BLR });
            builder.Align();
            a = builder.Here();
            addCase({ mr(31, 3), bl(at(1), setjmpStub), ADD_R30, CMP_R3, bne(at(4), at(7)), li(29, 0x22),
                bl(at(6), helper), mr(28, 3), BLR }, CASE_JUMP, "longjmp from a callee");

            builder.Align();
            a = builder.Here();
            addCase({ mr(31, 3), li(27, 3), mr(3, 31), bl(at(3), setjmpStub), CMP_R3, bne(at(5), at(12)),
                0x3B7BFFFF, 0x2F9B0000, 0x409E0000 | ((at(2) - at(8)) & 0xFFFC), // r27--, around while r27 != 0
                mr(3, 31), li(4, value), bl(at(11), longjmpStub), mr(28, 3), BLR }, CASE_JUMP, "setjmp in a loop");

            builder.Align();
            a = builder.Here();
            addCase({ mr(31, 3), 0x3B430200, bl(at(2), setjmpStub), CMP_R3, bne(at(4), at(13)), // r26 = r3 + 512
                mr(3, 26), bl(at(6), setjmpStub), ADD_R30, CMP_R3, bne(at(9), at(14)),
                mr(3, 26), li(4, value), bl(at(12), longjmpStub),
                BLR, mr(3, 31), li(4, 9), bl(at(16), longjmpStub), BLR }, CASE_JUMP, "two buffers");
        }
    }

    // The graphs. A slot is seven instructions; see the words below.
    for (size_t graph = 0; graph < graphs; graph++)
    {
        builder.Align();
        const uint32_t address = builder.Here();
        const uint32_t slots = 3 + random() % 40;
        constexpr uint32_t SLOT_WORDS = 7;

        auto target = [&]() { return address + (random() % slots) * SLOT_WORDS * 4; };

        std::vector<uint32_t> code;
        for (uint32_t slot = 0; slot < slots; slot++)
        {
            const uint32_t here = address + slot * SLOT_WORDS * 4;
            code.push_back(0x3BFFFFFF);                  // addi r31,r31,-1
            code.push_back(0x2F9F0000);                  // cmpwi cr7,r31,0
            code.push_back(0x4D9E0020);                  // beqlr cr7
            code.push_back(0x1C630000 | (random() & 0x7FFF) | 1); // mulli r3,r3,odd
            code.push_back(0x38630000 | (slot + 1));     // addi r3,r3,slot

            const uint32_t last = here + 6 * 4;
            switch (random() % 6)
            {
            case 0:
            case 1:
            case 2: // andi. r4,r3,bit; bne target
                code.push_back(0x70640000 | (1u << (random() % 8)));
                code.push_back(0x40820000 | ((target() - last) & 0xFFFC));
                break;

            case 3: // b target
                code.push_back(NOP);
                code.push_back(0x48000000 | ((target() - last) & 0x03FFFFFC));
                break;

            case 4: // a jump table on the low two bits
            {
                code.push_back(0x546507BE); // clrlwi r5,r3,30
                code.push_back(0x4E800420); // bctr
                auto& table = config.switchTables[last];
                table.r = 5;
                for (size_t i = 0; i < 4; i++)
                    table.labels.push_back(target());
                break;
            }

            default: // fall through
                code.push_back(NOP);
                code.push_back(NOP);
                break;
            }
        }
        code.push_back(BLR);
        cases.push_back({ address, CASE_GRAPH, fmt::format("graph {} of {} slots", graph, slots) });
        builder.Add(code);
    }

    // The image.
    image.base = IMAGE_BASE;
    const uint32_t codeSize = uint32_t(builder.words.size()) * 4;
    image.size = (CODE_BASE - IMAGE_BASE) + ((codeSize + 0xFFFF) & ~0xFFFFu);
    auto* code = new uint8_t[codeSize]; // lives as long as the process
    for (size_t i = 0; i < builder.words.size(); i++)
        reinterpret_cast<uint32_t*>(code)[i] = ByteSwap(builder.words[i]);
    image.Map(".text", CODE_BASE - IMAGE_BASE, codeSize, SectionFlags_Code, code);

    for (auto [begin, size] : builder.ranges)
    {
        Function fn{};
        fn.base = begin;
        fn.size = size;
        functions.push_back(fn);
        image.symbols.emplace(fmt::format("sub_{:X}", begin), begin, size, Symbol_Function);
    }

    config.directoryPath.clear();
    config.outDirectoryPath = directory.string();
    std::filesystem::create_directories(directory / "cxx");

    Recompile(headerFilePath);

    for (auto& fn : functions)
        Recompiler::Recompile(fn);

    println("PPCFuncMapping PPCFuncMappings[] = {{");
    for (auto& symbol : image.symbols)
        println("\t{{ 0x{:X}, {} }},", symbol.address, symbol.name);
    println("\t{{ 0, nullptr }}");
    println("}};");
    {
        std::string body;
        std::swap(body, out);
        println("#include \"ppc_recomp_shared.h\"\n");
        out += body;
    }
    SaveCurrentOutData("cxx/selftest.cpp");

    for (const Case& test : cases)
    {
        std::string text = test.text;
        for (char& c : text)
        {
            if (c == '"' || c == '\\')
                c = ' ';
        }
        println("{{ 0x{:X}, {}, \"{}\" }},", test.address, test.kind, text);
    }
    SaveCurrentOutData("cases.inc");

    fmt::println("{} opcode ids handled, {} cases", handled.size(), cases.size());
}
