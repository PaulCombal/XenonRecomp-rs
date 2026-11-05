#include "pch.h"
#include "rust_backend.h"

// The files around the recompiled functions: see the layout in rust_backend.h.

namespace
{
    constexpr size_t CHUNK_SIZE = 256; // as Recompiler::Recompile splits its files

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.good())
            throw std::runtime_error(fmt::format("cannot read {}", path.string()));

        std::stringstream ss;
        ss << stream.rdbuf();
        return ss.str();
    }

    std::string Crate(size_t chunk)
    {
        return fmt::format("ppc_recomp_c{:03}", chunk);
    }

    // PPCContext's fields in order, each with its size and alignment, as
    // ppc_context.h declares them. The two must agree: both sides assert it.
    struct Field
    {
        std::string name;
        std::string type;
        size_t size;
        size_t align;
    };

    std::vector<Field> ContextFields(const RecompilerConfig& config)
    {
        std::vector<Field> fields;

        auto gpr = [&](size_t i) { fields.push_back({ fmt::format("r{}", i), "PPCRegister", 8, 8 }); };
        gpr(3);
        gpr(0);
        gpr(1);
        gpr(2);
        for (size_t i = 4; i < 32; i++)
            gpr(i);

        if (!config.skipLr)
            fields.push_back({ "lr", "u64", 8, 8 });
        fields.push_back({ "ctr", "PPCRegister", 8, 8 });
        fields.push_back({ "xer", "PPCXERRegister", 3, 1 });
        fields.push_back({ "reserved", "PPCRegister", 8, 8 });
        if (!config.skipMsr)
            fields.push_back({ "msr", "u32", 4, 4 });
        for (size_t i = 0; i < 8; i++)
            fields.push_back({ fmt::format("cr{}", i), "PPCCRRegister", 4, 1 });
        fields.push_back({ "fpscr", "PPCFPSCRRegister", 4, 4 });
        for (size_t i = 0; i < 32; i++)
            fields.push_back({ fmt::format("f{}", i), "PPCRegister", 8, 8 });
        for (size_t i = 0; i < 128; i++)
            fields.push_back({ fmt::format("v{}", i), "PPCVRegister", 16, 16 });

        return fields;
    }

    struct ContextLayout
    {
        size_t fpscrOffset = 0;
        size_t lastVectorOffset = 0;
        size_t size = 0;
    };

    ContextLayout ComputeContextLayout(const std::vector<Field>& fields)
    {
        ContextLayout layout;
        size_t offset = 0;
        for (const Field& field : fields)
        {
            offset = (offset + field.align - 1) & ~(field.align - 1);
            if (field.name == "fpscr")
                layout.fpscrOffset = offset;
            if (field.name == "v127")
                layout.lastVectorOffset = offset;
            offset += field.size;
        }
        layout.size = (offset + 63) & ~size_t(63);
        return layout;
    }
}

void RustRecompiler::WriteRuntimeCrate(const std::filesystem::path& headerFilePath)
{
    println("[package]");
    println("name = \"ppc_rt\"");
    println("version = \"0.0.0\"");
    println("edition = \"2024\"");
    println("publish = false");
    println("\n[lib]");
    println("path = \"src/lib.rs\"");
    SaveCurrentOutData("ppc_rt/Cargo.toml");

    out = ReadFile(runtimeDirectory / "ppc_rt.rs");
    SaveCurrentOutData("ppc_rt/src/lib.rs");

    out = ReadFile(hooksFilePath.empty() ? runtimeDirectory / "hooks.rs" : std::filesystem::path(config.directoryPath) / hooksFilePath);
    SaveCurrentOutData("ppc_rt/src/hooks.rs");

    auto [codeMin, codeMax] = CodeRange();
    println("pub const PPC_IMAGE_BASE: u64 = 0x{:X};", image.base);
    println("pub const PPC_IMAGE_SIZE: u64 = 0x{:X};", image.size);
    println("pub const PPC_CODE_BASE: u64 = 0x{:X};", codeMin);
    println("pub const PPC_CODE_SIZE: u64 = 0x{:X};\n", codeMax - codeMin);

    const auto fields = ContextFields(config);
    println("#[repr(C, align(64))]");
    println("pub struct PPCContext {{");
    for (const Field& field : fields)
        println("\tpub {}: {},", field.name, field.type);
    println("}}\n");

    const ContextLayout layout = ComputeContextLayout(fields);
    println("const _: () = assert!(core::mem::size_of::<PPCContext>() == {});", layout.size);
    println("const _: () = assert!(core::mem::offset_of!(PPCContext, fpscr) == {});", layout.fpscrOffset);
    println("const _: () = assert!(core::mem::offset_of!(PPCContext, v127) == {});", layout.lastVectorOffset);
    SaveCurrentOutData("ppc_rt/src/context.rs");
}

void RustRecompiler::WriteChunk(size_t index, const std::string& code, const std::set<std::string>& externs, const std::vector<std::string>& defined)
{
    println("[package]");
    println("name = \"{}\"", Crate(index));
    println("version = \"0.0.0\"");
    println("edition = \"2024\"");
    println("publish = false");
    println("\n[lib]");
    println("path = \"src/lib.rs\"");
    println("\n[dependencies]");
    println("ppc_rt = {{ path = \"../../ppc_rt\" }}");
    SaveCurrentOutData(fmt::format("chunks/c{:03}/Cargo.toml", index));

    println("#![no_std]");
    println("#![allow(warnings)]");
    println("#![allow(unconditional_panic, arithmetic_overflow)]");
    println("use ppc_rt::*;\n");

    // sub_X is a weak alias of __imp__sub_X, as the C++ backend makes it: a hook
    // is a strong sub_X anywhere in the program, and it reaches the original as
    // __imp__sub_X. rustc has no stable way to say either, the assembler does.
    if (!defined.empty())
    {
        println("core::arch::global_asm!(");
        for (const std::string& name : defined)
        {
            println("\t\".weak {}\",", name);
            println("\t\".set {0}, __imp__{0}\",", name);
        }
        println(");\n");
    }

    if (!externs.empty())
    {
        println("unsafe extern \"C\" {{");
        for (const std::string& name : externs)
            println("\tfn {}(ctx: &mut PPCContext, base: *mut u8);", name);
        println("}}\n");
    }

    out += code;
    SaveCurrentOutData(fmt::format("chunks/c{:03}/src/lib.rs", index));
}

void RustRecompiler::WriteWorkspace(size_t chunkCount, const std::filesystem::path& headerFilePath)
{
    println("# Generated by XenonRecomp.");
    println("[workspace]");
    println("resolver = \"2\"");
    println("members = [\"ppc_rt\", \"ppc_recomp\", \"chunks/*\"]\n");
    println("[profile.release]");
    println("opt-level = 3");
    println("codegen-units = 1");
    println("incremental = false");
    println("debug = false");
    // No frame of generated code may have a landing pad: longjmp goes through them.
    println("panic = \"abort\"");
    SaveCurrentOutData("Cargo.toml");

    println("[build]");
    println("rustflags = [\"-Ctarget-cpu=x86-64-v2\"]");
    SaveCurrentOutData(".cargo/config.toml");

    // The compiler the output was tested with: rustc has miscompiled it before
    // (docs/rust-backend.md). Run union_locals.rs before moving this.
    println("[toolchain]");
    println("channel = \"1.97.1\"");
    println("targets = [\"x86_64-pc-windows-gnullvm\"]");
    SaveCurrentOutData("rust-toolchain.toml");

    println("[package]");
    println("name = \"ppc_recomp\"");
    println("version = \"0.0.0\"");
    println("edition = \"2024\"");
    println("publish = false");
    println("build = \"build.rs\"");
    println("\n[lib]");
    println("path = \"src/lib.rs\"");
    println("crate-type = [\"rlib\", \"staticlib\"]");
    println("\n[dependencies]");
    println("ppc_rt = {{ path = \"../ppc_rt\" }}");
    for (size_t i = 0; i < chunkCount; i++)
        println("{0} = {{ path = \"../chunks/c{1:03}\" }}", Crate(i), i);
    SaveCurrentOutData("ppc_recomp/Cargo.toml");

    println("#![allow(warnings)]");
    println("pub use ppc_rt::*;\n");
    for (size_t i = 0; i < chunkCount; i++)
        println("extern crate {};", Crate(i));

    size_t count = 1;
    println("\nunsafe extern \"C\" {{");
    for (auto& symbol : image.symbols)
    {
        println("\tfn {}(ctx: &mut PPCContext, base: *mut u8);", symbol.name);
        count++;
    }
    println("}}\n");

    // Every function, hooks included, by the name a call uses.
    println("#[unsafe(no_mangle)]");
    println("pub static PPCFuncMappings: [PPCFuncMapping; {}] = [", count);
    for (auto& symbol : image.symbols)
        println("\tPPCFuncMapping {{ guest: 0x{:X}, host: Some({}) }},", symbol.address, symbol.name);
    println("\tPPCFuncMapping {{ guest: 0, host: None }},");
    println("];");
    SaveCurrentOutData("ppc_recomp/src/lib.rs");

    const auto simde = std::filesystem::absolute(headerFilePath).lexically_normal().parent_path().parent_path() / "thirdparty" / "simde";
    out = ReadFile(runtimeDirectory / "build.rs");
    constexpr std::string_view marker = "@SIMDE@";
    out.replace(out.find(marker), marker.size(), simde.generic_string());
    SaveCurrentOutData("ppc_recomp/build.rs");
}

void RustRecompiler::WriteCppFallback(const std::string& code, const std::filesystem::path& headerFilePath)
{
    WriteConfigHeader("cpp/include/");
    WriteContextHeader(headerFilePath, "cpp/include/");

    println("#pragma once\n");
    println("#include \"ppc_config.h\"");
    println("#include \"ppc_context.h\"\n");
    println("extern \"C\" {{");
    for (auto& symbol : image.symbols)
        println("PPC_FUNC({});", symbol.name);
    println("}}");
    SaveCurrentOutData("cpp/include/ppc_recomp_shared.h");

    const ContextLayout layout = ComputeContextLayout(ContextFields(config));

    // A title that keeps its own ppc_recomp_shared.h, for its own
    // PPC_CALL_INDIRECT_FUNC, builds this file against that one.
    println("// The functions the Rust backend leaves to C++: those with a mid-asm hook.\n");
    println("#include <cstddef>");
    println("#include \"ppc_recomp_shared.h\"\n");
    println("static_assert(sizeof(PPCContext) == {});", layout.size);
    println("static_assert(offsetof(PPCContext, fpscr) == {});", layout.fpscrOffset);
    println("static_assert(offsetof(PPCContext, v127) == {});\n", layout.lastVectorOffset);
    out += code;
    SaveCurrentOutData("cpp/ppc_fallback.cpp");
}

void RustRecompiler::Recompile(const std::filesystem::path& headerFilePath)
{
    out.reserve(10 * 1024 * 1024);

    const auto root = std::filesystem::path(config.directoryPath) / config.outDirectoryPath;
    const size_t neededChunks = (functions.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (config.rustChunkCount != 0 && config.rustChunkCount < neededChunks)
        throw std::runtime_error(fmt::format("rust_chunk_count is {}, the functions need {} chunks", config.rustChunkCount, neededChunks));
    const size_t chunkCount = std::max(neededChunks, config.rustChunkCount);

    for (const char* directory : { ".cargo", "ppc_rt/src", "ppc_recomp/src", "cpp/include" })
        std::filesystem::create_directories(root / directory);
    for (size_t i = 0; i < chunkCount; i++)
        std::filesystem::create_directories(root / fmt::format("chunks/c{:03}/src", i));

    WriteRuntimeCrate(headerFilePath);
    WriteWorkspace(chunkCount, headerFilePath);

    FindRegisterFlow();

    std::string fallback;
    std::string code;
    std::set<std::string> externs;
    std::vector<std::string> defined;

    for (size_t i = 0; i < functions.size(); i++)
    {
        if ((i % 2048) == 0 || (i == (functions.size() - 1)))
            fmt::println("Recompiling functions... {}%", static_cast<float>(i + 1) / functions.size() * 100.0f);

        const Function& fn = functions[i];

        if (NeedsCppFallback(fn))
        {
            std::swap(out, fallback);
            Recompiler::Recompile(fn);
            std::swap(out, fallback);
            fallbackFunctions++;
        }
        else
        {
            auto symbol = image.symbols.find(fn.base);
            defined.push_back(symbol != image.symbols.end() ? symbol->name : fmt::format("sub_{}", fn.base));
            code += RecompileFunction(fn, externs);
        }

        if ((i % CHUNK_SIZE) == CHUNK_SIZE - 1 || i == functions.size() - 1)
        {
            WriteChunk(i / CHUNK_SIZE, code, externs, defined);
            code.clear();
            externs.clear();
            defined.clear();
        }
    }

    // The empty crates rust_chunk_count asks for past the last function.
    for (size_t i = neededChunks; i < chunkCount; i++)
        WriteChunk(i, {}, {}, {});

    WriteCppFallback(fallback, headerFilePath);

    fmt::println("Rust backend: {} functions in {} crates, {} in C++ ({} irreducible regions, {} edges through a dispatcher, nesting depth {})",
        functions.size() - fallbackFunctions, chunkCount, fallbackFunctions,
        structureTotals.irreducibleRegions, structureTotals.dispatchEdges, structureTotals.maxDepth);
}
