// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "LlvmJit.h"

#include "Luau/Common.h"

#include <cstring>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if (defined(__linux__) || defined(__APPLE__)) && (defined(CODEGEN_TARGET_X64) || defined(CODEGEN_TARGET_A64))
#ifndef LUAU_ENABLE_REGISTER_FRAME
#define REGISTER_FRAME_WEAK __attribute__((weak))
#else
#define REGISTER_FRAME_WEAK
#endif

extern "C" void __register_frame(const void*) REGISTER_FRAME_WEAK;
extern "C" void __deregister_frame(const void*) REGISTER_FRAME_WEAK;
#endif

LUAU_FASTFLAG(LuauCodegenProtectData)

namespace Luau
{
namespace CodeGen
{
namespace
{

#if !defined(_WIN32)
size_t getPageSize()
{
    long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? size_t(page) : size_t(4096);
}

void protectRange(void* mem, size_t size, int prot)
{
    mprotect(mem, size, prot);
}
#else
size_t getPageSize()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return size_t(info.dwPageSize);
}

void protectRange(void* mem, size_t size, int prot)
{
    DWORD winProt = prot & PROT_EXEC ? (prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (prot & PROT_WRITE ? PAGE_READWRITE : PAGE_READONLY);
    DWORD old;
    VirtualProtect(mem, size, winProt, &old);
}
#endif

} // namespace

LlvmJitCompileResult compileLlvmProtos(const std::vector<Proto*>& protos, Llvm::OptLevel level)
{
    LlvmJitCompileResult result;

    if (protos.empty())
    {
        result.success = true;
        return result;
    }

    // The engine is process-wide and not thread-safe: serialize compilation
    static std::mutex compileMutex;
    std::lock_guard<std::mutex> lock(compileMutex);

    static Llvm::LlvmEngine* engine = [] -> Llvm::LlvmEngine* {
        auto* e = new Llvm::LlvmEngine();
        if (!e->initialize())
        {
            delete e;
            return nullptr;
        }
        return e;
    }();

    if (!engine)
    {
        result.error = "LLVM engine is not available";
        return result;
    }

    // The VM's onEnter computes target = exectarget + execdata[savedpc - code]
    // and calls the gate with (L, proto, target, ctx). The assembly gate is a
    // trampoline that sets up custom-ABI registers and jumps to target, with
    // target living in the gate's stack frame. The LLVM gate cannot do that:
    // it is a standard C function that indirect-calls target, which is a
    // standard C function itself. Each proto function dispatches from the
    // resume index (derived from L->ci->savedpc) to a per-instruction block;
    // native fast paths run inline, and anything else writes the resume index
    // back to L->ci->savedpc and returns 1 so the VM continues the function.
    void* modulePtr = engine->createModuleForLowering();
    if (!modulePtr)
    {
        result.error = engine->lastError();
        return result;
    }

    if (!lowerJitModule(engine->getModuleFromHandle(modulePtr), protos, result.error))
    {
        engine->releaseModule(modulePtr);
        return result;
    }

    std::string object = engine->compileModuleToNativeObject(modulePtr, level);
    engine->releaseModule(modulePtr);

    if (object.empty())
    {
        result.error = engine->lastError();
        return result;
    }

    Jit::JitObjectLayout layout;
    if (!Jit::loadJitObject(reinterpret_cast<const uint8_t*>(object.data()), object.size(), layout))
    {
        result.error = layout.error;
        return result;
    }

    result.entryOffsets.resize(protos.size());
    for (size_t i = 0; i < protos.size(); ++i)
    {
        const std::string symbol = llvmJitEntrySymbolName(uint32_t(protos[i]->bytecodeid));
        const uint64_t* offset = Jit::findSymbolOffset(layout, symbol.c_str());
        if (!offset)
        {
            result.error = "entry symbol missing from object: " + symbol;
            return result;
        }

        if (*offset < layout.codeOffset)
        {
            result.error = "entry symbol is not in the code region";
            return result;
        }

        result.entryOffsets[i] = uint32_t(*offset - layout.codeOffset);
    }

    const uint64_t* gateOffset = Jit::findSymbolOffset(layout, "luau_jit_gate");
    if (!gateOffset)
    {
        result.error = "gate symbol missing from object: luau_jit_gate";
        return result;
    }

    if (*gateOffset < layout.codeOffset)
    {
        result.error = "gate symbol is not in the code region";
        return result;
    }

    result.gateOffset = uint32_t(*gateOffset - layout.codeOffset);

    result.data = std::move(layout.data);
    result.code = std::move(layout.code);
    result.layout = std::move(layout);
    result.success = true;
    return result;
}

void finalizeLlvmAllocation(const CodeAllocationData& allocation, size_t dataSize, const Jit::JitObjectLayout& layout)
{
    if (allocation.start == nullptr)
        return;

    const size_t pageSize = getPageSize();

    uintptr_t start = reinterpret_cast<uintptr_t>(allocation.start);
    uintptr_t end = start + allocation.size;
    uintptr_t firstPage = start & ~(pageSize - 1);
    uintptr_t lastPage = (end + pageSize - 1) & ~(pageSize - 1);

    // Open a temporary writable window over the allocation pages and apply
    // the deferred relocations with the final base address.
    protectRange(reinterpret_cast<void*>(firstPage), lastPage - firstPage, PROT_READ | PROT_WRITE);

    Jit::applyJitRelocations(layout, allocation.start);

    // Restore the W^X protections, mirroring CodeAllocator::allocate.
    bool protectData = FFlag::LuauCodegenProtectData && dataSize != 0;
    uintptr_t codeStart = reinterpret_cast<uintptr_t>(allocation.codeStart);

    for (uintptr_t page = firstPage; page < lastPage; page += pageSize)
    {
        // Pages entirely inside the data region become read-only when data
        // protection is enabled; everything else is read+execute.
        int prot = (protectData && page + pageSize <= codeStart) ? PROT_READ : (PROT_READ | PROT_EXEC);
        protectRange(reinterpret_cast<void*>(page), pageSize, prot);
    }

    // Register the .eh_frame regions so C++ exceptions can unwind through the
    // JIT frames (required once native code calls C functions that throw).
#if (defined(__linux__) || defined(__APPLE__)) && (defined(CODEGEN_TARGET_X64) || defined(CODEGEN_TARGET_A64))
    if (__register_frame)
    {
        for (const auto& [ehFrameOffset, ehFrameSize] : layout.ehFrames)
            __register_frame(reinterpret_cast<const void*>(allocation.start + ehFrameOffset));
    }
#endif
}

} // namespace CodeGen
} // namespace Luau
