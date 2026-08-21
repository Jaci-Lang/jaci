// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/HirData.h"
#include "Luau/IrData.h"

#include <unordered_map>
#include <vector>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

class HirBuilder
{
public:
    HirBuilder();

    // Lift existing IR function into semantic SSA HIR function
    Function liftFromIr(const IrFunction& irFunction);

    // Direct HIR construction primitives
    uint32_t createBlock(std::string name = "");
    void setInsertionBlock(uint32_t blockIdx);
    uint32_t getInsertionBlock() const;

    Value emit(Cmd cmd, Type type = Type(TypeKind::Any), SmallVector<Value, 4> args = {}, uint32_t extra = 0, std::string stringData = "");

    Value emitConstNil();
    Value emitConstBool(bool b);
    Value emitConstInt(int32_t i);
    Value emitConstInt64(int64_t i64);
    Value emitConstDouble(double d);
    Value emitConstString(std::string s);

    Value emitAdd(Value a, Value b, Type type = Type(TypeKind::Number));
    Value emitSub(Value a, Value b, Type type = Type(TypeKind::Number));
    Value emitMul(Value a, Value b, Type type = Type(TypeKind::Number));
    Value emitDiv(Value a, Value b, Type type = Type(TypeKind::Number));

    Value emitGetTable(Value table, Value key);
    Value emitSetTable(Value table, Value key, Value val);
    Value emitGetShapeSlot(Value table, uint32_t slotIdx, Type type = Type(TypeKind::Any));
    Value emitSetShapeSlot(Value table, uint32_t slotIdx, Value val);
    Value emitGetArrayElement(Value table, Value index, Type type = Type(TypeKind::Any));
    Value emitSetArrayElement(Value table, Value index, Value val);

    Value emitAllocTable(uint32_t shapeId = 0);
    Value emitAllocVirtualTable(uint32_t shapeId = 0);

    Value emitJump(uint32_t targetBlock);
    Value emitBranch(Value cond, uint32_t thenBlock, uint32_t elseBlock);
    Value emitReturn(SmallVector<Value, 4> returnValues);

    Value emitSnapshot(uint32_t pcpos);

    Function& getFunction()
    {
        return function;
    }

private:
    Function function;
    uint32_t currentBlock = ~0u;

    struct BlockState
    {
        std::unordered_map<uint8_t, Value> regMap;
        std::unordered_map<uint8_t, Type> regTypes;
    };

    std::vector<BlockState> blockStates;
    std::unordered_map<uint32_t, uint32_t> irToHirBlockMap;
    std::unordered_map<uint32_t, Value> irInstToHirValueMap;

    void liftBlock(const IrFunction& irFunction, uint32_t irBlockIdx);
    void liftInstruction(const IrFunction& irFunction, const IrInst& inst, uint32_t instIdx, BlockState& state);
    Value resolveIrOp(const IrFunction& irFunction, IrOp op, const BlockState& state);
};

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
