// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/MirOpt.h"
#include "Luau/MirAnalysis.h"

#include <unordered_map>
#include <unordered_set>

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

MirOptimizer::MirOptimizer(Function& function)
    : function(function)
{
}

void MirOptimizer::runRedundantGuardElimination()
{
    MirAnalysis analysis(function);
    analysis.computeControlFlow();
    analysis.computeDominance();

    // Map: (guardCmd, targetVal.index, extra) -> dominating guard instIdx
    std::unordered_map<uint64_t, uint32_t> activeGuards;

    for (uint32_t bidx = 0; bidx < function.blocks.size(); ++bidx)
    {
        Block& block = function.blocks[bidx];
        for (uint32_t instIdx : block.instIndices)
        {
            Inst& inst = function.instructions[instIdx];
            if (inst.cmd == Cmd::GuardTag || inst.cmd == Cmd::GuardShape || inst.cmd == Cmd::GuardNotNil)
            {
                if (!inst.args.empty())
                {
                    uint32_t valIdx = inst.args[0].index;
                    uint64_t key = (uint64_t(inst.cmd) << 48) | (uint64_t(valIdx) << 32) | uint64_t(inst.extra);

                    auto it = activeGuards.find(key);
                    if (it != activeGuards.end())
                    {
                        uint32_t domGuardIdx = it->second;
                        // Check dominance
                        if (analysis.isDominating(function.instructions[domGuardIdx].offset, bidx) || domGuardIdx < instIdx)
                        {
                            // Guard is redundant!
                            inst.cmd = Cmd::Nop;
                            inst.args.clear();
                        }
                    }
                    else
                    {
                        inst.offset = int32_t(bidx); // record block index for dominance check
                        activeGuards[key] = instIdx;
                    }
                }
            }
        }
    }
}

void MirOptimizer::runBoundsCheckElimination()
{
    // Eliminate redundant bounds checks when index is checked against known limits
    std::unordered_set<uint64_t> activeBounds;

    for (Block& block : function.blocks)
    {
        for (uint32_t instIdx : block.instIndices)
        {
            Inst& inst = function.instructions[instIdx];
            if (inst.cmd == Cmd::GuardBounds && inst.args.size() >= 2)
            {
                uint64_t key = (uint64_t(inst.args[0].index) << 32) | uint64_t(inst.args[1].index);
                if (activeBounds.find(key) != activeBounds.end())
                {
                    inst.cmd = Cmd::Nop;
                    inst.args.clear();
                }
                else
                {
                    activeBounds.insert(key);
                }
            }
        }
    }
}

void MirOptimizer::runGvnCse()
{
    struct InstKey
    {
        uint16_t cmd;
        uint8_t type;
        int32_t offset;
        uint32_t arg0;
        uint32_t arg1;

        bool operator==(const InstKey& o) const
        {
            return cmd == o.cmd && type == o.type && offset == o.offset && arg0 == o.arg0 && arg1 == o.arg1;
        }
    };

    struct InstKeyHash
    {
        size_t operator()(const InstKey& k) const
        {
            size_t h = size_t(k.cmd) * 31 + size_t(k.type);
            h = h * 31 + size_t(k.offset);
            h = h * 31 + size_t(k.arg0);
            h = h * 31 + size_t(k.arg1);
            return h;
        }
    };

    std::unordered_map<InstKey, Value, InstKeyHash> exprMap;
    std::unordered_map<uint32_t, Value> replacements;

    for (Block& block : function.blocks)
    {
        for (uint32_t instIdx : block.instIndices)
        {
            Inst& inst = function.instructions[instIdx];

            // Replace args if already replaced
            for (Value& arg : inst.args)
            {
                if (arg.isInst())
                {
                    auto rit = replacements.find(arg.index);
                    if (rit != replacements.end())
                        arg = rit->second;
                }
            }

            // Pure operations suitable for GVN
            if (inst.cmd == Cmd::AddInt || inst.cmd == Cmd::SubInt || inst.cmd == Cmd::MulInt || inst.cmd == Cmd::DivInt ||
                inst.cmd == Cmd::AddFloat || inst.cmd == Cmd::SubFloat || inst.cmd == Cmd::MulFloat || inst.cmd == Cmd::DivFloat ||
                inst.cmd == Cmd::LoadField)
            {
                InstKey key;
                key.cmd = uint16_t(inst.cmd);
                key.type = uint8_t(inst.type.kind);
                key.offset = inst.offset;
                key.arg0 = inst.args.size() > 0 ? inst.args[0].index : ~0u;
                key.arg1 = inst.args.size() > 1 ? inst.args[1].index : ~0u;

                auto it = exprMap.find(key);
                if (it != exprMap.end())
                {
                    replacements[instIdx] = it->second;
                    inst.cmd = Cmd::Nop;
                    inst.args.clear();
                }
                else
                {
                    exprMap[key] = Value(ValueKind::Inst, instIdx);
                }
            }
        }
    }
}

void MirOptimizer::runLicm()
{
    MirAnalysis analysis(function);
    analysis.computeControlFlow();
    analysis.computeDominance();

    for (uint32_t bidx = 0; bidx < function.blocks.size(); ++bidx)
    {
        Block& header = function.blocks[bidx];
        if (header.isLoopHeader && !header.predecessors.empty())
        {
            uint32_t preheader = header.predecessors[0];
            if (preheader < function.blocks.size() && preheader != bidx)
            {
                // Inspect instructions in loop header
                std::vector<uint32_t> toHoist;
                for (uint32_t instIdx : header.instIndices)
                {
                    const Inst& inst = function.instructions[instIdx];
                    if (inst.cmd == Cmd::LoadField || inst.cmd == Cmd::AddInt || inst.cmd == Cmd::AddFloat)
                    {
                        bool loopInvariant = true;
                        for (const Value& arg : inst.args)
                        {
                            if (arg.isInst() && arg.index >= header.instIndices.front())
                            {
                                loopInvariant = false;
                                break;
                            }
                        }
                        if (loopInvariant)
                        {
                            toHoist.push_back(instIdx);
                        }
                    }
                }

                // Hoist loop invariants to preheader
                for (uint32_t instIdx : toHoist)
                {
                    function.blocks[preheader].instIndices.push_back(instIdx);
                    auto& hindices = header.instIndices;
                    hindices.erase(std::remove(hindices.begin(), hindices.end(), instIdx), hindices.end());
                }
            }
        }
    }
}

void MirOptimizer::runLoadStoreElimination()
{
    for (Block& block : function.blocks)
    {
        std::unordered_map<int32_t, Value> fieldValues;

        for (uint32_t instIdx : block.instIndices)
        {
            Inst& inst = function.instructions[instIdx];

            if (inst.cmd == Cmd::StoreField && inst.args.size() >= 2)
            {
                fieldValues[inst.offset] = inst.args[1];
            }
            else if (inst.cmd == Cmd::LoadField)
            {
                auto it = fieldValues.find(inst.offset);
                if (it != fieldValues.end())
                {
                    // Forward stored value
                    inst.cmd = Cmd::Nop;
                    inst.args = {it->second};
                }
            }
            else if (inst.memoryEffect == MemoryEffect::Write && inst.locationClass == LocationClass::GenericHeap)
            {
                fieldValues.clear();
            }
        }
    }
}

void MirOptimizer::runGcBarrierElimination()
{
    for (Block& block : function.blocks)
    {
        std::unordered_set<uint32_t> freshlyAllocated;

        for (uint32_t instIdx : block.instIndices)
        {
            Inst& inst = function.instructions[instIdx];

            if (inst.cmd == Cmd::AllocTable || inst.cmd == Cmd::AllocClosure || inst.cmd == Cmd::AllocBuffer)
            {
                freshlyAllocated.insert(instIdx);
            }
            else if (inst.cmd == Cmd::GcWriteBarrier && !inst.args.empty())
            {
                Value parent = inst.args[0];
                if (parent.isInst() && freshlyAllocated.find(parent.index) != freshlyAllocated.end())
                {
                    // Redundant GC barrier on freshly allocated object
                    inst.cmd = Cmd::Nop;
                    inst.args.clear();
                }
            }
        }
    }
}

void MirOptimizer::runDeadCodeElimination()
{
    MirAnalysis analysis(function);
    analysis.computeUseCounts();

    for (uint32_t i = 0; i < function.instructions.size(); ++i)
    {
        Inst& inst = function.instructions[i];
        if (inst.useCount == 0 && (inst.cmd == Cmd::AddInt || inst.cmd == Cmd::SubInt || inst.cmd == Cmd::MulInt ||
                                   inst.cmd == Cmd::AddFloat || inst.cmd == Cmd::SubFloat || inst.cmd == Cmd::MulFloat ||
                                   inst.cmd == Cmd::ConstInt32 || inst.cmd == Cmd::ConstFloat64 || inst.cmd == Cmd::ConstBool))
        {
            inst.cmd = Cmd::Nop;
            inst.args.clear();
        }
    }
}

void MirOptimizer::runAllPasses()
{
    runRedundantGuardElimination();
    runBoundsCheckElimination();
    runLoadStoreElimination();
    runGvnCse();
    runLicm();
    runGcBarrierElimination();
    runDeadCodeElimination();
}

void optimizeMir(Function& function)
{
    MirOptimizer opt(function);
    opt.runAllPasses();
}

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
