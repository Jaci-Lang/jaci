// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "JitObjectLoader.h"

#include "Luau/CodeAllocator.h"
#include "Luau/LlvmEngine.h"

#include "lobject.h"

#include <string>
#include <vector>

namespace Luau
{
namespace CodeGen
{

// Result of compiling a batch of protos through the LLVM backend
struct LlvmJitCompileResult
{
    bool success = false;
    std::string error;

    // entry symbol offset within the code region, one per proto (input order)
    std::vector<uint32_t> entryOffsets;

    // offset of the module's global gate function within the code region;
    // the gate indirect-calls the per-proto entry for the VM's GateFn protocol
    uint32_t gateOffset = 0;

    std::vector<uint8_t> data;
    std::vector<uint8_t> code;

    // object layout: relocations + .eh_frame regions to finalize after allocation
    Jit::JitObjectLayout layout;
};

// Entry symbol name for a proto in the LLVM module
inline std::string llvmJitEntrySymbolName(uint32_t bytecodeId)
{
    return "luau_jit_proto_" + std::to_string(bytecodeId);
}

// Compile the protos through the LLVM pipeline (parse IR, optimize, emit
// object, load into the Jaci layout). Entries dispatch from the LLVM-compiled
// function and return to the VM at L->ci->savedpc when they fall back.
LlvmJitCompileResult compileLlvmProtos(const std::vector<Proto*>& protos, Llvm::OptLevel level = Llvm::OptLevel::O2);

// Lower the full JIT module (gate + one function per proto) into the in-memory
// module behind a handle from LlvmEngine::createModuleForLowering.
bool lowerJitModule(void* moduleHandle, const std::vector<Proto*>& protos, std::string& error);

// Finalize an allocated LLVM module: apply the deferred relocations to the
// pages (through a temporary writable window), restore the W^X protections,
// and register the .eh_frame regions.
void finalizeLlvmAllocation(const CodeAllocationData& allocation, size_t dataSize, const Jit::JitObjectLayout& layout);

} // namespace CodeGen
} // namespace Luau
