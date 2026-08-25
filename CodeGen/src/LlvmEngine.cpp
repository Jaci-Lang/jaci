// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/LlvmEngine.h"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#if LUAU_USE_LLVM

#include "JitObjectLoader.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{
using namespace llvm;

struct LlvmEngine::Impl
{
    std::once_flag targetInitFlag;
    bool targetInitialized = false;

    std::unique_ptr<TargetMachine> tm;
    std::string tripleStr;
    DataLayout dataLayout{""};

    // Register the host target and its MC object-emission support once.
    void initTargets()
    {
        std::call_once(
            targetInitFlag,
            []
            {
                InitializeNativeTarget();
                InitializeNativeTargetAsmPrinter();
                InitializeNativeTargetAsmParser();
                InitializeNativeTargetDisassembler();
            }
        );
        targetInitialized = true;
    }

    OptimizationLevel toOptimizationLevel(OptLevel level)
    {
        switch (level)
        {
        case OptLevel::O0:
            return OptimizationLevel::O0;
        case OptLevel::O1:
            return OptimizationLevel::O1;
        case OptLevel::O2:
            return OptimizationLevel::O2;
        case OptLevel::O3:
            return OptimizationLevel::O3;
        }
        return OptimizationLevel::O2;
    }
};

namespace
{

struct ModuleHandle
{
    std::unique_ptr<LLVMContext> context;
    std::unique_ptr<Module> module;
};

// Allocate a private read-write page
void* allocateExecutablePage(size_t size, size_t& pageSize)
{
#if defined(_WIN32)
    pageSize = (size + 0xFFF) & ~size_t(0xFFF);
    return ::VirtualAlloc(nullptr, pageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    long page = sysconf(_SC_PAGESIZE);
    pageSize = (size + (page - 1)) & ~size_t(page - 1);
    void* mem = ::mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return mem == MAP_FAILED ? nullptr : mem;
#endif
}

void freeExecutablePage(void* mem, size_t pageSize)
{
#if defined(_WIN32)
    ::VirtualFree(mem, 0, MEM_RELEASE);
#else
    ::munmap(mem, pageSize);
#endif
}

void makePageExecutable(void* mem, size_t pageSize)
{
#if defined(_WIN32)
    DWORD old;
    ::VirtualProtect(mem, pageSize, PAGE_EXECUTE_READ, &old);
#else
    ::mprotect(mem, pageSize, PROT_READ | PROT_EXEC);
#endif
}

} // namespace

LlvmEngine::LlvmEngine()
    : pImpl(new Impl())
{
}

LlvmEngine::~LlvmEngine()
{
    for (const ExecutableRegion& region : regions)
        freeExecutablePage(region.base, region.size);
    delete pImpl;
}

bool LlvmEngine::initialize()
{
    if (engineInitialized)
        return true;

    pImpl->initTargets();

    Triple targetTriple(sys::getProcessTriple());
    targetTriple.setObjectFormat(Triple::ELF);

    std::string tripleStr = targetTriple.str();
    pImpl->tripleStr = tripleStr;

#if LLVM_VERSION_MAJOR >= 21
    const Target* target = TargetRegistry::lookupTarget(targetTriple, lastErrorMessage);
#else
    const Target* target = TargetRegistry::lookupTarget(tripleStr, lastErrorMessage);
#endif
    if (!target)
    {
        if (lastErrorMessage.empty())
            lastErrorMessage = "no LLVM target for triple " + tripleStr;
        return false;
    }

    pImpl->tm.reset(target->createTargetMachine(
#if LLVM_VERSION_MAJOR >= 21
        targetTriple,
#else
        tripleStr,
#endif
        "generic",
        /*features=*/"",
        TargetOptions(),
        Reloc::Model::Static,
        CodeModel::Large,
        CodeGenOptLevel::Default,
        /*JIT=*/false
    ));
    if (!pImpl->tm)
    {
        lastErrorMessage = "failed to create target machine";
        return false;
    }

    pImpl->dataLayout = pImpl->tm->createDataLayout();
    engineInitialized = true;
    return true;
}

void* LlvmEngine::createModuleFromIrText(const std::string& irText)
{
    if (!irText.empty())
        lastErrorMessage.clear();

    if (irText.empty())
    {
        lastErrorMessage = "empty IR text";
        return nullptr;
    }

    auto moduleHandle = new ModuleHandle();
    moduleHandle->context = std::make_unique<LLVMContext>();

    auto buffer = MemoryBuffer::getMemBuffer(StringRef(irText), "jit-ir");
    SMDiagnostic diagnostic;
    std::unique_ptr<Module> module = parseIR(buffer->getMemBufferRef(), diagnostic, *moduleHandle->context);
    if (!module)
    {
        lastErrorMessage = std::string("failed to parse LLVM IR: ") + diagnostic.getMessage().str();
        delete moduleHandle;
        return nullptr;
    }

#if LLVM_VERSION_MAJOR >= 21
    module->setTargetTriple(Triple(pImpl->tripleStr));
#else
    module->setTargetTriple(pImpl->tripleStr);
#endif
    module->setDataLayout(pImpl->dataLayout);

    {
        std::string verifyMsg;
        raw_string_ostream os(verifyMsg);
        if (verifyModule(*module, &os))
        {
            lastErrorMessage = std::string("module verification failed: ") + verifyMsg;
            delete moduleHandle;
            return nullptr;
        }
    }

    moduleHandle->module = std::move(module);
    return moduleHandle;
}

void LlvmEngine::releaseModule(void* modulePtr)
{
    delete static_cast<ModuleHandle*>(modulePtr);
}

std::string LlvmEngine::compileModuleToNativeObject(void* modulePtr, OptLevel level)
{
    if (!engineInitialized)
    {
        lastErrorMessage = "engine not initialized";
        return {};
    }

    auto* moduleHandle = static_cast<ModuleHandle*>(modulePtr);
    if (!moduleHandle || !moduleHandle->module)
    {
        lastErrorMessage = "invalid module handle";
        return {};
    }

    Module& module = *moduleHandle->module;

    {
        std::string verifyMsg;
        raw_string_ostream os(verifyMsg);
        if (verifyModule(module, &os))
        {
            lastErrorMessage = verifyMsg;
            return {};
        }
    }

    // Run the new-pass-manager optimization pipeline
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;

    PassBuilder pb(pImpl->tm.get());
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(pImpl->toOptimizationLevel(level));
    mpm.run(module, mam);

    {
        std::string verifyMsg;
        raw_string_ostream os(verifyMsg);
        if (verifyModule(module, &os))
        {
            lastErrorMessage = verifyMsg;
            return {};
        }
    }

    // Emit an ELF relocatable object through the legacy emission pipeline
    SmallString<128> objectBuffer;
    raw_svector_ostream emitOs(objectBuffer);

    legacy::PassManager pm;
    if (pImpl->tm->addPassesToEmitFile(pm, emitOs, nullptr, CodeGenFileType::ObjectFile))
    {
        lastErrorMessage = "target does not support object emission";
        return {};
    }

    // Some LLVM builds report a failure from the legacy pass manager even
    // when a complete object is emitted, so validate the actual bytes rather
    // than trusting the return value. A real emission produces a non-empty
    // buffer whose leading bytes identify an ELF object; JitObjectLoader
    // performs the full structural validation downstream.
    pm.run(module);

    if (objectBuffer.empty())
    {
        lastErrorMessage = "object emission produced no output";
        return {};
    }

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(objectBuffer.data());
    bool validMagic = objectBuffer.size() >= 4 && bytes[0] == 0x7f && bytes[1] == 0x45 && bytes[2] == 0x4c && bytes[3] == 0x46;

    if (!validMagic)
    {
        lastErrorMessage = "object emission produced an unrecognized object format";
        return {};
    }

    lastErrorMessage.clear();
    return std::string(objectBuffer.data(), objectBuffer.size());
}

void* LlvmEngine::compileFunction(const std::string& irCode, const std::string& entrySymbol, OptLevel level)
{
    if (!initialize())
        return nullptr;

    void* modulePtr = createModuleFromIrText(irCode);
    if (!modulePtr)
        return nullptr;

    std::string object = compileModuleToNativeObject(modulePtr, level);
    releaseModule(modulePtr);

    if (object.empty())
        return nullptr;

    Jit::JitObjectLayout layout;
    if (!Jit::loadJitObject(reinterpret_cast<const uint8_t*>(object.data()), object.size(), layout))
    {
        lastErrorMessage = layout.error;
        return nullptr;
    }

    const uint64_t* entryOffset = Jit::findSymbolOffset(layout, entrySymbol.c_str());
    if (!entryOffset)
    {
        lastErrorMessage = "entry symbol not found in object: " + entrySymbol;
        return nullptr;
    }

    size_t pageSize = 0;
    const size_t dataSize = layout.data.size();
    const size_t codeSize = layout.code.size();
    void* base = allocateExecutablePage(dataSize + codeSize, pageSize);
    if (!base)
    {
        lastErrorMessage = "failed to allocate executable memory";
        return nullptr;
    }

    uint8_t* codeStart = static_cast<uint8_t*>(base) + dataSize;
    if (dataSize > 0)
        std::memcpy(base, layout.data.data(), dataSize);
    if (codeSize > 0)
        std::memcpy(codeStart, layout.code.data(), codeSize);

    Jit::applyJitRelocations(layout, codeStart, dataSize);

    makePageExecutable(base, pageSize);

    ExecutableRegion region;
    region.base = base;
    region.size = pageSize;
    void* entry = codeStart + *entryOffset;
    region.entries.push_back(entry);
    regions.push_back(region);

    lastErrorMessage.clear();
    return entry;
}

void LlvmEngine::releaseExecutable(void* entry)
{
    for (auto it = regions.begin(); it != regions.end(); ++it)
    {
        for (size_t i = 0; i < it->entries.size(); ++i)
        {
            if (it->entries[i] == entry)
            {
                freeExecutablePage(it->base, it->size);
                regions.erase(it);
                return;
            }
        }
    }
}

void* LlvmEngine::createModuleForLowering()
{
    if (!engineInitialized)
    {
        lastErrorMessage = "engine not initialized";
        return nullptr;
    }

    lastErrorMessage.clear();

    auto moduleHandle = new ModuleHandle();
    moduleHandle->context = std::make_unique<LLVMContext>();

    auto module = std::make_unique<Module>("luau_jit", *moduleHandle->context);
#if LLVM_VERSION_MAJOR >= 21
    module->setTargetTriple(Triple(pImpl->tripleStr));
#else
    module->setTargetTriple(pImpl->tripleStr);
#endif
    module->setDataLayout(pImpl->dataLayout);

    moduleHandle->module = std::move(module);
    return moduleHandle;
}

void* LlvmEngine::getModuleFromHandle(void* modulePtr) const
{
    const auto* moduleHandle = static_cast<const ModuleHandle*>(modulePtr);
    return moduleHandle ? moduleHandle->module.get() : nullptr;
}

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau

#else // LUAU_USE_LLVM

// Stubs for builds without LLVM: the engine reports itself unavailable and
// the assembly backend remains the only code producer.
namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

LlvmEngine::LlvmEngine() = default;
LlvmEngine::~LlvmEngine() = default;

bool LlvmEngine::initialize()
{
    lastErrorMessage = "LLVM backend is not available in this build";
    return false;
}

void* LlvmEngine::createModuleFromIrText(const std::string&)
{
    return nullptr;
}

void LlvmEngine::releaseModule(void*) {}

std::string LlvmEngine::compileModuleToNativeObject(void*, OptLevel)
{
    return {};
}

void* LlvmEngine::compileFunction(const std::string&, const std::string&, OptLevel)
{
    return nullptr;
}

void LlvmEngine::releaseExecutable(void*) {}

void* LlvmEngine::createModuleForLowering()
{
    return nullptr;
}

void* LlvmEngine::getModuleFromHandle(void*) const
{
    return nullptr;
}

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau

#endif // LUAU_USE_LLVM

// comparePerformance is a host-side benchmark harness (it times two plain C++
// callbacks); it uses no LLVM APIs and behaves identically in builds with and
// without the LLVM backend.
namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

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
            assemblyFn();
        auto t1 = std::chrono::high_resolution_clock::now();
        result.assemblyTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    // Measure LLVM Backend
    if (llvmFn)
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < iterations; ++i)
            llvmFn();
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
    ss << "[" << benchmarkName << "] Assembly: " << std::fixed << std::setprecision(3) << result.assemblyTimeMs << " ms | LLVM: " << result.llvmTimeMs
       << " ms | Speedup: " << result.speedupRatio << "x";
    result.summary = ss.str();

    return result;
}

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
