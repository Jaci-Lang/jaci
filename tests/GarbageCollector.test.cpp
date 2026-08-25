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

TEST_CASE("GcIncrementalPropagationAndBarriers")
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

TEST_CASE("GcTableScanning")
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

TEST_CASE("GcTableScanningMarksArrayPositions")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    std::string script = R"(
        local root = table.create(16, 0)
        for i = 1, 16 do
            root[i] = i
        end

        root[1] = { lane = 0 }
        root[6] = { lane = 1 }
        root[11] = { lane = 2 }
        root[16] = { lane = 3 }

        return root
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_vectorized_scan_lanes", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 1, 0);
    CHECK(status == LUA_OK);

    lua_gc(L, LUA_GCCOLLECT, 0);

    const int positions[] = {1, 6, 11, 16};
    for (int lane = 0; lane < 4; ++lane)
    {
        lua_rawgeti(L, -1, positions[lane]);
        CHECK(lua_istable(L, -1));
        lua_getfield(L, -1, "lane");
        CHECK(lua_tointeger(L, -1) == lane);
        lua_pop(L, 2);
    }

    lua_pop(L, 1);
}

TEST_CASE("GcTableScanningPreservesLeafObjects")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    std::string script = R"(
        local bytes = buffer.create(8)
        buffer.writeu8(bytes, 0, 0x78)
        return { "leaf-string", bytes, vector.create(1, 2, 3) }
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_leaf_marking", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 1, 0);
    CHECK(status == LUA_OK);

    lua_gc(L, LUA_GCCOLLECT, 0);

    lua_rawgeti(L, -1, 1);
    CHECK(std::string(lua_tostring(L, -1)) == "leaf-string");
    lua_pop(L, 1);

    lua_rawgeti(L, -1, 2);
    CHECK(lua_isbuffer(L, -1));
    size_t length = 0;
    unsigned char* bytes = static_cast<unsigned char*>(lua_tobuffer(L, -1, &length));
    CHECK(length == 8);
    CHECK(bytes[0] == 0x78);
    lua_pop(L, 1);

    lua_rawgeti(L, -1, 3);
    CHECK(lua_isvector(L, -1));
    const LUA_VECTOR_TYPE* vector = lua_tovector(L, -1);
    CHECK(vector[0] == LUA_VECTOR_TYPE(1));
    CHECK(vector[1] == LUA_VECTOR_TYPE(2));
    CHECK(vector[2] == LUA_VECTOR_TYPE(3));
    lua_pop(L, 2);
}

TEST_CASE("GcStrongHashTableFastPath")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    std::string script = R"(
        local plain = {}
        local metatable = {}
        local decorated = setmetatable({}, metatable)

        for i = 1, 4096 do
            plain["plain_" .. i] = { value = i }
            decorated["decorated_" .. i] = { value = -i }
        end

        return plain, decorated, metatable
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_strong_hash_fast_path", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 3, 0);
    CHECK(status == LUA_OK);

    lua_gc(L, LUA_GCCOLLECT, 0);

    CHECK(lua_getmetatable(L, -2) == 1);
    CHECK(lua_rawequal(L, -1, -2) == 1);
    lua_pop(L, 1);

    lua_getfield(L, -3, "plain_4096");
    lua_getfield(L, -1, "value");
    CHECK(lua_tointeger(L, -1) == 4096);
    lua_pop(L, 2);

    lua_getfield(L, -2, "decorated_4096");
    lua_getfield(L, -1, "value");
    CHECK(lua_tointeger(L, -1) == -4096);
    lua_pop(L, 5);
}

TEST_CASE("GcIncrementalStrongTableScanMutation")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    std::string script = R"(
        local root = table.create(32768)
        for i = 1, 32768 do
            root[i] = { value = i }
        end
        return root
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_incremental_table_scan_mutation", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 1, 0);
    CHECK(status == LUA_OK);

    lua_gc(L, LUA_GCCOLLECT, 0);

    for (int step = 0; step < 100000 && !L->global->gcscantable; ++step)
        lua_gc(L, LUA_GCSTEP, 1);

    REQUIRE(L->global->gcscantable != nullptr);
    luaC_validate(L);

    lua_newtable(L);
    lua_pushinteger(L, 111);
    lua_setfield(L, -2, "sentinel");
    lua_rawseti(L, -2, 1);

    lua_newtable(L);
    lua_pushinteger(L, 222);
    lua_setfield(L, -2, "sentinel");
    lua_rawseti(L, -2, 65536); // grow and rehash during a partial scan

    lua_newtable(L);
    CHECK(lua_setmetatable(L, -2) == 1); // restart the continuation with the new metatable
    luaC_validate(L);

    while (!lua_gc(L, LUA_GCSTEP, 1))
    {
    }

    lua_gc(L, LUA_GCCOLLECT, 0);

    lua_rawgeti(L, -1, 1);
    lua_getfield(L, -1, "sentinel");
    CHECK(lua_tointeger(L, -1) == 111);
    lua_pop(L, 2);

    lua_rawgeti(L, -1, 65536);
    lua_getfield(L, -1, "sentinel");
    CHECK(lua_tointeger(L, -1) == 222);
    lua_pop(L, 2);

    for (int step = 0; step < 100000 && !L->global->gcscantable; ++step)
        lua_gc(L, LUA_GCSTEP, 1);

    REQUIRE(L->global->gcscantable != nullptr);
    lua_gc(L, LUA_GCCOLLECT, 0); // cancel a partial scan and restart a complete cycle
    CHECK(L->global->gcscantable == nullptr);
    CHECK(L->global->gcstate == GCSpause);

    lua_rawgeti(L, -1, 65536);
    lua_getfield(L, -1, "sentinel");
    CHECK(lua_tointeger(L, -1) == 222);
    lua_pop(L, 3);
}

TEST_CASE("GcIncrementalTableScanWeakModeChange")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    std::string script = R"(
        local root = {}
        for i = 1, 16384 do
            root["key_" .. i] = { value = i }
        end
        return root
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_incremental_weak_mode_change", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 1, 0);
    CHECK(status == LUA_OK);

    lua_gc(L, LUA_GCCOLLECT, 0);
    for (int step = 0; step < 100000 && !L->global->gcscantable; ++step)
        lua_gc(L, LUA_GCSTEP, 1);

    REQUIRE(L->global->gcscantable != nullptr);
    luaC_validate(L);

    lua_newtable(L);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    CHECK(lua_setmetatable(L, -2) == 1);
    luaC_validate(L);

    while (!lua_gc(L, LUA_GCSTEP, 1))
    {
    }

    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);

    lua_pushnil(L);
    CHECK(lua_next(L, -2) == 0);
    CHECK(L->global->gcscantable == nullptr);
    CHECK(L->global->gcstate == GCSpause);
    luaC_validate(L);
    lua_pop(L, 1);
}

TEST_CASE("GcMixedWorkloadStability")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    std::string script = R"(
        local roots = {}
        local weakMeta = { __mode = "k" }
        local weak = setmetatable({}, weakMeta)
        local modes = { "k", "v", "kv" }
        local checksum = 0

        for cycle = 1, 80 do
            weakMeta.__mode = modes[(cycle - 1) % #modes + 1]

            for i = 1, 96 do
                local key = { cycle, i }
                local bytes = buffer.create(16)
                buffer.writeu32(bytes, 0, cycle * 1000 + i)

                local suspended = coroutine.create(function(seed)
                    local captured = { seed = seed }
                    coroutine.yield(captured)
                    return captured.seed * 2
                end)
                local ok, captured = coroutine.resume(suspended, cycle + i)
                assert(ok and captured.seed == cycle + i)

                local value = {
                    cycle = cycle,
                    index = i,
                    bytes = bytes,
                    direction = vector.create(cycle, i, cycle + i),
                    suspended = suspended,
                    closure = function(offset)
                        return cycle + i + offset
                    end,
                }

                roots[i] = value
                weak[key] = value
                checksum += value.closure(1)
            end

            for i = 1, 96, 3 do
                roots[i] = nil
            end

            if cycle % 4 == 0 then
                table.clear(roots)
            end
        end

        assert(checksum == 691200)
        return roots, weak
    )";

    std::string bytecode = Luau::compile(script);
    int status = luau_load(L, "test_mixed_gc_stability", bytecode.data(), bytecode.size(), 0);
    CHECK(status == LUA_OK);
    status = lua_pcall(L, 0, 2, 0);
    CHECK(status == LUA_OK);

    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);

    CHECK(lua_istable(L, -2));
    CHECK(lua_istable(L, -1));
    CHECK(L->global->gcscantable == nullptr);
    CHECK(L->global->gcstate == GCSpause);
    luaC_validate(L);
    lua_pop(L, 2);
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
