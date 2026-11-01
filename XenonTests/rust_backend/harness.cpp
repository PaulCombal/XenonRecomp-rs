// Runs every case of a selftest image on hostile inputs and writes one line per
// run: the same program linked with the C++ backend's output and with the Rust
// backend's, whose two output files compare.py then compares.
//
// Guest memory is all readable, zero but for the arena, and a store anywhere else
// faults: a fault's address is part of the outcome, so a wild store is compared
// too. Registers are compared after a clean return.

#include <csetjmp>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "ppc_recomp_shared.h"

struct Case
{
    uint32_t address;
    int kind;
    const char* text;
};

static const Case cases[] = {
#include "cases.inc"
};

static constexpr uint32_t ARENA = 0x40000000;
static constexpr uint32_t ARENA_SIZE = 0x20000;

// Beyond the arena, out of the comparison: C++ fills a jmp_buf, Rust does not.
static constexpr uint32_t JMP_BUF = ARENA + ARENA_SIZE;
static constexpr uint32_t JMP_BUF_SIZE = 0x10000;

static uint8_t* guest;
static uint8_t pristine[ARENA_SIZE];
static sigjmp_buf recover;
static volatile sig_atomic_t faultKind;
static volatile uint64_t faultAddress;

static uint64_t timebase;
extern "C" uint64_t PPC_QueryTimebase() { return ++timebase; }

static void OnSignal(int signal, siginfo_t* info, void*)
{
    faultKind = signal;
    faultAddress = signal == SIGSEGV ? uint64_t((uint8_t*)info->si_addr - guest) & 0xFFFFFFFF : 0;
    siglongjmp(recover, 1);
}

// xorshift, seeded per run so that both binaries see the same inputs.
struct Random
{
    uint64_t s;
    uint64_t Next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t Below(uint32_t n) { return uint32_t(Next() >> 16) % n; }
};

static uint64_t Integer(Random& r, int style)
{
    static const uint64_t special[] = { 0, 1, ~0ull, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x100000000, 2, 31, 32, 63, 64 };
    switch (style)
    {
    case 0: return ARENA + 0x8000 + r.Below(0x8000);             // an address in the arena
    case 1: return r.Below(0x8000);                              // a small index
    case 2: return special[r.Below(sizeof(special) / sizeof(special[0]))];
    default: return r.Next();
    }
}

static uint64_t Double(Random& r)
{
    static const uint64_t special[] = {
        0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
        0x7FF8000000000000, 0xFFF8000000000000, 0x7FF0000000000001, 0x0000000000000001, 0x800FFFFFFFFFFFFF,
        0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000, 0xC3E0000000000000, 0x47F0000000000000 };
    switch (r.Below(5))
    {
    case 0: return special[r.Below(sizeof(special) / sizeof(special[0]))];
    case 1: { double d = double(int32_t(r.Next())) / 1024.0; uint64_t u; memcpy(&u, &d, 8); return u; }
    case 2: { float f = float(int32_t(r.Next() >> 40)) / 64.0f; double d = f; uint64_t u; memcpy(&u, &d, 8); return u; }
    default: return r.Next();
    }
}

static uint32_t Float(Random& r)
{
    static const uint32_t special[] = { 0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 1, 0x807FFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0x477FE000, 0x47800000, 0x40400000, 0x404000FF };
    switch (r.Below(4))
    {
    case 0: return special[r.Below(sizeof(special) / sizeof(special[0]))];
    case 1: { float f = float(int32_t(r.Next() >> 44)) / 256.0f; uint32_t u; memcpy(&u, &f, 4); return u; }
    default: return uint32_t(r.Next());
    }
}

static void Input(Random& r, PPCContext& c, const Case& test, uint32_t csr, uint32_t stubAddress)
{
    memset((void*)&c, 0, sizeof(c));
#ifndef PPC_CONFIG_SKIP_MSR
    c.msr = uint32_t(r.Next());
#endif
    c.lr = r.Next();

    auto* gpr = &c.r3;
    const int style = int(r.Below(5));
    for (size_t i = 0; i < 32; i++)
        gpr[i].u64 = Integer(r, style == 4 ? int(r.Below(4)) : style);

    c.ctr.u64 = test.kind == 1 ? stubAddress : r.Below(4);
    c.xer.so = r.Below(2);
    c.xer.ov = r.Below(2);
    c.xer.ca = r.Below(2);
    c.reserved.u64 = r.Next();

    auto* cr = &c.cr0;
    for (size_t i = 0; i < 8; i++)
    {
        cr[i].lt = r.Below(2);
        cr[i].gt = r.Below(2);
        cr[i].eq = r.Below(2);
        cr[i].so = r.Below(2);
    }

    auto* fpr = &c.f0;
    for (size_t i = 0; i < 32; i++)
        fpr[i].u64 = Double(r);

    auto* vr = &c.v0;
    const int vectorStyle = int(r.Below(3));
    for (size_t i = 0; i < 128; i++)
    {
        for (size_t lane = 0; lane < 4; lane++)
        {
            vr[i].u32[lane] = vectorStyle == 0 ? Float(r) : vectorStyle == 1 ? uint32_t(r.Next()) : r.Below(64) * 0x01010101u;
        }
    }

    if (test.kind == 3)
    {
        for (size_t i = 0; i < 32; i++)
            gpr[i].u64 &= 0x7FFFFFFF;
        for (size_t i = 0; i < 128; i++)
        {
            for (size_t lane = 0; lane < 4; lane++)
                vr[i].u32[lane] &= 0x7FFFFFFF;
        }
    }

    if (test.kind == 4)
        c.r3.u64 = JMP_BUF;

    if (test.kind == 2)
    {
        c.r31.u64 = 16 + r.Below(240);
        c.r3.u64 = r.Next();
    }

    c.fpscr.csr = csr;
}

struct Hash
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    void Add(uint64_t v)
    {
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 47;
    }
    void Bytes(const void* p, size_t size)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        for (; size >= 8; size -= 8, b += 8)
        {
            uint64_t v;
            memcpy(&v, b, 8);
            Add(v);
        }
        for (; size > 0; size--, b++)
            Add(*b);
    }
};

// A NaN's sign and payload are what x86 leaves to operand order, which the
// compilers choose, so the second hash ignores them.
static uint64_t Canonical(PPCContext& c)
{
    PPCContext copy;
    memcpy((void*)&copy, (void*)&c, sizeof(c));

    auto* fpr = &copy.f0;
    for (size_t i = 0; i < 32; i++)
    {
        if ((fpr[i].u64 & 0x7FFFFFFFFFFFFFFF) > 0x7FF0000000000000)
            fpr[i].u64 = 0x7FF8000000000000;
    }

    auto* vr = &copy.v0;
    for (size_t i = 0; i < 128; i++)
    {
        for (size_t lane = 0; lane < 4; lane++)
        {
            if ((vr[i].u32[lane] & 0x7FFFFFFF) > 0x7F800000)
                vr[i].u32[lane] = 0x7FC00000;
        }
    }

    Hash h;
    h.Bytes(&copy, sizeof(copy));
    return h.h;
}

static PPCFunc* Find(uint32_t address)
{
    for (PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; m++)
    {
        if (m->guest == address)
            return m->host;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    const uint32_t inputs = argc > 2 ? uint32_t(atoi(argv[2])) : 64;
    const int dump = argc > 3 ? atoi(argv[3]) : -1; // a case, whose context to print
    const uint32_t dumpInput = argc > 4 ? uint32_t(atoi(argv[4])) : 0;

    FILE* out = fopen(argv[1], "w");

    guest = (uint8_t*)mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (guest == MAP_FAILED)
        return 1;

    // The function table and the arena stay writable.
    const uint64_t tableStart = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    const uint64_t tableEnd = (tableStart + PPC_CODE_SIZE * 2 + 0xFFFF) & ~0xFFFFull;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; m++)
        *(PPCFunc**)(guest + tableStart + (m->guest - PPC_CODE_BASE) * 2) = m->host;

    // Host pointers differ between the two programs, so a load must not see them.
    // Only the cases that make an indirect call need the table.
    const auto table = [&](int protection) { mprotect(guest + (tableStart & ~0xFFFFull), tableEnd - (tableStart & ~0xFFFFull), protection); };

    mprotect(guest, ARENA, PROT_READ);
    mprotect(guest + JMP_BUF + JMP_BUF_SIZE, PPC_MEMORY_SIZE - JMP_BUF - JMP_BUF_SIZE, PROT_READ);

    table(PROT_NONE);

    Random fill{ 0x123456789ABCDEF1ull };
    for (size_t i = 0; i < ARENA_SIZE; i++)
        pristine[i] = uint8_t(fill.Next());
    memcpy(guest + ARENA, pristine, ARENA_SIZE);

    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);

    uint32_t stubAddress = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr && stubAddress == 0; m++)
        stubAddress = uint32_t(m->guest);

    static PPCContext ctx;
    static const uint32_t csrs[] = { 0x1F80, 0x1F80, 0x9FC0, 0x3F80, 0x5F80, 0x7F80, 0xBFC0 };

    const size_t count = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < count; i++)
    {
        const Case& test = cases[i];
        PPCFunc* function = Find(test.address);
        if (function == nullptr)
        {
            fprintf(stderr, "no function at %X\n", test.address);
            return 1;
        }

        if (getenv("HARNESS_TRACE") != nullptr)
            fprintf(stderr, "case %zu [%s]\n", i, test.text);

        if (test.kind == 1)
            table(PROT_READ);

        for (uint32_t input = 0; input < inputs; input++)
        {
            Random r{ (uint64_t(i) + 1) * 0x9E3779B97F4A7C15ull ^ (uint64_t(input + 1) * 0xC2B2AE3D27D4EB4Full) };
            r.Next();
            const uint32_t csr = csrs[r.Below(sizeof(csrs) / sizeof(csrs[0]))];
            Input(r, ctx, test, csr, stubAddress);
            timebase = 1000;
            faultKind = 0;
            faultAddress = 0;

            if (sigsetjmp(recover, 1) == 0)
            {
                __asm__ volatile("ldmxcsr %0" : : "m"(csr));
                function(ctx, guest);
            }
            uint32_t endCsr;
            __asm__ volatile("stmxcsr %0" : "=m"(endCsr));
            const uint32_t restore = 0x1F80;
            __asm__ volatile("ldmxcsr %0" : : "m"(restore));

            // The exception flags are sticky and the compilers evaluate
            // different things at compile time: they say nothing.
            ctx.fpscr.csr &= ~0x3Fu;
            endCsr &= ~0x3Fu;

            Hash memory;
            memory.Bytes(guest + ARENA, ARENA_SIZE);

            uint64_t state;
            uint64_t canonical;
            if (faultKind != 0)
            {
                // The context is whatever the fault left: the compilers keep
                // context writes in registers as long as they like.
                state = canonical = 0;
            }
            else
            {
                Hash raw;
                raw.Bytes(&ctx, sizeof(ctx));
                raw.Add(endCsr);
                state = raw.h;
                canonical = Canonical(ctx) ^ endCsr;
            }

            fprintf(out, "%zu %u %d %llx %llx %llx %llx\n", i, input, int(faultKind), (unsigned long long)faultAddress,
                (unsigned long long)memory.h, (unsigned long long)state, (unsigned long long)canonical);

            if (int(i) == dump && input == dumpInput)
            {
                printf("fault %d at %llx\n", int(faultKind), (unsigned long long)faultAddress);
                const auto* words = reinterpret_cast<const uint64_t*>(&ctx);
                for (size_t w = 0; w < sizeof(ctx) / 8; w++)
                    printf("%04zx %016llx\n", w * 8, (unsigned long long)words[w]);
                for (size_t b = 0; b < ARENA_SIZE; b++)
                {
                    if (guest[ARENA + b] != pristine[b])
                        printf("mem %05zx %02x\n", b, guest[ARENA + b]);
                }
            }

            memcpy(guest + ARENA, pristine, ARENA_SIZE);
        }

        if (test.kind == 1)
            table(PROT_NONE);
    }

    fclose(out);
    return 0;
}
