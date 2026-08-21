// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Hir.h"
#include "Luau/Mir.h"
#include "Luau/Llvm.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("LlvmExecution");

TEST_CASE("EndToEndMultiTierPipelineToLlvm")
{
    // 1. Build HIR
    Hir::HirBuilder hbuilder;
    uint32_t b0 = hbuilder.createBlock("entry");
    hbuilder.setInsertionBlock(b0);

    Hir::Value tbl = hbuilder.emitAllocTable(0);
    Hir::Value k = hbuilder.emitConstString("counter");
    Hir::Value v = hbuilder.emitConstInt(42);

    hbuilder.emitSetTable(tbl, k, v);
    Hir::Value getVal = hbuilder.emitGetTable(tbl, k);
    hbuilder.emitReturn({getVal});

    Hir::Function& hfn = hbuilder.getFunction();

    // 2. Optimize HIR
    Hir::optimizeHir(hfn);

    // 3. Lower to MIR
    Mir::MirBuilder mbuilder;
    Mir::Function mfn = mbuilder.lowerFromHir(hfn);

    // 4. Optimize MIR
    Mir::optimizeMir(mfn);

    // 5. Lower to LLVM IR
    Llvm::LlvmBuilder lbuilder("pipeline_llvm");
    Llvm::TableSpecializer tableSpec;
    bool ok = Llvm::lowerMirToLlvm(lbuilder, tableSpec, mfn, nullptr);
    CHECK(ok);

    std::string moduleIr = lbuilder.getModuleIr();
    CHECK(moduleIr.find("define i32 @jaci_entry") != std::string::npos);
    CHECK(moduleIr.find("ret i32") != std::string::npos);
}

TEST_CASE("ComparativePerformanceBenchmark")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    // Benchmark comparing baseline computation
    int accumAssembly = 0;
    auto assemblyBenchmark = [&accumAssembly]() {
        for (int i = 0; i < 1000; ++i)
        {
            accumAssembly += (i * 3 + 7) ^ 15;
        }
    };

    int accumLlvm = 0;
    auto llvmBenchmark = [&accumLlvm]() {
        for (int i = 0; i < 1000; ++i)
        {
            accumLlvm += (i * 3 + 7) ^ 15;
        }
    };

    Llvm::BenchmarkResult result = engine.comparePerformance("ArithmeticLoop", assemblyBenchmark, llvmBenchmark, 500);

    CHECK_GT(result.assemblyTimeMs, 0.0);
    CHECK_GT(result.llvmTimeMs, 0.0);
    CHECK_GT(result.speedupRatio, 0.0);
    CHECK_FALSE(result.summary.empty());
}

TEST_SUITE_END();
