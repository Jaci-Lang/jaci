// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Luau
{

enum class BundleMode
{
    Auto,    // Direct bundle if available/preferred, or native C++ toolchain
    Direct,  // Pure self-contained standalone binary packaging (zero external toolchain required)
    Native,  // Native C++ compilation via C++ compiler toolchain (MSVC, Clang, GCC, Zig)
};

struct SingleBinaryOptions
{
    std::string entryFilePath;
    std::string outputBinaryPath;
    std::string targetArchitecture; // e.g. "windows-x64", "windows-msvc", "windows-gui", "linux-x64", "macos-arm64", "direct", etc.
    std::string compilerCommand;    // optional explicit compiler command override (e.g. "cl.exe", "clang++", "zig c++", "g++")
    std::string customStubPath;     // optional custom base executable / runner stub path
    std::vector<std::string> assetPaths; // files/directories to embed into the binary VFS
    std::vector<std::string> embedPaths; // module files/directories to embed beyond the static require graph
    BundleMode bundleMode = BundleMode::Auto;
    int optimizationLevel = 1;
    int debugLevel = 1;
    bool codegen = true;
    bool verbose = false;
    bool windowed = false;          // Windows GUI / main window application mode (subsystem:windows, no console window)
    bool strip = true;             // Strip debug symbols from output binary for minimal artifact size
    bool compress = true;          // Compress bytecode and asset payload using LZ VFS compression
    bool optimizeForSize = false;  // Optimize compiler code generation for minimal size (-Os / /O1)
};

class SingleBinaryCompiler
{
public:
    static bool compile(const SingleBinaryOptions& options);

    // Check if the current executable contains an embedded/appended Jaci bundle payload.
    // If so, deserializes and executes it, returning the process exit code.
    // If not, returns std::nullopt so the host CLI can proceed normally.
    static std::optional<int> checkAndRunBundledPayload(int argc, char** argv);
};

} // namespace Luau
