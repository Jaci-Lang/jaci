// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/Common.h"
#include "Luau/DenseHash2.h"
#include "Luau/SmallVector.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <stdint.h>

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

enum class TypeKind : uint8_t
{
    Void,
    Bool,
    Int32,
    Int64,
    Float64,
    Tag,
    RawPtr,
    GcPtr,
    ManagedRef,
    TValueBoxed,
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
        return kind == TypeKind::Int32 || kind == TypeKind::Int64;
    }
    bool isFloat() const
    {
        return kind == TypeKind::Float64;
    }
    bool isPointer() const
    {
        return kind == TypeKind::RawPtr || kind == TypeKind::GcPtr || kind == TypeKind::ManagedRef;
    }
    bool isBoxed() const
    {
        return kind == TypeKind::TValueBoxed;
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

enum class LocationClass : uint8_t
{
    GenericHeap,
    TableHeader,
    TableProperties,
    TableArray,
    UpvalueSlot,
    BufferBytes,
    GlobalEnv,
    VmRegisterStack,
};

enum class MemoryEffect : uint8_t
{
    None,
    Read,
    Write,
    ReadWrite,
    GcBarrier,
};

enum class Cmd : uint16_t
{
    Nop,

    // Constants
    ConstBool,
    ConstInt32,
    ConstInt64,
    ConstFloat64,
    ConstTag,
    ConstNull,

    // SSA & Control Flow
    BlockArg,
    Jump,
    BranchCond,
    Switch,
    Return,
    Unreachable,
    VmExit,

    // Concrete Arithmetic & Logic
    AddInt,
    SubInt,
    MulInt,
    DivInt,
    ModInt,
    AndInt,
    OrInt,
    XorInt,
    ShlInt,
    ShrInt,
    SarInt,
    NegInt,
    NotInt,

    AddFloat,
    SubFloat,
    MulFloat,
    DivFloat,
    NegFloat,
    AbsFloat,
    SqrtFloat,

    // Comparisons
    CmpEqInt,
    CmpLtInt,
    CmpLeInt,
    CmpEqFloat,
    CmpLtFloat,
    CmpLeFloat,
    CmpEqPtr,

    // Conversions & Representations
    IntToFloat,
    FloatToInt,
    BoxValue,
    UnboxValue,
    GetTag,
    GetPayload,

    // Explicit Guards
    GuardTag,
    GuardShape,
    GuardBounds,
    GuardNotNil,
    GuardCondition,

    // Concrete Memory Operations
    Load,
    Store,
    LoadField,
    StoreField,
    LoadArrayElement,
    StoreArrayElement,
    GetElementPtr,

    // GC & Allocations
    AllocTable,
    AllocClosure,
    AllocBuffer,
    GcWriteBarrier,

    // Calls
    CallDirect,
    CallIndirect,
    CallBuiltin,

    // Metadata
    Snapshot,
};

enum class ValueKind : uint8_t
{
    None,
    Inst,
    Constant,
    BlockArg,
};

struct ConstantValue
{
    TypeKind kind = TypeKind::Void;
    union
    {
        bool b;
        int32_t i32;
        int64_t i64;
        double f64;
        uint8_t tag;
        uint64_t raw;
    } val = {};

    ConstantValue() = default;
    explicit ConstantValue(bool b)
        : kind(TypeKind::Bool)
    {
        val.b = b;
    }
    explicit ConstantValue(int32_t i)
        : kind(TypeKind::Int32)
    {
        val.i32 = i;
    }
    explicit ConstantValue(int64_t i)
        : kind(TypeKind::Int64)
    {
        val.i64 = i;
    }
    explicit ConstantValue(double d)
        : kind(TypeKind::Float64)
    {
        val.f64 = d;
    }
    explicit ConstantValue(uint8_t tag, bool)
        : kind(TypeKind::Tag)
    {
        val.tag = tag;
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

    bool operator==(const Value& o) const
    {
        return kind == o.kind && index == o.index;
    }
    bool operator!=(const Value& o) const
    {
        return !(*this == o);
    }
};

struct SnapshotSlot
{
    uint8_t reg = 0;
    Value value;
    Type type;
};

struct Snapshot
{
    uint32_t id = 0;
    uint32_t pcpos = 0;
    std::vector<SnapshotSlot> slots;
};

struct Inst
{
    Cmd cmd = Cmd::Nop;
    Type type = Type(TypeKind::Void);
    SmallVector<Value, 4> args;
    uint32_t extra = 0;
    int32_t offset = 0;
    LocationClass locationClass = LocationClass::GenericHeap;
    MemoryEffect memoryEffect = MemoryEffect::None;
    uint32_t pcpos = 0;
    uint32_t useCount = 0;

    Inst() = default;
    Inst(Cmd cmd, Type type = Type(TypeKind::Void))
        : cmd(cmd)
        , type(type)
    {
    }
};

struct BlockArg
{
    uint32_t blockIndex = 0;
    uint32_t argIndex = 0;
    Type type = Type(TypeKind::Void);
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
    bool isDead = false;
    bool isLoopHeader = false;
};

struct Function
{
    std::vector<Block> blocks;
    std::vector<Inst> instructions;
    std::vector<ConstantValue> constants;
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
        b.name = name.empty() ? ("mir_bb" + std::to_string(idx)) : std::move(name);
        blocks.push_back(std::move(b));
        return idx;
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

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
