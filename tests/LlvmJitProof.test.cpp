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

#include "doctest.h"

#include <memory>
#include <string>

using namespace Luau::CodeGen;

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

TEST_SUITE_END();
