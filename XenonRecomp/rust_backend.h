#pragma once

#include "recompiler.h"
#include "rust_cfg.h"

// Writes the recompiled code as a Cargo workspace instead of C++ files. It shares
// everything up to the printing with Recompiler: the analysis, the switch tables
// and the register flow (register_flow.cpp), so both backends agree on what each
// function does. Recompile() below follows Recompiler::Recompile() case by case.
//
//   Cargo.toml, .cargo/config.toml    the workspace
//   ppc_rt/                           the types and helpers (XenonUtils/rust/)
//   chunks/cNNN/                      256 functions each, a crate each
//   ppc_recomp/                       links them, and holds PPCFuncMappings
//   cpp/                              any function with a mid-asm hook, as C++
//
// One crate per chunk because rustc scales worse than linearly with crate size,
// and because chunks build in parallel. They call each other by symbol, so there
// is no dependency cycle, and the runtime can stay in C++, Rust or both.
//
// See docs/rust-backend.md.
struct RustRecompiler : Recompiler
{
    // Where ppc_rt.rs and hooks.rs are read from.
    std::filesystem::path runtimeDirectory;
    // Replaces hooks.rs, if not empty. Relative to the TOML, like its other paths.
    std::string hooksFilePath;

    size_t fallbackFunctions = 0;
    rustcfg::Stats structureTotals{};

    void Recompile(const std::filesystem::path& headerFilePath);

    // True for a function that has to go through the C++ backend: one with a
    // mid-asm hook, or a setjmp that is not a plain call in a function that keeps
    // its registers in the context, which no compiled title has.
    bool NeedsCppFallback(const Function& fn) const;

    // One function's Rust, added to `externs` with the symbols it calls.
    std::string RecompileFunction(const Function& fn, std::set<std::string>& externs);

    // The generated files that do not depend on the functions.
    void WriteRuntimeCrate(const std::filesystem::path& headerFilePath);
    void WriteChunk(size_t index, const std::string& code, const std::set<std::string>& externs, const std::vector<std::string>& defined);
    void WriteWorkspace(size_t chunkCount, const std::filesystem::path& headerFilePath);
    void WriteCppFallback(const std::string& code, const std::filesystem::path& headerFilePath);

    // Replaces the image with a synthetic one and writes it through both
    // backends, for XenonTests/rust_backend. See rust_selftest.cpp.
    void WriteSelfTest(const std::filesystem::path& directory, const std::filesystem::path& headerFilePath, uint32_t seed, size_t perOpcode, size_t graphs);

private:
    enum class Flow : uint8_t { None, Jump, Cond, Switch, Return };

    // The addresses execution continues at after each setjmp of the function
    // being recompiled.
    std::vector<uint32_t> setjmpResumes;

    struct PendingFlow
    {
        Flow kind = Flow::None;
        std::string cond;
        uint32_t target = 0;
        std::vector<uint32_t> targets; // Switch: guest addresses, 0 to return
    };

    bool Emit(
        const Function& fn,
        uint32_t base,
        const ppc_insn& insn,
        const uint32_t* data,
        std::unordered_map<uint32_t, RecompilerSwitchTable>::iterator& switchTable,
        RecompilerLocalVariables& localVariables,
        CSRState& csrState,
        std::set<std::string>& externs,
        PendingFlow& flow);
};
