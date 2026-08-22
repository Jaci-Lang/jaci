// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <memory>
#include <string>

class JniFixture
{
public:
    JniFixture()
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

TEST_SUITE_BEGIN("JniTests");

TEST_CASE_FIXTURE(JniFixture, "JniDetectionAndVersion")
{
    std::string err = run(R"luau(
        assert(type(jni) == "table")
        local path = jni.find_jvm_path()
        assert(type(path) == "string")
        assert(#path > 0)

        local ver = jni.get_version()
        assert(type(ver) == "string")
        assert(jni.is_initialized() == true)
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniStaticMethodsAndFields")
{
    std::string err = run(R"luau(
        local Math = jni.find_class("java.lang.Math")
        assert(Math ~= nil)

        -- Static fields
        assert(type(Math.PI) == "number")
        assert(math.abs(Math.PI - 3.141592653589793) < 1e-10)
        assert(type(Math.E) == "number")

        -- Static methods
        assert(Math.max(10, 20) == 20)
        assert(Math.min(10, 20) == 10)
        assert(Math.sqrt(25) == 5)
        assert(Math.pow(2, 4) == 16)

        -- String.valueOf
        local String = jni.find_class("java.lang.String")
        assert(String.valueOf(9876) == "9876")
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniConstructorsAndInstanceMethods")
{
    std::string err = run(R"luau(
        local StringBuilder = jni.find_class("java.lang.StringBuilder")
        local sb = StringBuilder:new("Hello")
        assert(tostring(sb) == "Hello")

        sb:append(" ")
        sb:append("World")
        sb:append("!")
        assert(sb:toString() == "Hello World!")
        assert(sb:length() == 12)
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniCollectionsAndConversion")
{
    std::string err = run(R"luau(
        -- ArrayList
        local ArrayList = jni.find_class("java.util.ArrayList")
        local list = ArrayList:new()
        list:add("first")
        list:add("second")
        assert(list:size() == 2)
        assert(list:get(0) == "first")
        assert(list:get(1) == "second")

        -- HashMap
        local HashMap = jni.find_class("java.util.HashMap")
        local map = HashMap:new()
        map:put("k1", "v1")
        map:put("k2", "v2")
        assert(map:size() == 2)
        assert(map:get("k1") == "v1")
        assert(map:get("k2") == "v2")

        -- to_java and to_luau
        local origMap = { a = "foo", b = "bar" }
        local jMap = jni.to_java(origMap)
        assert(jni.instanceof(jMap, "java.util.Map"))
        local roundtripMap = jni.to_luau(jMap)
        assert(roundtripMap.a == "foo" and roundtripMap.b == "bar")

        local origList = { "x", "y", "z" }
        local jList = jni.to_java(origList)
        assert(jni.instanceof(jList, "java.util.List"))
        local roundtripList = jni.to_luau(jList)
        assert(roundtripList[1] == "x" and roundtripList[2] == "y" and roundtripList[3] == "z")
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniArrays")
{
    std::string err = run(R"luau(
        -- Int array
        local arr = jni.array("int", {10, 20, 30, 40})
        assert(#arr == 4)
        assert(arr[1] == 10)
        assert(arr[4] == 40)
        arr[2] = 99
        assert(arr[2] == 99)

        local tbl = arr:to_table()
        assert(#tbl == 4)
        assert(tbl[1] == 10 and tbl[2] == 99 and tbl[3] == 30 and tbl[4] == 40)

        -- String array
        local strArr = jni.array("java.lang.String", {"alpha", "beta"})
        assert(#strArr == 2)
        assert(strArr[1] == "alpha")
        assert(strArr[2] == "beta")
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniDirectByteBuffer")
{
    std::string err = run(R"luau(
        local buf = buffer.create(16)
        buffer.writeu8(buf, 0, 42)
        buffer.writeu8(buf, 1, 84)

        local byteBuf = jni.wrap_buffer(buf)
        assert(byteBuf:capacity() == 16)
        assert(byteBuf:get(0) == 42)
        assert(byteBuf:get(1) == 84)
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniDirectSignatureCalls")
{
    std::string err = run(R"luau(
        local resInt = jni.call_static("java.lang.Math", "max", "(II)I", 50, 150)
        assert(resInt == 150)

        local resDouble = jni.call_static("java.lang.Math", "sqrt", "(D)D", 64.0)
        assert(resDouble == 8.0)
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniExceptionHandling")
{
    std::string err = run(R"luau(
        local Integer = jni.find_class("java.lang.Integer")
        local ok, exc = pcall(function()
            Integer.parseInt("not_a_number")
        end)
        assert(ok == false)
        assert(string.find(tostring(exc), "NumberFormatException") ~= nil)
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniTypedValues")
{
    std::string err = run(R"luau(
        local z = jni.jboolean(true)
        assert(tostring(z) == "jboolean(true)")

        local i = jni.jint(42)
        assert(tostring(i) == "jint(42)")

        local j = jni.jlong(10000000000)
        assert(tostring(j) == "jlong(10000000000)")

        local d = jni.jdouble(2.5)
        assert(tostring(d) == "jdouble(2.5)" or tostring(d) == "jdouble(2,5)")
    )luau");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(JniFixture, "JniLocalFrame")
{
    std::string err = run(R"luau(
        local count = 0
        jni.with_local_frame(32, function()
            local String = jni.find_class("java.lang.String")
            for i = 1, 100 do
                local s = String.valueOf(i)
                assert(s == tostring(i))
                count += 1
            end
        end)
        assert(count == 100)
    )luau");
    CHECK(err == "");
}

TEST_SUITE_END();
