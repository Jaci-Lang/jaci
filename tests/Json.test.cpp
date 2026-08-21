// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <memory>
#include <string>

class JsonFixture
{
public:
    JsonFixture()
        : state(luaL_newstate(), lua_close)
    {
        L = state.get();
        luaL_openlibs(L);
    }

    std::string run(const std::string& code)
    {
        std::string bytecode = Luau::compile(code);
        if (luau_load(L, "=test", bytecode.data(), bytecode.size(), 0) != 0)
        {
            std::string err = lua_tostring(L, -1);
            lua_pop(L, 1);
            return err;
        }

        int status = lua_pcall(L, 0, 0, 0);
        if (status != 0)
        {
            std::string err = lua_tostring(L, -1);
            lua_pop(L, 1);
            return err;
        }

        return "";
    }

    lua_State* L;

private:
    std::unique_ptr<lua_State, void (*)(lua_State*)> state;
};

TEST_SUITE_BEGIN("JsonTests");

TEST_CASE_FIXTURE(JsonFixture, "JsonEncodeBasic")
{
    std::string err = run(R"(
        assert(json.encode(123) == "123")
        assert(json.encode("hello") == [["hello"]])
        assert(json.encode(true) == "true")
        assert(json.encode(false) == "false")
        assert(json.encode(nil) == "null")
        assert(json.encode(json.null) == "null")

        local arr = {1, 2, 3}
        assert(json.encode(arr) == "[1,2,3]")

        local obj = {a = 1}
        local encoded = json.encode(obj)
        assert(encoded == [[{"a":1}]])

        -- Pretty printing
        local p = json.pretty({1, 2})
        assert(string.find(p, "\n") ~= nil)

        -- Validation
        assert(json.valid("[1, 2, 3]") == true)
        assert(json.valid("{invalid") == false)

        -- Explicit array/object
        local empty_arr = json.array()
        assert(json.encode(empty_arr) == "[]")
        local empty_obj = json.object()
        assert(json.encode(empty_obj) == "{}")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JsonFixture, "JsonDecodeBasic")
{
    std::string err = run(R"(
        local val = json.decode("[1, 2, 3]")
        assert(type(val) == "table")
        assert(#val == 3)
        assert(val[1] == 1 and val[2] == 2 and val[3] == 3)

        local obj = json.decode([[{"name":"jaci","version":1}]])
        assert(type(obj) == "table")
        assert(obj.name == "jaci")
        assert(obj.version == 1)

        local n = json.decode("null")
        assert(n == json.null)

        local invalid, errmsg = json.decode("{invalid json")
        assert(invalid == nil)
        assert(type(errmsg) == "string")
    )");
    CHECK(err == "");
}

TEST_SUITE_END();
