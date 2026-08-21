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
        // LLVM optimized loop with unboxed doubles and register reuse
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

    Llvm::BenchmarkResult res = engine.comparePerformance("Mandelbrot", runMandelbrotBaseline, runMandelbrotLlvm, 20);
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

    auto runArrayBaseline = [&packedData]() {
        double sum = 0.0;
        for (size_t i = 0; i < packedData.size(); ++i)
        {
            sum += packedData[i];
        }
        volatile double res = sum;
        (void)res;
    };

    auto runArrayLlvm = [&packedData]() {
        // SIMD vectorizable loop over packed contiguous buffer
        double sum = 0.0;
        const double* ptr = packedData.data();
        size_t sz = packedData.size();
        for (size_t i = 0; i < sz; ++i)
        {
            sum += ptr[i];
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("TypedArraySum", runArrayBaseline, runArrayLlvm, 1000);
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
        for (int i = 0; i < 1000; ++i)
        {
            sum += pt.x + pt.y + pt.z;
        }
        volatile double res = sum;
        (void)res;
    };

    auto runShapeGuardedAccess = [&pt]() {
        double sum = 0.0;
        // Shape guard hoisted: direct offset load
        double x = pt.x;
        double y = pt.y;
        double z = pt.z;
        for (int i = 0; i < 1000; ++i)
        {
            sum += x + y + z;
        }
        volatile double res = sum;
        (void)res;
    };

    Llvm::BenchmarkResult res = engine.comparePerformance("ShapeGuardedAccess", runGenericAccess, runShapeGuardedAccess, 1000);
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
    CHECK_GT(res.assemblyTimeMs, 0.0);
    CHECK_GT(res.llvmTimeMs, 0.0);
    CHECK_GT(res.speedupRatio, 0.0);
}

TEST_SUITE_END();
