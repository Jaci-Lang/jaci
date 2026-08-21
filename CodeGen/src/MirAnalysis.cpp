// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/MirAnalysis.h"

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

MirAnalysis::MirAnalysis(Function& function)
    : function(function)
{
}

void MirAnalysis::computeControlFlow()
{
    for (Block& b : function.blocks)
    {
        b.successors.clear();
        b.predecessors.clear();
    }

    for (uint32_t bidx = 0; bidx < function.blocks.size(); ++bidx)
    {
        Block& b = function.blocks[bidx];
        if (b.isDead || b.instIndices.empty())
            continue;

        uint32_t lastInstIdx = b.instIndices.back();
        const Inst& inst = function.instructions[lastInstIdx];

        if (inst.cmd == Cmd::Jump)
        {
            if (!inst.args.empty() && inst.args[0].kind == ValueKind::Inst)
            {
                uint32_t target = inst.args[0].index;
                b.successors.push_back(target);
            }
        }
        else if (inst.cmd == Cmd::BranchCond)
        {
            if (inst.args.size() >= 3)
            {
                if (inst.args[1].kind == ValueKind::Inst)
                    b.successors.push_back(inst.args[1].index);
                if (inst.args[2].kind == ValueKind::Inst)
                    b.successors.push_back(inst.args[2].index);
            }
        }
    }

    for (uint32_t bidx = 0; bidx < function.blocks.size(); ++bidx)
    {
        for (uint32_t succ : function.blocks[bidx].successors)
        {
            if (succ < function.blocks.size())
            {
                function.blocks[succ].predecessors.push_back(bidx);
                if (succ <= bidx)
                {
                    function.blocks[succ].isLoopHeader = true;
                }
            }
        }
    }
}

void MirAnalysis::computeDominance()
{
    uint32_t numBlocks = uint32_t(function.blocks.size());
    idoms.assign(numBlocks, ~0u);
    domChildren.assign(numBlocks, {});

    if (numBlocks == 0)
        return;

    idoms[function.entryBlock] = function.entryBlock;

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (uint32_t i = 0; i < numBlocks; ++i)
        {
            if (i == function.entryBlock || function.blocks[i].isDead)
                continue;

            const Block& b = function.blocks[i];
            uint32_t newIdom = ~0u;

            for (uint32_t pred : b.predecessors)
            {
                if (idoms[pred] != ~0u)
                {
                    if (newIdom == ~0u)
                    {
                        newIdom = pred;
                    }
                    else
                    {
                        uint32_t f1 = pred;
                        uint32_t f2 = newIdom;
                        while (f1 != f2)
                        {
                            while (f1 > f2)
                                f1 = idoms[f1];
                            while (f2 > f1)
                                f2 = idoms[f2];
                        }
                        newIdom = f1;
                    }
                }
            }

            if (newIdom != ~0u && idoms[i] != newIdom)
            {
                idoms[i] = newIdom;
                changed = true;
            }
        }
    }

    for (uint32_t i = 0; i < numBlocks; ++i)
    {
        if (i != function.entryBlock && idoms[i] != ~0u && idoms[i] != i)
        {
            domChildren[idoms[i]].push_back(i);
        }
    }
}

bool MirAnalysis::isDominating(uint32_t a, uint32_t b) const
{
    if (a == b)
        return true;
    if (b >= idoms.size())
        return false;
    uint32_t curr = b;
    while (curr != ~0u && curr != function.entryBlock)
    {
        uint32_t parent = idoms[curr];
        if (parent == a)
            return true;
        if (parent == curr)
            break;
        curr = parent;
    }
    return false;
}

bool MirAnalysis::canAlias(const Inst& instA, const Inst& instB) const
{
    if (instA.locationClass != instB.locationClass)
        return false;

    if (instA.locationClass == LocationClass::TableProperties)
    {
        // If they access different slot offsets, they do not alias
        if (instA.offset != instB.offset)
            return false;
    }

    return true;
}

void MirAnalysis::computeUseCounts()
{
    for (Inst& inst : function.instructions)
        inst.useCount = 0;

    for (const Inst& inst : function.instructions)
    {
        for (const Value& arg : inst.args)
        {
            if (arg.isInst() && arg.index < function.instructions.size())
            {
                function.instructions[arg.index].useCount++;
            }
        }
    }
}

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
