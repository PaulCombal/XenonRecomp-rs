#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Rust has no goto. A function's control flow is rebuilt here from its basic
// blocks into labeled blocks and loops, whose break and continue reach every
// edge of a reducible graph. See rust_cfg.cpp.
namespace rustcfg
{
    constexpr int RETURN = -1;

    enum class Term : uint8_t
    {
        Jump,   // to succ[0]
        Cond,   // to succ[0] if cond, else to succ[1]
        Switch, // to succ[i] if selector == values[i], else return
    };

    struct Block
    {
        // Names the block's labels; unique in the function.
        uint32_t id = 0;
        std::string body;
        Term term = Term::Jump;
        std::string cond;              // Cond: a Rust expression; Switch: the selector
        std::vector<uint32_t> values;  // Switch: one per successor
        std::vector<int> succ;         // block indices, or RETURN
    };

    struct Stats
    {
        size_t irreducibleRegions = 0; // regions that needed a dispatcher
        size_t dispatchEdges = 0;      // edges that go through one
        size_t maxDepth = 0;
    };

    // The body of a Rust function: statements that always end by returning, so
    // nothing may follow them. Block 0 is the entry. When stats->dispatchEdges is
    // not zero the output assigns and reads a u32 local named __state, which the
    // caller declares.
    std::string Structure(std::vector<Block> blocks, Stats* stats = nullptr);
}
