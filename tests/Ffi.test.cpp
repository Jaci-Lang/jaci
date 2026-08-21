// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <memory>
#include <string>

class FfiFixture
{
public:
    FfiFixture()
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

TEST_SUITE_BEGIN("FfiTests");

TEST_CASE_FIXTURE(FfiFixture, "FfiBasicAndLibm")
{
    std::string err = run(R"(
        assert(ffi.nullptr == nil or type(ffi.nullptr) == "userdata")
        assert(ffi.sizeof("i32") == 4)
        assert(ffi.sizeof("i64") == 8)
        assert(ffi.sizeof("f64") == 8)

        local b = buffer.create(16)
        local p = ffi.ptr(b)
        assert(type(p) == "userdata")

        local ok, libm = pcall(ffi.open, "libm.so.6")
        if not ok then
            ok, libm = pcall(ffi.open, "libm.so")
        end

        if ok and libm then
            local cos = ffi.sym(libm, "cos", "f64", "f64")
            local result = cos(0.0)
            assert(math.abs(result - 1.0) < 1e-6, "cos(0) should be 1.0")

            local sqrt = ffi.sym(libm, "sqrt", "f64", "f64")
            local res2 = sqrt(16.0)
            assert(math.abs(res2 - 4.0) < 1e-6, "sqrt(16) should be 4.0")
        end
    )");
    CHECK(err == "");
}

TEST_SUITE_END();
