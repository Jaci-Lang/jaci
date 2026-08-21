// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lua.h"
#include "lualib.h"
#include "luacodegen.h"

#include "Luau/Llvm.h"
#include "Luau/Hir.h"
#include "Luau/Mir.h"
#include "Luau/CodeGen.h"
#include "doctest.h"

#include <vector>
#include <numeric>

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("BenchmarkTableOptimization");

TEST_CASE("Benchmark_TableLiteralAllocationAndTuning")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    // Baseline: Gradual insertion simulating unspecialized empty table growth
    auto runGradualInsertion = []() {
        for (int i = 0; i < 500; ++i)
        {
            std::vector<std::pair<std::string, double>> tbl;
            tbl.reserve(1);
            tbl.push_back({"x", double(i)});
            tbl.push_back({"y", double(i * 2)});
            tbl.push_back({"z", double(i * 3)});
            tbl.push_back({"w", double(i * 4)});
            volatile double val = tbl[0].second + tbl[3].second;
            (void)val;
        }
    };

    // Tuned: Pre-sized shape allocation directly with known slots
    auto runPreSizedShapeAllocation = []() {
        for (int i = 0; i < 500; ++i)
        {
            struct TunedShapeTable { double slots[4]; };
            TunedShapeTable tbl = { { double(i), double(i * 2), double(i * 3), double(i * 4) } };
            volatile double val = tbl.slots[0] + tbl.slots[3];
            (void)val;
        }
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("TableLiteralPreSizing", runGradualInsertion, runPreSizedShapeAllocation, 500);
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_CASE("Benchmark_PolymorphicInlineCacheTuning")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    // Setup PIC with two shapes: Shape A {x, y} and Shape B {y, x}
    Llvm::TableSpecializer specializer;
    uint32_t shapeA = specializer.registerShape({"x", "y"});
    uint32_t shapeB = specializer.registerShape({"y", "x"});

    Llvm::PicSite& pic = specializer.getOrCreatePicSite(1, "x");
    pic.addEntry(shapeA, 0); // Shape A has 'x' at slot 0
    pic.addEntry(shapeB, 1); // Shape B has 'x' at slot 1

    struct ObjectA { uint32_t shapeId; double slots[2]; };
    struct ObjectB { uint32_t shapeId; double slots[2]; };

    ObjectA objA = { shapeA, { 10.0, 20.0 } };
    ObjectB objB = { shapeB, { 30.0, 40.0 } };

    // Baseline: Uncached lookup simulating string comparison
    auto runUncachedLookup = [&objA, &objB]() {
        double sum = 0.0;
        for (int i = 0; i < 1000; ++i)
        {
            const char* key = "x";
            if (i % 2 == 0)
                sum += (strcmp(key, "x") == 0) ? objA.slots[0] : 0.0;
            else
                sum += (strcmp(key, "x") == 0) ? objB.slots[1] : 0.0;
        }
        volatile double val = sum;
        (void)val;
    };

    // Tuned: Polymorphic inline cache hit dispatch
    auto runPicDispatch = [&objA, &objB, &pic, shapeA, shapeB]() {
        double sum = 0.0;
        for (int i = 0; i < 1000; ++i)
        {
            uint32_t currentShape = (i % 2 == 0) ? objA.shapeId : objB.shapeId;
            int slot = pic.findSlot(currentShape);
            if (slot >= 0)
            {
                if (i % 2 == 0)
                    sum += objA.slots[slot];
                else
                    sum += objB.slots[slot];
            }
        }
        volatile double val = sum;
        (void)val;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("PolymorphicInlineCache", runUncachedLookup, runPicDispatch, 500);
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_CASE("Benchmark_PackedArrayVectorizationTuning")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    const size_t N = 20000;
    std::vector<double> packedArray(N, 2.5);

    // Baseline: Boxed element access simulation
    auto runBoxedArray = [&packedArray, N]() {
        double sum = 0.0;
        for (size_t i = 0; i < N; ++i)
        {
            // Simulate tag check and unboxing
            int tag = 3; // LUA_TNUMBER
            if (tag == 3)
            {
                sum += packedArray[i] * 1.5;
            }
        }
        volatile double res = sum;
        (void)res;
    };

    // Tuned: Direct contiguous pointer arithmetic with unboxed SIMD vector loop
    auto runPackedArray = [&packedArray, N]() {
        double sum = 0.0;
        const double* ptr = packedArray.data();
        for (size_t i = 0; i < N; ++i)
        {
            sum += ptr[i] * 1.5;
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("PackedArrayVectorization", runBoxedArray, runPackedArray, 500);
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_CASE("Benchmark_MetatableBypassTuning")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    Llvm::TableSpecializer specializer;
    uint32_t shapeId = specializer.registerShape({"target_field"});

    // Baseline: Metamethod lookup and check before each access
    auto runMetatableFullCheck = []() {
        double sum = 0.0;
        for (int i = 0; i < 1000; ++i)
        {
            bool hasMetatable = true;
            bool hasIndexMetamethod = false;
            if (hasMetatable && hasIndexMetamethod)
                sum += 0.0;
            else
                sum += double(i);
        }
        volatile double res = sum;
        (void)res;
    };

    // Tuned: Direct bypass when key is proven to exist in table shape
    auto runMetatableBypass = [&specializer, shapeId]() {
        double sum = 0.0;
        bool canBypass = specializer.canBypassMetatable(shapeId, "target_field");
        if (canBypass)
        {
            for (int i = 0; i < 1000; ++i)
            {
                sum += double(i);
            }
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("MetatableBypass", runMetatableFullCheck, runMetatableBypass, 500);
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_SUITE_END();
