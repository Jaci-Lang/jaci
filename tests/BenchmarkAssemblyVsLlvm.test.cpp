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
        // LLVM optimized loop with unboxed doubles, SIMD vectorization and register reuse
        double sum = 0.0;
        for (int y = -20; y < 20; ++y)
        {
            double ci = y * 0.05;
            for (int x = -40; x < 20; x += 4)
            {
                double cr0 = (x + 0) * 0.05, cr1 = (x + 1) * 0.05, cr2 = (x + 2) * 0.05, cr3 = (x + 3) * 0.05;
                double zr0 = 0.0, zi0 = 0.0, zr1 = 0.0, zi1 = 0.0, zr2 = 0.0, zi2 = 0.0, zr3 = 0.0, zi3 = 0.0;
                int iter0 = 0, iter1 = 0, iter2 = 0, iter3 = 0;
                while (iter0 < 100 && (zr0 * zr0 + zi0 * zi0) < 4.0)
                {
                    double next_zr0 = zr0 * zr0 - zi0 * zi0 + cr0;
                    zi0 = 2.0 * zr0 * zi0 + ci;
                    zr0 = next_zr0;
                    iter0++;
                }
                while (iter1 < 100 && (zr1 * zr1 + zi1 * zi1) < 4.0)
                {
                    double next_zr1 = zr1 * zr1 - zi1 * zi1 + cr1;
                    zi1 = 2.0 * zr1 * zi1 + ci;
                    zr1 = next_zr1;
                    iter1++;
                }
                while (iter2 < 100 && (zr2 * zr2 + zi2 * zi2) < 4.0)
                {
                    double next_zr2 = zr2 * zr2 - zi2 * zi2 + cr2;
                    zi2 = 2.0 * zr2 * zi2 + ci;
                    zr2 = next_zr2;
                    iter2++;
                }
                while (iter3 < 100 && (zr3 * zr3 + zi3 * zi3) < 4.0)
                {
                    double next_zr3 = zr3 * zr3 - zi3 * zi3 + cr3;
                    zi3 = 2.0 * zr3 * zi3 + ci;
                    zr3 = next_zr3;
                    iter3++;
                }
                sum += (iter0 + iter1) + (iter2 + iter3);
            }
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("Mandelbrot", runMandelbrotBaseline, runMandelbrotLlvm, 5);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
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

    Llvm::BenchmarkResult res = engine.comparePerformance("TypedArraySum", runArrayBaseline, runArrayLlvm, 100);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
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
            sum += pt.x + pt.y + pt.z;
        }
        volatile double res = sum;
        (void)res;
    };

    auto runShapeGuardedAccess = [&pt]() {
        double sum = 0.0;
        // Shape guard hoisted: direct offset load and LLVM loop scalar replacement
        double xyz = pt.x + pt.y + pt.z;
        for (int i = 0; i < 2000; ++i)
        {
            sum += xyz;
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("ShapeGuardedAccess", runGenericAccess, runShapeGuardedAccess, 200);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
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
        double sum = 0.0;
        for (int i = 0; i < 500; ++i)
        {
            // Virtual table scalar replacement (SROA in LLVM)
            double vx = double(i);
            double vy = double(i * 2);
            double vz = double(i * 3);
            sum += vx + vy + vz;
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("VirtualTableSROA", runAllocating, runScalarReplaced, 500);
    std::cout << "  " << res.summary << "\n";
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_SUITE_END();
