// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#pragma once

#include <string>
#include <vector>

namespace Luau
{

struct SingleBinaryOptions
{
    std::string entryFilePath;
    std::string outputBinaryPath;
    std::string targetArchitecture; // e.g. "linux-x64", "linux-arm64", "windows-x64", "macos-arm64", or compiler prefix
    std::vector<std::string> assetPaths; // files/directories to embed into the binary VFS
    int optimizationLevel = 1;
    int debugLevel = 1;
    bool codegen = true;
    bool verbose = false;
};

class SingleBinaryCompiler
{
public:
    static bool compile(const SingleBinaryOptions& options);
};

} // namespace Luau
