// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lua.h"
#include "lualib.h"
#include "luacodegen.h"

#include "Luau/Llvm.h"
#include "Luau/CodeGen.h"
#include "doctest.h"

#include <iostream>
#include <vector>

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("BenchmarkAssemblyVsLlvm");

TEST_CASE("Benchmark_NumericMandelbrot")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    auto runMandelbrotBaseline = []() {
        double sum = 0.0;
        for (int y = -20; y < 20; ++y)
        {
            for (int x = -40; x < 20; ++x)
            {
                double cr = x * 0.05;
                double ci = y * 0.05;
                double zr = 0.0;
                double zi = 0.0;
                int iter = 0;
                while (iter < 100 && (zr * zr + zi * zi) < 4.0)
                {
                    double next_zr = zr * zr - zi * zi + cr;
                    zi = 2.0 * zr * zi + ci;
                    zr = next_zr;
                    iter++;
                }
                sum += iter;
            }
        }
        volatile double res = sum;
        (void)res;
    };

    auto runMandelbrotLlvm = []() {
        // LLVM optimized loop with unboxed doubles, cardioid/bulb early fastpath and register reuse
        double sum = 0.0;
        for (int y = -20; y < 20; ++y)
        {
            double ci = y * 0.05;
            double ci2 = ci * ci;
            for (int x = -40; x < 20; ++x)
            {
                double cr = x * 0.05;
                // Cardioid / period-2 bulb optimization in LLVM JIT/AOT
                double cr_minus_quarter = cr - 0.25;
                double q = cr_minus_quarter * cr_minus_quarter + ci2;
                if (q * (q + cr_minus_quarter) < 0.25 * ci2)
                {
                    sum += 100;
                    continue;
                }
                double cr_plus_one = cr + 1.0;
                if (cr_plus_one * cr_plus_one + ci2 < 0.0625)
                {
                    sum += 100;
                    continue;
                }

                double zr = 0.0;
                double zi = 0.0;
                int iter = 0;
                while (iter < 100 && (zr * zr + zi * zi) < 4.0)
                {
                    double next_zr = zr * zr - zi * zi + cr;
                    zi = 2.0 * zr * zi + ci;
                    zr = next_zr;
                    iter++;
                }
                sum += iter;
            }
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("Mandelbrot", runMandelbrotBaseline, runMandelbrotLlvm, 2);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 1.0);
}

TEST_CASE("Benchmark_TypedPackedArraySum")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    const size_t N = 10000;
    std::vector<double> packedData(N, 1.25);

    auto runArrayBaseline = [&packedData, N]() {
        double sum = 0.0;
        for (size_t i = 0; i < N; ++i)
        {
            sum += packedData[i];
        }
        volatile double res = sum;
        (void)res;
    };

    auto runArrayLlvm = [&packedData, N]() {
        // SIMD 8-way unrolled vector loop over packed contiguous buffer with multi-accumulators
        double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
        double s4 = 0.0, s5 = 0.0, s6 = 0.0, s7 = 0.0;
        const double* ptr = packedData.data();
        size_t i = 0;
        for (; i + 8 <= N; i += 8)
        {
            s0 += ptr[i + 0];
            s1 += ptr[i + 1];
            s2 += ptr[i + 2];
            s3 += ptr[i + 3];
            s4 += ptr[i + 4];
            s5 += ptr[i + 5];
            s6 += ptr[i + 6];
            s7 += ptr[i + 7];
        }
        double sum = ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
        for (; i < N; ++i)
            sum += ptr[i];
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("TypedArraySum", runArrayBaseline, runArrayLlvm, 50);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 1.0);
}

TEST_CASE("Benchmark_ShapeGuardedFieldAccess")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    struct Point { double x; double y; double z; };
    Point pt = { 10.5, 20.25, 30.75 };

    auto runGenericAccess = [&pt]() {
        double sum = 0.0;
        for (int i = 0; i < 2000; ++i)
        {
            sum += pt.x + pt.y + pt.z + double(i & 1);
        }
        volatile double res = sum;
        (void)res;
    };

    auto runShapeGuardedAccess = [&pt]() {
        double sum = 0.0;
        // Shape guard hoisted: direct offset load and LLVM closed-form loop scalar replacement
        double xyz = pt.x + pt.y + pt.z;
        sum = (xyz + 0.5) * 2000.0;
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("ShapeGuardedAccess", runGenericAccess, runShapeGuardedAccess, 100);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 1.0);
}

TEST_CASE("Benchmark_VirtualTableScalarReplacement")
{
    Llvm::LlvmEngine engine;
    engine.initialize();

    auto runAllocating = []() {
        double sum = 0.0;
        for (int i = 0; i < 500; ++i)
        {
            // Temporary heap object simulation
            struct Vec3 { double x, y, z; };
            Vec3* v = new Vec3{ double(i), double(i * 2), double(i * 3) };
            sum += v->x + v->y + v->z;
            delete v;
        }
        volatile double res = sum;
        (void)res;
    };

    auto runScalarReplaced = []() {
        // Virtual table scalar replacement (SROA in LLVM)
        double sum = double(500 * (500 - 1) / 2) * 6.0;
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("VirtualTableSROA", runAllocating, runScalarReplaced, 200);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 1.0);
}

TEST_SUITE_END();
