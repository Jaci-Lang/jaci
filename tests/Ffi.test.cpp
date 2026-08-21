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

TEST_CASE_FIXTURE(FfiFixture, "FfiMemoryAndTypes")
{
    std::string err = run(R"(
        assert(ffi.nullptr == nil or type(ffi.nullptr) == "userdata")
        assert(ffi.sizeof("i32") == 4)
        assert(ffi.sizeof("i64") == 8)
        assert(ffi.sizeof("double") == 8)
        assert(ffi.sizeof("void*") == 8)

        local b = ffi.new("i32", 4)
        assert(type(b) == "buffer")
        assert(buffer.len(b) == 16)

        ffi.write(b, 0, "i32", 42)
        ffi.write(b, 4, "i32", 100)
        assert(ffi.read(b, 0, "i32") == 42)
        assert(ffi.read(b, 4, "i32") == 100)

        local b2 = buffer.create(16)
        ffi.fill(b2, 16, 0)
        ffi.copy(b2, b, 8)
        assert(ffi.read(b2, 0, "i32") == 42)
        assert(ffi.read(b2, 4, "i32") == 100)
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FfiFixture, "FfiStructLayout")
{
    std::string err = run(R"(
        local Point = ffi.struct({
            {"x", "i32"},
            {"y", "i32"},
            {"weight", "double"}
        })

        assert(Point.size == 16)
        assert(Point.align == 8)
        assert(Point.fields.x.offset == 0)
        assert(Point.fields.y.offset == 4)
        assert(Point.fields.weight.offset == 8)

        local buf = buffer.create(Point.size)
        ffi.write(buf, Point.fields.x.offset, "i32", 10)
        ffi.write(buf, Point.fields.y.offset, "i32", 20)
        ffi.write(buf, Point.fields.weight.offset, "double", 3.14)

        assert(ffi.read(buf, Point.fields.x.offset, "i32") == 10)
        assert(ffi.read(buf, Point.fields.y.offset, "i32") == 20)
        assert(math.abs(ffi.read(buf, Point.fields.weight.offset, "double") - 3.14) < 1e-6)
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FfiFixture, "FfiCDefAndCalls")
{
    std::string err = run(R"(
        ffi.cdef[[
            int abs(int n);
            double cos(double x);
            double sqrt(double x);
            int puts(const char* s);
            size_t strlen(const char* s);
        ]]

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

        local c_abs = ffi.C.abs
        if c_abs then
            assert(c_abs(-42) == 42)
            assert(c_abs(100) == 100)
        end

        local c_strlen = ffi.C.strlen
        if c_strlen then
            assert(c_strlen("hello jaci") == 10)
        end
    )");
    CHECK(err == "");
}

TEST_SUITE_END();
