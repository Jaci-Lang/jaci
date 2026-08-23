// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/LlvmData.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

// LLVM-based code generation engine.
//
// The engine owns the LLVM target machine and the optimization pipeline.
// It compiles LLVM IR (text or in-memory module) to ELF relocatable objects
// and can place them into executable memory. The JIT pipeline (LlvmJit)
// reuses the same engine and targets the Jaci code allocator instead of the
// engine's private executable pages.
class LlvmEngine
{
public:
    LlvmEngine();
    ~LlvmEngine();

    // Register targets, create the target machine for the host triple.
    // Safe to call multiple times; returns true when the engine is usable.
    bool initialize();

    bool isInitialized() const
    {
        return engineInitialized;
    }

    // Last error message when a compile/load operation failed.
    const std::string& lastError() const
    {
        return lastErrorMessage;
    }

    // Run the optimization pipeline over `module` (takes ownership of the
    // LLVMContext via the module) and emit an ELF relocatable object.
    // Returns the object bytes, or an empty string on failure.
    //
    // `modulePtr` is an opaque handle created by createModuleFromIrText;
    // it must be released with releaseModule.
    void* createModuleFromIrText(const std::string& irText);
    void releaseModule(void* modulePtr);
    std::string compileModuleToNativeObject(void* modulePtr, OptLevel level = OptLevel::O2);

    // Create an empty in-memory module for the target triple (data layout
    // included) for direct C++ API lowering. The handle is consumed by
    // compileModuleToNativeObject or released with releaseModule.
    void* createModuleForLowering();

    // The llvm::Module behind a module handle; cast the result to
    // llvm::Module* in translation units that include the LLVM C++ API.
    void* getModuleFromHandle(void* modulePtr) const;

    // Convenience: parse IR text, optimize, emit object, place it into a
    // private executable page, and return the entry point address. The
    // engine keeps the page alive until releaseExecutable is called.
    void* compileFunction(const std::string& irCode, const std::string& entrySymbol, OptLevel level = OptLevel::O2);
    void releaseExecutable(void* entry);

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
    struct Impl;
    Impl* pImpl = nullptr;

    BackendMode currentMode = BackendMode::Llvm;
    bool engineInitialized = false;
    std::string lastErrorMessage;

    // private executable pages owned by this engine (test/benchmark use)
    struct ExecutableRegion
    {
        void* base = nullptr;
        size_t size = 0;
        std::vector<void*> entries;
    };
    std::vector<ExecutableRegion> regions;
};

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
