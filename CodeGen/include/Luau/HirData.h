// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/Bytecode.h"
#include "Luau/Common.h"
#include "Luau/DenseHash2.h"
#include "Luau/IrData.h"
#include "Luau/SmallVector.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <stdint.h>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

enum class TypeKind : uint8_t
{
    Bottom,
    Nil,
    Boolean,
    Number,
    Integer,
    String,
    Table,
    Function,
    Thread,
    Userdata,
    Vector,
    Buffer,
    Any,
};

struct Type
{
    TypeKind kind = TypeKind::Any;
    bool optional = false;

    constexpr Type() = default;
    constexpr Type(TypeKind kind, bool optional = false)
        : kind(kind)
        , optional(optional)
    {
    }

    bool isSubtypeOf(const Type& other) const
    {
        if (other.kind == TypeKind::Any)
            return true;
        if (kind == TypeKind::Bottom)
            return true;
        if (kind == other.kind)
            return !optional || other.optional;
        if (optional && kind == TypeKind::Nil && other.optional)
            return true;
        return false;
    }

    bool isExact() const
    {
        return kind != TypeKind::Any && kind != TypeKind::Bottom;
    }

    bool operator==(const Type& o) const
    {
        return kind == o.kind && optional == o.optional;
    }

    bool operator!=(const Type& o) const
    {
        return !(*this == o);
    }
};

struct Range
{
    int64_t min = std::numeric_limits<int64_t>::min();
    int64_t max = std::numeric_limits<int64_t>::max();

    constexpr Range() = default;
    constexpr Range(int64_t exact)
        : min(exact)
        , max(exact)
    {
    }
    constexpr Range(int64_t min, int64_t max)
        : min(min)
        , max(max)
    {
    }

    bool isKnown() const
    {
        return min > std::numeric_limits<int64_t>::min() || max < std::numeric_limits<int64_t>::max();
    }

    bool isExact() const
    {
        return min == max;
    }

    bool contains(int64_t v) const
    {
        return v >= min && v <= max;
    }

    bool isNonNegative() const
    {
        return min >= 0;
    }

    Range intersectWith(const Range& other) const
    {
        return Range(std::max(min, other.min), std::min(max, other.max));
    }

    Range unionWith(const Range& other) const
    {
        return Range(std::min(min, other.min), std::max(max, other.max));
    }

    Range add(const Range& other) const
    {
        int64_t nmin = (min == std::numeric_limits<int64_t>::min() || other.min == std::numeric_limits<int64_t>::min()) ? std::numeric_limits<int64_t>::min() : min + other.min;
        int64_t nmax = (max == std::numeric_limits<int64_t>::max() || other.max == std::numeric_limits<int64_t>::max()) ? std::numeric_limits<int64_t>::max() : max + other.max;
        return Range(nmin, nmax);
    }
};

enum class ArrayStorageKind : uint8_t
{
    Generic,
    PackedInt,
    PackedDouble,
    PackedRef,
    HoleyGeneric,
};

struct ArrayLayout
{
    ArrayStorageKind kind = ArrayStorageKind::Generic;
    uint32_t knownCapacity = 0;
    bool boundsChecked = true;

    bool isPacked() const
    {
        return kind == ArrayStorageKind::PackedInt || kind == ArrayStorageKind::PackedDouble || kind == ArrayStorageKind::PackedRef;
    }
};

struct PropertySlot
{
    std::string name;
    uint32_t slotIndex = 0;
    Type expectedType = Type(TypeKind::Any);
    bool isReadOnly = false;
};

struct TableShape
{
    uint32_t shapeId = 0;
    std::vector<PropertySlot> slots;
    std::unordered_map<std::string, uint32_t> slotMap;
    ArrayLayout arrayLayout;
    bool hasDictionaryFallback = true;
    bool hasMetatable = false;
    uint32_t transitionToShapeId = 0;

    int findSlot(const std::string& key) const
    {
        auto it = slotMap.find(key);
        if (it != slotMap.end())
            return int(it->second);
        return -1;
    }

    void addProperty(const std::string& key, Type type = Type(TypeKind::Any))
    {
        if (slotMap.find(key) != slotMap.end())
            return;
        uint32_t idx = uint32_t(slots.size());
        slots.push_back(PropertySlot{key, idx, type, false});
        slotMap[key] = idx;
    }
};

enum class EscapeState : uint8_t
{
    NoEscape,
    EscapeArg,
    GlobalEscape,
};

enum class AliasClass : uint8_t
{
    Unknown,
    TableHeader,
    TableProperties,
    TableArray,
    UpvalueSlot,
    BufferBytes,
    GlobalEnv,
    VirtualObject,
};

enum class Cmd : uint16_t
{
    Nop,
    // Constants
    ConstNil,
    ConstBool,
    ConstInt,
    ConstInt64,
    ConstDouble,
    ConstString,
    ConstTag,

    // SSA & Control Flow
    Phi,
    BlockArg,
    Jump,
    Branch,
    Return,
    Unreachable,

    // High-Level Structured Control
    StructuredIf,
    StructuredLoop,

    // Arithmetic & Bitwise
    Add,
    Sub,
    Mul,
    Div,
    FloorDiv,
    Mod,
    Pow,
    Neg,
    Not,
    Len,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    BitLShift,
    BitRShift,
    BitARShift,

    // Comparisons
    CmpEq,
    CmpLt,
    CmpLe,

    // Type Conversions & Intrinsic Operations
    CheckTag,
    CheckType,
    CheckNotNil,
    CheckShape,
    CheckBounds,
    AssertTruth,
    Cast,

    // High-Level Table Operations
    AllocTable,
    GetTable,
    SetTable,
    GetTableRaw,
    SetTableRaw,
    GetShapeSlot,
    SetShapeSlot,
    GetArrayElement,
    SetArrayElement,
    GetArrayLength,
    TableLen,
    TableNext,

    // Virtual Object Operations
    AllocVirtualTable,
    ReadVirtualField,
    WriteVirtualField,
    MaterializeVirtual,

    // Upvalues & Closures
    AllocClosure,
    GetUpvalue,
    SetUpvalue,
    CloseUpvalues,

    // Calls & Multivalues
    Call,
    CallBuiltin,
    ReturnValues,
    PackMultivalue,
    ExtractMultivalue,
    GetVarargs,

    // State, Memory & Snapshots
    Snapshot,
    GcBarrier,
    LoadVmReg,
    StoreVmReg,
    VmExit,
};

enum class ValueKind : uint8_t
{
    None,
    Inst,
    Constant,
    BlockArg,
    VirtualObject,
    VmRegister,
    Multivalue,
};

struct ConstantValue
{
    TypeKind kind = TypeKind::Nil;
    union
    {
        bool b;
        int32_t i;
        int64_t i64;
        double d;
        uint32_t stringId;
    } val = {};

    std::string str;

    ConstantValue()
    {
        kind = TypeKind::Nil;
    }
    explicit ConstantValue(bool b)
        : kind(TypeKind::Boolean)
    {
        val.b = b;
    }
    explicit ConstantValue(int32_t i)
        : kind(TypeKind::Integer)
    {
        val.i = i;
    }
    explicit ConstantValue(int64_t i64)
        : kind(TypeKind::Integer)
    {
        val.i64 = i64;
    }
    explicit ConstantValue(double d)
        : kind(TypeKind::Number)
    {
        val.d = d;
    }
    explicit ConstantValue(std::string s)
        : kind(TypeKind::String)
        , str(std::move(s))
    {
    }
};

struct Value
{
    ValueKind kind = ValueKind::None;
    uint32_t index = ~0u;

    constexpr Value() = default;
    constexpr Value(ValueKind kind, uint32_t index)
        : kind(kind)
        , index(index)
    {
    }

    bool isNone() const
    {
        return kind == ValueKind::None;
    }
    bool isInst() const
    {
        return kind == ValueKind::Inst;
    }
    bool isConstant() const
    {
        return kind == ValueKind::Constant;
    }
    bool isBlockArg() const
    {
        return kind == ValueKind::BlockArg;
    }
    bool isVirtual() const
    {
        return kind == ValueKind::VirtualObject;
    }

    bool operator==(const Value& o) const
    {
        return kind == o.kind && index == o.index;
    }
    bool operator!=(const Value& o) const
    {
        return !(*this == o);
    }
};

struct VirtualField
{
    std::string name;
    Value value;
    Type type = Type(TypeKind::Any);
};

struct VirtualTable
{
    uint32_t id = 0;
    std::vector<VirtualField> properties;
    std::vector<Value> arrayElements;
    EscapeState escape = EscapeState::NoEscape;
    uint32_t shapeId = 0;
    bool materialized = false;
    Value materializedValue;

    int findProperty(const std::string& name) const
    {
        for (size_t i = 0; i < properties.size(); ++i)
        {
            if (properties[i].name == name)
                return int(i);
        }
        return -1;
    }

    void setProperty(const std::string& name, Value val, Type type = Type(TypeKind::Any))
    {
        int idx = findProperty(name);
        if (idx >= 0)
        {
            properties[idx].value = val;
            properties[idx].type = type;
        }
        else
        {
            properties.push_back(VirtualField{name, val, type});
        }
    }
};

struct Multivalue
{
    uint32_t id = 0;
    std::vector<Value> values;
    bool isDynamic = false;
    uint32_t dynamicCount = 0;
};

struct SnapshotReg
{
    uint8_t reg = 0;
    Value value;
    Type type = Type(TypeKind::Any);
};

struct Snapshot
{
    uint32_t id = 0;
    uint32_t pcpos = 0;
    std::vector<SnapshotReg> regs;
    std::vector<uint32_t> liveVirtualObjects;
    bool isFallback = false;
};

enum class CallEffect : uint8_t
{
    Pure,
    ReadOnly,
    Harmless,
    Mutating,
};

struct Inst
{
    Cmd cmd = Cmd::Nop;
    Type type = Type(TypeKind::Any);
    Range range;
    uint32_t pcpos = 0;
    uint32_t useCount = 0;

    SmallVector<Value, 4> args;

    // Side facts / attributes
    uint32_t extra = 0;
    std::string stringData;
    CallEffect callEffect = CallEffect::Mutating;

    Inst() = default;
    Inst(Cmd cmd, Type type = Type(TypeKind::Any))
        : cmd(cmd)
        , type(type)
    {
    }
};

enum class LoopKind : uint8_t
{
    Numeric,
    Generic,
    While,
    ExplicitCFG,
};

struct StructuredLoop
{
    LoopKind kind = LoopKind::Numeric;
    Value initValue;
    Value limitValue;
    Value stepValue;
    uint32_t headerBlock = ~0u;
    uint32_t bodyBlock = ~0u;
    uint32_t exitBlock = ~0u;
    Range inductionRange;
};

struct StructuredIf
{
    Value condition;
    uint32_t thenBlock = ~0u;
    uint32_t elseBlock = ~0u;
    uint32_t mergeBlock = ~0u;
};

struct BlockArg
{
    uint32_t blockIndex = 0;
    uint32_t argIndex = 0;
    Type type = Type(TypeKind::Any);
    Range range;
    std::vector<Value> incomingValues;
};

struct Block
{
    uint32_t index = 0;
    std::string name;
    std::vector<uint32_t> instIndices;
    std::vector<uint32_t> predecessors;
    std::vector<uint32_t> successors;
    std::vector<BlockArg> args;
    uint32_t loopHeader = ~0u;
    bool isLoopHeader = false;
    bool isDead = false;

    // Structured control metadata
    std::optional<StructuredLoop> structuredLoop;
    std::optional<StructuredIf> structuredIf;
};

struct Function
{
    Proto* proto = nullptr;
    std::vector<Block> blocks;
    std::vector<Inst> instructions;
    std::vector<ConstantValue> constants;
    std::vector<TableShape> tableShapes;
    std::vector<VirtualTable> virtualTables;
    std::vector<Multivalue> multivalues;
    std::vector<Snapshot> snapshots;
    uint32_t entryBlock = 0;

    Value addConstant(ConstantValue cv)
    {
        uint32_t idx = uint32_t(constants.size());
        constants.push_back(std::move(cv));
        return Value(ValueKind::Constant, idx);
    }

    Value addInstruction(Inst inst, uint32_t blockIdx)
    {
        uint32_t idx = uint32_t(instructions.size());
        instructions.push_back(std::move(inst));
        if (blockIdx < blocks.size())
            blocks[blockIdx].instIndices.push_back(idx);
        return Value(ValueKind::Inst, idx);
    }

    uint32_t createBlock(std::string name = "")
    {
        uint32_t idx = uint32_t(blocks.size());
        Block b;
        b.index = idx;
        b.name = name.empty() ? ("block" + std::to_string(idx)) : std::move(name);
        blocks.push_back(std::move(b));
        return idx;
    }

    uint32_t createTableShape()
    {
        uint32_t id = uint32_t(tableShapes.size());
        TableShape s;
        s.shapeId = id;
        tableShapes.push_back(std::move(s));
        return id;
    }

    uint32_t createVirtualTable(uint32_t shapeId = 0)
    {
        uint32_t id = uint32_t(virtualTables.size());
        VirtualTable vt;
        vt.id = id;
        vt.shapeId = shapeId;
        virtualTables.push_back(std::move(vt));
        return id;
    }

    uint32_t createMultivalue(std::vector<Value> vals, bool dynamic = false)
    {
        uint32_t id = uint32_t(multivalues.size());
        Multivalue mv;
        mv.id = id;
        mv.values = std::move(vals);
        mv.isDynamic = dynamic;
        mv.dynamicCount = uint32_t(mv.values.size());
        multivalues.push_back(std::move(mv));
        return id;
    }

    uint32_t createSnapshot(uint32_t pcpos)
    {
        uint32_t id = uint32_t(snapshots.size());
        Snapshot s;
        s.id = id;
        s.pcpos = pcpos;
        snapshots.push_back(std::move(s));
        return id;
    }
};

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
