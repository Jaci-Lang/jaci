// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <memory>
#include <string>

class TaskFixture
{
public:
    TaskFixture()
        : state(luaL_newstate(), lua_close)
    {
        L = state.get();
        luaL_openlibs(L);
    }

    std::string run(const std::string& code)
    {
        std::string bytecode = Luau::compile(code);
        lua_State* th = lua_newthread(L);
        if (luau_load(th, "=test", bytecode.data(), bytecode.size(), 0) != 0)
        {
            std::string err = lua_tostring(th, -1);
            lua_pop(L, 1);
            return err;
        }

        int status = lua_resume(th, NULL, 0);
        if (status == 0 || status == LUA_YIELD)
        {
            luaL_runtasks(L);
            int curStatus = lua_status(th);
            if (status == LUA_YIELD && (curStatus == LUA_OK || curStatus == LUA_YIELD))
                status = 0;
            else if (curStatus != LUA_OK && curStatus != LUA_YIELD)
                status = curStatus;
        }

        if (status != 0 && status != LUA_YIELD)
        {
            std::string err = lua_tostring(th, -1);
            lua_pop(L, 1);
            return err;
        }

        lua_pop(L, 1);
        return "";
    }

    lua_State* L;

private:
    std::unique_ptr<lua_State, void (*)(lua_State*)> state;
};

TEST_SUITE_BEGIN("TaskTests");

TEST_CASE_FIXTURE(TaskFixture, "TaskSpawnImmediate")
{
    std::string err = run(R"(
        local executed = false
        local passedArg = ""
        task.spawn(function(msg)
            executed = true
            passedArg = msg
        end, "hello")

        assert(executed, "task.spawn must execute immediately")
        assert(passedArg == "hello", "task.spawn must forward arguments")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "TaskDeferExecution")
{
    std::string err = run(R"(
        local order = {}
        table.insert(order, 1)
        task.defer(function()
            table.insert(order, 3)
        end)
        table.insert(order, 2)
        task.poll()
        assert(#order == 3, "all items must be executed")
        assert(order[1] == 1 and order[2] == 2 and order[3] == 3, "deferred task must run after current turn")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "TaskDelayAndCancel")
{
    std::string err = run(R"(
        local executed = false
        local t = task.delay(0.01, function()
            executed = true
        end)
        task.cancel(t)
        task.wait(0.03)
        assert(not executed, "cancelled task must not execute")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "TaskWaitDelta")
{
    std::string err = run(R"(
        local dt = task.wait(0.02)
        assert(type(dt) == "number", "task.wait must return elapsed seconds")
        assert(dt >= 0.01, "elapsed time must be at least requested interval")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "TaskYieldCooperative")
{
    std::string err = run(R"(
        local order = {}
        task.spawn(function()
            table.insert(order, "A1")
            task.yield()
            table.insert(order, "A2")
        end)
        task.spawn(function()
            table.insert(order, "B1")
            task.yield()
            table.insert(order, "B2")
        end)
        task.run()
        assert(#order == 4, "all cooperative points must execute")
        assert(order[1] == "A1" and order[2] == "B1" and order[3] == "A2" and order[4] == "B2", "task.yield must interleave coroutines")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "TaskNowClock")
{
    std::string err = run(R"(
        local t1 = task.now()
        local t2 = task.clock()
        assert(type(t1) == "number" and type(t2) == "number", "timestamps must be numbers")
        assert(t1 > 0 and t2 >= t1, "timestamps must be monotonically non-decreasing")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "NativeAwaitKeywordWithPromise")
{
    std::string err = run(R"(
        local p = task.resolve("jaci async power")
        local result = await p
        assert(result == "jaci async power", "native await token must unpack resolved promise value")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "NativeAwaitInTaskPromise")
{
    std::string err = run(R"(
        local p = task.promise(function(resolve, reject)
            task.delay(0.01, function()
                resolve(42, "second")
            end)
        end)

        local val1, val2 = await p
        assert(val1 == 42, "await must resolve async delayed promise")
        assert(val2 == "second", "await must unpack multiple return values")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "TaskAsyncWrapper")
{
    std::string err = run(R"(
        local fetchData = task.async(function(id)
            task.wait(0.01)
            return "data_" .. tostring(id)
        end)

        local p = fetchData(123)
        assert(p:status() == "pending" or p:status() == "fulfilled", "async wrapper must return promise")
        local res = await p
        assert(res == "data_123", "awaited async function must return computation result")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "PromiseCombinatorsAll")
{
    std::string err = run(R"(
        local p1 = task.resolve(10)
        local p2 = task.resolve(20)
        local p3 = task.resolve(30)

        local combined = task.all({p1, p2, p3})
        local res = await combined
        assert(type(res) == "table", "task.all must return table of results")
        assert(res[1] == 10 and res[2] == 20 and res[3] == 30, "task.all must preserve result order")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "PromiseCombinatorsRaceAndAny")
{
    std::string err = run(R"(
        local p1 = task.promise(function(resolve)
            task.delay(0.05, function() resolve("slow") end)
        end)
        local p2 = task.resolve("fast")

        local winner = await task.race({p1, p2})
        assert(winner == "fast", "task.race must resolve with fastest promise")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "PromiseCombinatorsAllSettled")
{
    std::string err = run(R"(
        local p1 = task.resolve("ok_val")
        local p2 = task.reject("err_reason")

        local results = await task.allSettled({p1, p2})
        assert(#results == 2, "allSettled must have two entries")
        assert(results[1].status == "fulfilled" and results[1].value == "ok_val", "first promise must be fulfilled")
        assert(results[2].status == "rejected" and results[2].reason == "err_reason", "second promise must be rejected")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "ChannelBuffered")
{
    std::string err = run(R"(
        local ch = task.channel(3)
        assert(ch:cap() == 3, "channel capacity must match")
        assert(ch:len() == 0, "initial channel length must be 0")

        ch:send("msg1")
        ch:send("msg2")
        assert(ch:len() == 2, "buffered channel length must be 2")

        local val1, ok1 = ch:recv()
        local val2, ok2 = ch:recv()
        assert(val1 == "msg1" and ok1 == true, "first received message must match")
        assert(val2 == "msg2" and ok2 == true, "second received message must match")
        assert(ch:len() == 0, "channel must be empty after receives")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "ChannelRendezvousPingPong")
{
    std::string err = run(R"(
        local ch = task.channel() -- unbuffered capacity = 0
        local received = {}

        task.spawn(function()
            for i = 1, 5 do
                ch:send(i * 10)
            end
            ch:close()
        end)

        task.spawn(function()
            while true do
                local val, ok = await ch
                if not ok then break end
                table.insert(received, val)
            end
        end)

        task.run()

        assert(#received == 5, "must receive all 5 rendezvous messages")
        assert(received[1] == 10 and received[5] == 50, "rendezvous messages must arrive in order")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "TaskTimerObject")
{
    std::string err = run(R"(
        local ticks = 0
        local tm = task.timer(0.01, function()
            ticks += 1
        end, true)

        assert(tm:is_active(), "timer must be active")
        task.wait(0.035)
        tm:stop()
        assert(not tm:is_active(), "timer must be inactive after stop")
        assert(ticks >= 2, "repeating timer must tick multiple times")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(TaskFixture, "StandaloneAwaitStatement")
{
    std::string err = run(R"(
        local step = 0
        task.spawn(function()
            step = 1
            await task.wait(0.01)
            step = 2
        end)
        task.run()
        assert(step == 2, "standalone await statement must complete")
    )");
    CHECK(err == "");
}

TEST_SUITE_END();
