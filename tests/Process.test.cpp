// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <memory>
#include <string>

class ProcessFixture
{
public:
    ProcessFixture()
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

TEST_SUITE_BEGIN("ProcessTests");

TEST_CASE_FIXTURE(ProcessFixture, "ProcessSpawnAndEnv")
{
    std::string err = run(R"(
        process.env["JACI_TEST_VAR_PROC"] = "hello_proc"
        assert(process.env["JACI_TEST_VAR_PROC"] == "hello_proc")
        process.env["JACI_TEST_VAR_PROC"] = nil
        assert(process.env["JACI_TEST_VAR_PROC"] == nil)

        local res = process.spawn("echo", {"hello world"})
        assert(res.exitcode == 0)
        assert(string.find(res.stdout, "hello world") ~= nil)
    )");
    CHECK(err == "");
}

TEST_SUITE_END();
