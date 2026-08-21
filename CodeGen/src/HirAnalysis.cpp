// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/HirAnalysis.h"

#include <queue>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

bool AbstractValueFacts::meetWith(const AbstractValueFacts& other)
{
    bool changed = false;

    if (type.kind == TypeKind::Bottom)
    {
        if (other.type.kind != TypeKind::Bottom)
        {
            type = other.type;
            changed = true;
        }
    }
    else if (other.type.kind != TypeKind::Bottom && type != other.type)
    {
        if (type.kind != other.type.kind)
        {
            if (type.kind != TypeKind::Any)
            {
                type = Type(TypeKind::Any);
                changed = true;
            }
        }
        else
        {
            bool newOpt = type.optional || other.type.optional;
            if (type.optional != newOpt)
            {
                type.optional = newOpt;
                changed = true;
            }
        }
    }

    Range newRange = range.unionWith(other.range);
    if (newRange.min != range.min || newRange.max != range.max)
    {
        range = newRange;
        changed = true;
    }

    if (exactConstant && other.exactConstant)
    {
        if (exactConstant->kind != other.exactConstant->kind)
        {
            exactConstant.reset();
            changed = true;
        }
    }
    else if (exactConstant)
    {
        exactConstant.reset();
        changed = true;
    }

    if (truthiness && other.truthiness)
    {
        if (*truthiness != *other.truthiness)
        {
            truthiness.reset();
            changed = true;
        }
    }
    else if (truthiness)
    {
        truthiness.reset();
        changed = true;
    }

    if (other.escape > escape)
    {
        escape = other.escape;
        changed = true;
    }

    if (knownShapeId != other.knownShapeId)
    {
        if (knownShapeId != ~0u)
        {
            knownShapeId = ~0u;
            changed = true;
        }
    }

    bool newNotNil = isKnownNotNil && other.isKnownNotNil;
    if (isKnownNotNil != newNotNil)
    {
        isKnownNotNil = newNotNil;
        changed = true;
    }

    if (aliasClass != other.aliasClass)
    {
        if (aliasClass != AliasClass::Unknown)
        {
            aliasClass = AliasClass::Unknown;
            changed = true;
        }
    }

    return changed;
}

const AbstractValueFacts* FactSet::getInstFacts(uint32_t instIdx) const
{
    auto it = instFacts.find(instIdx);
    if (it != instFacts.end())
        return &it->second;
    return nullptr;
}

const AbstractValueFacts* FactSet::getBlockArgFacts(uint32_t argIdx) const
{
    auto it = blockArgFacts.find(argIdx);
    if (it != blockArgFacts.end())
        return &it->second;
    return nullptr;
}

const AbstractValueFacts* FactSet::getVmRegFacts(uint8_t reg) const
{
    auto it = vmRegFacts.find(reg);
    if (it != vmRegFacts.end())
        return &it->second;
    return nullptr;
}

void FactSet::setInstFacts(uint32_t instIdx, AbstractValueFacts facts)
{
    instFacts[instIdx] = std::move(facts);
}

void FactSet::setBlockArgFacts(uint32_t argIdx, AbstractValueFacts facts)
{
    blockArgFacts[argIdx] = std::move(facts);
}

void FactSet::setVmRegFacts(uint8_t reg, AbstractValueFacts facts)
{
    vmRegFacts[reg] = std::move(facts);
}

bool FactSet::meetWith(const FactSet& other)
{
    bool changed = false;

    for (const auto& [instIdx, facts] : other.instFacts)
    {
        auto it = instFacts.find(instIdx);
        if (it == instFacts.end())
        {
            instFacts[instIdx] = facts;
            changed = true;
        }
        else
        {
            if (it->second.meetWith(facts))
                changed = true;
        }
    }

    for (const auto& [argIdx, facts] : other.blockArgFacts)
    {
        auto it = blockArgFacts.find(argIdx);
        if (it == blockArgFacts.end())
        {
            blockArgFacts[argIdx] = facts;
            changed = true;
        }
        else
        {
            if (it->second.meetWith(facts))
                changed = true;
        }
    }

    for (const auto& [reg, facts] : other.vmRegFacts)
    {
        auto it = vmRegFacts.find(reg);
        if (it == vmRegFacts.end())
        {
            vmRegFacts[reg] = facts;
            changed = true;
        }
        else
        {
            if (it->second.meetWith(facts))
                changed = true;
        }
    }

    return changed;
}

HirAnalysis::HirAnalysis(Function& function)
    : function(function)
{
}

void HirAnalysis::computeDominance()
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
                        // intersect
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

bool HirAnalysis::isDominating(uint32_t a, uint32_t b) const
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

void HirAnalysis::computeControlFlow()
{
    for (Block& b : function.blocks)
    {
        b.successors.clear();
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
        else if (inst.cmd == Cmd::Branch)
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

    for (Block& b : function.blocks)
    {
        b.predecessors.clear();
    }

    for (uint32_t bidx = 0; bidx < function.blocks.size(); ++bidx)
    {
        for (uint32_t succ : function.blocks[bidx].successors)
        {
            if (succ < function.blocks.size())
                function.blocks[succ].predecessors.push_back(bidx);
        }
    }
}

void HirAnalysis::propagateBlock(uint32_t blockIdx, FactSet& currentFacts)
{
    Block& b = function.blocks[blockIdx];
    for (uint32_t instIdx : b.instIndices)
    {
        Inst& inst = function.instructions[instIdx];
        AbstractValueFacts facts;
        facts.type = inst.type;
        facts.range = inst.range;

        switch (inst.cmd)
        {
        case Cmd::ConstNil:
            facts.type = Type(TypeKind::Nil);
            facts.truthiness = false;
            facts.isKnownNotNil = false;
            break;
        case Cmd::ConstBool:
            facts.type = Type(TypeKind::Boolean);
            if (!inst.args.empty() && inst.args[0].isConstant())
            {
                const ConstantValue& cv = function.constants[inst.args[0].index];
                facts.truthiness = cv.val.b;
                facts.exactConstant = cv;
            }
            facts.isKnownNotNil = true;
            break;
        case Cmd::ConstInt:
        case Cmd::ConstInt64:
            facts.type = Type(TypeKind::Integer);
            if (!inst.args.empty() && inst.args[0].isConstant())
            {
                const ConstantValue& cv = function.constants[inst.args[0].index];
                int64_t val = (inst.cmd == Cmd::ConstInt) ? int64_t(cv.val.i) : cv.val.i64;
                facts.range = Range(val);
                facts.exactConstant = cv;
            }
            facts.truthiness = true;
            facts.isKnownNotNil = true;
            break;
        case Cmd::ConstDouble:
            facts.type = Type(TypeKind::Number);
            if (!inst.args.empty() && inst.args[0].isConstant())
            {
                const ConstantValue& cv = function.constants[inst.args[0].index];
                facts.exactConstant = cv;
            }
            facts.truthiness = true;
            facts.isKnownNotNil = true;
            break;
        case Cmd::ConstString:
            facts.type = Type(TypeKind::String);
            facts.truthiness = true;
            facts.isKnownNotNil = true;
            break;
        case Cmd::AllocTable:
        case Cmd::AllocVirtualTable:
            facts.type = Type(TypeKind::Table);
            facts.truthiness = true;
            facts.isKnownNotNil = true;
            facts.aliasClass = (inst.cmd == Cmd::AllocVirtualTable) ? AliasClass::VirtualObject : AliasClass::TableHeader;
            if (inst.extra < function.tableShapes.size())
                facts.knownShapeId = inst.extra;
            break;
        case Cmd::Add:
        case Cmd::Sub:
        case Cmd::Mul:
            if (inst.type.kind == TypeKind::Integer && inst.args.size() >= 2)
            {
                const AbstractValueFacts* f0 = currentFacts.getInstFacts(inst.args[0].index);
                const AbstractValueFacts* f1 = currentFacts.getInstFacts(inst.args[1].index);
                if (f0 && f1 && f0->range.isKnown() && f1->range.isKnown())
                {
                    if (inst.cmd == Cmd::Add)
                        facts.range = f0->range.add(f1->range);
                }
            }
            facts.isKnownNotNil = true;
            break;
        case Cmd::CheckNotNil:
            facts.isKnownNotNil = true;
            break;
        case Cmd::CheckShape:
            facts.isKnownNotNil = true;
            if (!inst.args.empty())
                facts.knownShapeId = inst.extra;
            break;
        case Cmd::StoreVmReg:
            if (inst.args.size() >= 2)
            {
                uint8_t reg = uint8_t(inst.extra);
                const AbstractValueFacts* valFacts = currentFacts.getInstFacts(inst.args[0].index);
                if (valFacts)
                    currentFacts.setVmRegFacts(reg, *valFacts);
            }
            break;
        case Cmd::LoadVmReg:
        {
            uint8_t reg = uint8_t(inst.extra);
            const AbstractValueFacts* regFacts = currentFacts.getVmRegFacts(reg);
            if (regFacts)
                facts = *regFacts;
            break;
        }
        default:
            break;
        }

        currentFacts.setInstFacts(instIdx, facts);
        inst.range = facts.range;
        inst.type = facts.type;
    }
}

void HirAnalysis::widenLoop(uint32_t headerIdx)
{
    LoopAnalysisResult& lres = loopResults[headerIdx];
    lres.isAnalyzed = true;

    Block& header = function.blocks[headerIdx];
    if (header.structuredLoop.has_value())
    {
        StructuredLoop& sl = *header.structuredLoop;
        lres.isInductionKnown = true;
        lres.inductionRange = sl.inductionRange;
        lres.loopBlocks.push_back(headerIdx);
        if (sl.bodyBlock != ~0u)
            lres.loopBlocks.push_back(sl.bodyBlock);
        if (sl.exitBlock != ~0u)
            lres.loopExits.push_back(sl.exitBlock);
    }
    else
    {
        lres.inductionRange = Range(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
    }
}

void HirAnalysis::analyzeLoops()
{
    for (uint32_t bidx = 0; bidx < function.blocks.size(); ++bidx)
    {
        Block& b = function.blocks[bidx];
        if (b.isLoopHeader)
        {
            widenLoop(bidx);
        }
    }
}

void HirAnalysis::analyzeEscapes()
{
    for (VirtualTable& vt : function.virtualTables)
    {
        vt.escape = EscapeState::NoEscape;
    }

    for (const Inst& inst : function.instructions)
    {
        if (inst.cmd == Cmd::Call || inst.cmd == Cmd::ReturnValues || inst.cmd == Cmd::SetUpvalue)
        {
            for (const Value& arg : inst.args)
            {
                if (arg.isVirtual() && arg.index < function.virtualTables.size())
                {
                    function.virtualTables[arg.index].escape = EscapeState::GlobalEscape;
                }
            }
        }
    }
}

void HirAnalysis::propagateFacts()
{
    uint32_t numBlocks = uint32_t(function.blocks.size());
    if (numBlocks == 0)
        return;

    entryFacts.assign(numBlocks, FactSet{});
    exitFacts.assign(numBlocks, FactSet{});

    std::queue<uint32_t> worklist;
    std::vector<bool> inWorklist(numBlocks, false);
    std::vector<uint32_t> visitCount(numBlocks, 0);

    worklist.push(function.entryBlock);
    inWorklist[function.entryBlock] = true;

    const uint32_t kMaxVisitsPerBlock = 8;

    while (!worklist.empty())
    {
        uint32_t bidx = worklist.front();
        worklist.pop();
        inWorklist[bidx] = false;
        visitCount[bidx]++;

        // If loop header visited multiple times, apply widening
        if (visitCount[bidx] > 2 && function.blocks[bidx].isLoopHeader)
        {
            widenLoop(bidx);
        }

        FactSet currentFacts = entryFacts[bidx];
        propagateBlock(bidx, currentFacts);
        exitFacts[bidx] = currentFacts;

        for (uint32_t succ : function.blocks[bidx].successors)
        {
            if (succ >= numBlocks)
                continue;

            bool changed = entryFacts[succ].meetWith(exitFacts[bidx]);

            if (changed && !inWorklist[succ] && visitCount[succ] < kMaxVisitsPerBlock)
            {
                worklist.push(succ);
                inWorklist[succ] = true;
            }
        }
    }
}

void HirAnalysis::runPasses()
{
    computeControlFlow();
    computeDominance();
    analyzeLoops();
    propagateFacts();
    analyzeEscapes();
}

const FactSet& HirAnalysis::getBlockEntryFacts(uint32_t blockIdx) const
{
    return entryFacts[blockIdx];
}

const FactSet& HirAnalysis::getBlockExitFacts(uint32_t blockIdx) const
{
    return exitFacts[blockIdx];
}

const LoopAnalysisResult* HirAnalysis::getLoopInfo(uint32_t loopHeaderBlock) const
{
    auto it = loopResults.find(loopHeaderBlock);
    if (it != loopResults.end())
        return &it->second;
    return nullptr;
}

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
