// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "JitObjectLoader.h"

#if LUAU_USE_LLVM

#include "llvm/Object/Binary.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/TargetParser/Triple.h"

#include <cassert>
#include <cstring>

namespace Luau
{
namespace CodeGen
{
namespace Jit
{
using namespace llvm;
using namespace llvm::object;

namespace
{

uint32_t alignUp(uint32_t value, uint32_t align)
{
    return align == 0 ? value : (value + (align - 1)) & ~(align - 1);
}

// x86-64 relocation type values
namespace x64
{
constexpr uint64_t k64 = 1;
constexpr uint64_t kPC32 = 2;
constexpr uint64_t kPLT32 = 4;
constexpr uint64_t kGOTPCREL = 9;
constexpr uint64_t k32 = 10;
constexpr uint64_t k32S = 11;
constexpr uint64_t kPC64 = 24;
constexpr uint64_t kGOTPCRELX = 41;
constexpr uint64_t kREXGOTPCRELX = 42;
constexpr uint64_t kRELATIVE = 0x401;
} // namespace x64

// AArch64 relocation type values
namespace a64
{
constexpr uint64_t kCALL26 = 256;
constexpr uint64_t kABS64 = 257;
constexpr uint64_t kPREL32 = 258;
constexpr uint64_t kPREL64 = 259;
constexpr uint64_t kABS32 = 260;
constexpr uint64_t kADDABSLO12NC = 266;
constexpr uint64_t kADRPRELPGHI21 = 275;
constexpr uint64_t kADRPRELPGHI21NC = 276;
constexpr uint64_t kRELATIVE = 511;
} // namespace a64

enum class RelocArch
{
    Unsupported,
    X86_64,
    AArch64,
};

RelocArch archForTriple(Triple::ArchType arch)
{
    if (arch == Triple::x86_64)
        return RelocArch::X86_64;
    if (arch == Triple::aarch64)
        return RelocArch::AArch64;
    return RelocArch::Unsupported;
}

struct SectionInfo
{
    SectionRef sec;
    uint32_t size = 0;
    bool inCode = false;
    bool ehFrame = false;
    uint32_t layoutOffset = 0;
};

bool isCodeSection(const SectionRef& sec, StringRef name)
{
    return sec.isText() || name == ".plt" || name.starts_with(".plt.");
}

bool isAllocableSection(const SectionRef& sec, StringRef name)
{
    if (sec.isBSS())
        return false;

    return sec.isText() || name.starts_with(".rodata") || name.starts_with(".data") || name.starts_with(".eh_frame")
        || name == ".plt" || name.starts_with(".plt.");
}

void writeValue(uint8_t* site, size_t size, uint64_t value)
{
    switch (size)
    {
        case 2:
            std::memcpy(site, &value, 2);
            break;
        case 4:
            std::memcpy(site, &value, 4);
            break;
        case 8:
            std::memcpy(site, &value, 8);
            break;
        default:
            assert(false && "unsupported write size");
    }
}

} // namespace

bool loadJitObject(const uint8_t* bytes, size_t size, JitObjectLayout& out)
{
    auto maybeBuf = MemoryBuffer::getMemBuffer(StringRef(reinterpret_cast<const char*>(bytes), size), "jit.o");
    if (!maybeBuf)
    {
        out.error = "failed to wrap object buffer";
        return false;
    }

    auto maybeObj = ObjectFile::createObjectFile(maybeBuf->getMemBufferRef());
    if (!maybeObj)
    {
        out.error = "not a supported relocatable object";
        return false;
    }

    const auto& objPtr = *maybeObj;
    const ObjectFile& obj = *objPtr;

    RelocArch arch = archForTriple(obj.getArch());
    if (arch == RelocArch::Unsupported)
    {
        out.error = "unsupported object architecture";
        return false;
    }

    // Pass 1: collect allocatable sections and assign layout offsets
    std::vector<SectionInfo> sections;
    uint32_t dataPos = 0;
    uint32_t codePos = 0;

    for (auto secIt = obj.section_begin(); secIt != obj.section_end(); ++secIt)
    {
        SectionRef sec = *secIt;

        auto nameResult = sec.getName();
        if (!nameResult)
            continue;

        StringRef name = *nameResult;

        if (!isAllocableSection(sec, name))
            continue;

        uint64_t secSize = sec.getSize();
        if (secSize == 0)
            continue;

        uint64_t secAlign = sec.getAlignment().value();
        if (secAlign == 0)
            secAlign = 1;
        if (secAlign > 4096)
        {
            out.error = "section alignment too large";
            return false;
        }

        SectionInfo info;
        info.sec = sec;
        info.size = uint32_t(secSize);
        info.inCode = isCodeSection(sec, name);
        info.ehFrame = name.starts_with(".eh_frame");

        if (info.inCode)
        {
            codePos = alignUp(codePos, uint32_t(secAlign));
            info.layoutOffset = codePos;
            codePos += info.size;
        }
        else
        {
            dataPos = alignUp(dataPos, uint32_t(secAlign));
            info.layoutOffset = dataPos;
            dataPos += info.size;
        }

        sections.push_back(std::move(info));
    }

    if (sections.empty())
    {
        out.error = "object contains no allocatable sections";
        return false;
    }

    uint32_t dataSize = dataPos;
    uint32_t codeSize = codePos;

    out.data.resize(dataSize);
    out.code.resize(codeSize);

    for (const SectionInfo& info : sections)
    {
        auto contents = info.sec.getContents();
        if (!contents)
        {
            out.error = "failed to read section contents";
            return false;
        }

        if (info.inCode)
            std::memcpy(out.code.data() + info.layoutOffset, contents->data(), info.size);
        else
            std::memcpy(out.data.data() + info.layoutOffset, contents->data(), info.size);
    }

    // Resolve section info by identity
    auto findSectionInfo = [&sections](const SectionRef& sec) -> const SectionInfo* {
        for (const SectionInfo& info : sections)
            if (info.sec == sec)
                return &info;
        return nullptr;
    };

    // Symbols: record code-relative layout offsets for defined code symbols
    for (auto symIt = obj.symbol_begin(); symIt != obj.symbol_end(); ++symIt)
    {
        SymbolRef sym = *symIt;

        auto nameResult = sym.getName();
        if (!nameResult)
            continue;

        auto flagsResult = sym.getFlags();
        if (!flagsResult)
            continue;

        uint32_t flags = *flagsResult;
        if (flags & BasicSymbolRef::SF_Undefined)
            continue;

        StringRef name = *nameResult;
        if (name.empty())
            continue;

        if (flags & BasicSymbolRef::SF_Absolute)
            continue;

        auto secResult = sym.getSection();
        if (!secResult)
            continue;

        const SectionInfo* info = findSectionInfo(**secResult);
        if (!info)
            continue;

        uint64_t offset = uint64_t(info->layoutOffset) + *sym.getAddress();
        if (info->inCode)
            out.symbols.emplace_back(std::string(name), offset);
    }

    // .eh_frame regions
    for (const SectionInfo& info : sections)
        if (info.ehFrame)
            out.ehFrames.emplace_back(info.layoutOffset, info.size);

    // Relocations: scan all relocation sections in the object
    for (auto secIt = obj.section_begin(); secIt != obj.section_end(); ++secIt)
    {
        SectionRef relSec = *secIt;
        auto targetSecIt = relSec.getRelocatedSection();
        if (!targetSecIt || *targetSecIt == obj.section_end())
            continue;

        SectionRef targetSec = **targetSecIt;
        const SectionInfo* info = findSectionInfo(targetSec);
        if (!info)
            continue;

        for (const auto& rel : relSec.relocations())
        {
            JitRelocation r;
            r.inCode = info->inCode;
            r.siteOffset = info->layoutOffset + uint32_t(rel.getOffset());
            ELFRelocationRef elfRel(rel);
            auto addendResult = elfRel.getAddend();
            r.addend = addendResult ? *addendResult : 0;

            SymbolRef sym = *rel.getSymbol();

            auto symFlagsResult = sym.getFlags();
            if (!symFlagsResult)
            {
                out.error = "relocation symbol flags unavailable";
                return false;
            }

            uint32_t symFlags = *symFlagsResult;
            StringRef symName = sym.getName() ? *sym.getName() : StringRef();

            if (symFlags & BasicSymbolRef::SF_Absolute)
            {
                r.symbolAbsolute = true;
                r.symbolValue = *sym.getAddress();
            }
            else
            {
                if ((symFlags & BasicSymbolRef::SF_Undefined) && !symName.empty())
                {
                    out.error = std::string("undefined symbol in JIT object: ") + symName.str();
                    return false;
                }

                auto secResult = sym.getSection();
                if (!secResult)
                {
                    out.error = "relocation symbol has no section";
                    return false;
                }

                const SectionInfo* targetSec = findSectionInfo(**secResult);
                if (!targetSec)
                {
                    out.error = "relocation target section not allocatable";
                    return false;
                }

                r.symbolAbsolute = false;
                r.targetInCode = targetSec->inCode;
                r.symbolOffset = uint64_t(targetSec->layoutOffset) + *sym.getAddress();
            }

            uint64_t type = rel.getType();

            if (arch == RelocArch::X86_64)
            {
                switch (type)
                {
                    case x64::k64:
                        r.kind = RelocKind::Abs64;
                        break;
                    case x64::k32:
                        r.kind = RelocKind::Abs32;
                        break;
                    case x64::k32S:
                        r.kind = RelocKind::Abs32S;
                        break;
                    case x64::kPC32:
                    case x64::kPLT32:
                        r.kind = RelocKind::Pc32;
                        break;
                    case x64::kPC64:
                        r.kind = RelocKind::Prel64;
                        break;
                    case x64::kRELATIVE:
                        r.kind = RelocKind::Relative;
                        break;
                    case x64::kGOTPCREL:
                    case x64::kGOTPCRELX:
                    case x64::kREXGOTPCRELX:
                        out.error = "GOT relocations are not supported (object must be static no-PIC)";
                        return false;
                    default:
                        out.error = "unsupported x86-64 relocation type " + std::to_string(type);
                        return false;
                }
            }
            else
            {
                switch (type)
                {
                    case a64::kABS64:
                        r.kind = RelocKind::Abs64;
                        break;
                    case a64::kABS32:
                        r.kind = RelocKind::Abs32;
                        break;
                    case a64::kPREL32:
                        r.kind = RelocKind::Prel32;
                        break;
                    case a64::kPREL64:
                        r.kind = RelocKind::Prel64;
                        break;
                    case a64::kCALL26:
                        r.kind = RelocKind::Call26;
                        break;
                    case a64::kADRPRELPGHI21:
                    case a64::kADRPRELPGHI21NC:
                        r.kind = RelocKind::AdrPrelPgHi21;
                        break;
                    case a64::kADDABSLO12NC:
                        r.kind = RelocKind::AddAbsLo12Nc;
                        break;
                    case a64::kRELATIVE:
                        r.kind = RelocKind::Relative;
                        break;
                    default:
                        out.error = "unsupported AArch64 relocation type " + std::to_string(type);
                        return false;
                }
            }

            out.relocations.push_back(r);
        }
    }

    out.success = true;
    return true;
}

void applyJitRelocations(const JitObjectLayout& layout, uint8_t* codeStart, size_t dataSize)
{
    for (const JitRelocation& r : layout.relocations)
    {
        uint8_t* site = r.inCode ? (codeStart + r.siteOffset) : (codeStart - dataSize + r.siteOffset);

        uint64_t symbolValue = r.symbolAbsolute
            ? r.symbolValue
            : (r.targetInCode ? (uint64_t(codeStart) + r.symbolOffset) : (uint64_t(codeStart - dataSize) + r.symbolOffset));
        uint64_t siteAddress = uint64_t(site);

        switch (r.kind)
        {
            case RelocKind::Abs64:
                writeValue(site, 8, symbolValue + uint64_t(r.addend));
                break;
            case RelocKind::Abs32:
                writeValue(site, 4, (symbolValue + uint64_t(r.addend)) & 0xFFFFFFFFull);
                break;
            case RelocKind::Abs32S:
                writeValue(site, 4, uint64_t(int32_t(int64_t(symbolValue + uint64_t(r.addend)))));
                break;
            case RelocKind::Relative:
                writeValue(site, 8, siteAddress + uint64_t(r.addend));
                break;
            case RelocKind::Pc32:
                writeValue(site, 4, uint64_t(int32_t(int64_t(symbolValue + uint64_t(r.addend) - siteAddress))));
                break;
            case RelocKind::Prel32:
                writeValue(site, 4, uint64_t(int32_t(int64_t(symbolValue + uint64_t(r.addend) - siteAddress))));
                break;
            case RelocKind::Prel64:
                writeValue(site, 8, uint64_t(int64_t(symbolValue + uint64_t(r.addend) - siteAddress)));
                break;
            case RelocKind::Call26:
            {
                int64_t delta = int64_t(symbolValue + uint64_t(r.addend) - siteAddress);
                int64_t rel26 = delta >> 2;
                assert(rel26 >= -(1LL << 25) && rel26 < (1LL << 25) && "call26 out of range");
                uint32_t insn;
                std::memcpy(&insn, site, 4);
                insn = (insn & ~0x3FFFFFFu) | uint32_t(rel26 & 0x3FFFFFFLL);
                std::memcpy(site, &insn, 4);
                break;
            }
            case RelocKind::AdrPrelPgHi21:
            {
                int64_t rel = (int64_t(symbolValue + uint64_t(r.addend)) >> 12) - (int64_t(siteAddress) >> 12);
                assert(rel >= -(1LL << 20) && rel < (1LL << 20) && "adr page reloc out of range");
                uint32_t insn;
                std::memcpy(&insn, site, 4);
                // 21-bit signed page offset: immlo in bits 30:29, immhi in bits 23:5.
                insn &= ~((0x3u << 29) | (0x7FFFFu << 5));
                insn |= uint32_t((rel & 0x3) << 29) | uint32_t(((rel >> 2) & 0x7FFFF) << 5);
                std::memcpy(site, &insn, 4);
                break;
            }
            case RelocKind::AddAbsLo12Nc:
            {
                uint32_t insn;
                std::memcpy(&insn, site, 4);
                insn = (insn & ~0xFFFu) | uint32_t((symbolValue + uint64_t(r.addend)) & 0xFFFu);
                std::memcpy(site, &insn, 4);
                break;
            }
        }
    }
}

const uint64_t* findSymbolOffset(const JitObjectLayout& layout, const char* name)
{
    for (const auto& [symName, offset] : layout.symbols)
        if (symName == name)
            return &offset;
    return nullptr;
}

} // namespace Jit
} // namespace CodeGen
} // namespace Luau

#else // LUAU_USE_LLVM

// Stubs for builds without LLVM: object loading is unavailable.
namespace Luau
{
namespace CodeGen
{
namespace Jit
{

bool loadJitObject(const uint8_t*, size_t, JitObjectLayout& out)
{
    out.error = "LLVM backend is not available in this build";
    return false;
}

void applyJitRelocations(const JitObjectLayout&, uint8_t*)
{
}

const uint64_t* findSymbolOffset(const JitObjectLayout&, const char*)
{
    return nullptr;
}

} // namespace Jit
} // namespace CodeGen
} // namespace Luau

#endif // LUAU_USE_LLVM
