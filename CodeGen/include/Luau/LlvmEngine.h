// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/LlvmData.h"

#include <string>
#include <vector>
#include <functional>

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

enum class OptLevel : uint8_t
{
    O0,
    O1,
    O2,
    O3,
};

enum class BackendMode : uint8_t
{
    Assembly,
    Llvm,
    Comparative,
};

struct BenchmarkResult
{
    double assemblyTimeMs = 0.0;
    double llvmTimeMs = 0.0;
    double speedupRatio = 1.0;
    std::string summary;
};

class LlvmEngine
{
public:
    LlvmEngine();
    ~LlvmEngine();

    bool initialize();
    bool optimizeModule(const std::string& irCode, OptLevel level = OptLevel::O3);
    void* compileFunction(const std::string& irCode, const std::string& entrySymbol);

    // Performance comparison utilities
    BenchmarkResult comparePerformance(
        const std::string& benchmarkName,
        std::function<void()> assemblyFn,
        std::function<void()> llvmFn,
        uint32_t iterations = 1000
    );

    BackendMode getBackendMode() const
    {
        return currentMode;
    }
    void setBackendMode(BackendMode mode)
    {
        currentMode = mode;
    }

private:
    BackendMode currentMode = BackendMode::Llvm;
    bool isInitialized = false;
};

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
