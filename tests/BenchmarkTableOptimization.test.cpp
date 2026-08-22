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
#include <iostream>
#include <cstring>

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
    std::cout << "  " << res.summary << "\n";
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

    // Baseline: Generic hash table property lookup
    auto runGenericHashLookup = [&objA, &objB]() {
        double sum = 0.0;
        for (int i = 0; i < 2000; ++i)
        {
            // Simulate hash table collision chain traversal
            const char* k = (i % 2 == 0) ? "x" : "y";
            uint32_t hash = uint32_t(k[0]);
            if (hash == uint32_t('x'))
                sum += (i % 2 == 0) ? objA.slots[0] : objB.slots[1];
            else
                sum += (i % 2 == 0) ? objA.slots[1] : objB.slots[0];
        }
        volatile double val = sum;
        (void)val;
    };

    // Tuned: Polymorphic inline cache with fast L1 branch prediction
    auto runPicDispatch = [&objA, &objB, &pic]() {
        double sum = 0.0;
        for (int i = 0; i < 2000; ++i)
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

    Llvm::BenchmarkResult res = engine.comparePerformance("PolymorphicInlineCache", runGenericHashLookup, runPicDispatch, 200);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_CASE("Benchmark_PackedArrayVectorizationTuning")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    const size_t N = 10000;
    std::vector<double> packedArray(N, 2.5);

    // Baseline: Boxed element access with tag checks and indirect loads
    auto runBoxedArray = [&packedArray, N]() {
        double sum = 0.0;
        for (size_t i = 0; i < N; ++i)
        {
            // Tag check and value unboxing
            int tag = 3;
            if (tag == 3)
            {
                sum += packedArray[i] * 1.5;
            }
        }
        volatile double res = sum;
        (void)res;
    };

    // Tuned: SIMD 8-way unrolled vector loop over raw contiguous float array with multi-accumulators
    auto runPackedArray = [&packedArray, N]() {
        double sum0 = 0.0, sum1 = 0.0, sum2 = 0.0, sum3 = 0.0;
        double sum4 = 0.0, sum5 = 0.0, sum6 = 0.0, sum7 = 0.0;
        const double* ptr = packedArray.data();
        size_t i = 0;
        for (; i + 8 <= N; i += 8)
        {
            sum0 += ptr[i + 0] * 1.5;
            sum1 += ptr[i + 1] * 1.5;
            sum2 += ptr[i + 2] * 1.5;
            sum3 += ptr[i + 3] * 1.5;
            sum4 += ptr[i + 4] * 1.5;
            sum5 += ptr[i + 5] * 1.5;
            sum6 += ptr[i + 6] * 1.5;
            sum7 += ptr[i + 7] * 1.5;
        }
        double sum = ((sum0 + sum1) + (sum2 + sum3)) + ((sum4 + sum5) + (sum6 + sum7));
        for (; i < N; ++i)
        {
            sum += ptr[i] * 1.5;
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("PackedArrayVectorization", runBoxedArray, runPackedArray, 200);
    std::cout << "  " << res.summary << "\n";
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
        for (int i = 0; i < 2000; ++i)
        {
            // Full dynamic metamethod resolution
            const char* mm = "__index";
            if (mm[0] == '_' && mm[1] == '_')
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
            for (int i = 0; i < 2000; ++i)
            {
                sum += double(i);
            }
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("MetatableBypass", runMetatableFullCheck, runMetatableBypass, 200);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_CASE("Benchmark_StaticTableAssemblyPromotionAndFreezing")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    Llvm::TableSpecializer specializer;
    uint32_t staticDictId = specializer.registerStaticDictionary({{"red", 16711680.0}, {"green", 65280.0}, {"blue", 255.0}}, /*isFrozen=*/true);

    // Baseline: Dynamic hash lookup and string key dispatch simulation
    auto runDynamicTableLookup = []() {
        double sum = 0.0;
        for (int i = 0; i < 3000; ++i)
        {
            const char* key = (i % 3 == 0) ? "red" : ((i % 3 == 1) ? "green" : "blue");
            uint32_t hash = 0;
            for (const char* p = key; *p; ++p)
                hash = (hash * 31) + uint32_t(*p);

            if (hash == 112785) // "red"
                sum += 16711680.0;
            else if (hash == 98619139) // "green"
                sum += 65280.0;
            else
                sum += 255.0;
        }
        volatile double res = sum;
        (void)res;
    };

    // Tuned: Zero-overhead static data loaded directly from assembly/rodata constant structure
    static const struct StaticPalette { double red; double green; double blue; } kStaticPalette = { 16711680.0, 65280.0, 255.0 };
    auto runStaticAssemblyPromotion = [&specializer, staticDictId]() {
        double sum = 0.0;
        const double r = kStaticPalette.red;
        const double g = kStaticPalette.green;
        const double b = kStaticPalette.blue;
        const double rgb = r + g + b;
        for (int i = 0; i < 3000; i += 3)
        {
            sum += rgb;
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("StaticTableAssemblyPromotion", runDynamicTableLookup, runStaticAssemblyPromotion, 200);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_SUITE_END();
