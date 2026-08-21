// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/MirData.h"

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

class MirOptimizer
{
public:
    explicit MirOptimizer(Function& function);

    void runAllPasses();

    // Individual optimization passes
    void runRedundantGuardElimination();
    void runBoundsCheckElimination();
    void runGvnCse();
    void runLicm();
    void runLoadStoreElimination();
    void runGcBarrierElimination();
    void runDeadCodeElimination();

private:
    Function& function;
};

void optimizeMir(Function& function);

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
