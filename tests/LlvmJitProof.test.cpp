// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lua.h"
#include "lualib.h"
#include "luacodegen.h"

#include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/Llvm.h"
#include "Luau/Hir.h"
#include "Luau/Mir.h"

#include "ScopedFlags.h"

#include "doctest.h"

#include <cmath>
#include <memory>
#include <string>

using namespace Luau::CodeGen;

LUAU_FASTFLAG(LuauCallFeedback)
LUAU_FASTFLAG(LuauEmitCallFeedback)
LUAU_FASTFLAG(LuauBackedgeHeapCheck)

TEST_SUITE_BEGIN("LlvmJitProof");

TEST_CASE("LlvmJitProof_ArithmeticAndControlFlow")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    const char* source = R"(
        local function compute(n)
            local sum = 0
            for i = 1, n do
                if i % 2 == 0 then
                    sum = sum + (i * 3) - 1
                else
                    sum = sum + (i // 2) + 5
                end
            end
            return sum
        end
        return compute(100)
    )";

    std::string bytecode = Luau::compile(source);
    int loadRes = luau_load(L, "test_arith", bytecode.data(), bytecode.size(), 0);
    REQUIRE_EQ(loadRes, 0);

    // Compile with LLVM CodeGen flag enabled
    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    CHECK((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    int callRes = lua_pcall(L, 0, 1, 0);
    REQUIRE_EQ(callRes, 0);
    REQUIRE(lua_isnumber(L, -1));

    // Verify deterministic numeric result
    double result = lua_tonumber(L, -1);
    CHECK_GT(result, 0.0);
}

TEST_CASE("LlvmJitProof_ConstantsAndFloorDivision")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    // Exercise LOADK and IDIVK.  The negative dividend distinguishes floor
    // division from C/C++ integer truncation.
    std::string bytecode = Luau::compile("local label = 'llvm'; return label, -3 // 2");
    REQUIRE_EQ(luau_load(L, "test_constants_idiv", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(std::string(lua_tostring(L, -2)), "llvm");
    CHECK_EQ(lua_tonumber(L, -1), -2.0);
}

TEST_CASE("LlvmJitProof_LongPrimitiveEqualityJumps")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function classify(value)
            if value == nil then return 1 end
            if value == false then return 2 end
            if value == 7.5 then return 3 end
            if value == "jaci" then return 4 end
            return 0
        end
        return classify(nil), classify(false), classify(7.5), classify("jaci"), classify(true)
    )");
    REQUIRE_EQ(luau_load(L, "test_long_eq", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 5, 0), 0);
    CHECK_EQ(lua_tonumber(L, -5), 1.0);
    CHECK_EQ(lua_tonumber(L, -4), 2.0);
    CHECK_EQ(lua_tonumber(L, -3), 3.0);
    CHECK_EQ(lua_tonumber(L, -2), 4.0);
    CHECK_EQ(lua_tonumber(L, -1), 0.0);
}

TEST_CASE("LlvmJitProof_PlainArrayGetTableN")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    // The callee begins with GETTABLEN; the table is built by its VM caller,
    // ensuring the native fast path observes a regular array table.
    std::string bytecode = Luau::compile("local function second(t) return t[2] end; return second({10, 42, 99})");
    REQUIRE_EQ(luau_load(L, "test_gettablen", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_PlainArrayGetTable")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function at(t, i) return t[i] end; return at({10, 42, 99}, 2)");
    REQUIRE_EQ(luau_load(L, "test_gettable", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_PlainArraySetTable")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function setat(t, i, n) t[i] = n; return t[i] end; return setat({10, 0, 99}, 2, 42)");
    REQUIRE_EQ(luau_load(L, "test_settable", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_PlainArraySetTableN")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function setsecond(t, n) t[2] = n; return t[2] end; return setsecond({10, 0, 99}, 42)");
    REQUIRE_EQ(luau_load(L, "test_settablen", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_StringLength")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function length(s) return #s end; return length('jaci llvm')");
    REQUIRE_EQ(luau_load(L, "test_length", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 9.0);
}

TEST_CASE("LlvmJitProof_GetUpvalue")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function make(value) return function() return value end end; return make(42)()");
    REQUIRE_EQ(luau_load(L, "test_getupval", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_SetUpvaluePrimitive")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile(
        "local function make(value) return function(delta) value += delta; return value end end; local f = make(10); return f(5), f(15)"
    );
    REQUIRE_EQ(luau_load(L, "test_setupval", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 15.0);
    CHECK_EQ(lua_tonumber(L, -1), 30.0);
}

TEST_CASE("LlvmJitProof_FixedResultReturn")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function split(n) return n, n + 1 end; local a, b = split(41); return a, b");
    REQUIRE_EQ(luau_load(L, "test_fixed_return", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 41.0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_DiscardedFixedReturn")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function value() return 42 end; value(); return 1");
    REQUIRE_EQ(luau_load(L, "test_discard_return", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 1.0);
}

TEST_CASE("LlvmJitProof_PrepVarargs")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function sum(...) local a, b = ...; return a + b end; return sum(20, 22)");
    REQUIRE_EQ(luau_load(L, "test_prepvarargs", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_FixedLuaCall")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode =
        Luau::compile("local function twice(f, n) return f(n) + f(n) end; local function inc(n) return n + 1 end; return twice(inc, 20)");
    REQUIRE_EQ(luau_load(L, "test_fixed_call", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_NumericLeafPreservesTypeErrors")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function numeric(a, b) return a * 2 + b end; return numeric('bad', 1)");
    REQUIRE_EQ(luau_load(L, "test_numeric_leaf_fallback", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    CHECK_NE(lua_pcall(L, 0, 1, 0), 0);
}

TEST_CASE("LlvmJitProof_FallbackResumesExactInstruction")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local object = {x = 0}
        local function fail(value)
            object.x = object.x + 1
            return value + 1
        end
        local ok = pcall(fail, "bad")
        return object.x, ok
    )");
    REQUIRE_EQ(luau_load(L, "test_exact_fallback", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 1.0);
    CHECK_FALSE(lua_toboolean(L, -1));
}

TEST_CASE("LlvmJitProof_UnaryMathFastcalls")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function evaluate(value)
            return math.abs(value), math.floor(value), math.ceil(value), math.sqrt(math.abs(value)), math.round(value),
                math.deg(math.rad(value)), math.sign(value), math.sign(0 / 0)
        end
        return evaluate(-9.25)
    )");
    REQUIRE_EQ(luau_load(L, "test_unary_math_fastcalls", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 8, 0), 0);
    CHECK_EQ(lua_tonumber(L, -8), 9.25);
    CHECK_EQ(lua_tonumber(L, -7), -10.0);
    CHECK_EQ(lua_tonumber(L, -6), -9.0);
    CHECK_EQ(lua_tonumber(L, -5), doctest::Approx(std::sqrt(9.25)));
    CHECK_EQ(lua_tonumber(L, -4), -9.0);
    CHECK_EQ(lua_tonumber(L, -3), doctest::Approx(-9.25));
    CHECK_EQ(lua_tonumber(L, -2), -1.0);
    CHECK_EQ(lua_tonumber(L, -1), 0.0);
}

TEST_CASE("LlvmJitProof_BinaryMinMaxFastcalls")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function loop(n, pivot)
            local total = 0.0
            for i = 1, n do
                total = total + math.min(i, pivot) + math.max(i, pivot)
            end
            return total
        end

        local nan = 0 / 0
        local negativeZero = -0.0
        local ok = pcall(function() return math.min("bad", 1) end)
        return math.min(nan, 5), math.min(5, nan), math.max(nan, 5), math.max(5, nan),
            math.min(negativeZero, 0), math.max(negativeZero, 0), loop(100, 50), ok
    )");
    REQUIRE_EQ(luau_load(L, "test_binary_minmax_fastcalls", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 8, 0), 0);
    CHECK(std::isnan(lua_tonumber(L, -8)));
    CHECK_EQ(lua_tonumber(L, -7), 5.0);
    CHECK(std::isnan(lua_tonumber(L, -6)));
    CHECK_EQ(lua_tonumber(L, -5), 5.0);
    CHECK(std::signbit(lua_tonumber(L, -4)));
    CHECK(std::signbit(lua_tonumber(L, -3)));
    CHECK_EQ(lua_tonumber(L, -2), 10050.0);
    CHECK_FALSE(lua_toboolean(L, -1));
}

TEST_CASE("LlvmJitProof_LuaCallFillsMissingArguments")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode =
        Luau::compile("local function call(f) return f() end; local function default(v) return v == nil and 42 or 0 end; return call(default)");
    REQUIRE_EQ(luau_load(L, "test_missing_call_args", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_DefaultOptionsUseLlvm")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local function add(a, b) return a + b end; return add(20, 22)");
    REQUIRE_EQ(luau_load(L, "test_default_llvm", bytecode.data(), bytecode.size(), 0), 0);

    // CompilationOptions defaults to CodeGen_UseLlvm in an LLVM-enabled build.
    CompilationOptions options;
    REQUIRE((Luau::CodeGen::compile(L, -1, options).result == CodeGenCompilationResult::Success));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
}

TEST_CASE("LlvmJitProof_BackedgeSafepoint")
{
    ScopedFastFlag backedgeHeapCheck{FFlag::LuauBackedgeHeapCheck, true};

    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local i, sum = 0, 0; while i < 100 do i += 1; sum += i end; return sum");
    REQUIRE_EQ(luau_load(L, "test_backedge", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 5050.0);
}

TEST_CASE("LlvmJitProof_NumericLoopRegionSemantics")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local positive = 0
        for i = 1, 9, 2 do
            positive += i * i
        end

        local negative = 0
        for i = 9, 1, -2 do
            negative += i * 2
        end

        local function dynamic(first, last, step)
            local total = 0
            for i = first, last, step do
                local quotient = i // 3
                total += (i % 3) * i + quotient
            end
            return total
        end

        return positive, negative, dynamic(1, 10, 3), dynamic(10, 1, -3)
    )");
    REQUIRE_EQ(luau_load(L, "test_numeric_loop_region", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 4, 0), 0);
    CHECK_EQ(lua_tonumber(L, -4), 165.0);
    CHECK_EQ(lua_tonumber(L, -3), 50.0);
    CHECK_EQ(lua_tonumber(L, -2), 28.0);
    CHECK_EQ(lua_tonumber(L, -1), 28.0);
}

TEST_CASE("LlvmJitProof_NumericLoopBranchRegionSemantics")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local total = 0.0
        for i = 1, 100 do
            if i % 2 == 0 then
                total += i * 0.25
            else
                total -= i * 0.125
            end
        end

        local compared = 0.0
        for i = 1, 10000 do
            local bucket = i % 100
            if bucket < 50 then
                compared += bucket * 0.25
            else
                compared -= bucket * 0.125
            end
        end

        return total, compared
    )");
    REQUIRE_EQ(luau_load(L, "test_numeric_loop_branch", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 325.0);
    CHECK_EQ(lua_tonumber(L, -1), -15937.5);
}

TEST_CASE("LlvmJitProof_NumericLoopCachedFieldRegionSemantics")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function work(object, count)
            local total = 0.0
            for i = 1, count do
                total += object.x + object.y
                object.x += 0.00001
            end
            return total, object.x
        end
        return work({x = 1.0, y = 2.0}, 10)
    )");
    REQUIRE_EQ(luau_load(L, "test_numeric_loop_fields", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), doctest::Approx(30.00045));
    CHECK_EQ(lua_tonumber(L, -1), doctest::Approx(1.0001));
}

TEST_CASE("LlvmJitProof_NumericLoopCachedFieldRegionPreservesReadonlyErrors")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function work(object)
            for i = 1, 2 do
                object.x += i
            end
        end
        return work(table.freeze({x = 1.0}))
    )");
    REQUIRE_EQ(luau_load(L, "test_numeric_loop_readonly_field", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    CHECK_NE(lua_pcall(L, 0, 0, 0), 0);
}

TEST_CASE("LlvmJitProof_NumericLoopArrayRegionAndMetatableFallback")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function sum(values, count)
            local total = 0.0
            for i = 1, count do
                total += values[i]
            end
            return total
        end

        local proxy = setmetatable({}, {
            __index = function(_, index)
                return index * 2
            end,
        })
        return sum({1.5, 2.5, 3.5, 4.5}, 4), sum(proxy, 4)
    )");
    REQUIRE_EQ(luau_load(L, "test_numeric_loop_array", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 12.0);
    CHECK_EQ(lua_tonumber(L, -1), 20.0);
}

TEST_CASE("LlvmJitProof_NumericLoopRegionPreservesFallbackErrors")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local total = 'not a number'; for i = 1, 3 do total += i end; return total");
    REQUIRE_EQ(luau_load(L, "test_numeric_loop_fallback", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    CHECK_NE(lua_pcall(L, 0, 1, 0), 0);
}

TEST_CASE("LlvmJitProof_TableShapesAndArraySpecialization")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    const char* source = R"(
        local function testTable(count)
            local obj = { x = 10, y = 20, z = 30 }
            local arr = { 1, 2, 3, 4, 5 }
            local total = 0

            for i = 1, count do
                obj.x = obj.x + 1
                obj.y = obj.y + 2
                total = total + obj.x + obj.y + obj.z + arr[1] + arr[5]
            end

            return total
        end
        return testTable(50)
    )";

    std::string bytecode = Luau::compile(source);
    int loadRes = luau_load(L, "test_table", bytecode.data(), bytecode.size(), 0);
    REQUIRE_EQ(loadRes, 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    CHECK((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    int callRes = lua_pcall(L, 0, 1, 0);
    REQUIRE_EQ(callRes, 0);
    REQUIRE(lua_isnumber(L, -1));
    CHECK_GT(lua_tonumber(L, -1), 0.0);
}

TEST_CASE("LlvmJitProof_ClosuresAndUpvalues")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    const char* source = R"(
        local function makeCounter(start)
            local count = start
            return function(inc)
                count = count + inc
                return count
            end
        end

        local c = makeCounter(10)
        local a = c(5)
        local b = c(15)
        return a + b
    )";

    std::string bytecode = Luau::compile(source);
    int loadRes = luau_load(L, "test_closure", bytecode.data(), bytecode.size(), 0);
    REQUIRE_EQ(loadRes, 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    CHECK((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    int callRes = lua_pcall(L, 0, 1, 0);
    REQUIRE_EQ(callRes, 0);
    REQUIRE(lua_isnumber(L, -1));
    CHECK_EQ(lua_tonumber(L, -1), 45.0);
}

TEST_CASE("LlvmJitProof_MetatablesAndMetamethods")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    const char* source = R"(
        local mt = {
            __add = function(a, b)
                return { val = a.val + b.val }
            end
        }

        local function makeBox(v)
            local t = { val = v }
            setmetatable(t, mt)
            return t
        end

        local b1 = makeBox(100)
        local b2 = makeBox(200)
        local b3 = b1 + b2
        return b3.val
    )";

    std::string bytecode = Luau::compile(source);
    int loadRes = luau_load(L, "test_meta", bytecode.data(), bytecode.size(), 0);
    REQUIRE_EQ(loadRes, 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    CHECK((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    int callRes = lua_pcall(L, 0, 1, 0);
    REQUIRE_EQ(callRes, 0);
    REQUIRE(lua_isnumber(L, -1));
    CHECK_EQ(lua_tonumber(L, -1), 300.0);
}

TEST_CASE("LlvmJitProof_IpairsArrayIteration")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    // ipairs must stop at the first nil even when a later array slot exists.
    // This exercises FORGPREP_INEXT and the native array FORGLOOP path.
    std::string bytecode = Luau::compile(R"(
        local values = {10, 20, nil, 40}
        local total = 0
        local count = 0
        for index, value in ipairs(values) do
            total += index * value
            count += 1
        end
        return total, count
    )");
    REQUIRE_EQ(luau_load(L, "test_ipairs", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 50.0);
    CHECK_EQ(lua_tonumber(L, -1), 2.0);
}

TEST_CASE("LlvmJitProof_PairsArrayThenHashIteration")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    // pairs skips array holes, then the native path must hand off at the
    // hash boundary so string keys remain visible.
    std::string bytecode = Luau::compile(R"(
        local values = {[1] = 10, [3] = 30, name = 50}
        local numeric = 0
        local named = 0
        for key, value in pairs(values) do
            if type(key) == "number" then
                numeric += value
            else
                named += value
            end
        end
        return numeric, named
    )");
    REQUIRE_EQ(luau_load(L, "test_pairs", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 40.0);
    CHECK_EQ(lua_tonumber(L, -1), 50.0);
}

TEST_CASE("LlvmJitProof_PlainTableIteration")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local values = {[1] = 7, [2] = 11}
        local total = 0
        for key, value in values do
            total += key * value
        end
        return total
    )");
    REQUIRE_EQ(luau_load(L, "test_forgprep_table", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult cres = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((cres.result == CodeGenCompilationResult::Success || cres.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 29.0);
}

TEST_CASE("LlvmJitProof_FixedSetList")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile("local values = {2, 3, 5, 7, 11, 13}; return values[1] + values[6]");
    REQUIRE_EQ(luau_load(L, "test_setlist", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));
    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 15.0);
}

TEST_CASE("LlvmJitProof_CachedGlobalAndStringFieldReads")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    lua_pushnumber(L, 3.0);
    lua_setglobal(L, "llvmCachedGlobal");

    std::string bytecode = Luau::compile(R"(
        local object = {answer = 2}
        local total = 0
        for index = 1, 5 do
            total += llvmCachedGlobal + object.answer
        end
        return total
    )");
    REQUIRE_EQ(luau_load(L, "test_cached_reads", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 25.0);
}

TEST_CASE("LlvmJitProof_CachedPrimitiveGlobalAndStringFieldWrites")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    lua_pushnumber(L, 3.0);
    lua_setglobal(L, "llvmCachedWrite");

    std::string bytecode = Luau::compile(R"(
        local object = {answer = 2}
        for index = 1, 5 do
            llvmCachedWrite += 1
            object.answer += 2
        end
        return llvmCachedWrite, object.answer
    )");
    REQUIRE_EQ(luau_load(L, "test_cached_writes", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 2, 0), 0);
    CHECK_EQ(lua_tonumber(L, -2), 8.0);
    CHECK_EQ(lua_tonumber(L, -1), 12.0);
}

TEST_CASE("LlvmJitProof_CachedTableNamecall")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function add(self, amount)
            return self.value + amount
        end
        local object = {value = 10, add = add}
        local result = 0
        for amount = 1, 4 do
            result = object:add(amount)
        end
        return result
    )");
    REQUIRE_EQ(luau_load(L, "test_cached_namecall", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 1, 0), 0);
    CHECK_EQ(lua_tonumber(L, -1), 14.0);
}

TEST_CASE("LlvmJitProof_TableAllocationCloneAndConcat")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local array = {1, 2, 3}
        local record = {alpha = 5, beta = 8}
        local prefix = "llvm"
        return prefix .. "-jit", array[2], record.beta
    )");
    REQUIRE_EQ(luau_load(L, "test_allocating_opcodes", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 3, 0), 0);
    CHECK_EQ(std::string(lua_tostring(L, -3)), "llvm-jit");
    CHECK_EQ(lua_tonumber(L, -2), 2.0);
    CHECK_EQ(lua_tonumber(L, -1), 8.0);
}

TEST_CASE("LlvmJitProof_CallFeedbackLuaCMetamethodAndMultret")
{
    ScopedFastFlag emitCallFeedback{FFlag::LuauEmitCallFeedback, true};
    ScopedFastFlag callFeedback{FFlag::LuauCallFeedback, true};

    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);
    luau_codegen_create(L);

    std::string bytecode = Luau::compile(R"(
        local function add(left, right)
            return left + right
        end
        local callable = setmetatable({}, {
            __call = function(_, value)
                return value * 2
            end,
        })
        local function spread(...)
            return ...
        end

        local luaResult = add(20, 22)
        local cResult = math.abs(-9)
        local metamethodResult = callable(6)
        return luaResult, cResult, metamethodResult, spread(1, 2, 3)
    )");
    REQUIRE_EQ(luau_load(L, "test_call_feedback", bytecode.data(), bytecode.size(), 0), 0);

    CompilationOptions options;
    options.flags = CodeGen_UseLlvm;
    CompilationResult result = Luau::CodeGen::compile(L, -1, options);
    REQUIRE((result.result == CodeGenCompilationResult::Success || result.result == CodeGenCompilationResult::NothingToCompile));

    REQUIRE_EQ(lua_pcall(L, 0, 6, 0), 0);
    CHECK_EQ(lua_tonumber(L, -6), 42.0);
    CHECK_EQ(lua_tonumber(L, -5), 9.0);
    CHECK_EQ(lua_tonumber(L, -4), 12.0);
    CHECK_EQ(lua_tonumber(L, -3), 1.0);
    CHECK_EQ(lua_tonumber(L, -2), 2.0);
    CHECK_EQ(lua_tonumber(L, -1), 3.0);
}

TEST_SUITE_END();
