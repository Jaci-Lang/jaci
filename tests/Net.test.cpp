// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <memory>
#include <string>

class NetFixture
{
public:
    NetFixture()
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

TEST_SUITE_BEGIN("NetTests");

TEST_CASE_FIXTURE(NetFixture, "NetUrlHelpers")
{
    std::string err = run(R"(
        local parsed = net.parseurl("http://example.com:8080/api/v1?q=test")
        assert(parsed ~= nil)
        assert(parsed.scheme == "http")
        assert(parsed.host == "example.com")
        assert(parsed.port == 8080)
        assert(parsed.path == "/api/v1?q=test")

        local encoded = net.urlencode("hello world & foo=bar")
        assert(encoded == "hello+world+%26+foo%3Dbar")
        assert(net.urldecode(encoded) == "hello world & foo=bar")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(NetFixture, "NetTcpLoopback")
{
    std::string err = run(R"(
        local listener = net.listen("127.0.0.1", 0)
        assert(listener ~= nil)

        listener:close()
    )");
    CHECK(err == "");
}

TEST_SUITE_END();
