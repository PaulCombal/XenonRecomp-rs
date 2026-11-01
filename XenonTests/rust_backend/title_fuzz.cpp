// Runs each function of one chunk of a real title from random machine states, in
// a forked child, so that a fault or an endless loop stays contained. Built twice,
// with the C++ chunk and with the Rust one, and title_fuzz.py compares the output.
//
// Unlike harness.cpp this sees functions as the title has them, registers in
// locals, which is where a rustc bug hid from the instruction tests. Every call
// out of the chunk is a stub that records it and returns values derived from it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "ppc_recomp_shared.h"

struct Entry
{
    const char* name;
    uint32_t id;
    PPCFunc* function;
};
extern Entry g_entries[];
extern size_t g_count;

static uint64_t g_trace = 0x9E3779B97F4A7C15ull;
static uint64_t g_calls = 0;

static inline uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 32;
    return h;
}

extern "C" void stub_call(uint32_t id, PPCContext& ctx)
{
    if (++g_calls > 200000)
        _exit(77);
    g_trace = mix(g_trace, id);
    g_trace = mix(g_trace, ctx.r3.u32);
    g_trace = mix(g_trace, ctx.r4.u32);
    g_trace = mix(g_trace, ctx.r5.u32);
    g_trace = mix(g_trace, ctx.r6.u32);
    // A NaN's sign and payload are the compilers' choice; everything after this
    // depends on the trace, so they stay out of it.
    g_trace = mix(g_trace, ctx.f1.f64 != ctx.f1.f64 ? 0x7FF8000000000000ull : ctx.f1.u64);
    ctx.r3.u64 = uint32_t(g_trace);
    ctx.r4.u64 = uint32_t(g_trace >> 32);
    ctx.f1.f64 = double(int32_t(g_trace)) / 1024.0;
}

extern "C" void stub_indirect(uint32_t address, PPCContext& ctx) { stub_call(address ^ 0x80000000u, ctx); }
extern "C" uint64_t PPC_QueryTimebase() { return ++g_calls; }

struct Rng
{
    uint64_t s;
    uint64_t next()
    {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

static constexpr uint32_t ARENA = 0x40000000, ARENA_SIZE = 1 << 20;

struct Result
{
    uint64_t raw;       // registers, as they are
    uint64_t canonical; // the same with every NaN alike
    uint64_t memory;    // pages written
    uint64_t memoryCanonical; // the same with what reads as a NaN alike
};

static Result Run(PPCFunc* function, uint64_t seed)
{
    auto* base = (uint8_t*)mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED)
        _exit(99);

    Rng r{ seed };
    for (uint32_t i = 0; i < ARENA_SIZE; i += 8)
    {
        uint64_t v = r.next();
        memcpy(base + ARENA + i, &v, 8);
    }
    // Words that point back into the arena, byte-swapped as the guest keeps them,
    // so that a chain of dereferences stays on data worth reading.
    for (uint32_t i = 0; i < ARENA_SIZE; i += 16)
    {
        uint32_t p = __builtin_bswap32(ARENA + uint32_t(r.next() % (ARENA_SIZE - 0x4000)));
        memcpy(base + ARENA + i, &p, 4);
    }

    alignas(0x40) static PPCContext ctx;
    memset((void*)&ctx, 0, sizeof(ctx));
#define PTR() (ARENA + uint32_t(r.next() % (ARENA_SIZE - 0x4000)))
#define VAL() ((r.next() & 1) ? uint64_t(uint32_t(r.next())) : (r.next() & 0xFF))
    ctx.r1.u64 = ARENA + ARENA_SIZE / 2;
    ctx.r13.u64 = PTR();
    PPCRegister* gp[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
    for (auto* p : gp)
        p->u64 = (r.next() & 3) ? PTR() : VAL();
    PPCRegister* fp[] = { &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8 };
    for (auto* p : fp)
        p->f64 = double(int64_t(r.next()) >> 40) / 256.0;
    PPCVRegister* vp[] = { &ctx.v1, &ctx.v2, &ctx.v3, &ctx.v4, &ctx.v5, &ctx.v6, &ctx.v7, &ctx.v8 };
    for (auto* p : vp)
    {
        for (int i = 0; i < 4; i++)
            p->f32[i] = float(int32_t(r.next()) >> 12) / 256.0f;
    }
    ctx.lr = 0x82000000;
    ctx.msr = 0x200A000;
    ctx.fpscr.csr = ctx.fpscr.getcsr();

    struct itimerval timer { { 0, 0 }, { 0, 250000 } };
    setitimer(ITIMER_REAL, &timer, nullptr);
    function(ctx, base);
    timer = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &timer, nullptr);

    static FILE* dump = getenv("FUZZ_DUMP") ? fopen(getenv("FUZZ_DUMP"), "wb") : nullptr;
    if (dump != nullptr)
    {
        fwrite(&ctx, sizeof(ctx), 1, dump);
        fwrite(&g_trace, 8, 1, dump);
    }

    Result result{};
    result.raw = g_trace;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&ctx);
    ctx.fpscr.csr &= ~0x3Fu;
    for (size_t i = 0; i < sizeof(ctx); i++)
        result.raw = mix(result.raw, bytes[i]);

    auto* fpr = &ctx.f0;
    for (size_t i = 0; i < 32; i++)
    {
        if ((fpr[i].u64 & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull)
            fpr[i].u64 = 0x7FF8000000000000ull;
    }
    auto* vr = &ctx.v0;
    for (size_t i = 0; i < 128; i++)
    {
        for (size_t lane = 0; lane < 4; lane++)
        {
            if ((vr[i].u32[lane] & 0x7FFFFFFF) > 0x7F800000)
                vr[i].u32[lane] = 0x7FC00000;
        }
    }
    result.canonical = g_trace;
    for (size_t i = 0; i < sizeof(ctx); i++)
        result.canonical = mix(result.canonical, bytes[i]);

    // Every resident page that is not all zero.
    static std::vector<unsigned char> resident(0x100000);
    mincore(base, 0x100000000ull, resident.data());
    for (size_t page = 0; page < resident.size(); page++)
    {
        if ((resident[page] & 1) == 0)
            continue;
        const auto* q = reinterpret_cast<const uint64_t*>(base + (page << 12));
        uint64_t h = 0, any = 0;
        for (int i = 0; i < 512; i++)
        {
            h = mix(h, q[i]);
            any |= q[i];
        }
        if (any != 0)
        {
            result.memory ^= mix(page, h);

            // Big-endian words, as floats, and as the high word of a double.
            uint64_t canonical = 0;
            const auto* word = reinterpret_cast<const uint32_t*>(q);
            for (int i = 0; i < 1024; i++)
            {
                uint32_t w = __builtin_bswap32(word[i]);
                if ((w & 0x7FFFFFFF) > 0x7F800000)
                {
                    canonical = mix(canonical, 0x7FC00000);
                    if ((w & 0x7FF00000) == 0x7FF00000 && i + 1 < 1024)
                        i++;
                }
                else
                {
                    canonical = mix(canonical, w);
                }
            }
            result.memoryCanonical ^= mix(page, canonical);

            if (dump != nullptr)
            {
                const uint64_t address = page << 12;
                fwrite(&address, 8, 1, dump);
                fwrite(q, 4096, 1, dump);
            }
        }
    }
    return result;
}

int main(int argc, char** argv)
{
    const int seeds = atoi(argv[1]);

    // One function and seed, in this process, for FUZZ_DUMP.
    if (argc > 3)
    {
        for (size_t i = 0; i < g_count; i++)
        {
            if (strcmp(g_entries[i].name, argv[2]) == 0)
                Run(g_entries[i].function, (uint64_t(g_entries[i].id) << 20) | uint64_t(atoi(argv[3])));
        }
        return 0;
    }

    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    for (size_t i = 0; i < g_count; i++)
    {
        const Entry& entry = g_entries[i];
        for (int s = 0; s < seeds; s++)
        {
            int fds[2];
            if (pipe(fds) != 0)
                return 1;
            fflush(stdout);
            const pid_t pid = fork();
            if (pid == 0)
            {
                close(fds[0]);
                const Result result = Run(entry.function, (uint64_t(entry.id) << 20) | uint64_t(s));
                (void)!write(fds[1], &result, sizeof(result));
                _exit(0);
            }
            close(fds[1]);
            Result result{};
            const ssize_t got = read(fds[0], &result, sizeof(result));
            close(fds[0]);
            int status;
            waitpid(pid, &status, 0);
            int kind = 0;
            if (WIFSIGNALED(status))
                kind = WTERMSIG(status);
            else if (WEXITSTATUS(status) == 77)
                kind = 77;
            else if (got != sizeof(result))
                kind = 1000;
            printf("%s %d %d %llx %llx %llx %llx\n", entry.name, s, kind, (unsigned long long)result.raw,
                (unsigned long long)result.canonical, (unsigned long long)result.memory, (unsigned long long)result.memoryCanonical);
        }
    }
    return 0;
}
