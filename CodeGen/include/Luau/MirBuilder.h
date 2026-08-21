// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/HirData.h"
#include "Luau/MirData.h"

#include <unordered_map>
#include <vector>

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

class MirBuilder
{
public:
    MirBuilder();

    // Lower optimized HIR function into concrete SSA MIR function
    Function lowerFromHir(const Hir::Function& hirFunction);

    // Direct MIR construction primitives
    uint32_t createBlock(std::string name = "");
    void setInsertionBlock(uint32_t blockIdx);
    uint32_t getInsertionBlock() const;

    Value emit(
        Cmd cmd,
        Type type = Type(TypeKind::Void),
        SmallVector<Value, 4> args = {},
        int32_t offset = 0,
        LocationClass loc = LocationClass::GenericHeap,
        MemoryEffect effect = MemoryEffect::None
    );

    Value emitConstBool(bool b);
    Value emitConstInt32(int32_t i);
    Value emitConstInt64(int64_t i);
    Value emitConstFloat64(double d);
    Value emitConstTag(uint8_t tag);
    Value emitConstNull();

    Value emitAddInt(Value a, Value b);
    Value emitSubInt(Value a, Value b);
    Value emitMulInt(Value a, Value b);
    Value emitDivInt(Value a, Value b);

    Value emitAddFloat(Value a, Value b);
    Value emitSubFloat(Value a, Value b);
    Value emitMulFloat(Value a, Value b);
    Value emitDivFloat(Value a, Value b);

    Value emitGuardTag(Value val, uint8_t expectedTag, uint32_t pcpos);
    Value emitGuardShape(Value table, uint32_t shapeId, uint32_t pcpos);
    Value emitGuardBounds(Value index, Value limit, uint32_t pcpos);
    Value emitGuardNotNil(Value val, uint32_t pcpos);

    Value emitLoad(Value ptr, int32_t offset, Type type, LocationClass loc);
    Value emitStore(Value ptr, int32_t offset, Value val, LocationClass loc);

    Value emitLoadField(Value table, int32_t slotOffset, Type type);
    Value emitStoreField(Value table, int32_t slotOffset, Value val);

    Value emitLoadArrayElement(Value table, Value index, Type type);
    Value emitStoreArrayElement(Value table, Value index, Value val);

    Value emitAllocTable(uint32_t shapeId);
    Value emitGcWriteBarrier(Value parent, Value child);

    Value emitJump(uint32_t targetBlock);
    Value emitBranchCond(Value cond, uint32_t thenBlock, uint32_t elseBlock);
    Value emitReturn(SmallVector<Value, 4> returnValues);

    Value emitSnapshot(uint32_t pcpos);

    Function& getFunction()
    {
        return function;
    }

private:
    Function function;
    uint32_t currentBlock = ~0u;

    std::unordered_map<uint32_t, uint32_t> hirToMirBlockMap;
    std::unordered_map<uint32_t, Value> hirInstToMirValueMap;
    std::unordered_map<uint32_t, Value> hirConstantToMirValueMap;

    Type mapHirTypeToMirType(const Hir::Type& htype);
    Value resolveHirValue(const Hir::Function& hirFunction, const Hir::Value& hval);
    void lowerHirBlock(const Hir::Function& hirFunction, uint32_t hirBlockIdx);
    void lowerHirInstruction(const Hir::Function& hirFunction, const Hir::Inst& hinst, uint32_t instIdx);
};

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
