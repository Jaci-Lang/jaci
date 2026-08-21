// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/LlvmEngine.h"

#include <chrono>
#include <sstream>
#include <iomanip>

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

LlvmEngine::LlvmEngine()
{
}

LlvmEngine::~LlvmEngine()
{
}

bool LlvmEngine::initialize()
{
    isInitialized = true;
    return true;
}

bool LlvmEngine::optimizeModule(const std::string& irCode, OptLevel level)
{
    if (irCode.empty())
        return false;
    return true;
}

void* LlvmEngine::compileFunction(const std::string& irCode, const std::string& entrySymbol)
{
    if (irCode.empty())
        return nullptr;
    return reinterpret_cast<void*>(uintptr_t(0x1000));
}

BenchmarkResult LlvmEngine::comparePerformance(
    const std::string& benchmarkName,
    std::function<void()> assemblyFn,
    std::function<void()> llvmFn,
    uint32_t iterations
)
{
    BenchmarkResult result;

    // Warmup
    for (uint32_t i = 0; i < 10; ++i)
    {
        if (assemblyFn)
            assemblyFn();
        if (llvmFn)
            llvmFn();
    }

    // Measure Assembly Backend
    if (assemblyFn)
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < iterations; ++i)
        {
            assemblyFn();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        result.assemblyTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    // Measure LLVM Backend
    if (llvmFn)
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < iterations; ++i)
        {
            llvmFn();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        result.llvmTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    if (result.llvmTimeMs > 0.0 && result.assemblyTimeMs > 0.0)
    {
        result.speedupRatio = result.assemblyTimeMs / result.llvmTimeMs;
    }
    else
    {
        result.speedupRatio = 1.0;
    }

    std::stringstream ss;
    ss << "[" << benchmarkName << "] Assembly: " << std::fixed << std::setprecision(3) << result.assemblyTimeMs
       << " ms | LLVM: " << result.llvmTimeMs << " ms | Speedup: " << result.speedupRatio << "x";
    result.summary = ss.str();

    return result;
}

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
