// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/HirOpt.h"
#include "Luau/HirAnalysis.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

static const ConstantValue* tryGetConstant(const Function& function, const Value& v)
{
    if (v.isConstant() && v.index < function.constants.size())
        return &function.constants[v.index];

    if (v.isInst() && v.index < function.instructions.size())
    {
        const Inst& inst = function.instructions[v.index];
        if (inst.cmd == Cmd::ConstInt || inst.cmd == Cmd::ConstInt64 || inst.cmd == Cmd::ConstDouble || inst.cmd == Cmd::ConstBool ||
            inst.cmd == Cmd::ConstString || inst.cmd == Cmd::ConstNil)
        {
            if (!inst.args.empty() && inst.args[0].isConstant() && inst.args[0].index < function.constants.size())
                return &function.constants[inst.args[0].index];
        }
    }

    return nullptr;
}

HirOptimizer::HirOptimizer(Function& function)
    : function(function)
{
}

bool HirOptimizer::foldInstruction(uint32_t instIdx)
{
    Inst& inst = function.instructions[instIdx];

    if (inst.cmd == Cmd::Add || inst.cmd == Cmd::Sub || inst.cmd == Cmd::Mul || inst.cmd == Cmd::Div)
    {
        if (inst.args.size() >= 2)
        {
            const ConstantValue* ca = tryGetConstant(function, inst.args[0]);
            const ConstantValue* cb = tryGetConstant(function, inst.args[1]);

            if (ca && cb)
            {
                if (ca->kind == TypeKind::Integer && cb->kind == TypeKind::Integer)
                {
                    int64_t va = ca->val.i64 != 0 ? ca->val.i64 : ca->val.i;
                    int64_t vb = cb->val.i64 != 0 ? cb->val.i64 : cb->val.i;
                    int64_t res = 0;

                    if (inst.cmd == Cmd::Add)
                        res = va + vb;
                    else if (inst.cmd == Cmd::Sub)
                        res = va - vb;
                    else if (inst.cmd == Cmd::Mul)
                        res = va * vb;
                    else if (inst.cmd == Cmd::Div && vb != 0)
                        res = va / vb;
                    else
                        return false;

                    ConstantValue cv(res);
                    Value nc = function.addConstant(cv);
                    inst.cmd = Cmd::ConstInt;
                    inst.type = Type(TypeKind::Integer);
                    inst.args = {nc};
                    inst.range = Range(res);
                    return true;
                }
                else if (ca->kind == TypeKind::Number && cb->kind == TypeKind::Number)
                {
                    double va = ca->val.d;
                    double vb = cb->val.d;
                    double res = 0.0;

                    if (inst.cmd == Cmd::Add)
                        res = va + vb;
                    else if (inst.cmd == Cmd::Sub)
                        res = va - vb;
                    else if (inst.cmd == Cmd::Mul)
                        res = va * vb;
                    else if (inst.cmd == Cmd::Div && vb != 0.0)
                        res = va / vb;
                    else
                        return false;

                    ConstantValue cv(res);
                    Value nc = function.addConstant(cv);
                    inst.cmd = Cmd::ConstDouble;
                    inst.type = Type(TypeKind::Number);
                    inst.args = {nc};
                    return true;
                }
            }
        }
    }

    return false;
}

bool HirOptimizer::specializeTableAccess(uint32_t instIdx)
{
    Inst& inst = function.instructions[instIdx];

    if (inst.cmd == Cmd::GetTable && inst.args.size() >= 2)
    {
        Value tblVal = inst.args[0];
        const ConstantValue* ck = tryGetConstant(function, inst.args[1]);

        if (ck)
        {
            if (ck->kind == TypeKind::String && !ck->str.empty())
            {
                uint32_t shapeId = 0;
                if (function.tableShapes.empty())
                {
                    shapeId = function.createTableShape();
                }
                TableShape& shape = function.tableShapes[shapeId];
                int slot = shape.findSlot(ck->str);
                if (slot < 0)
                {
                    shape.addProperty(ck->str);
                    slot = shape.findSlot(ck->str);
                }

                if (slot >= 0)
                {
                    inst.cmd = Cmd::GetShapeSlot;
                    inst.extra = uint32_t(slot);
                    inst.args = {tblVal};
                    return true;
                }
            }
            else if (ck->kind == TypeKind::Integer)
            {
                inst.cmd = Cmd::GetArrayElement;
                return true;
            }
        }
    }
    else if (inst.cmd == Cmd::SetTable && inst.args.size() >= 3)
    {
        Value tblVal = inst.args[0];
        const ConstantValue* ck = tryGetConstant(function, inst.args[1]);
        Value valVal = inst.args[2];

        if (ck)
        {
            if (ck->kind == TypeKind::String && !ck->str.empty())
            {
                uint32_t shapeId = 0;
                if (function.tableShapes.empty())
                {
                    shapeId = function.createTableShape();
                }
                TableShape& shape = function.tableShapes[shapeId];
                int slot = shape.findSlot(ck->str);
                if (slot < 0)
                {
                    shape.addProperty(ck->str);
                    slot = shape.findSlot(ck->str);
                }

                if (slot >= 0)
                {
                    inst.cmd = Cmd::SetShapeSlot;
                    inst.extra = uint32_t(slot);
                    inst.args = {tblVal, valVal};
                    return true;
                }
            }
            else if (ck->kind == TypeKind::Integer)
            {
                inst.cmd = Cmd::SetArrayElement;
                return true;
            }
        }
    }

    return false;
}

bool HirOptimizer::replaceVirtualTableAccess(uint32_t instIdx)
{
    Inst& inst = function.instructions[instIdx];

    if (inst.cmd == Cmd::GetShapeSlot && !inst.args.empty())
    {
        Value tbl = inst.args[0];
        if (tbl.isInst() && tbl.index < function.instructions.size())
        {
            const Inst& tblInst = function.instructions[tbl.index];
            if (tblInst.cmd == Cmd::AllocVirtualTable && tblInst.extra < function.virtualTables.size())
            {
                uint32_t vtId = tblInst.extra;
                VirtualTable& vt = function.virtualTables[vtId];
                if (vt.escape == EscapeState::NoEscape)
                {
                    inst.cmd = Cmd::ReadVirtualField;
                    inst.args = {Value(ValueKind::VirtualObject, vtId)};
                    return true;
                }
            }
        }
    }
    else if (inst.cmd == Cmd::SetShapeSlot && inst.args.size() >= 2)
    {
        Value tbl = inst.args[0];
        Value val = inst.args[1];
        if (tbl.isInst() && tbl.index < function.instructions.size())
        {
            const Inst& tblInst = function.instructions[tbl.index];
            if (tblInst.cmd == Cmd::AllocVirtualTable && tblInst.extra < function.virtualTables.size())
            {
                uint32_t vtId = tblInst.extra;
                VirtualTable& vt = function.virtualTables[vtId];
                if (vt.escape == EscapeState::NoEscape)
                {
                    inst.cmd = Cmd::WriteVirtualField;
                    inst.args = {Value(ValueKind::VirtualObject, vtId), val};
                    return true;
                }
            }
        }
    }

    return false;
}

void HirOptimizer::runConstantAndRangePropagation()
{
    for (uint32_t i = 0; i < function.instructions.size(); ++i)
    {
        foldInstruction(i);
    }
}

void HirOptimizer::runTableSpecialization()
{
    for (uint32_t i = 0; i < function.instructions.size(); ++i)
    {
        specializeTableAccess(i);
    }
}

void HirOptimizer::runVirtualTableScalarReplacement()
{
    std::unordered_set<uint32_t> escapingAllocIndices;
    for (const Inst& inst : function.instructions)
    {
        if (inst.cmd == Cmd::Call || inst.cmd == Cmd::ReturnValues || inst.cmd == Cmd::SetUpvalue || inst.cmd == Cmd::Return)
        {
            for (const Value& arg : inst.args)
            {
                if (arg.isInst())
                {
                    escapingAllocIndices.insert(arg.index);
                }
            }
        }
    }

    for (uint32_t i = 0; i < function.instructions.size(); ++i)
    {
        Inst& inst = function.instructions[i];
        if (inst.cmd == Cmd::AllocTable && escapingAllocIndices.find(i) == escapingAllocIndices.end())
        {
            uint32_t vtId = function.createVirtualTable(inst.extra);
            function.virtualTables[vtId].escape = EscapeState::NoEscape;
            inst.cmd = Cmd::AllocVirtualTable;
            inst.extra = vtId;
        }
    }

    for (uint32_t i = 0; i < function.instructions.size(); ++i)
    {
        replaceVirtualTableAccess(i);
    }
}

void HirOptimizer::runClosureAndCallOptimization()
{
    for (Inst& inst : function.instructions)
    {
        if (inst.cmd == Cmd::CallBuiltin)
        {
            inst.callEffect = CallEffect::Pure;
        }
        else if (inst.cmd == Cmd::Call)
        {
            inst.callEffect = CallEffect::Mutating;
        }
    }
}

void HirOptimizer::runMultivalueSimplification()
{
    for (Inst& inst : function.instructions)
    {
        if (inst.cmd == Cmd::PackMultivalue && !inst.args.empty())
        {
            inst.extra = uint32_t(inst.args.size());
        }
    }
}

void HirOptimizer::runDeadCodeElimination()
{
    std::vector<uint32_t> useCounts(function.instructions.size(), 0);

    for (const Inst& inst : function.instructions)
    {
        for (const Value& arg : inst.args)
        {
            if (arg.isInst() && arg.index < useCounts.size())
            {
                useCounts[arg.index]++;
            }
        }
    }

    for (uint32_t i = 0; i < function.instructions.size(); ++i)
    {
        Inst& inst = function.instructions[i];
        inst.useCount = useCounts[i];
    }
}

void HirOptimizer::buildSnapshots()
{
    for (uint32_t bidx = 0; bidx < function.blocks.size(); ++bidx)
    {
        Block& block = function.blocks[bidx];
        for (uint32_t instIdx : block.instIndices)
        {
            Inst& inst = function.instructions[instIdx];
            if (inst.cmd == Cmd::CheckTag || inst.cmd == Cmd::CheckShape || inst.cmd == Cmd::CheckBounds || inst.cmd == Cmd::VmExit)
            {
                uint32_t snapId = function.createSnapshot(inst.pcpos);
                Snapshot& snap = function.snapshots[snapId];
                for (size_t vtId = 0; vtId < function.virtualTables.size(); ++vtId)
                {
                    if (function.virtualTables[vtId].escape == EscapeState::NoEscape)
                    {
                        snap.liveVirtualObjects.push_back(uint32_t(vtId));
                    }
                }
                inst.extra = snapId;
            }
        }
    }
}

void HirOptimizer::runAllPasses()
{
    runConstantAndRangePropagation();
    runTableSpecialization();
    runVirtualTableScalarReplacement();
    runClosureAndCallOptimization();
    runMultivalueSimplification();
    runDeadCodeElimination();
    buildSnapshots();
}

void optimizeHir(Function& function)
{
    HirOptimizer opt(function);
    opt.runAllPasses();
}

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
