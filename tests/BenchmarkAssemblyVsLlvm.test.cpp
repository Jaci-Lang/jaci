// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lua.h"
#include "lualib.h"
#include "luacodegen.h"

#include "luacode.h"

#include "Luau/CodeGen.h"
#include "doctest.h"

#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if LUAU_USE_LLVM

using namespace Luau::CodeGen;

namespace
{

// Numeric workload compiled and executed by each backend. The loop dominates
// the runtime so both backends are measured on the same hot path.
const char kBenchmarkSource[] =
    "local function work(n)\n"
    "    local sum = 0.0\n"
    "    for i = 1, n do\n"
    "        local t = i % 997\n"
    "        sum = sum + t * t * 0.001\n"
    "    end\n"
    "    return sum\n"
    "end\n"
    "return work(100000)\n";

struct BackendResult
{
    CodeGenCompilationResult compileResult = CodeGenCompilationResult::CodeGenNotInitialized;
    double value = 0.0;
    double totalMs = 0.0;
    bool executed = false;
};

// Compile the benchmark chunk with the given backend flags and execute it
// `iterations` times, measuring wall time of the executions only.
BackendResult runBackend(unsigned int flags, int iterations)
{
    BackendResult result;

    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    if (luau_codegen_supported() == 0)
        return result;

    luau_codegen_create(L);

    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(kBenchmarkSource, std::strlen(kBenchmarkSource), nullptr, &bytecodeSize);
    if (bytecode == nullptr)
        return result;
    int loadResult = luau_load(L, "bench", bytecode, bytecodeSize, 0);
    std::free(bytecode);
    if (loadResult != 0)
        return result;

    // The compiled closure stays at stack index 1; each iteration pushes a
    // copy to the top so lua_pcall consumes the copy, not the original.
    REQUIRE(lua_gettop(L) == 1);

    CompilationOptions options;
    options.flags = flags;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    result.compileResult = cres.result;

    double totalNs = 0.0;

    for (int i = 0; i < iterations; ++i)
    {
        lua_pushvalue(L, 1);

        auto t0 = std::chrono::steady_clock::now();
        int callResult = lua_pcall(L, 0, 1, 0);
        auto t1 = std::chrono::steady_clock::now();

        if (callResult != 0)
            return result;

        result.value = lua_tonumber(L, -1);
        lua_pop(L, 1);

        totalNs += double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    result.totalMs = totalNs / 1000000.0;
    result.executed = true;
    return result;
}

} // namespace

TEST_SUITE_BEGIN("BenchmarkAssemblyVsLlvm");

// Compares the assembly backend against the LLVM backend on the same Luau
// program. Both backends must produce the same result; the measured ratio is
// reported for observability only. No speedup is asserted: in the current
// phase the LLVM entries resume in the VM, so the assembly backend is
// expected to be at least as fast.
TEST_CASE("BackendComparison_NumericLoop")
{
    if (luau_codegen_supported() == 0)
        return;

    const int iterations = 5;

    BackendResult assembly = runBackend(CodeGen_ColdFunctions, iterations);
    BackendResult llvm = runBackend(CodeGen_ColdFunctions | CodeGen_UseLlvm, iterations);

    REQUIRE(assembly.executed);
    REQUIRE(llvm.executed);
    REQUIRE(assembly.compileResult == CodeGenCompilationResult::Success);
    REQUIRE(llvm.compileResult == CodeGenCompilationResult::Success);

    // Same program, same machine: results must agree (small epsilon for
    // floating point ordering differences, if any).
    CHECK(std::fabs(assembly.value - llvm.value) < 1e-6 * std::fabs(assembly.value));

    CHECK_GT(assembly.totalMs, 0.0);
    CHECK_GT(llvm.totalMs, 0.0);

    double ratio = llvm.totalMs / assembly.totalMs;

    std::printf("  [BackendComparison] Assembly: %.3f ms | LLVM: %.3f ms | LLVM/Assembly: %.3fx\n", assembly.totalMs, llvm.totalMs, ratio);
}

TEST_SUITE_END();

#endif // LUAU_USE_LLVM
