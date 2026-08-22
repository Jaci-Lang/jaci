// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/Common.h"
#include "Luau/Repl.h"
#include "lua.h"
#include "lualib.h"
#include "doctest.h"
#include <memory>
#include <string>

TEST_SUITE_BEGIN("EnhancedDebugTests");

TEST_CASE("DebugInspectionFunctions")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local outerVar = 42

        local function myFunc(a, b)
            local localVar = a + b
            local stack = debug.dumpstack()
            assert(type(stack) == "table")
            assert(#stack >= 1)
            assert(stack[1].level == 1)

            local locals = debug.getlocals(1)
            assert(type(locals) == "table")

            return localVar + outerVar
        end

        local upvals = debug.getupvalues(myFunc)
        assert(type(upvals) == "table")

        local res = myFunc(10, 20)
        assert(res == 72)
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_SUITE_END();
