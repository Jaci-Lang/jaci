// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/HirData.h"

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

class HirOptimizer
{
public:
    explicit HirOptimizer(Function& function);

    void runAllPasses();

    // Individual optimization passes
    void runConstantAndRangePropagation();
    void runTableSpecialization();
    void runVirtualTableScalarReplacement();
    void runClosureAndCallOptimization();
    void runMultivalueSimplification();
    void runDeadCodeElimination();
    void buildSnapshots();

private:
    Function& function;

    bool foldInstruction(uint32_t instIdx);
    bool specializeTableAccess(uint32_t instIdx);
    bool replaceVirtualTableAccess(uint32_t instIdx);
};

void optimizeHir(Function& function);

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
