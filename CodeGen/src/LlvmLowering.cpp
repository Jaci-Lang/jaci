// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/LlvmLowering.h"

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

LlvmLowering::LlvmLowering(LlvmBuilder& builder, TableSpecializer& tableSpecializer)
    : builder(builder)
    , tableSpecializer(tableSpecializer)
{
}

Type LlvmLowering::mapMirTypeToLlvmType(const Mir::Type& mtype)
{
    switch (mtype.kind)
    {
    case Mir::TypeKind::Void:
        return Type(TypeKind::Void);
    case Mir::TypeKind::Bool:
        return Type(TypeKind::Int1);
    case Mir::TypeKind::Int32:
        return Type(TypeKind::Int32);
    case Mir::TypeKind::Int64:
        return Type(TypeKind::Int64);
    case Mir::TypeKind::Float64:
        return Type(TypeKind::Double);
    case Mir::TypeKind::Tag:
        return Type(TypeKind::Int8);
    case Mir::TypeKind::RawPtr:
    case Mir::TypeKind::GcPtr:
    case Mir::TypeKind::ManagedRef:
        return Type(TypeKind::Pointer);
    case Mir::TypeKind::TValueBoxed:
    default:
        return Type(TypeKind::TValue);
    }
}

LlvmValue LlvmLowering::resolveMirValue(const Mir::Function& mirFunction, const Mir::Value& val)
{
    switch (val.kind)
    {
    case Mir::ValueKind::None:
        return LlvmValue();
    case Mir::ValueKind::Inst:
    {
        auto it = mirValueToLlvmMap.find(val.index);
        if (it != mirValueToLlvmMap.end())
            return it->second;
        return LlvmValue();
    }
    case Mir::ValueKind::Constant:
    {
        if (val.index < mirFunction.constants.size())
        {
            const Mir::ConstantValue& cv = mirFunction.constants[val.index];
            switch (cv.kind)
            {
            case Mir::TypeKind::Bool:
                return builder.constInt1(cv.val.b);
            case Mir::TypeKind::Int32:
                return builder.constInt32(cv.val.i32);
            case Mir::TypeKind::Int64:
                return builder.constInt64(cv.val.i64);
            case Mir::TypeKind::Float64:
                return builder.constDouble(cv.val.f64);
            case Mir::TypeKind::Tag:
                return builder.constInt8(cv.val.tag);
            default:
                return builder.constNullPointer();
            }
        }
        return LlvmValue();
    }
    default:
        return LlvmValue();
    }
}

bool LlvmLowering::lowerFunction(const Mir::Function& mirFunction, Proto* proto, const std::string& functionName)
{
    mirValueToLlvmMap.clear();
    mirBlockToBlockNameMap.clear();

    for (size_t i = 0; i < mirFunction.blocks.size(); ++i)
    {
        const Mir::Block& mb = mirFunction.blocks[i];
        if (!mb.isDead)
        {
            mirBlockToBlockNameMap[uint32_t(i)] = mb.name.empty() ? ("block_" + std::to_string(i)) : mb.name;
        }
    }

    // Function Signature: ptr @jaci_entry(ptr %L, ptr %closure, ptr %base)
    std::vector<Type> paramTypes = {Type(TypeKind::Pointer), Type(TypeKind::Pointer), Type(TypeKind::Pointer)};
    std::vector<std::string> paramNames = {"L", "closure", "base"};
    builder.beginFunction(functionName, Type(TypeKind::Int32), paramTypes, paramNames);

    for (size_t i = 0; i < mirFunction.blocks.size(); ++i)
    {
        const Mir::Block& mb = mirFunction.blocks[i];
        if (mb.isDead)
            continue;

        std::string blockName = mirBlockToBlockNameMap[uint32_t(i)];
        builder.createBlock(blockName);

        for (uint32_t instIdx : mb.instIndices)
        {
            if (instIdx >= mirFunction.instructions.size())
                continue;

            const Mir::Inst& inst = mirFunction.instructions[instIdx];

            switch (inst.cmd)
            {
            case Mir::Cmd::Nop:
                break;

            case Mir::Cmd::ConstBool:
            case Mir::Cmd::ConstInt32:
            case Mir::Cmd::ConstInt64:
            case Mir::Cmd::ConstFloat64:
            case Mir::Cmd::ConstTag:
                if (!inst.args.empty())
                {
                    mirValueToLlvmMap[instIdx] = resolveMirValue(mirFunction, inst.args[0]);
                }
                break;

            case Mir::Cmd::AddInt:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitAdd(a, b);
                break;
            }

            case Mir::Cmd::SubInt:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitSub(a, b);
                break;
            }

            case Mir::Cmd::MulInt:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitMul(a, b);
                break;
            }

            case Mir::Cmd::DivInt:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitDiv(a, b);
                break;
            }

            case Mir::Cmd::AddFloat:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitAdd(a, b);
                break;
            }

            case Mir::Cmd::SubFloat:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitSub(a, b);
                break;
            }

            case Mir::Cmd::MulFloat:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitMul(a, b);
                break;
            }

            case Mir::Cmd::DivFloat:
            {
                LlvmValue a = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue b = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitDiv(a, b);
                break;
            }

            case Mir::Cmd::GuardShape:
            {
                LlvmValue tbl = resolveMirValue(mirFunction, inst.args[0]);
                uint32_t shapeId = inst.extra;
                std::string fallbackBlock = "deopt_exit_" + std::to_string(inst.pcpos);
                builder.emitShapeGuard(tbl, shapeId, fallbackBlock);
                break;
            }

            case Mir::Cmd::LoadField:
            {
                LlvmValue tbl = resolveMirValue(mirFunction, inst.args[0]);
                uint32_t slotOffset = uint32_t(inst.offset);
                Type fieldType = mapMirTypeToLlvmType(inst.type);
                mirValueToLlvmMap[instIdx] = builder.emitLoadFieldSlot(tbl, slotOffset, fieldType);
                break;
            }

            case Mir::Cmd::StoreField:
            {
                LlvmValue tbl = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue val = resolveMirValue(mirFunction, inst.args[1]);
                uint32_t slotOffset = uint32_t(inst.offset);
                builder.emitStoreFieldSlot(tbl, slotOffset, val);
                break;
            }

            case Mir::Cmd::LoadArrayElement:
            {
                LlvmValue arr = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue idx = resolveMirValue(mirFunction, inst.args[1]);
                mirValueToLlvmMap[instIdx] = builder.emitLoadPackedArray(arr, idx, ArraySpecialization::PackedDouble);
                break;
            }

            case Mir::Cmd::StoreArrayElement:
            {
                LlvmValue arr = resolveMirValue(mirFunction, inst.args[0]);
                LlvmValue idx = resolveMirValue(mirFunction, inst.args[1]);
                LlvmValue val = resolveMirValue(mirFunction, inst.args[2]);
                builder.emitStorePackedArray(arr, idx, val, ArraySpecialization::PackedDouble);
                break;
            }

            case Mir::Cmd::AllocTable:
            {
                // luaH_new(L, narray, nhash)
                LlvmValue L = LlvmValue("%L", Type(TypeKind::Pointer));
                LlvmValue narr = builder.constInt32(16);
                LlvmValue nhash = builder.constInt32(0);
                mirValueToLlvmMap[instIdx] = builder.emitCall("luaH_new", Type(TypeKind::Pointer), {L, narr, nhash});
                break;
            }

            case Mir::Cmd::Jump:
            {
                if (!inst.args.empty())
                {
                    uint32_t targetMirBlock = inst.args[0].index;
                    std::string targetName = mirBlockToBlockNameMap[targetMirBlock];
                    builder.emitBranch(targetName);
                }
                break;
            }

            case Mir::Cmd::BranchCond:
            {
                if (inst.args.size() >= 3)
                {
                    LlvmValue cond = resolveMirValue(mirFunction, inst.args[0]);
                    uint32_t targetThen = inst.args[1].index;
                    uint32_t targetElse = inst.args[2].index;

                    std::string tName = mirBlockToBlockNameMap[targetThen];
                    std::string eName = mirBlockToBlockNameMap[targetElse];
                    builder.emitCondBranch(cond, tName, eName);
                }
                break;
            }

            case Mir::Cmd::Return:
            {
                LlvmValue retVal = builder.constInt32(0);
                if (!inst.args.empty())
                {
                    LlvmValue val = resolveMirValue(mirFunction, inst.args[0]);
                    if (val.type.kind == TypeKind::Int32)
                        retVal = val;
                }
                builder.emitReturn(retVal);
                break;
            }

            case Mir::Cmd::GcWriteBarrier:
                // Write barrier for reference types
                if (inst.args.size() >= 2)
                {
                    LlvmValue parent = resolveMirValue(mirFunction, inst.args[0]);
                    LlvmValue child = resolveMirValue(mirFunction, inst.args[1]);
                    if (child.type.isPointer())
                    {
                        LlvmValue L = LlvmValue("%L", Type(TypeKind::Pointer));
                        builder.emitCall("luaC_barriertable", Type(TypeKind::Void), {L, parent, child});
                    }
                }
                break;

            default:
                break;
            }
        }
    }

    builder.endFunction();
    return true;
}

bool lowerMirToLlvm(LlvmBuilder& builder, TableSpecializer& tableSpecializer, const Mir::Function& mirFunction, Proto* proto)
{
    LlvmLowering lowering(builder, tableSpecializer);
    return lowering.lowerFunction(mirFunction, proto);
}

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
