// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/MirLowering.h"
#include "Luau/IrUtils.h"

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

MirLowering::MirLowering(IrBuilder& irBuilder)
    : ir(irBuilder)
{
}

IrOp MirLowering::resolveMirValue(const Function& mirFunction, const Value& val)
{
    switch (val.kind)
    {
    case ValueKind::None:
        return ir.undef();
    case ValueKind::Inst:
    {
        auto it = mirValueToIrOpMap.find(val.index);
        if (it != mirValueToIrOpMap.end())
            return it->second;
        return ir.undef();
    }
    case ValueKind::Constant:
    {
        if (val.index < mirFunction.constants.size())
        {
            const ConstantValue& cv = mirFunction.constants[val.index];
            switch (cv.kind)
            {
            case TypeKind::Bool:
                return ir.constInt(cv.val.b ? 1 : 0);
            case TypeKind::Int32:
                return ir.constInt(cv.val.i32);
            case TypeKind::Int64:
                return ir.constInt64(cv.val.i64);
            case TypeKind::Float64:
                return ir.constDouble(cv.val.f64);
            case TypeKind::Tag:
                return ir.constTag(cv.val.tag);
            default:
                return ir.undef();
            }
        }
        return ir.undef();
    }
    default:
        return ir.undef();
    }
}

bool MirLowering::lowerFunction(const Function& mirFunction, Proto* proto)
{
    mirValueToIrOpMap.clear();
    mirBlockToIrBlockMap.clear();

    // Map each MIR block to an IR block
    for (size_t i = 0; i < mirFunction.blocks.size(); ++i)
    {
        const Block& mb = mirFunction.blocks[i];
        if (!mb.isDead)
        {
            IrOp irBlock = ir.block(IrBlockKind::Internal);
            mirBlockToIrBlockMap[uint32_t(i)] = irBlock;
        }
    }

    // Lower block by block
    for (size_t i = 0; i < mirFunction.blocks.size(); ++i)
    {
        const Block& mb = mirFunction.blocks[i];
        if (mb.isDead)
            continue;

        IrOp irBlock = mirBlockToIrBlockMap[uint32_t(i)];
        ir.beginBlock(irBlock);

        for (uint32_t instIdx : mb.instIndices)
        {
            if (instIdx >= mirFunction.instructions.size())
                continue;

            const Inst& minst = mirFunction.instructions[instIdx];

            switch (minst.cmd)
            {
            case Cmd::Nop:
                break;

            case Cmd::ConstBool:
            case Cmd::ConstInt32:
            case Cmd::ConstInt64:
            case Cmd::ConstFloat64:
            case Cmd::ConstTag:
                if (!minst.args.empty())
                {
                    mirValueToIrOpMap[instIdx] = resolveMirValue(mirFunction, minst.args[0]);
                }
                break;

            case Cmd::AddInt:
            {
                IrOp a = resolveMirValue(mirFunction, minst.args[0]);
                IrOp b = resolveMirValue(mirFunction, minst.args[1]);
                mirValueToIrOpMap[instIdx] = ir.inst(IrCmd::ADD_INT, a, b);
                break;
            }

            case Cmd::SubInt:
            {
                IrOp a = resolveMirValue(mirFunction, minst.args[0]);
                IrOp b = resolveMirValue(mirFunction, minst.args[1]);
                mirValueToIrOpMap[instIdx] = ir.inst(IrCmd::SUB_INT, a, b);
                break;
            }

            case Cmd::AddFloat:
            {
                IrOp a = resolveMirValue(mirFunction, minst.args[0]);
                IrOp b = resolveMirValue(mirFunction, minst.args[1]);
                mirValueToIrOpMap[instIdx] = ir.inst(IrCmd::ADD_NUM, a, b);
                break;
            }

            case Cmd::SubFloat:
            {
                IrOp a = resolveMirValue(mirFunction, minst.args[0]);
                IrOp b = resolveMirValue(mirFunction, minst.args[1]);
                mirValueToIrOpMap[instIdx] = ir.inst(IrCmd::SUB_NUM, a, b);
                break;
            }

            case Cmd::MulFloat:
            {
                IrOp a = resolveMirValue(mirFunction, minst.args[0]);
                IrOp b = resolveMirValue(mirFunction, minst.args[1]);
                mirValueToIrOpMap[instIdx] = ir.inst(IrCmd::MUL_NUM, a, b);
                break;
            }

            case Cmd::DivFloat:
            {
                IrOp a = resolveMirValue(mirFunction, minst.args[0]);
                IrOp b = resolveMirValue(mirFunction, minst.args[1]);
                mirValueToIrOpMap[instIdx] = ir.inst(IrCmd::DIV_NUM, a, b);
                break;
            }

            case Cmd::GuardTag:
            {
                IrOp val = resolveMirValue(mirFunction, minst.args[0]);
                IrOp tag = resolveMirValue(mirFunction, minst.args[1]);
                IrOp fallback = ir.fallbackBlock(minst.pcpos);
                ir.inst(IrCmd::CHECK_TAG, val, tag, fallback);
                break;
            }

            case Cmd::Jump:
            {
                if (!minst.args.empty())
                {
                    uint32_t targetMirBlock = minst.args[0].index;
                    auto it = mirBlockToIrBlockMap.find(targetMirBlock);
                    if (it != mirBlockToIrBlockMap.end())
                    {
                        ir.inst(IrCmd::JUMP, it->second);
                    }
                }
                break;
            }

            case Cmd::BranchCond:
            {
                if (minst.args.size() >= 3)
                {
                    IrOp cond = resolveMirValue(mirFunction, minst.args[0]);
                    uint32_t targetThen = minst.args[1].index;
                    uint32_t targetElse = minst.args[2].index;

                    IrOp bThen = mirBlockToIrBlockMap[targetThen];
                    IrOp bElse = mirBlockToIrBlockMap[targetElse];

                    ir.inst(IrCmd::JUMP_IF_TRUTHY, cond, bThen, bElse);
                }
                break;
            }

            case Cmd::Return:
            {
                IrOp retVal = ir.constUint(0);
                if (!minst.args.empty())
                    retVal = resolveMirValue(mirFunction, minst.args[0]);
                ir.inst(IrCmd::RETURN, retVal);
                break;
            }

            default:
                break;
            }
        }
    }

    return true;
}

bool lowerMirToIr(IrBuilder& irBuilder, const Function& mirFunction, Proto* proto)
{
    MirLowering lowering(irBuilder);
    return lowering.lowerFunction(mirFunction, proto);
}

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
