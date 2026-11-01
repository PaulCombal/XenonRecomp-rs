#include "pch.h"
#include "rust_backend.h"
#include "test_recompiler.h"

int main(int argc, char* argv[])
{
    // Options, which override the TOML, come first and are taken off the arguments.
    std::string backend;
    std::string outDirectory;
    std::string rustHooks;
    std::string selfTest;
    uint32_t seed = 1;
    while (argc > 1 && strncmp(argv[1], "--", 2) == 0)
    {
        const std::string_view option = argv[1];
        if (option.rfind("--backend=", 0) == 0)
            backend = option.substr(10);
        else if (option.rfind("--out=", 0) == 0)
            outDirectory = option.substr(6);
        else if (option.rfind("--rust-hooks=", 0) == 0)
            rustHooks = option.substr(13);
        else if (option.rfind("--selftest=", 0) == 0)
            selfTest = option.substr(11);
        else if (option.rfind("--seed=", 0) == 0)
            seed = uint32_t(atoi(argv[1] + 7));
        else
        {
            fmt::println("Unknown option {}", option);
            return EXIT_FAILURE;
        }
        argv[1] = argv[0];
        argv++;
        argc--;
    }

    if (!selfTest.empty())
    {
        if (argc < 2)
        {
            printf("Usage: XenonRecomp --selftest=directory [--seed=n] [PPC context header file path]");
            return EXIT_FAILURE;
        }

        RustRecompiler recompiler;
        recompiler.runtimeDirectory = std::filesystem::path(argv[1]).parent_path() / "rust";
        recompiler.WriteSelfTest(selfTest, argv[1], seed, 16, 400);
        return EXIT_SUCCESS;
    }

#ifndef XENON_RECOMP_CONFIG_FILE_PATH
    if (argc < 3)
    {
        printf("Usage: XenonRecomp [--backend=cpp|rust] [--out=directory] [--rust-hooks=file] [input TOML file path] [PPC context header file path]");
        return EXIT_SUCCESS;
    }
#endif

    const char* path = 
    #ifdef XENON_RECOMP_CONFIG_FILE_PATH
        XENON_RECOMP_CONFIG_FILE_PATH
    #else
        argv[1]
    #endif
        ;

    if (std::filesystem::is_regular_file(path))
    {
        RustRecompiler recompiler;
        if (!recompiler.LoadConfig(path))
            return -1;

        if (!backend.empty())
            recompiler.config.backend = backend;
        if (!outDirectory.empty())
            recompiler.config.outDirectoryPath = outDirectory;
        if (!rustHooks.empty())
            recompiler.config.rustHooksFilePath = rustHooks;

        if (recompiler.config.backend != "cpp" && recompiler.config.backend != "rust")
        {
            fmt::println("Unknown backend {}", recompiler.config.backend);
            return EXIT_FAILURE;
        }

        recompiler.Analyse();

        auto entry = recompiler.image.symbols.find(recompiler.image.entry_point);
        if (entry != recompiler.image.symbols.end())
        {
            entry->name = "_xstart";
        }

        const char* headerFilePath =
#ifdef XENON_RECOMP_HEADER_FILE_PATH
            XENON_RECOMP_HEADER_FILE_PATH
#else
            argv[2]
#endif
            ;

        if (recompiler.config.backend == "rust")
        {
            recompiler.runtimeDirectory = std::filesystem::path(headerFilePath).parent_path() / "rust";
            recompiler.hooksFilePath = recompiler.config.rustHooksFilePath;
            recompiler.Recompile(headerFilePath);
        }
        else
        {
            recompiler.Recompiler::Recompile(headerFilePath);
        }
    }
    else
    {
        TestRecompiler::RecompileTests(path, argv[2]);
    }

    return EXIT_SUCCESS;
}
