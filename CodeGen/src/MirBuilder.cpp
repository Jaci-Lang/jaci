// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/MirBuilder.h"

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

MirBuilder::MirBuilder()
{
}

uint32_t MirBuilder::createBlock(std::string name)
{
    return function.createBlock(std::move(name));
}

void MirBuilder::setInsertionBlock(uint32_t blockIdx)
{
    currentBlock = blockIdx;
}

uint32_t MirBuilder::getInsertionBlock() const
{
    return currentBlock;
}

Value MirBuilder::emit(Cmd cmd, Type type, SmallVector<Value, 4> args, int32_t offset, LocationClass loc, MemoryEffect effect)
{
    Inst inst(cmd, type);
    inst.args = std::move(args);
    inst.offset = offset;
    inst.locationClass = loc;
    inst.memoryEffect = effect;
    return function.addInstruction(std::move(inst), currentBlock);
}

Value MirBuilder::emitConstBool(bool b)
{
    ConstantValue cv(b);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstBool, Type(TypeKind::Bool), {cval});
}

Value MirBuilder::emitConstInt32(int32_t i)
{
    ConstantValue cv(i);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstInt32, Type(TypeKind::Int32), {cval});
}

Value MirBuilder::emitConstInt64(int64_t i)
{
    ConstantValue cv(i);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstInt64, Type(TypeKind::Int64), {cval});
}

Value MirBuilder::emitConstFloat64(double d)
{
    ConstantValue cv(d);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstFloat64, Type(TypeKind::Float64), {cval});
}

Value MirBuilder::emitConstTag(uint8_t tag)
{
    ConstantValue cv(tag, true);
    Value cval = function.addConstant(cv);
    return emit(Cmd::ConstTag, Type(TypeKind::Tag), {cval});
}

Value MirBuilder::emitConstNull()
{
    return emit(Cmd::ConstNull, Type(TypeKind::RawPtr));
}

Value MirBuilder::emitAddInt(Value a, Value b)
{
    return emit(Cmd::AddInt, Type(TypeKind::Int32), {a, b});
}

Value MirBuilder::emitSubInt(Value a, Value b)
{
    return emit(Cmd::SubInt, Type(TypeKind::Int32), {a, b});
}

Value MirBuilder::emitMulInt(Value a, Value b)
{
    return emit(Cmd::MulInt, Type(TypeKind::Int32), {a, b});
}

Value MirBuilder::emitDivInt(Value a, Value b)
{
    return emit(Cmd::DivInt, Type(TypeKind::Int32), {a, b});
}

Value MirBuilder::emitAddFloat(Value a, Value b)
{
    return emit(Cmd::AddFloat, Type(TypeKind::Float64), {a, b});
}

Value MirBuilder::emitSubFloat(Value a, Value b)
{
    return emit(Cmd::SubFloat, Type(TypeKind::Float64), {a, b});
}

Value MirBuilder::emitMulFloat(Value a, Value b)
{
    return emit(Cmd::MulFloat, Type(TypeKind::Float64), {a, b});
}

Value MirBuilder::emitDivFloat(Value a, Value b)
{
    return emit(Cmd::DivFloat, Type(TypeKind::Float64), {a, b});
}

Value MirBuilder::emitGuardTag(Value val, uint8_t expectedTag, uint32_t pcpos)
{
    Value tagVal = emitConstTag(expectedTag);
    Value g = emit(Cmd::GuardTag, Type(TypeKind::Void), {val, tagVal});
    function.instructions[g.index].extra = expectedTag;
    function.instructions[g.index].pcpos = pcpos;
    return g;
}

Value MirBuilder::emitGuardShape(Value table, uint32_t shapeId, uint32_t pcpos)
{
    Value shapeVal = emitConstInt32(int32_t(shapeId));
    Value g = emit(Cmd::GuardShape, Type(TypeKind::Void), {table, shapeVal});
    function.instructions[g.index].extra = shapeId;
    function.instructions[g.index].pcpos = pcpos;
    return g;
}

Value MirBuilder::emitGuardBounds(Value index, Value limit, uint32_t pcpos)
{
    Value g = emit(Cmd::GuardBounds, Type(TypeKind::Void), {index, limit});
    function.instructions[g.index].pcpos = pcpos;
    return g;
}

Value MirBuilder::emitGuardNotNil(Value val, uint32_t pcpos)
{
    Value g = emit(Cmd::GuardNotNil, Type(TypeKind::Void), {val});
    function.instructions[g.index].pcpos = pcpos;
    return g;
}

Value MirBuilder::emitLoad(Value ptr, int32_t offset, Type type, LocationClass loc)
{
    return emit(Cmd::Load, type, {ptr}, offset, loc, MemoryEffect::Read);
}

Value MirBuilder::emitStore(Value ptr, int32_t offset, Value val, LocationClass loc)
{
    return emit(Cmd::Store, Type(TypeKind::Void), {ptr, val}, offset, loc, MemoryEffect::Write);
}

Value MirBuilder::emitLoadField(Value table, int32_t slotOffset, Type type)
{
    return emit(Cmd::LoadField, type, {table}, slotOffset, LocationClass::TableProperties, MemoryEffect::Read);
}

Value MirBuilder::emitStoreField(Value table, int32_t slotOffset, Value val)
{
    return emit(Cmd::StoreField, Type(TypeKind::Void), {table, val}, slotOffset, LocationClass::TableProperties, MemoryEffect::Write);
}

Value MirBuilder::emitLoadArrayElement(Value table, Value index, Type type)
{
    return emit(Cmd::LoadArrayElement, type, {table, index}, 0, LocationClass::TableArray, MemoryEffect::Read);
}

Value MirBuilder::emitStoreArrayElement(Value table, Value index, Value val)
{
    return emit(Cmd::StoreArrayElement, Type(TypeKind::Void), {table, index, val}, 0, LocationClass::TableArray, MemoryEffect::Write);
}

Value MirBuilder::emitAllocTable(uint32_t shapeId)
{
    Value shapeVal = emitConstInt32(int32_t(shapeId));
    return emit(Cmd::AllocTable, Type(TypeKind::GcPtr), {shapeVal}, 0, LocationClass::TableHeader, MemoryEffect::Write);
}

Value MirBuilder::emitGcWriteBarrier(Value parent, Value child)
{
    return emit(Cmd::GcWriteBarrier, Type(TypeKind::Void), {parent, child}, 0, LocationClass::GenericHeap, MemoryEffect::GcBarrier);
}

Value MirBuilder::emitJump(uint32_t targetBlock)
{
    Value targetVal(ValueKind::Inst, targetBlock);
    return emit(Cmd::Jump, Type(TypeKind::Void), {targetVal});
}

Value MirBuilder::emitBranchCond(Value cond, uint32_t thenBlock, uint32_t elseBlock)
{
    Value tVal(ValueKind::Inst, thenBlock);
    Value eVal(ValueKind::Inst, elseBlock);
    return emit(Cmd::BranchCond, Type(TypeKind::Void), {cond, tVal, eVal});
}

Value MirBuilder::emitReturn(SmallVector<Value, 4> returnValues)
{
    return emit(Cmd::Return, Type(TypeKind::Void), std::move(returnValues));
}

Value MirBuilder::emitSnapshot(uint32_t pcpos)
{
    uint32_t snapId = function.createSnapshot(pcpos);
    return emit(Cmd::Snapshot, Type(TypeKind::Void), {}, int32_t(snapId));
}

Type MirBuilder::mapHirTypeToMirType(const Hir::Type& htype)
{
    switch (htype.kind)
    {
    case Hir::TypeKind::Nil:
    case Hir::TypeKind::Bottom:
        return Type(TypeKind::Void);
    case Hir::TypeKind::Boolean:
        return Type(TypeKind::Bool);
    case Hir::TypeKind::Integer:
        return Type(TypeKind::Int32);
    case Hir::TypeKind::Number:
        return Type(TypeKind::Float64);
    case Hir::TypeKind::String:
    case Hir::TypeKind::Table:
    case Hir::TypeKind::Function:
    case Hir::TypeKind::Thread:
    case Hir::TypeKind::Userdata:
    case Hir::TypeKind::Buffer:
        return Type(TypeKind::GcPtr);
    case Hir::TypeKind::Vector:
    case Hir::TypeKind::Any:
    default:
        return Type(TypeKind::TValueBoxed);
    }
}

Value MirBuilder::resolveHirValue(const Hir::Function& hirFunction, const Hir::Value& hval)
{
    switch (hval.kind)
    {
    case Hir::ValueKind::None:
        return Value();
    case Hir::ValueKind::Inst:
    {
        auto it = hirInstToMirValueMap.find(hval.index);
        if (it != hirInstToMirValueMap.end())
            return it->second;
        return Value();
    }
    case Hir::ValueKind::Constant:
    {
        auto it = hirConstantToMirValueMap.find(hval.index);
        if (it != hirConstantToMirValueMap.end())
            return it->second;

        if (hval.index < hirFunction.constants.size())
        {
            const Hir::ConstantValue& hc = hirFunction.constants[hval.index];
            Value mv;
            switch (hc.kind)
            {
            case Hir::TypeKind::Boolean:
                mv = emitConstBool(hc.val.b);
                break;
            case Hir::TypeKind::Integer:
                mv = emitConstInt32(hc.val.i);
                break;
            case Hir::TypeKind::Number:
                mv = emitConstFloat64(hc.val.d);
                break;
            default:
                mv = emitConstNull();
                break;
            }
            hirConstantToMirValueMap[hval.index] = mv;
            return mv;
        }
        return Value();
    }
    case Hir::ValueKind::BlockArg:
        return Value(ValueKind::BlockArg, hval.index);
    default:
        return Value();
    }
}

void MirBuilder::lowerHirInstruction(const Hir::Function& hirFunction, const Hir::Inst& hinst, uint32_t instIdx)
{
    switch (hinst.cmd)
    {
    case Hir::Cmd::Nop:
        break;

    case Hir::Cmd::ConstNil:
        hirInstToMirValueMap[instIdx] = emitConstNull();
        break;

    case Hir::Cmd::ConstBool:
        if (!hinst.args.empty())
        {
            Value bval = resolveHirValue(hirFunction, hinst.args[0]);
            hirInstToMirValueMap[instIdx] = bval;
        }
        break;

    case Hir::Cmd::ConstInt:
        if (!hinst.args.empty())
        {
            Value ival = resolveHirValue(hirFunction, hinst.args[0]);
            hirInstToMirValueMap[instIdx] = ival;
        }
        break;

    case Hir::Cmd::ConstDouble:
        if (!hinst.args.empty())
        {
            Value dval = resolveHirValue(hirFunction, hinst.args[0]);
            hirInstToMirValueMap[instIdx] = dval;
        }
        break;

    case Hir::Cmd::Add:
    case Hir::Cmd::Sub:
    case Hir::Cmd::Mul:
    case Hir::Cmd::Div:
    {
        Value a = resolveHirValue(hirFunction, hinst.args[0]);
        Value b = resolveHirValue(hirFunction, hinst.args[1]);
        if (hinst.type.kind == Hir::TypeKind::Integer)
        {
            if (hinst.cmd == Hir::Cmd::Add)
                hirInstToMirValueMap[instIdx] = emitAddInt(a, b);
            else if (hinst.cmd == Hir::Cmd::Sub)
                hirInstToMirValueMap[instIdx] = emitSubInt(a, b);
            else if (hinst.cmd == Hir::Cmd::Mul)
                hirInstToMirValueMap[instIdx] = emitMulInt(a, b);
            else if (hinst.cmd == Hir::Cmd::Div)
                hirInstToMirValueMap[instIdx] = emitDivInt(a, b);
        }
        else
        {
            if (hinst.cmd == Hir::Cmd::Add)
                hirInstToMirValueMap[instIdx] = emitAddFloat(a, b);
            else if (hinst.cmd == Hir::Cmd::Sub)
                hirInstToMirValueMap[instIdx] = emitSubFloat(a, b);
            else if (hinst.cmd == Hir::Cmd::Mul)
                hirInstToMirValueMap[instIdx] = emitMulFloat(a, b);
            else if (hinst.cmd == Hir::Cmd::Div)
                hirInstToMirValueMap[instIdx] = emitDivFloat(a, b);
        }
        break;
    }

    case Hir::Cmd::CheckTag:
    {
        Value val = resolveHirValue(hirFunction, hinst.args[0]);
        uint8_t tag = uint8_t(hinst.extra);
        hirInstToMirValueMap[instIdx] = emitGuardTag(val, tag, hinst.pcpos);
        break;
    }

    case Hir::Cmd::CheckShape:
    {
        Value tbl = resolveHirValue(hirFunction, hinst.args[0]);
        uint32_t shapeId = hinst.extra;
        hirInstToMirValueMap[instIdx] = emitGuardShape(tbl, shapeId, hinst.pcpos);
        break;
    }

    case Hir::Cmd::GetShapeSlot:
    {
        Value tbl = resolveHirValue(hirFunction, hinst.args[0]);
        int32_t slotOffset = int32_t(hinst.extra * 16); // 16 bytes per TValue slot
        Type resType = mapHirTypeToMirType(hinst.type);
        hirInstToMirValueMap[instIdx] = emitLoadField(tbl, slotOffset, resType);
        break;
    }

    case Hir::Cmd::SetShapeSlot:
    {
        Value tbl = resolveHirValue(hirFunction, hinst.args[0]);
        Value val = resolveHirValue(hirFunction, hinst.args[1]);
        int32_t slotOffset = int32_t(hinst.extra * 16);
        hirInstToMirValueMap[instIdx] = emitStoreField(tbl, slotOffset, val);
        emitGcWriteBarrier(tbl, val);
        break;
    }

    case Hir::Cmd::GetArrayElement:
    {
        Value tbl = resolveHirValue(hirFunction, hinst.args[0]);
        Value idx = resolveHirValue(hirFunction, hinst.args[1]);
        Type elemType = mapHirTypeToMirType(hinst.type);
        hirInstToMirValueMap[instIdx] = emitLoadArrayElement(tbl, idx, elemType);
        break;
    }

    case Hir::Cmd::SetArrayElement:
    {
        Value tbl = resolveHirValue(hirFunction, hinst.args[0]);
        Value idx = resolveHirValue(hirFunction, hinst.args[1]);
        Value val = resolveHirValue(hirFunction, hinst.args[2]);
        hirInstToMirValueMap[instIdx] = emitStoreArrayElement(tbl, idx, val);
        emitGcWriteBarrier(tbl, val);
        break;
    }

    case Hir::Cmd::AllocTable:
    {
        hirInstToMirValueMap[instIdx] = emitAllocTable(hinst.extra);
        break;
    }

    case Hir::Cmd::Jump:
    {
        if (!hinst.args.empty())
        {
            uint32_t targetHirBlock = hinst.args[0].index;
            auto it = hirToMirBlockMap.find(targetHirBlock);
            uint32_t targetMirBlock = (it != hirToMirBlockMap.end()) ? it->second : targetHirBlock;
            hirInstToMirValueMap[instIdx] = emitJump(targetMirBlock);
        }
        break;
    }

    case Hir::Cmd::Branch:
    {
        if (hinst.args.size() >= 3)
        {
            Value cond = resolveHirValue(hirFunction, hinst.args[0]);
            uint32_t thHir = hinst.args[1].index;
            uint32_t elHir = hinst.args[2].index;

            auto itt = hirToMirBlockMap.find(thHir);
            auto ite = hirToMirBlockMap.find(elHir);

            uint32_t mt = (itt != hirToMirBlockMap.end()) ? itt->second : thHir;
            uint32_t me = (ite != hirToMirBlockMap.end()) ? ite->second : elHir;

            hirInstToMirValueMap[instIdx] = emitBranchCond(cond, mt, me);
        }
        break;
    }

    case Hir::Cmd::Return:
    {
        SmallVector<Value, 4> retVals;
        for (const Hir::Value& hv : hinst.args)
        {
            retVals.push_back(resolveHirValue(hirFunction, hv));
        }
        hirInstToMirValueMap[instIdx] = emitReturn(std::move(retVals));
        break;
    }

    case Hir::Cmd::Snapshot:
    {
        hirInstToMirValueMap[instIdx] = emitSnapshot(hinst.pcpos);
        break;
    }

    default:
        break;
    }
}

void MirBuilder::lowerHirBlock(const Hir::Function& hirFunction, uint32_t hirBlockIdx)
{
    const Hir::Block& hblock = hirFunction.blocks[hirBlockIdx];
    if (hblock.isDead)
        return;

    uint32_t mirBlockIdx = hirToMirBlockMap[hirBlockIdx];
    setInsertionBlock(mirBlockIdx);

    for (uint32_t instIdx : hblock.instIndices)
    {
        if (instIdx < hirFunction.instructions.size())
        {
            lowerHirInstruction(hirFunction, hirFunction.instructions[instIdx], instIdx);
        }
    }
}

Function MirBuilder::lowerFromHir(const Hir::Function& hirFunction)
{
    function.blocks.clear();
    function.instructions.clear();
    function.constants.clear();
    function.snapshots.clear();

    hirToMirBlockMap.clear();
    hirInstToMirValueMap.clear();
    hirConstantToMirValueMap.clear();

    for (size_t i = 0; i < hirFunction.blocks.size(); ++i)
    {
        const Hir::Block& hb = hirFunction.blocks[i];
        if (!hb.isDead)
        {
            uint32_t mirIdx = createBlock(hb.name.empty() ? ("mir_bb" + std::to_string(i)) : ("mir_" + hb.name));
            hirToMirBlockMap[uint32_t(i)] = mirIdx;
        }
    }

    for (size_t i = 0; i < hirFunction.blocks.size(); ++i)
    {
        const Hir::Block& hb = hirFunction.blocks[i];
        if (!hb.isDead)
        {
            lowerHirBlock(hirFunction, uint32_t(i));
        }
    }

    return std::move(function);
}

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
