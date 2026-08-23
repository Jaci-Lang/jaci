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

TEST_CASE("GlobalWarnAndWarnHandler")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        assert(type(warn) == "function")
        assert(type(debug.setwarnhandler) == "function")
        assert(type(debug.getwarnhandler) == "function")

        local captured = {}
        debug.setwarnhandler(function(...)
            local args = {...}
            table.insert(captured, args)
        end)

        assert(type(debug.getwarnhandler()) == "function")

        warn("test warning 1", 123, true)
        warn("second warning")

        assert(#captured == 2)
        assert(captured[1][1] == "test warning 1")
        assert(captured[1][2] == 123)
        assert(captured[1][3] == true)
        assert(captured[2][1] == "second warning")

        -- Clear handler
        debug.setwarnhandler(nil)
        assert(debug.getwarnhandler() == nil)
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("DebugInspectAndDump")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        assert(type(debug.inspect) == "function")
        assert(type(debug.dump) == "function")

        -- Test primitives
        assert(debug.inspect(nil) == "nil")
        assert(debug.inspect(true) == "true")
        assert(debug.inspect(false) == "false")
        assert(debug.inspect(123) == "123")
        assert(debug.inspect("hello") == "\"hello\"")
        assert(debug.inspect("line\nbreak") == "\"line\\nbreak\"")

        -- Test table compact
        local t = { a = 1, b = "two", [1] = "first" }
        local compactStr = debug.inspect(t, { compact = true })
        assert(string.find(compactStr, "first") ~= nil)
        assert(string.find(compactStr, "a = 1") ~= nil)

        -- Test nested table & indentation
        local nested = {
            name = "root",
            child = {
                val = 42
            }
        }
        local fullStr = debug.inspect(nested)
        assert(string.find(fullStr, "name = \"root\"") ~= nil)
        assert(string.find(fullStr, "val = 42") ~= nil)

        -- Test cycle detection
        local cyclic = { name = "cycleTest" }
        cyclic.self = cyclic
        local cycStr = debug.inspect(cyclic, { compact = true })
        assert(string.find(cycStr, "<cycle:") ~= nil)

        -- Test dump returns string
        local dumpRes = debug.dump({ x = 10 }, { compact = true })
        assert(dumpRes == "{x = 10}")
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("DebugSetlocalAndSetupvalue")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);
    int prevDebugLevel = getReplDebugLevel();
    setReplDebugLevel(2);

    const char* script = R"(
        -- Test debug.setlocal
        local function testLocal(x)
            local y = 100
            local name = debug.setlocal(1, 2, 999)
            assert(name == "y")
            local n, v = debug.getlocal(1, 2)
            assert(n == "y")
            assert(v == 999)
            return v
        end
        assert(testLocal(1) == 999)

        -- Test debug.setupvalue by index and by name
        local counter = 10
        counter = counter + 1
        local function getCounter()
            return counter
        end

        local name1, val1 = debug.getupvalue(getCounter, 1)
        assert(name1 == "counter")
        assert(val1 == 11)

        local name2, val2 = debug.getupvalue(getCounter, "counter")
        assert(name2 == "counter")
        assert(val2 == 11)

        local sname = debug.setupvalue(getCounter, "counter", 50)
        assert(sname == "counter")
        assert(getCounter() == 50)

        local sname2 = debug.setupvalue(getCounter, 1, 100)
        assert(sname2 == "counter")
        assert(getCounter() == 100)
    )";
    std::string err = runCode(L, script);
    setReplDebugLevel(prevDebugLevel);
    CHECK(err.empty());
}

TEST_CASE("DebugMetatableAndRegistryAndEnv")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        -- Test protected metatable bypass
        local mt = { __metatable = "Protected!", tag = "secret" }
        local obj = setmetatable({}, mt)

        assert(getmetatable(obj) == "Protected!")
        local rawMt = debug.getmetatable(obj)
        assert(type(rawMt) == "table")
        assert(rawMt.tag == "secret")

        local newMt = { hello = "world" }
        debug.setmetatable(obj, newMt)
        assert(debug.getmetatable(obj).hello == "world")

        -- Test registry
        local reg = debug.getregistry()
        assert(type(reg) == "table")

        -- Test getfenv / setfenv
        local env = { x = 777 }
        local function f()
            return x
        end
        debug.setfenv(f, env)
        assert(debug.getfenv(f) == env)
        assert(f() == 777)
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("DebugConstantsAndProtosAndStack")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local function targetFn(a, b)
            local function innerFn()
                return "inner"
            end
            return a + b .. "hello constant" .. 12345
        end

        local k = debug.getconstants(targetFn)
        assert(type(k) == "table")
        local foundMsg = false
        for _, v in ipairs(k) do
            if v == "hello constant" then
                foundMsg = true
            end
        end
        assert(foundMsg)

        local protos = debug.getprotos(targetFn)
        assert(type(protos) == "table")
        assert(#protos >= 1)
        local protoFn = debug.getproto(targetFn, 1)
        assert(type(protoFn) == "function")

        -- Test debug.getstack
        local function stackCheck(p1, p2)
            local st = debug.getstack(1)
            assert(type(st) == "table")
            assert(#st >= 2)
            assert(debug.getstack(1, 1) == p1)
            assert(debug.getstack(1, 2) == p2)
            return true
        end
        assert(stackCheck(10, 20))
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("DebugSetGlobalWarning")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local warnings = {}
        debug.setwarnhandler(function(...)
            local msg = tostring(...)
            table.insert(warnings, msg)
        end)

        debug.setglobalwarning(true)

        -- Access undeclared global should trigger warning
        local _ = nonexistentGlobalVar

        assert(#warnings >= 1)
        assert(string.find(warnings[1], "undeclared global variable 'nonexistentGlobalVar'") ~= nil)

        -- Assignment to undeclared global should trigger warning
        myNewGlobal = 999
        assert(#warnings >= 2)
        assert(string.find(warnings[2], "assignment to undeclared global variable 'myNewGlobal'") ~= nil)
        assert(myNewGlobal == 999)

        -- Disable global warning
        debug.setglobalwarning(false)
        local countBefore = #warnings
        local _ = anotherNonexistent
        assert(#warnings == countBefore)

        debug.setwarnhandler(nil)
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_SUITE_END();
