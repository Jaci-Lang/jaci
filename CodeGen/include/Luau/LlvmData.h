// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/Common.h"
#include "Luau/MirData.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <stdint.h>

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

enum class TypeKind : uint8_t
{
    Void,
    Int1,       // Boolean
    Int8,       // Tag / Byte
    Int32,      // Integer 32-bit
    Int64,      // Integer 64-bit unboxed
    Double,     // Float 64-bit unboxed
    Pointer,    // Opaque pointer (ptr)
    TValue,     // 16-byte boxed Lua value { double / i64, i32, i32 }
    Vector,     // SIMD vector <2 x double> or <4 x float>
};

struct Type
{
    TypeKind kind = TypeKind::Void;

    constexpr Type() = default;
    constexpr Type(TypeKind kind)
        : kind(kind)
    {
    }

    bool isVoid() const
    {
        return kind == TypeKind::Void;
    }
    bool isInt() const
    {
        return kind == TypeKind::Int1 || kind == TypeKind::Int8 || kind == TypeKind::Int32 || kind == TypeKind::Int64;
    }
    bool isDouble() const
    {
        return kind == TypeKind::Double;
    }
    bool isPointer() const
    {
        return kind == TypeKind::Pointer;
    }
    bool isTValue() const
    {
        return kind == TypeKind::TValue;
    }

    bool operator==(const Type& o) const
    {
        return kind == o.kind;
    }
    bool operator!=(const Type& o) const
    {
        return !(*this == o);
    }
};

enum class AliasScopeKind : uint8_t
{
    GeneralHeap,
    TableHeader,
    TableProperties,
    TableArray,
    UpvalueSlot,
    BufferBytes,
    GlobalEnv,
    VmStack,
};

enum class ArraySpecialization : uint8_t
{
    Generic,
    PackedInt64,
    PackedDouble,
    PackedRef,
};

struct PicEntry
{
    uint32_t shapeId = 0;
    uint32_t slotOffset = 0;
};

struct PicSite
{
    uint32_t siteId = 0;
    std::string propertyName;
    std::vector<PicEntry> entries;
    uint32_t maxEntries = 4;
    bool isMegamorphic = false;

    int findSlot(uint32_t shapeId) const
    {
        for (const auto& entry : entries)
        {
            if (entry.shapeId == shapeId)
                return int(entry.slotOffset);
        }
        return -1;
    }

    void addEntry(uint32_t shapeId, uint32_t slotOffset)
    {
        if (findSlot(shapeId) >= 0)
            return;
        if (entries.size() >= maxEntries)
        {
            isMegamorphic = true;
            return;
        }
        entries.push_back({shapeId, slotOffset});
    }
};

struct TableLayoutDescriptor
{
    uint32_t shapeId = 0;
    ArraySpecialization arraySpec = ArraySpecialization::Generic;
    uint32_t propertyCapacity = 0;
    uint32_t arrayCapacity = 0;
    bool hasMetatable = false;
};

struct FunctionSpecializationKey
{
    std::vector<Type> argTypes;
    std::vector<uint32_t> tableShapes;

    bool operator==(const FunctionSpecializationKey& o) const
    {
        return argTypes == o.argTypes && tableShapes == o.tableShapes;
    }
};

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
