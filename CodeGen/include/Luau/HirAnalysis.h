// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/HirData.h"

#include <bitset>
#include <unordered_map>
#include <vector>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

struct AbstractValueFacts
{
    Type type = Type(TypeKind::Any);
    Range range;
    std::optional<ConstantValue> exactConstant;
    std::optional<bool> truthiness;
    EscapeState escape = EscapeState::NoEscape;
    uint32_t knownShapeId = ~0u;
    bool isKnownNotNil = false;
    AliasClass aliasClass = AliasClass::Unknown;

    bool meetWith(const AbstractValueFacts& other);
};

struct FactSet
{
    std::unordered_map<uint32_t, AbstractValueFacts> instFacts;
    std::unordered_map<uint32_t, AbstractValueFacts> blockArgFacts;
    std::unordered_map<uint8_t, AbstractValueFacts> vmRegFacts;

    const AbstractValueFacts* getInstFacts(uint32_t instIdx) const;
    const AbstractValueFacts* getBlockArgFacts(uint32_t argIdx) const;
    const AbstractValueFacts* getVmRegFacts(uint8_t reg) const;

    void setInstFacts(uint32_t instIdx, AbstractValueFacts facts);
    void setBlockArgFacts(uint32_t argIdx, AbstractValueFacts facts);
    void setVmRegFacts(uint8_t reg, AbstractValueFacts facts);

    bool meetWith(const FactSet& other);
};

struct LoopAnalysisResult
{
    bool isAnalyzed = false;
    bool isInductionKnown = false;
    int64_t tripCount = -1;
    Range inductionRange;
    std::vector<uint32_t> loopBlocks;
    std::vector<uint32_t> loopExits;
};

class HirAnalysis
{
public:
    explicit HirAnalysis(Function& function);

    void runPasses();

    void computeDominance();
    void computeControlFlow();
    void propagateFacts();
    void analyzeLoops();
    void analyzeEscapes();

    const FactSet& getBlockEntryFacts(uint32_t blockIdx) const;
    const FactSet& getBlockExitFacts(uint32_t blockIdx) const;
    const LoopAnalysisResult* getLoopInfo(uint32_t loopHeaderBlock) const;

    bool isDominating(uint32_t a, uint32_t b) const;

private:
    Function& function;
    std::vector<FactSet> entryFacts;
    std::vector<FactSet> exitFacts;
    std::vector<uint32_t> idoms;
    std::vector<std::vector<uint32_t>> domChildren;
    std::unordered_map<uint32_t, LoopAnalysisResult> loopResults;

    void propagateBlock(uint32_t blockIdx, FactSet& currentFacts);
    void widenLoop(uint32_t headerIdx);
};

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
