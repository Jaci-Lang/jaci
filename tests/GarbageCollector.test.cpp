// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lua.h"
#include "lualib.h"
#include "lstate.h"
#include "lgc.h"
#include "lmem.h"

#include "Luau/Compiler.h"
#include "doctest.h"

#include <memory>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("GarbageCollector");

TEST_CASE("GcPagePoolReuse")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    global_State* g = L->global;
    CHECK(g->pagepool_size_small == 0);
    CHECK(g->pagepool_size_large == 0);

    // Allocate many temporary tables to populate multiple pages
    std::string script = R"(
        for i = 1, 5000 do
            local t = { a = i, b = i * 2, c = "string_" .. tostring(i) }
        end
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_page_pool", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 0, 0);
    CHECK(status == LUA_OK);

    // Trigger full GC sweep: empty pages should be returned to pagepool rather than OS deallocated
    luaC_fullgc(L);

    CHECK(g->pagepool_size_small > 0);

    // Run again: allocation should reuse cached pages from pagepool without OS allocations
    status = luau_load(L, "test_page_pool_reuse", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 0, 0);
    CHECK(status == LUA_OK);

    // After full GC, pool is replenished and state is consistent
    luaC_fullgc(L);
    CHECK(g->pagepool_size_small > 0);
}

TEST_CASE("GcMultiPassPropagationAndBarriers")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    // Test table mutations during incremental GC to exercise write barriers & grayagain propagation
    std::string script = R"(
        local root = {}
        for i = 1, 1000 do
            root[i] = { val = i }
        end

        -- Step GC incrementally
        gcinfo()

        -- Mutate existing black tables to trigger write barriers (luaC_barriert)
        for i = 1, 1000 do
            root[i].next_val = { sub = i * 10 }
        end

        local count = 0
        for i = 1, 1000 do
            if root[i].next_val.sub == i * 10 then
                count = count + 1
            end
        end
        return count
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_barriers", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 1, 0);
    CHECK(status == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 1000);
    lua_pop(L, 1);
}

TEST_CASE("GcVectorizedTableScanning")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    // Verify correct GC marking of large dense numeric arrays and sparse maps
    std::string script = R"(
        local arr = table.create(10000)
        for i = 1, 10000 do
            arr[i] = i * 3.14159
        end

        local map = {}
        for i = 1, 5000 do
            map["key_" .. tostring(i)] = { num = i }
        end

        local sum = 0
        for i = 1, 10000 do
            sum = sum + arr[i]
        end

        local map_count = 0
        for i = 1, 5000 do
            if map["key_" .. tostring(i)].num == i then
                map_count = map_count + 1
            end
        end

        return sum > 0 and map_count == 5000
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_vectorized_scan", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);

    // Trigger full GC before running
    lua_gc(L, LUA_GCCOLLECT, 0);

    status = lua_pcall(L, 0, 1, 0);
    CHECK(status == LUA_OK);
    CHECK(lua_toboolean(L, -1) == 1);
    lua_pop(L, 1);
}

TEST_CASE("GcWeakTablesAndUpvalues")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    std::string script = R"(
        local weak = setmetatable({}, { __mode = "kv" })
        local function populate()
            local key = { name = "tempKey" }
            local val = { name = "tempVal" }
            weak[key] = val
        end
        populate()
        return weak
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_weak", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 1, 0);
    CHECK(status == LUA_OK);

    // Object is out of scope; full GC must clear weak entries
    lua_gc(L, LUA_GCCOLLECT, 0);

    // Verify weak table is empty
    lua_pushnil(L);
    int count = 0;
    while (lua_next(L, -2) != 0)
    {
        count++;
        lua_pop(L, 1);
    }
    CHECK(count == 0);
    lua_pop(L, 1); // pop weak table
}

TEST_SUITE_END();
