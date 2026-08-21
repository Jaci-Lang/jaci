// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/MirData.h"

#include <unordered_map>
#include <vector>

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

class MirAnalysis
{
public:
    explicit MirAnalysis(Function& function);

    void computeControlFlow();
    void computeDominance();
    void computeUseCounts();

    bool isDominating(uint32_t a, uint32_t b) const;
    bool canAlias(const Inst& instA, const Inst& instB) const;

    const std::vector<uint32_t>& getIdoms() const
    {
        return idoms;
    }

private:
    Function& function;
    std::vector<uint32_t> idoms;
    std::vector<std::vector<uint32_t>> domChildren;
};

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
