// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Luau
{
namespace CodeGen
{
namespace Jit
{

// Normalized relocation kinds understood by the loader
enum class RelocKind : uint8_t
{
    Abs64 = 0,            // R = S + A (64-bit write)
    Abs32 = 1,            // R = S + A (32-bit unsigned write)
    Abs32S = 2,           // R = S + A (32-bit signed write)
    Relative = 3,         // R = final site address (+ addend)
    Pc32 = 4,             // R = S + A - P (32-bit signed, x86-64)
    Prel32 = 5,           // R = S + A - P (32-bit signed, AArch64)
    Prel64 = 6,           // R = S + A - P (64-bit)
    Call26 = 7,           // AArch64 direct call: 26-bit signed (S + A - P) / 2
    AdrPrelPgHi21 = 8,    // AArch64 ADR page-relative upper bits
    AddAbsLo12Nc = 9,     // AArch64 ADD immediate lower 12 bits
};

// A deferred relocation: applied once the final base address is known.
// All offsets are in unified layout coordinates (data region at 0, code
// region at JitObjectLayout::codeOffset).
struct JitRelocation
{
    bool inCode = false;        // true if site is in code, false if in data
    uint32_t siteOffset = 0;    // offset of site within code (if inCode) or within data (if !inCode)
    RelocKind kind = RelocKind::Abs64;
    bool symbolAbsolute = false;
    bool targetInCode = false;  // true if target is in code, false if in data (when !symbolAbsolute)
    uint64_t symbolOffset = 0;  // offset of target within code or data (when !symbolAbsolute)
    uint64_t symbolValue = 0;   // st_value (when symbolAbsolute)
    int64_t addend = 0;
};

// Result of loading an ELF relocatable object into the Jaci code layout
struct JitObjectLayout
{
    bool success = false;
    std::string error;

    std::vector<uint8_t> data;
    std::vector<uint8_t> code;

    // symbol name -> code-relative offset
    std::vector<std::pair<std::string, uint64_t>> symbols;

    // .eh_frame sections: (dataOffset, size) pairs; register the allocated
    // addresses (codeStart - dataSize + dataOffset) via __register_frame after allocation
    std::vector<std::pair<uint32_t, uint32_t>> ehFrames;

    std::vector<JitRelocation> relocations;
};

// Parse the ELF relocatable object and produce the Jaci code layout.
// Supports the relocation set emitted by LLVM for static, no-PIC x86-64 and
// AArch64 objects; anything else fails with a descriptive error.
bool loadJitObject(const uint8_t* bytes, size_t size, JitObjectLayout& out);

// Apply the deferred relocations to the allocated pages. `codeStart` is the
// address of the code region (CodeAllocationData::codeStart), and `dataSize`
// is the size of the data region placed immediately before it.
void applyJitRelocations(const JitObjectLayout& layout, uint8_t* codeStart, size_t dataSize);

// Offset of a named symbol within the layout, or nullptr when missing.
const uint64_t* findSymbolOffset(const JitObjectLayout& layout, const char* name);

} // namespace Jit
} // namespace CodeGen
} // namespace Luau
