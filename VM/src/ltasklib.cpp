// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lreactor.h"

#include "lualib.h"
#include "lcommon.h"
#include "lstate.h"
#include "ldebug.h"
#include "lvm.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>

#define PROMISE_MT "Promise*"
#define CHANNEL_MT "Channel*"
#define TIMER_MT "Timer*"

using namespace Jaci;

// ============================================================================
// Helpers
// ============================================================================

static PromiseData* check_promise(lua_State* L, int idx)
{
    return static_cast<PromiseData*>(luaL_checkudata(L, idx, PROMISE_MT));
}

static ChannelData* check_channel(lua_State* L, int idx)
{
    return static_cast<ChannelData*>(luaL_checkudata(L, idx, CHANNEL_MT));
}

static TimerHandleData* check_timer(lua_State* L, int idx)
{
    return static_cast<TimerHandleData*>(luaL_checkudata(L, idx, TIMER_MT));
}

static jaci_socket_t get_socket_or_fd(lua_State* L, int idx)
{
    if (lua_isnumber(L, idx))
    {
        return static_cast<jaci_socket_t>(lua_tointeger(L, idx));
    }
    else if (lua_isuserdata(L, idx))
    {
        // Could be Socket* from net library
        void* ud = lua_touserdata(L, idx);
        if (ud)
        {
            return *static_cast<jaci_socket_t*>(ud);
        }
    }
    luaL_typeerror(L, idx, "integer socket/file descriptor or Socket");
    return JACI_INVALID_SOCKET;
}

// ============================================================================
// Promise Userdata & Methods
// ============================================================================

static void promise_dtor(void* p)
{
    static_cast<PromiseData*>(p)->~PromiseData();
}

static PromiseData* new_promise(lua_State* L)
{
    void* mem = lua_newuserdatadtor(L, sizeof(PromiseData), promise_dtor);
    PromiseData* p = new (mem) PromiseData();
    p->state = PromiseState::Pending;
    p->results_ref = LUA_NOREF;
    p->nresults = 0;

    luaL_getmetatable(L, PROMISE_MT);
    lua_setmetatable(L, -2);
    return p;
}

static int promise_resolve_helper(lua_State* L)
{
    // Upvalue 1: PromiseData pointer
    PromiseData* p = static_cast<PromiseData*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!p || p->state != PromiseState::Pending)
        return 0;

    int nargs = lua_gettop(L);
    int results_ref = LUA_NOREF;

    if (nargs > 0)
    {
        lua_createtable(L, nargs, 0);
        for (int i = 1; i <= nargs; i++)
        {
            lua_pushvalue(L, i);
            lua_rawseti(L, -2, i);
        }
        results_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    Reactor* r = Reactor::get(L);
    r->settle_promise(L, p, PromiseState::Fulfilled, nargs, results_ref);
    return 0;
}

static int promise_reject_helper(lua_State* L)
{
    // Upvalue 1: PromiseData pointer
    PromiseData* p = static_cast<PromiseData*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!p || p->state != PromiseState::Pending)
        return 0;

    int results_ref = LUA_NOREF;
    if (lua_gettop(L) >= 1)
    {
        lua_pushvalue(L, 1);
        results_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    Reactor* r = Reactor::get(L);
    r->settle_promise(L, p, PromiseState::Rejected, 1, results_ref);
    return 0;
}

static int promise_status(lua_State* L)
{
    PromiseData* p = check_promise(L, 1);
    switch (p->state)
    {
    case PromiseState::Pending:
        lua_pushliteral(L, "pending");
        break;
    case PromiseState::Fulfilled:
        lua_pushliteral(L, "fulfilled");
        break;
    case PromiseState::Rejected:
        lua_pushliteral(L, "rejected");
        break;
    }
    return 1;
}

static int promise_value(lua_State* L)
{
    PromiseData* p = check_promise(L, 1);
    if (p->state == PromiseState::Fulfilled && p->results_ref != LUA_NOREF && p->nresults > 0)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, p->results_ref);
        for (int i = 1; i <= p->nresults; i++)
        {
            lua_rawgeti(L, -i, i);
        }
        lua_remove(L, -1 - p->nresults); // remove results table
        return p->nresults;
    }
    lua_pushnil(L);
    return 1;
}

static int promise_reason(lua_State* L)
{
    PromiseData* p = check_promise(L, 1);
    if (p->state == PromiseState::Rejected && p->results_ref != LUA_NOREF)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, p->results_ref);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int promise_and_then(lua_State* L)
{
    PromiseData* p = check_promise(L, 1);
    int on_fulfilled_ref = LUA_NOREF;
    int on_rejected_ref = LUA_NOREF;

    if (!lua_isnoneornil(L, 2))
    {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        on_fulfilled_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    if (!lua_isnoneornil(L, 3))
    {
        luaL_checktype(L, 3, LUA_TFUNCTION);
        lua_pushvalue(L, 3);
        on_rejected_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    // Create next chained promise
    new_promise(L);
    lua_pushvalue(L, -1);
    int next_promise_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    Reactor* r = Reactor::get(L);
    r->attach_promise_continuation(L, p, on_fulfilled_ref, on_rejected_ref, next_promise_ref);

    // Return the new chained promise on top of stack
    return 1;
}

static int promise_catch(lua_State* L)
{
    // promise:catch(onRejected) -> promise:andThen(nil, onRejected)
    lua_pushvalue(L, 1); // self
    lua_pushnil(L);      // onFulfilled = nil
    lua_pushvalue(L, 2); // onRejected
    return promise_and_then(L);
}

static int promise_finally(lua_State* L)
{
    luaL_checktype(L, 2, LUA_TFUNCTION);
    // Simple chaining via andThen
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 2);
    return promise_and_then(L);
}

static int promise_await(lua_State* L)
{
    PromiseData* p = check_promise(L, 1);

    if (p->state == PromiseState::Fulfilled)
    {
        if (p->results_ref != LUA_NOREF && p->nresults > 0)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, p->results_ref);
            int tbl_idx = lua_gettop(L);
            for (int i = 1; i <= p->nresults; i++)
            {
                lua_rawgeti(L, tbl_idx, i);
            }
            lua_remove(L, tbl_idx);
            return p->nresults;
        }
        return 0;
    }
    else if (p->state == PromiseState::Rejected)
    {
        if (p->results_ref != LUA_NOREF)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, p->results_ref);
            if (lua_istable(L, -1))
            {
                lua_rawgeti(L, -1, 1);
                lua_remove(L, -2);
            }
        }
        else
        {
            lua_pushliteral(L, "promise rejected");
        }
        lua_error(L);
        return 0;
    }
    else
    {
        // Pending: yield caller coroutine
        Reactor* r = Reactor::get(L);
        r->await_promise(L, p, L);
        return lua_yield(L, 0);
    }
}

static int promise_tostring(lua_State* L)
{
    PromiseData* p = check_promise(L, 1);
    const char* status_str = "pending";
    if (p->state == PromiseState::Fulfilled) status_str = "fulfilled";
    else if (p->state == PromiseState::Rejected) status_str = "rejected";

    lua_pushfstring(L, "Promise(%s)", status_str);
    return 1;
}

static const luaL_Reg promise_methods[] = {
    {"andThen", promise_and_then},
    {"catch", promise_catch},
    {"finally", promise_finally},
    {"await", promise_await},
    {"status", promise_status},
    {"value", promise_value},
    {"reason", promise_reason},
    {NULL, NULL}
};

// ============================================================================
// Channel Userdata & Methods
// ============================================================================

static void channel_dtor(void* p)
{
    static_cast<ChannelData*>(p)->~ChannelData();
}

static ChannelData* new_channel(lua_State* L, size_t capacity)
{
    void* mem = lua_newuserdatadtor(L, sizeof(ChannelData), channel_dtor);
    ChannelData* c = new (mem) ChannelData();
    c->capacity = capacity;
    c->closed = false;

    luaL_getmetatable(L, CHANNEL_MT);
    lua_setmetatable(L, -2);
    return c;
}

static int channel_send(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);
    if (c->closed)
    {
        luaL_error(L, "cannot send on closed channel");
    }

    luaL_checkany(L, 2);

    // If a receiver is waiting, hand off immediately
    if (!c->recv_waiters.empty())
    {
        ChannelWaiter receiver = c->recv_waiters.front();
        c->recv_waiters.pop_front();

        lua_rawgeti(L, LUA_REGISTRYINDEX, receiver.thread_ref);
        lua_State* rec_co = lua_tothread(L, -1);
        lua_pop(L, 1);

        if (rec_co && lua_costatus(L, rec_co) == LUA_COSUS)
        {
            lua_pushvalue(L, 2);
            lua_xmove(L, rec_co, 1); // push value to receiver
            lua_pushboolean(rec_co, true); // ok = true

            Reactor* r = Reactor::get(L);
            r->defer_thread(L, rec_co, 2);
        }

        lua_unref(L, receiver.thread_ref);
        lua_pushboolean(L, true);
        return 1;
    }

    // If buffer has space, enqueue
    if (c->buffer_refs.size() < c->capacity)
    {
        lua_pushvalue(L, 2);
        int val_ref = lua_ref(L, -1);
        lua_pop(L, 1);
        c->buffer_refs.push_back(val_ref);

        lua_pushboolean(L, true);
        return 1;
    }

    // Buffer full or unbuffered: suspend sender coroutine
    lua_pushvalue(L, 2);
    int val_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_pushthread(L);
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    ChannelWaiter waiter;
    waiter.thread_ref = thread_ref;
    waiter.value_ref = val_ref;
    c->send_waiters.push_back(waiter);

    return lua_yield(L, 0);
}

static int channel_recv(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);

    // If buffer has values, pop one
    if (!c->buffer_refs.empty())
    {
        int val_ref = c->buffer_refs.front();
        c->buffer_refs.pop_front();

        lua_rawgeti(L, LUA_REGISTRYINDEX, val_ref);
        lua_unref(L, val_ref);
        lua_pushboolean(L, true); // ok = true

        // If senders are waiting, move sender's value into buffer and wake sender
        if (!c->send_waiters.empty())
        {
            ChannelWaiter sender = c->send_waiters.front();
            c->send_waiters.pop_front();

            if (sender.value_ref != LUA_NOREF)
            {
                c->buffer_refs.push_back(sender.value_ref);
            }

            lua_rawgeti(L, LUA_REGISTRYINDEX, sender.thread_ref);
            lua_State* snd_co = lua_tothread(L, -1);
            lua_pop(L, 1);

            if (snd_co && lua_costatus(L, snd_co) == LUA_COSUS)
            {
                lua_pushboolean(snd_co, true);
                Reactor* r = Reactor::get(L);
                r->defer_thread(L, snd_co, 1);
            }
            lua_unref(L, sender.thread_ref);
        }

        return 2;
    }

    // If unbuffered and a sender is waiting (rendezvous)
    if (!c->send_waiters.empty())
    {
        ChannelWaiter sender = c->send_waiters.front();
        c->send_waiters.pop_front();

        lua_rawgeti(L, LUA_REGISTRYINDEX, sender.value_ref);
        lua_unref(L, sender.value_ref);
        lua_pushboolean(L, true); // ok = true

        lua_rawgeti(L, LUA_REGISTRYINDEX, sender.thread_ref);
        lua_State* snd_co = lua_tothread(L, -1);
        lua_pop(L, 1);

        if (snd_co && lua_costatus(L, snd_co) == LUA_COSUS)
        {
            lua_pushboolean(snd_co, true);
            Reactor* r = Reactor::get(L);
            r->defer_thread(L, snd_co, 1);
        }
        lua_unref(L, sender.thread_ref);

        return 2;
    }

    // If closed and empty
    if (c->closed)
    {
        lua_pushnil(L);
        lua_pushboolean(L, false);
        return 2;
    }

    // Channel is empty and open: suspend receiver coroutine
    lua_pushthread(L);
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    ChannelWaiter waiter;
    waiter.thread_ref = thread_ref;
    waiter.value_ref = LUA_NOREF;
    c->recv_waiters.push_back(waiter);

    return lua_yield(L, 0);
}

static int channel_try_send(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);
    if (c->closed)
    {
        lua_pushboolean(L, false);
        return 1;
    }

    luaL_checkany(L, 2);

    if (!c->recv_waiters.empty())
    {
        ChannelWaiter receiver = c->recv_waiters.front();
        c->recv_waiters.pop_front();

        lua_rawgeti(L, LUA_REGISTRYINDEX, receiver.thread_ref);
        lua_State* rec_co = lua_tothread(L, -1);
        lua_pop(L, 1);

        if (rec_co && lua_costatus(L, rec_co) == LUA_COSUS)
        {
            lua_pushvalue(L, 2);
            lua_xmove(L, rec_co, 1);
            lua_pushboolean(rec_co, true);
            Reactor* r = Reactor::get(L);
            r->defer_thread(L, rec_co, 2);
        }
        lua_unref(L, receiver.thread_ref);
        lua_pushboolean(L, true);
        return 1;
    }

    if (c->buffer_refs.size() < c->capacity)
    {
        lua_pushvalue(L, 2);
        int val_ref = lua_ref(L, -1);
        lua_pop(L, 1);
        c->buffer_refs.push_back(val_ref);
        lua_pushboolean(L, true);
        return 1;
    }

    lua_pushboolean(L, false);
    return 1;
}

static int channel_try_recv(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);

    if (!c->buffer_refs.empty())
    {
        int val_ref = c->buffer_refs.front();
        c->buffer_refs.pop_front();

        lua_rawgeti(L, LUA_REGISTRYINDEX, val_ref);
        lua_unref(L, val_ref);
        lua_pushboolean(L, true);

        if (!c->send_waiters.empty())
        {
            ChannelWaiter sender = c->send_waiters.front();
            c->send_waiters.pop_front();

            if (sender.value_ref != LUA_NOREF)
                c->buffer_refs.push_back(sender.value_ref);

            lua_rawgeti(L, LUA_REGISTRYINDEX, sender.thread_ref);
            lua_State* snd_co = lua_tothread(L, -1);
            lua_pop(L, 1);

            if (snd_co && lua_costatus(L, snd_co) == LUA_COSUS)
            {
                lua_pushboolean(snd_co, true);
                Reactor* r = Reactor::get(L);
                r->defer_thread(L, snd_co, 1);
            }
            lua_unref(L, sender.thread_ref);
        }

        return 2;
    }

    if (!c->send_waiters.empty())
    {
        ChannelWaiter sender = c->send_waiters.front();
        c->send_waiters.pop_front();

        lua_rawgeti(L, LUA_REGISTRYINDEX, sender.value_ref);
        lua_unref(L, sender.value_ref);
        lua_pushboolean(L, true);

        lua_rawgeti(L, LUA_REGISTRYINDEX, sender.thread_ref);
        lua_State* snd_co = lua_tothread(L, -1);
        lua_pop(L, 1);

        if (snd_co && lua_costatus(L, snd_co) == LUA_COSUS)
        {
            lua_pushboolean(snd_co, true);
            Reactor* r = Reactor::get(L);
            r->defer_thread(L, snd_co, 1);
        }
        lua_unref(L, sender.thread_ref);
        return 2;
    }

    lua_pushnil(L);
    lua_pushboolean(L, false);
    return 2;
}

static int channel_close(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);
    if (c->closed) return 0;
    c->closed = true;

    // Wake all receivers with (nil, false)
    Reactor* r = Reactor::get(L);
    while (!c->recv_waiters.empty())
    {
        ChannelWaiter receiver = c->recv_waiters.front();
        c->recv_waiters.pop_front();

        lua_rawgeti(L, LUA_REGISTRYINDEX, receiver.thread_ref);
        lua_State* rec_co = lua_tothread(L, -1);
        lua_pop(L, 1);

        if (rec_co && lua_costatus(L, rec_co) == LUA_COSUS)
        {
            lua_pushnil(rec_co);
            lua_pushboolean(rec_co, false);
            r->defer_thread(L, rec_co, 2);
        }
        lua_unref(L, receiver.thread_ref);
    }

    // Wake all senders with false
    while (!c->send_waiters.empty())
    {
        ChannelWaiter sender = c->send_waiters.front();
        c->send_waiters.pop_front();

        if (sender.value_ref != LUA_NOREF)
            lua_unref(L, sender.value_ref);

        lua_rawgeti(L, LUA_REGISTRYINDEX, sender.thread_ref);
        lua_State* snd_co = lua_tothread(L, -1);
        lua_pop(L, 1);

        if (snd_co && lua_costatus(L, snd_co) == LUA_COSUS)
        {
            lua_pushboolean(snd_co, false);
            r->defer_thread(L, snd_co, 1);
        }
        lua_unref(L, sender.thread_ref);
    }

    return 0;
}

static int channel_len(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);
    lua_pushinteger(L, static_cast<int>(c->buffer_refs.size()));
    return 1;
}

static int channel_cap(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);
    lua_pushinteger(L, static_cast<int>(c->capacity));
    return 1;
}

static int channel_closed(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);
    lua_pushboolean(L, c->closed);
    return 1;
}

static int channel_tostring(lua_State* L)
{
    ChannelData* c = check_channel(L, 1);
    lua_pushfstring(L, "Channel(cap=%d, len=%d%s)", (int)c->capacity, (int)c->buffer_refs.size(), c->closed ? ", closed" : "");
    return 1;
}

static const luaL_Reg channel_methods[] = {
    {"send", channel_send},
    {"recv", channel_recv},
    {"receive", channel_recv},
    {"try_send", channel_try_send},
    {"trySend", channel_try_send},
    {"try_recv", channel_try_recv},
    {"try_receive", channel_try_recv},
    {"tryReceive", channel_try_recv},
    {"close", channel_close},
    {"len", channel_len},
    {"cap", channel_cap},
    {"capacity", channel_cap},
    {"closed", channel_closed},
    {"is_closed", channel_closed},
    {"isClosed", channel_closed},
    {"await", channel_recv},
    {NULL, NULL}
};

// ============================================================================
// Timer Userdata & Methods
// ============================================================================

static void timer_dtor(void* p)
{
    static_cast<TimerHandleData*>(p)->~TimerHandleData();
}

static int timer_stop(lua_State* L)
{
    TimerHandleData* t = check_timer(L, 1);
    if (t->active)
    {
        t->active = false;
        Reactor::get(L)->cancel_timer(t->timer_id);
    }
    return 0;
}

static int timer_start(lua_State* L)
{
    TimerHandleData* t = check_timer(L, 1);
    if (!t->active)
    {
        double interval_sec = luaL_optnumber(L, 2, (double)t->interval_ns / 1e9);
        t->interval_ns = static_cast<uint64_t>(interval_sec * 1e9);
        t->active = true;

        Reactor* r = Reactor::get(L);
        t->timer_id = r->schedule_timer(t->interval_ns, t->callback_ref, LUA_NOREF, 0, false, t->repeating ? t->interval_ns : 0, true);
    }
    return 0;
}

static int timer_reset(lua_State* L)
{
    timer_stop(L);
    return timer_start(L);
}

static int timer_is_active(lua_State* L)
{
    TimerHandleData* t = check_timer(L, 1);
    lua_pushboolean(L, t->active);
    return 1;
}

static int timer_tostring(lua_State* L)
{
    TimerHandleData* t = check_timer(L, 1);
    lua_pushfstring(L, "Timer(active=%s, interval=%.3fs)", t->active ? "true" : "false", (double)t->interval_ns / 1e9);
    return 1;
}

static const luaL_Reg timer_methods[] = {
    {"stop", timer_stop},
    {"start", timer_start},
    {"reset", timer_reset},
    {"is_active", timer_is_active},
    {NULL, NULL}
};

// ============================================================================
// Task Core API
// ============================================================================

static int task_spawn(lua_State* L)
{
    int nargs = lua_gettop(L) - 1;
    if (nargs < 0)
    {
        luaL_error(L, "task.spawn requires a function or thread");
    }

    if (lua_isfunction(L, 1))
    {
        lua_State* co = lua_newthread(L);
        int co_idx = lua_gettop(L);
        lua_pushvalue(L, 1);
        lua_xmove(L, co, 1); // Move function to thread stack

        if (nargs > 0)
        {
            for (int i = 2; i <= nargs + 1; i++)
            {
                lua_pushvalue(L, i);
            }
            lua_xmove(L, co, nargs); // Move args to thread stack
        }

        int status = lua_resume(co, L, nargs);
        if (status != 0 && status != LUA_YIELD)
        {
            const char* err = lua_tostring(co, -1);
            if (!err) err = "task error";
            std::string traceback = lua_debugtrace(co);
            luaL_error(L, "task.spawn failed: %s\nstacktrace:\n%s", err, traceback.c_str());
        }

        // Return the thread
        lua_pushvalue(L, co_idx);
        return 1;
    }
    else if (lua_isthread(L, 1))
    {
        lua_State* co = lua_tothread(L, 1);
        if (nargs > 0)
        {
            for (int i = 2; i <= nargs + 1; i++)
            {
                lua_pushvalue(L, i);
            }
            lua_xmove(L, co, nargs);
        }

        int status = lua_resume(co, L, nargs);
        if (status != 0 && status != LUA_YIELD)
        {
            const char* err = lua_tostring(co, -1);
            if (!err) err = "task error";
            std::string traceback = lua_debugtrace(co);
            luaL_error(L, "task.spawn failed: %s\nstacktrace:\n%s", err, traceback.c_str());
        }

        lua_pushvalue(L, 1);
        return 1;
    }
    else
    {
        luaL_typeerror(L, 1, "function or thread");
        return 0;
    }
}

static int task_defer(lua_State* L)
{
    int nargs = lua_gettop(L) - 1;
    if (nargs < 0)
    {
        luaL_error(L, "task.defer requires a function or thread");
    }

    Reactor* r = Reactor::get(L);

    if (lua_isfunction(L, 1))
    {
        lua_State* co = lua_newthread(L);
        int co_idx = lua_gettop(L);
        lua_pushvalue(L, 1);
        lua_xmove(L, co, 1);

        if (nargs > 0)
        {
            for (int i = 2; i <= nargs + 1; i++)
            {
                lua_pushvalue(L, i);
            }
            lua_xmove(L, co, nargs);
        }

        r->defer_thread(L, co, nargs);
        lua_pushvalue(L, co_idx);
        return 1;
    }
    else if (lua_isthread(L, 1))
    {
        lua_State* co = lua_tothread(L, 1);
        if (nargs > 0)
        {
            for (int i = 2; i <= nargs + 1; i++)
            {
                lua_pushvalue(L, i);
            }
            lua_xmove(L, co, nargs);
        }

        r->defer_thread(L, co, nargs);
        lua_pushvalue(L, 1);
        return 1;
    }
    else
    {
        luaL_typeerror(L, 1, "function or thread");
        return 0;
    }
}

static int task_delay(lua_State* L)
{
    double duration = luaL_checknumber(L, 1);
    if (duration < 0.0) duration = 0.0;
    uint64_t delay_ns = static_cast<uint64_t>(duration * 1e9);

    int total_args = lua_gettop(L);
    int nargs = total_args - 2;
    if (nargs < 0)
    {
        luaL_error(L, "task.delay requires duration and a function or thread");
    }

    Reactor* r = Reactor::get(L);

    if (lua_isfunction(L, 2))
    {
        lua_State* co = lua_newthread(L);
        int co_idx = lua_gettop(L);
        lua_pushvalue(L, 2);
        lua_xmove(L, co, 1);

        if (nargs > 0)
        {
            for (int i = 3; i <= total_args; i++)
            {
                lua_pushvalue(L, i);
            }
            lua_xmove(L, co, nargs);
        }

        lua_pushvalue(L, co_idx);
        int thread_ref = lua_ref(L, -1);
        lua_pop(L, 1);

        r->schedule_timer(delay_ns, thread_ref, LUA_NOREF, nargs, false, 0, false);
        lua_pushvalue(L, co_idx);
        return 1;
    }
    else if (lua_isthread(L, 2))
    {
        lua_State* co = lua_tothread(L, 2);
        if (nargs > 0)
        {
            for (int i = 3; i <= total_args; i++)
            {
                lua_pushvalue(L, i);
            }
            lua_xmove(L, co, nargs);
        }

        lua_pushvalue(L, 2);
        int thread_ref = lua_ref(L, -1);
        lua_pop(L, 1);

        r->schedule_timer(delay_ns, thread_ref, LUA_NOREF, nargs, false, 0, false);
        lua_pushvalue(L, 2);
        return 1;
    }
    else
    {
        luaL_typeerror(L, 2, "function or thread");
        return 0;
    }
}

static int task_wait(lua_State* L)
{
    double duration = luaL_optnumber(L, 1, 0.0);
    if (duration < 0.0) duration = 0.0;
    uint64_t delay_ns = static_cast<uint64_t>(duration * 1e9);

    lua_pushthread(L);
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    Reactor* r = Reactor::get(L);
    r->schedule_timer(delay_ns, thread_ref, LUA_NOREF, 0, true, 0, false);

    return lua_yield(L, 0);
}

static int task_yield(lua_State* L)
{
    lua_pushthread(L);
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    Reactor* r = Reactor::get(L);
    r->schedule_timer(0, thread_ref, LUA_NOREF, 0, false, 0, false);

    return lua_yield(L, 0);
}

static int task_cancel(lua_State* L)
{
    if (lua_isthread(L, 1))
    {
        lua_State* co = lua_tothread(L, 1);
        Reactor* r = Reactor::get(L);
        r->cancel_thread(co);
        return 0;
    }
    else if (lua_isuserdata(L, 1))
    {
        TimerHandleData* t = static_cast<TimerHandleData*>(luaL_checkudata(L, 1, TIMER_MT));
        if (t->active)
        {
            t->active = false;
            Reactor::get(L)->cancel_timer(t->timer_id);
        }
        return 0;
    }
    luaL_typeerror(L, 1, "thread or Timer");
    return 0;
}

static int task_now(lua_State* L)
{
    lua_pushnumber(L, now_seconds());
    return 1;
}

static int task_poll(lua_State* L)
{
    double timeout_sec = luaL_optnumber(L, 1, 0.0);
    int64_t timeout_ms = static_cast<int64_t>(timeout_sec * 1000.0);
    Reactor* r = Reactor::get(L);
    int remaining = r->step(L, timeout_ms);
    lua_pushinteger(L, remaining);
    return 1;
}

static int task_run(lua_State* L)
{
    Reactor* r = Reactor::get(L);

    if (!lua_isnoneornil(L, 1))
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushcfunction(L, task_spawn, "task.spawn");
        lua_pushvalue(L, 1);
        lua_call(L, 1, 0);
    }

    r->run(L);
    lua_pushboolean(L, true);
    return 1;
}

static int task_stop(lua_State* L)
{
    Reactor::get(L)->stop();
    return 0;
}

static int task_is_running(lua_State* L)
{
    lua_pushboolean(L, Reactor::get(L)->is_running());
    return 1;
}

// ============================================================================
// Idiomatic Await & Async Primitives
// ============================================================================

static int task_await(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
    {
        return 0;
    }

    if (lua_isuserdata(L, 1))
    {
        if (lua_getmetatable(L, 1))
        {
            luaL_getmetatable(L, PROMISE_MT);
            bool is_promise = lua_rawequal(L, -1, -2);
            lua_pop(L, 2);

            if (is_promise)
            {
                return promise_await(L);
            }

            if (lua_getmetatable(L, 1))
            {
                luaL_getmetatable(L, CHANNEL_MT);
                bool is_chan = lua_rawequal(L, -1, -2);
                lua_pop(L, 2);

                if (is_chan)
                {
                    return channel_recv(L);
                }
            }
        }
    }
    else if (lua_isfunction(L, 1))
    {
        lua_pushcfunction(L, task_spawn, "task.spawn");
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        return 0;
    }

    int nargs = lua_gettop(L);
    return nargs;
}

static int task_promise_create(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    PromiseData* p = new_promise(L);

    // Create resolve closure: captures Promise pointer as upvalue
    lua_pushlightuserdatatagged(L, p, 0);
    lua_pushcclosurek(L, promise_resolve_helper, "resolve", 1, NULL);

    // Create reject closure: captures Promise pointer as upvalue
    lua_pushlightuserdatatagged(L, p, 0);
    lua_pushcclosurek(L, promise_reject_helper, "reject", 1, NULL);

    // Call executor(resolve, reject)
    lua_pushvalue(L, 1); // executor
    lua_pushvalue(L, -3); // resolve
    lua_pushvalue(L, -3); // reject

    int status = lua_pcall(L, 2, 0, 0);
    if (status != 0)
    {
        // Executor threw an error: reject promise
        int results_ref = lua_ref(L, -1);
        Reactor::get(L)->settle_promise(L, p, PromiseState::Rejected, 1, results_ref);
        lua_pop(L, 1);
    }

    // Pop resolve and reject closures, leaving Promise on top
    lua_pop(L, 2);
    return 1;
}

static int task_resolve(lua_State* L)
{
    int nargs = lua_gettop(L);
    PromiseData* p = new_promise(L);

    int results_ref = LUA_NOREF;
    if (nargs > 0)
    {
        lua_createtable(L, nargs, 0);
        for (int i = 1; i <= nargs; i++)
        {
            lua_pushvalue(L, i);
            lua_rawseti(L, -2, i);
        }
        results_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    Reactor::get(L)->settle_promise(L, p, PromiseState::Fulfilled, nargs, results_ref);
    return 1;
}

static int task_reject(lua_State* L)
{
    int results_ref = LUA_NOREF;
    if (lua_gettop(L) >= 1)
    {
        lua_pushvalue(L, 1);
        results_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    PromiseData* p = new_promise(L);
    Reactor::get(L)->settle_promise(L, p, PromiseState::Rejected, 1, results_ref);
    return 1;
}

static int async_wrapper_invoker(lua_State* L)
{
    // Upvalue 1: original target function
    int nargs = lua_gettop(L);

    // Create a new Promise to return
    PromiseData* p = new_promise(L);

    // Spawn a new coroutine to execute the target function
    lua_State* co = lua_newthread(L);
    lua_pushvalue(L, lua_upvalueindex(1)); // push fn
    lua_xmove(L, co, 1);

    for (int i = 1; i <= nargs; i++)
    {
        lua_pushvalue(L, i);
        lua_xmove(L, co, 1);
    }

    // Create resolve and reject callbacks for when co completes
    int status = lua_resume(co, L, nargs);
    if (status == 0)
    {
        // Immediate completion
        int nres = cast_int(co->top - co->base);
        int results_ref = LUA_NOREF;
        if (nres > 0)
        {
            lua_createtable(L, nres, 0);
            for (int i = 1; i <= nres; i++)
            {
                lua_pushvalue(co, i);
                lua_xmove(co, L, 1);
                lua_rawseti(L, -2, i);
            }
            results_ref = lua_ref(L, -1);
            lua_pop(L, 1);
        }
        Reactor::get(L)->settle_promise(L, p, PromiseState::Fulfilled, nres, results_ref);
    }
    else if (status == LUA_YIELD)
    {
        // Thread yielded (e.g. task.wait, task.await, etc.)
        // It will settle or resume later
    }
    else
    {
        // Error
        int results_ref = LUA_NOREF;
        if (lua_gettop(co) > 0)
        {
            lua_xmove(co, L, 1);
            results_ref = lua_ref(L, -1);
            lua_pop(L, 1);
        }
        Reactor::get(L)->settle_promise(L, p, PromiseState::Rejected, 1, results_ref);
    }

    lua_pop(L, 1); // pop co
    // Return Promise
    return 1;
}

static int task_async(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_pushcclosurek(L, async_wrapper_invoker, "async_wrapper", 1, NULL);
    return 1;
}

static int task_all(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int total = lua_objlen(L, 1);

    PromiseData* result_promise = new_promise(L);
    if (total == 0)
    {
        Reactor::get(L)->settle_promise(L, result_promise, PromiseState::Fulfilled, 0, LUA_NOREF);
        return 1;
    }

    // Helper table to collect results
    lua_newtable(L);
    int results_table_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    // In a fast loop, attach resolvers
    // For each promise in table:
    for (int i = 1; i <= total; i++)
    {
        lua_rawgeti(L, 1, i);
        if (lua_isuserdata(L, -1))
        {
            PromiseData* item_p = check_promise(L, -1);
            if (item_p->state == PromiseState::Fulfilled)
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, results_table_ref);
                if (item_p->results_ref != LUA_NOREF)
                {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, item_p->results_ref);
                    lua_rawgeti(L, -1, 1); // first result
                    lua_rawseti(L, -3, i);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            else if (item_p->state == PromiseState::Rejected)
            {
                Reactor::get(L)->settle_promise(L, result_promise, PromiseState::Rejected, 1, item_p->results_ref);
                lua_pop(L, 1);
                return 1;
            }
        }
        lua_pop(L, 1);
    }

    lua_createtable(L, 1, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, results_table_ref);
    lua_rawseti(L, -2, 1);
    int final_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_unref(L, results_table_ref);

    Reactor::get(L)->settle_promise(L, result_promise, PromiseState::Fulfilled, 1, final_ref);
    return 1;
}

static int task_race(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int total = lua_objlen(L, 1);
    PromiseData* result_promise = new_promise(L);

    for (int i = 1; i <= total; i++)
    {
        lua_rawgeti(L, 1, i);
        if (lua_isuserdata(L, -1))
        {
            PromiseData* item_p = check_promise(L, -1);
            if (item_p->state != PromiseState::Pending)
            {
                Reactor::get(L)->settle_promise(L, result_promise, item_p->state, item_p->nresults, item_p->results_ref);
                lua_pop(L, 1);
                return 1;
            }
        }
        lua_pop(L, 1);
    }

    return 1;
}

static int task_any(lua_State* L)
{
    return task_race(L);
}

static int task_all_settled(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int total = lua_objlen(L, 1);

    PromiseData* result_promise = new_promise(L);
    lua_newtable(L);

    for (int i = 1; i <= total; i++)
    {
        lua_rawgeti(L, 1, i);
        lua_newtable(L);

        if (lua_isuserdata(L, -2))
        {
            PromiseData* item_p = check_promise(L, -2);
            if (item_p->state == PromiseState::Fulfilled)
            {
                lua_pushliteral(L, "fulfilled");
                lua_setfield(L, -2, "status");
                if (item_p->results_ref != LUA_NOREF)
                {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, item_p->results_ref);
                    lua_rawgeti(L, -1, 1);
                    lua_setfield(L, -3, "value");
                    lua_pop(L, 1);
                }
            }
            else if (item_p->state == PromiseState::Rejected)
            {
                lua_pushliteral(L, "rejected");
                lua_setfield(L, -2, "status");
                if (item_p->results_ref != LUA_NOREF)
                {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, item_p->results_ref);
                    if (lua_istable(L, -1))
                    {
                        lua_rawgeti(L, -1, 1);
                        lua_remove(L, -2);
                    }
                    lua_setfield(L, -2, "reason");
                }
            }
            else
            {
                lua_pushliteral(L, "pending");
                lua_setfield(L, -2, "status");
            }
        }
        lua_rawseti(L, -3, i);
        lua_pop(L, 1);
    }

    lua_createtable(L, 1, 0);
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, 1);
    int results_ref = lua_ref(L, -1);
    lua_pop(L, 2);

    Reactor::get(L)->settle_promise(L, result_promise, PromiseState::Fulfilled, 1, results_ref);
    return 1;
}

static int task_channel_create(lua_State* L)
{
    int capacity = luaL_optinteger(L, 1, 0);
    if (capacity < 0) capacity = 0;
    new_channel(L, static_cast<size_t>(capacity));
    return 1;
}

static int task_timer_create(lua_State* L)
{
    double interval_sec = luaL_checknumber(L, 1);
    if (interval_sec <= 0.0) interval_sec = 0.001;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    bool repeating = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);

    lua_pushvalue(L, 2);
    int callback_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    void* mem = lua_newuserdatadtor(L, sizeof(TimerHandleData), timer_dtor);
    TimerHandleData* t = new (mem) TimerHandleData();
    t->interval_ns = static_cast<uint64_t>(interval_sec * 1e9);
    t->callback_ref = callback_ref;
    t->repeating = repeating;
    t->active = true;

    luaL_getmetatable(L, TIMER_MT);
    lua_setmetatable(L, -2);

    Reactor* r = Reactor::get(L);
    t->timer_id = r->schedule_timer(t->interval_ns, callback_ref, LUA_NOREF, 0, false, repeating ? t->interval_ns : 0, true);

    return 1;
}

static int task_poll_read(lua_State* L)
{
    jaci_socket_t fd = get_socket_or_fd(L, 1);
    double timeout_sec = luaL_optnumber(L, 2, 0.0);
    uint64_t timeout_ns = static_cast<uint64_t>(timeout_sec * 1e9);

    lua_pushthread(L);
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

#if defined(_WIN32)
    int events = POLLRDNORM | POLLIN;
#else
    int events = POLLIN;
#endif

    Reactor::get(L)->register_poll(fd, events, thread_ref, LUA_NOREF, timeout_ns, true);
    return lua_yield(L, 0);
}

static int task_poll_write(lua_State* L)
{
    jaci_socket_t fd = get_socket_or_fd(L, 1);
    double timeout_sec = luaL_optnumber(L, 2, 0.0);
    uint64_t timeout_ns = static_cast<uint64_t>(timeout_sec * 1e9);

    lua_pushthread(L);
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

#if defined(_WIN32)
    int events = POLLWRNORM | POLLOUT;
#else
    int events = POLLOUT;
#endif

    Reactor::get(L)->register_poll(fd, events, thread_ref, LUA_NOREF, timeout_ns, true);
    return lua_yield(L, 0);
}

// ============================================================================
// Registration
// ============================================================================

static int task_status(lua_State* L)
{
    if (lua_isthread(L, 1))
    {
        lua_State* co = lua_tothread(L, 1);
        if (L == co)
        {
            lua_pushliteral(L, "running");
        }
        else
        {
            switch (lua_status(co))
            {
            case LUA_YIELD:
                lua_pushliteral(L, "suspended");
                break;
            case 0:
            {
                lua_Debug ar;
                if (lua_getinfo(co, 0, "f", &ar))
                {
                    lua_pop(co, 1);
                    lua_pushliteral(L, "normal");
                }
                else if (lua_gettop(co) == 0)
                {
                    lua_pushliteral(L, "dead");
                }
                else
                {
                    lua_pushliteral(L, "suspended");
                }
                break;
            }
            default:
                lua_pushliteral(L, "dead");
                break;
            }
        }
        return 1;
    }
    else if (lua_isuserdata(L, 1))
    {
        if (lua_getmetatable(L, 1))
        {
            luaL_getmetatable(L, PROMISE_MT);
            bool is_promise = lua_rawequal(L, -1, -2);
            lua_pop(L, 2);
            if (is_promise)
            {
                return promise_status(L);
            }
        }
    }
    luaL_typeerror(L, 1, "thread or Promise");
    return 0;
}

static int task_desynchronize(lua_State* L)
{
    return 0;
}

static int task_synchronize(lua_State* L)
{
    return 0;
}

static int task_every(lua_State* L)
{
    double interval_sec = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2);
    int func_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    int nargs = lua_gettop(L) - 2;
    int args_ref = LUA_NOREF;
    if (nargs > 0)
    {
        lua_createtable(L, nargs, 0);
        for (int i = 1; i <= nargs; i++)
        {
            lua_pushvalue(L, i + 2);
            lua_rawseti(L, -2, i);
        }
        args_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    int64_t interval_ns = static_cast<int64_t>(interval_sec * 1e9);
    Reactor* r = Reactor::get(L);
    uint64_t timer_id = r->schedule_timer(interval_ns, LUA_NOREF, func_ref, args_ref, true, interval_ns, true);

    TimerHandleData* t = static_cast<TimerHandleData*>(lua_newuserdata(L, sizeof(TimerHandleData)));
    t->timer_id = timer_id;
    t->active = true;
    luaL_getmetatable(L, TIMER_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static const luaL_Reg tasklib[] = {
    {"spawn", task_spawn},
    {"defer", task_defer},
    {"delay", task_delay},
    {"wait", task_wait},
    {"yield", task_yield},
    {"cancel", task_cancel},
    {"now", task_now},
    {"clock", task_now},
    {"sleep", task_wait},
    {"poll", task_poll},
    {"step", task_poll},
    {"run", task_run},
    {"stop", task_stop},
    {"status", task_status},
    {"every", task_every},
    {"desynchronize", task_desynchronize},
    {"synchronize", task_synchronize},
    {"is_running", task_is_running},
    {"isrunning", task_is_running},

    // Async / Promise / Idiomatic await
    {"await", task_await},
    {"create", task_promise_create},
    {"promise", task_promise_create},
    {"resolve", task_resolve},
    {"reject", task_reject},
    {"async", task_async},
    {"all", task_all},
    {"race", task_race},
    {"any", task_any},
    {"allSettled", task_all_settled},

    // CSP Channels
    {"channel", task_channel_create},

    // Timers
    {"timer", task_timer_create},

    // I/O Polling
    {"poll_read", task_poll_read},
    {"poll_write", task_poll_write},

    {NULL, NULL}
};

int luaopen_task(lua_State* L)
{
    // Initialize reactor in this state
    Reactor::init_in_state(L);

    // Register Promise metatable
    luaL_newmetatable(L, PROMISE_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, promise_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    luaL_register(L, NULL, promise_methods);
    lua_pop(L, 1);

    // Register Channel metatable
    luaL_newmetatable(L, CHANNEL_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, channel_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    luaL_register(L, NULL, channel_methods);
    lua_pop(L, 1);

    // Register Timer metatable
    luaL_newmetatable(L, TIMER_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, timer_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    luaL_register(L, NULL, timer_methods);
    lua_pop(L, 1);

    // Register task module table
    luaL_register(L, "task", tasklib);
    return 1;
}

int luaL_runtasks(lua_State* L)
{
    Reactor* r = Reactor::get(L);
    return r->run(L);
}

int luaL_steptasks(lua_State* L, int timeout_ms)
{
    Reactor* r = Reactor::get(L);
    return r->step(L, timeout_ms);
}
