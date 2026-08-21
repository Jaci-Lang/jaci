// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/HirBuilder.h"

#include "lobject.h"
#include "lstate.h"

#include <assert.h>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

HirBuilder::HirBuilder()
{
}

uint32_t HirBuilder::createBlock(std::string name)
{
    return function.createBlock(std::move(name));
}

void HirBuilder::setInsertionBlock(uint32_t blockIdx)
{
    currentBlock = blockIdx;
}

uint32_t HirBuilder::getInsertionBlock() const
{
    return currentBlock;
}

Value HirBuilder::emit(Cmd cmd, Type type, SmallVector<Value, 4> args, uint32_t extra, std::string stringData)
{
    Inst inst(cmd, type);
    inst.args = std::move(args);
    inst.extra = extra;
    inst.stringData = std::move(stringData);
    return function.addInstruction(std::move(inst), currentBlock);
}

Value HirBuilder::emitConstNil()
{
    ConstantValue cv;
    cv.kind = TypeKind::Nil;
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstNil, Type(TypeKind::Nil), {cval});
}

Value HirBuilder::emitConstBool(bool b)
{
    ConstantValue cv(b);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstBool, Type(TypeKind::Boolean), {cval});
}

Value HirBuilder::emitConstInt(int32_t i)
{
    ConstantValue cv(i);
    Value cval = function.addConstant(cv);
    Value v = emit(Cmd::ConstInt, Type(TypeKind::Integer), {cval});
    function.instructions[v.index].range = Range(i);
    return v;
}

Value HirBuilder::emitConstInt64(int64_t i64)
{
    ConstantValue cv(i64);
    Value cval = function.addConstant(cv);
    Value v = emit(Cmd::ConstInt64, Type(TypeKind::Integer), {cval});
    function.instructions[v.index].range = Range(i64);
    return v;
}

Value HirBuilder::emitConstDouble(double d)
{
    ConstantValue cv(d);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstDouble, Type(TypeKind::Number), {cval});
}

Value HirBuilder::emitConstString(std::string s)
{
    ConstantValue cv(s);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstString, Type(TypeKind::String), {cval}, 0, std::move(s));
}

Value HirBuilder::emitAdd(Value a, Value b, Type type)
{
    return emit(Cmd::Add, type, {a, b});
}

Value HirBuilder::emitSub(Value a, Value b, Type type)
{
    return emit(Cmd::Sub, type, {a, b});
}

Value HirBuilder::emitMul(Value a, Value b, Type type)
{
    return emit(Cmd::Mul, type, {a, b});
}

Value HirBuilder::emitDiv(Value a, Value b, Type type)
{
    return emit(Cmd::Div, type, {a, b});
}

Value HirBuilder::emitGetTable(Value table, Value key)
{
    return emit(Cmd::GetTable, Type(TypeKind::Any), {table, key});
}

Value HirBuilder::emitSetTable(Value table, Value key, Value val)
{
    return emit(Cmd::SetTable, Type(TypeKind::Any), {table, key, val});
}

Value HirBuilder::emitGetShapeSlot(Value table, uint32_t slotIdx, Type type)
{
    return emit(Cmd::GetShapeSlot, type, {table}, slotIdx);
}

Value HirBuilder::emitSetShapeSlot(Value table, uint32_t slotIdx, Value val)
{
    return emit(Cmd::SetShapeSlot, Type(TypeKind::Any), {table, val}, slotIdx);
}

Value HirBuilder::emitGetArrayElement(Value table, Value index, Type type)
{
    return emit(Cmd::GetArrayElement, type, {table, index});
}

Value HirBuilder::emitSetArrayElement(Value table, Value index, Value val)
{
    return emit(Cmd::SetArrayElement, Type(TypeKind::Any), {table, index, val});
}

Value HirBuilder::emitAllocTable(uint32_t shapeId)
{
    return emit(Cmd::AllocTable, Type(TypeKind::Table), {}, shapeId);
}

Value HirBuilder::emitAllocVirtualTable(uint32_t shapeId)
{
    uint32_t vtId = function.createVirtualTable(shapeId);
    return emit(Cmd::AllocVirtualTable, Type(TypeKind::Table), {}, vtId);
}

Value HirBuilder::emitJump(uint32_t targetBlock)
{
    Value targetVal(ValueKind::Inst, targetBlock);
    return emit(Cmd::Jump, Type(TypeKind::Bottom), {targetVal});
}

Value HirBuilder::emitBranch(Value cond, uint32_t thenBlock, uint32_t elseBlock)
{
    Value tVal(ValueKind::Inst, thenBlock);
    Value eVal(ValueKind::Inst, elseBlock);
    return emit(Cmd::Branch, Type(TypeKind::Bottom), {cond, tVal, eVal});
}

Value HirBuilder::emitReturn(SmallVector<Value, 4> returnValues)
{
    return emit(Cmd::Return, Type(TypeKind::Bottom), std::move(returnValues));
}

Value HirBuilder::emitSnapshot(uint32_t pcpos)
{
    uint32_t snapId = function.createSnapshot(pcpos);
    return emit(Cmd::Snapshot, Type(TypeKind::Any), {}, snapId);
}

Value HirBuilder::resolveIrOp(const IrFunction& irFunction, IrOp op, const BlockState& state)
{
    switch (op.kind)
    {
    case IrOpKind::None:
    case IrOpKind::Undef:
        return Value();
    case IrOpKind::Constant:
    {
        uint32_t cidx = op.index;
        if (cidx < irFunction.constants.size())
        {
            const IrConst& c = irFunction.constants[cidx];
            switch (c.kind)
            {
            case IrConstKind::Int:
                return emitConstInt(c.valueInt);
            case IrConstKind::Int64:
                return emitConstInt64(c.valueInt64);
            case IrConstKind::Uint:
                return emitConstInt(int32_t(c.valueUint));
            case IrConstKind::Double:
                return emitConstDouble(c.valueDouble);
            case IrConstKind::Tag:
                return emit(Cmd::ConstTag, Type(TypeKind::Integer), {}, c.valueTag);
            default:
                break;
            }
        }
        return Value();
    }
    case IrOpKind::Inst:
    {
        auto it = irInstToHirValueMap.find(op.index);
        if (it != irInstToHirValueMap.end())
            return it->second;
        return Value();
    }
    case IrOpKind::VmReg:
    {
        uint8_t reg = uint8_t(op.index);
        auto it = state.regMap.find(reg);
        if (it != state.regMap.end())
            return it->second;
        return Value(ValueKind::VmRegister, reg);
    }
    case IrOpKind::VmConst:
    {
        if (irFunction.proto && op.index < uint32_t(irFunction.proto->sizek))
        {
            TValue k = irFunction.proto->k[op.index];
            if (k.tt == LUA_TNIL)
                return emitConstNil();
            if (k.tt == LUA_TBOOLEAN)
                return emitConstBool(bvalue(&k) != 0);
            if (k.tt == LUA_TNUMBER)
                return emitConstDouble(nvalue(&k));
            if (k.tt == LUA_TSTRING)
                return emitConstString(getstr(tsvalue(&k)));
        }
        return Value();
    }
    default:
        return Value();
    }
}

void HirBuilder::liftInstruction(const IrFunction& irFunction, const IrInst& inst, uint32_t instIdx, BlockState& state)
{
    switch (inst.cmd)
    {
    case IrCmd::NOP:
        break;

    case IrCmd::LOAD_TAG:
    case IrCmd::LOAD_DOUBLE:
    case IrCmd::LOAD_INT:
    case IrCmd::LOAD_INT64:
    case IrCmd::LOAD_POINTER:
    case IrCmd::LOAD_TVALUE:
    {
        IrOp src = inst.ops.size() > 0 ? inst.ops[0] : IrOp{};
        if (src.kind == IrOpKind::VmReg)
        {
            uint8_t reg = uint8_t(src.index);
            auto it = state.regMap.find(reg);
            if (it != state.regMap.end())
            {
                irInstToHirValueMap[instIdx] = it->second;
            }
            else
            {
                Value v = emit(Cmd::LoadVmReg, Type(TypeKind::Any), {}, reg);
                irInstToHirValueMap[instIdx] = v;
                state.regMap[reg] = v;
            }
        }
        else
        {
            Value sval = resolveIrOp(irFunction, src, state);
            irInstToHirValueMap[instIdx] = sval;
        }
        break;
    }

    case IrCmd::STORE_TAG:
    case IrCmd::STORE_DOUBLE:
    case IrCmd::STORE_INT:
    case IrCmd::STORE_INT64:
    case IrCmd::STORE_POINTER:
    case IrCmd::STORE_TVALUE:
    {
        IrOp dst = inst.ops.size() > 0 ? inst.ops[0] : IrOp{};
        IrOp val = inst.ops.size() > 1 ? inst.ops[1] : IrOp{};
        if (dst.kind == IrOpKind::VmReg)
        {
            uint8_t reg = uint8_t(dst.index);
            Value hval = resolveIrOp(irFunction, val, state);
            state.regMap[reg] = hval;
            Value sv = emit(Cmd::StoreVmReg, Type(TypeKind::Any), {hval}, reg);
            irInstToHirValueMap[instIdx] = sv;
        }
        break;
    }

    case IrCmd::ADD_NUM:
    case IrCmd::ADD_INT:
    case IrCmd::ADD_INT64:
    {
        Value a = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        Value b = resolveIrOp(irFunction, inst.ops.size() > 1 ? inst.ops[1] : IrOp{}, state);
        Type ty = (inst.cmd == IrCmd::ADD_NUM) ? Type(TypeKind::Number) : Type(TypeKind::Integer);
        Value res = emit(Cmd::Add, ty, {a, b});
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::SUB_NUM:
    case IrCmd::SUB_INT:
    case IrCmd::SUB_INT64:
    {
        Value a = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        Value b = resolveIrOp(irFunction, inst.ops.size() > 1 ? inst.ops[1] : IrOp{}, state);
        Type ty = (inst.cmd == IrCmd::SUB_NUM) ? Type(TypeKind::Number) : Type(TypeKind::Integer);
        Value res = emit(Cmd::Sub, ty, {a, b});
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::MUL_NUM:
    case IrCmd::MUL_INT64:
    {
        Value a = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        Value b = resolveIrOp(irFunction, inst.ops.size() > 1 ? inst.ops[1] : IrOp{}, state);
        Type ty = (inst.cmd == IrCmd::MUL_NUM) ? Type(TypeKind::Number) : Type(TypeKind::Integer);
        Value res = emit(Cmd::Mul, ty, {a, b});
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::DIV_NUM:
    case IrCmd::DIV_INT64:
    {
        Value a = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        Value b = resolveIrOp(irFunction, inst.ops.size() > 1 ? inst.ops[1] : IrOp{}, state);
        Type ty = (inst.cmd == IrCmd::DIV_NUM) ? Type(TypeKind::Number) : Type(TypeKind::Integer);
        Value res = emit(Cmd::Div, ty, {a, b});
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::GET_TABLE:
    {
        Value tbl = resolveIrOp(irFunction, inst.ops.size() > 1 ? inst.ops[1] : IrOp{}, state);
        Value key = resolveIrOp(irFunction, inst.ops.size() > 2 ? inst.ops[2] : IrOp{}, state);
        Value res = emit(Cmd::GetTable, Type(TypeKind::Any), {tbl, key});
        irInstToHirValueMap[instIdx] = res;
        if (inst.ops.size() > 0 && inst.ops[0].kind == IrOpKind::VmReg)
            state.regMap[uint8_t(inst.ops[0].index)] = res;
        break;
    }

    case IrCmd::SET_TABLE:
    {
        Value tbl = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        Value key = resolveIrOp(irFunction, inst.ops.size() > 1 ? inst.ops[1] : IrOp{}, state);
        Value val = resolveIrOp(irFunction, inst.ops.size() > 2 ? inst.ops[2] : IrOp{}, state);
        Value res = emit(Cmd::SetTable, Type(TypeKind::Any), {tbl, key, val});
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::JUMP:
    {
        uint32_t targetIrBlock = inst.ops.size() > 0 ? inst.ops[0].index : 0;
        auto it = irToHirBlockMap.find(targetIrBlock);
        uint32_t targetHirBlock = (it != irToHirBlockMap.end()) ? it->second : targetIrBlock;
        Value res = emitJump(targetHirBlock);
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::JUMP_IF_TRUTHY:
    case IrCmd::JUMP_IF_FALSY:
    {
        Value cond = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        uint32_t thenBlock = (inst.cmd == IrCmd::JUMP_IF_TRUTHY) ? (inst.ops.size() > 1 ? inst.ops[1].index : 0)
                                                                 : (inst.ops.size() > 2 ? inst.ops[2].index : 0);
        uint32_t elseBlock = (inst.cmd == IrCmd::JUMP_IF_TRUTHY) ? (inst.ops.size() > 2 ? inst.ops[2].index : 0)
                                                                 : (inst.ops.size() > 1 ? inst.ops[1].index : 0);

        auto itt = irToHirBlockMap.find(thenBlock);
        auto ite = irToHirBlockMap.find(elseBlock);

        uint32_t ht = (itt != irToHirBlockMap.end()) ? itt->second : thenBlock;
        uint32_t he = (ite != irToHirBlockMap.end()) ? ite->second : elseBlock;

        Value res = emitBranch(cond, ht, he);
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::CHECK_TAG:
    {
        Value val = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        uint8_t expectedTag = inst.ops.size() > 1 ? uint8_t(inst.ops[1].index) : 0;
        Value res = emit(Cmd::CheckTag, Type(TypeKind::Any), {val}, expectedTag);
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    case IrCmd::RETURN:
    {
        Value retVal = resolveIrOp(irFunction, inst.ops.size() > 0 ? inst.ops[0] : IrOp{}, state);
        SmallVector<Value, 4> retVals;
        if (!retVal.isNone())
            retVals.push_back(retVal);
        Value res = emitReturn(std::move(retVals));
        irInstToHirValueMap[instIdx] = res;
        break;
    }

    default:
        break;
    }
}

void HirBuilder::liftBlock(const IrFunction& irFunction, uint32_t irBlockIdx)
{
    const IrBlock& irBlock = irFunction.blocks[irBlockIdx];
    if (irBlock.kind == IrBlockKind::Dead)
        return;

    uint32_t hirBlockIdx = irToHirBlockMap[irBlockIdx];
    setInsertionBlock(hirBlockIdx);

    BlockState state;
    if (hirBlockIdx < blockStates.size())
        state = blockStates[hirBlockIdx];

    if (irBlock.start != ~0u && irBlock.finish != ~0u)
    {
        for (uint32_t i = irBlock.start; i <= irBlock.finish; ++i)
        {
            if (i < irFunction.instructions.size())
            {
                liftInstruction(irFunction, irFunction.instructions[i], i, state);
            }
        }
    }

    if (hirBlockIdx < blockStates.size())
        blockStates[hirBlockIdx] = std::move(state);
}

Function HirBuilder::liftFromIr(const IrFunction& irFunction)
{
    function.proto = irFunction.proto;
    function.blocks.clear();
    function.instructions.clear();
    function.constants.clear();
    function.tableShapes.clear();
    function.virtualTables.clear();
    function.multivalues.clear();
    function.snapshots.clear();

    irToHirBlockMap.clear();
    irInstToHirValueMap.clear();

    for (size_t i = 0; i < irFunction.blocks.size(); ++i)
    {
        const IrBlock& b = irFunction.blocks[i];
        if (b.kind != IrBlockKind::Dead)
        {
            uint32_t hirIdx = createBlock("bb" + std::to_string(i));
            irToHirBlockMap[uint32_t(i)] = hirIdx;
        }
    }

    blockStates.resize(function.blocks.size());

    for (size_t i = 0; i < irFunction.blocks.size(); ++i)
    {
        const IrBlock& b = irFunction.blocks[i];
        if (b.kind != IrBlockKind::Dead)
        {
            liftBlock(irFunction, uint32_t(i));
        }
    }

    return std::move(function);
}

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
