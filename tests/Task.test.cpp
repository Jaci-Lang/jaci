// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/Common.h"
#include "Luau/Repl.h"
#include "lua.h"
#include "lualib.h"
#include "doctest.h"
#include <memory>
#include <string>

TEST_SUITE_BEGIN("TaskTests");

TEST_CASE("TaskSpawnAndStatus")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local executed = false
        local co = task.spawn(function(msg)
            assert(msg == "hello")
            executed = true
        end, "hello")

        assert(executed == true)
        assert(task.status(co) == "dead")
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("TaskPromiseAndAwaitCombinators")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local p1 = task.resolve(100)
        local p2 = task.resolve(200)
        assert(task.status(p1) == "fulfilled")
        assert(p1:status() == "fulfilled")
        assert(p1:value() == 100)

        local all_p = task.all({ p1, p2 })
        assert(all_p:status() == "fulfilled")
        local res = all_p:value()
        assert(type(res) == "table")
        assert(res[1] == 100 and res[2] == 200)

        local race_p = task.race({ p1, p2 })
        assert(race_p:status() == "fulfilled")

        local settled_p = task.allSettled({ p1, p2 })
        assert(settled_p:status() == "fulfilled")
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("TaskChannelsAndTimers")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local ch = task.channel(5)
        assert(ch:capacity() == 5)
        assert(ch:len() == 0)

        ch:send("item1")
        ch:send("item2")
        assert(ch:len() == 2)

        local val1 = ch:try_receive()
        assert(val1 == "item1")

        ch:close()
        assert(ch:is_closed() == true)

        task.desynchronize()
        task.synchronize()
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_SUITE_END();
