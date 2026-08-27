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

const char kModuloLoop[] = "local function work(n)\n"
                           "    local sum = 0.0\n"
                           "    for i = 1, n do\n"
                           "        local t = i % 997\n"
                           "        sum = sum + t * t * 0.001\n"
                           "    end\n"
                           "    return sum\n"
                           "end\n"
                           "return work(100000)\n";

const char kPolynomialLoop[] = "local function work(n)\n"
                               "    local value = 0.25\n"
                               "    for i = 1, n do\n"
                               "        value = value * 1.000001 + i * 0.00001 - 0.000001\n"
                               "    end\n"
                               "    return value\n"
                               "end\n"
                               "return work(500000)\n";

const char kDynamicStepLoop[] = "local function work(first, last, step)\n"
                                "    local total = 0.0\n"
                                "    for i = first, last, step do\n"
                                "        total = total + (i % 31) * 0.125 + (i // 7)\n"
                                "    end\n"
                                "    return total\n"
                                "end\n"
                                "return work(1, 300000, 3)\n";

const char kBranchedLoop[] = "local function work(n)\n"
                             "    local total = 0.0\n"
                             "    for i = 1, n do\n"
                             "        if i % 2 == 0 then\n"
                             "            total = total + i * 0.25\n"
                             "        else\n"
                             "            total = total - i * 0.125\n"
                             "        end\n"
                             "    end\n"
                             "    return total\n"
                             "end\n"
                             "return work(100000)\n";

const char kComparedBranchLoop[] = "local function work(n)\n"
                                   "    local total = 0.0\n"
                                   "    for i = 1, n do\n"
                                   "        local bucket = i % 100\n"
                                   "        if bucket < 50 then\n"
                                   "            total = total + bucket * 0.25\n"
                                   "        else\n"
                                   "            total = total - bucket * 0.125\n"
                                   "        end\n"
                                   "    end\n"
                                   "    return total\n"
                                   "end\n"
                                   "return work(100000)\n";

const char kArrayLoop[] = "local function work(values)\n"
                          "    local total = 0.0\n"
                          "    for round = 1, 200 do\n"
                          "        for i = 1, 256 do\n"
                          "            total = total + values[i]\n"
                          "        end\n"
                          "    end\n"
                          "    return total\n"
                          "end\n"
                          "local values = table.create(256)\n"
                          "for i = 1, 256 do values[i] = i * 0.5 end\n"
                          "return work(values)\n";

const char kCachedFieldLoop[] = "local function work(object, n)\n"
                                "    local total = 0.0\n"
                                "    for i = 1, n do\n"
                                "        total = total + object.x + object.y\n"
                                "        object.x = object.x + 0.00001\n"
                                "    end\n"
                                "    return total\n"
                                "end\n"
                                "return work({x = 1.0, y = 2.0}, 100000)\n";

const char kLuaCallLoop[] = "local function step(value, index)\n"
                            "    return value * 1.000001 + index * 0.00001\n"
                            "end\n"
                            "local function work(callback, n)\n"
                            "    local total = 0.0\n"
                            "    for i = 1, n do\n"
                            "        total = callback(total, i)\n"
                            "    end\n"
                            "    return total\n"
                            "end\n"
                            "return work(step, 100000)\n";

const char kFieldCallLoop[] = "local function fieldSum(object)\n"
                              "    return object.x + object.y\n"
                              "end\n"
                              "local function work(callback, object, n)\n"
                              "    local total = 0.0\n"
                              "    for i = 1, n do\n"
                              "        total = total + callback(object)\n"
                              "    end\n"
                              "    return total\n"
                              "end\n"
                              "return work(fieldSum, {x = 1.0, y = 2.0}, 100000)\n";

const char kMathBuiltinLoop[] = "local function work(n)\n"
                                "    local total = 0.0\n"
                                "    for i = 1, n do\n"
                                "        total = total + math.sqrt(i) + math.abs(i - 50000)\n"
                                "        total = total + math.floor(i * 0.125) + math.ceil(i * 0.0625)\n"
                                "    end\n"
                                "    return total\n"
                                "end\n"
                                "return work(100000)\n";

const char kMinMaxBuiltinLoop[] = "local function work(n, pivot)\n"
                                  "    local total = 0.0\n"
                                  "    for i = 1, n do\n"
                                  "        total = total + math.min(i, pivot) + math.max(i, pivot)\n"
                                  "    end\n"
                                  "    return total\n"
                                  "end\n"
                                  "return work(100000, 50000)\n";

const char kScalarMathBuiltinLoop[] = "local function work(n, pivot)\n"
                                      "    local total = 0.0\n"
                                      "    for i = 1, n do\n"
                                      "        total = total + math.deg(math.rad(i)) + math.sign(i - pivot)\n"
                                      "    end\n"
                                      "    return total\n"
                                      "end\n"
                                      "return work(100000, 50000)\n";

struct BackendResult
{
    CodeGenCompilationResult compileResult = CodeGenCompilationResult::CodeGenNotInitialized;
    double value = 0.0;
    double totalMs = 0.0;
    bool executed = false;
};

// Compile the benchmark chunk with the given backend flags and execute it
// `iterations` times, measuring wall time of the executions only.
BackendResult runBackend(const char* source, unsigned int flags, int iterations)
{
    BackendResult result;

    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    if (luau_codegen_supported() == 0)
        return result;

    luau_codegen_create(L);

    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source, std::strlen(source), nullptr, &bytecodeSize);
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

void compareBackends(const char* name, const char* source)
{
    const int iterations = 5;
    BackendResult assembly = runBackend(source, CodeGen_ColdFunctions, iterations);
    BackendResult llvm = runBackend(source, CodeGen_ColdFunctions | CodeGen_UseLlvm, iterations);

    REQUIRE(assembly.executed);
    REQUIRE(llvm.executed);
    REQUIRE(assembly.compileResult == CodeGenCompilationResult::Success);
    REQUIRE(llvm.compileResult == CodeGenCompilationResult::Success);

    CHECK(std::fabs(assembly.value - llvm.value) < 1e-6 * std::fmax(1.0, std::fabs(assembly.value)));
    CHECK_GT(assembly.totalMs, 0.0);
    CHECK_GT(llvm.totalMs, 0.0);

    std::printf(
        "  [BackendComparison:%s] Assembly: %.3f ms | LLVM: %.3f ms | LLVM/Assembly: %.3fx\n",
        name,
        assembly.totalMs,
        llvm.totalMs,
        llvm.totalMs / assembly.totalMs
    );
}

} // namespace

TEST_SUITE_BEGIN("BenchmarkAssemblyVsLlvm");

TEST_CASE("BackendComparison_NumericLoop")
{
    if (luau_codegen_supported() == 0)
        return;

    compareBackends("modulo", kModuloLoop);
    compareBackends("polynomial", kPolynomialLoop);
    compareBackends("dynamic-step", kDynamicStepLoop);
    compareBackends("branched", kBranchedLoop);
    compareBackends("compared-branch", kComparedBranchLoop);
    compareBackends("array", kArrayLoop);
    compareBackends("cached-field", kCachedFieldLoop);
}

TEST_CASE("BackendComparison_LuaCallLoop")
{
    if (luau_codegen_supported() == 0)
        return;

    compareBackends("lua-call", kLuaCallLoop);
    compareBackends("field-call", kFieldCallLoop);
}

TEST_CASE("BackendComparison_MathBuiltinLoop")
{
    if (luau_codegen_supported() == 0)
        return;

    compareBackends("math-builtin", kMathBuiltinLoop);
    compareBackends("minmax-builtin", kMinMaxBuiltinLoop);
    compareBackends("scalar-math-builtin", kScalarMathBuiltinLoop);
}

TEST_SUITE_END();

#endif // LUAU_USE_LLVM
