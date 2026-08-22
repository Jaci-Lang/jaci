// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/Common.h"
#include "Luau/Repl.h"
#include "lua.h"
#include "lualib.h"
#include "doctest.h"
#include <memory>
#include <string>

TEST_SUITE_BEGIN("SecureFfiTests");

TEST_CASE("FfiSecurityModeAndPolicy")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        assert(ffi.mode() == "permissive")
        ffi.denyLibrary("forbidden_dangerous_lib.so")
        local ok, err = pcall(function()
            ffi.open("forbidden_dangerous_lib.so")
        end)
        assert(not ok)
        assert(string.find(tostring(err), "denied"))

        local safePtr = buffer.create(16)
        assert(ffi.isSafe(safePtr))

        assert(ffi.istype("i32", 123))
        assert(ffi.istype("bool", true))
        assert(ffi.istype("str", "hello"))
        assert(not ffi.istype("bool", 123))
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_SUITE_END();
