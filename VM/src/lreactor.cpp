// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lreactor.h"

#include "ldebug.h"
#include "lstate.h"
#include "lvm.h"

#include <chrono>
#include <thread>
#include <cstring>
#include <cstdio>

namespace Jaci
{

static const char* kReactorRegistryKey = "__jaci_reactor";

static void reactor_dtor(void* p)
{
    static_cast<Reactor*>(p)->~Reactor();
}

Reactor::Reactor(lua_State* L)
    : main_L_(L)
    , running_(false)
    , stop_requested_(false)
    , next_timer_id_(1)
{
}

Reactor::~Reactor()
{
    // Clean up all registry references held in timer heap
    if (main_L_)
    {
        for (auto& t : timer_heap_)
        {
            if (t.thread_ref != LUA_NOREF && t.thread_ref != LUA_REFNIL)
                lua_unref(main_L_, t.thread_ref);
            if (t.args_ref != LUA_NOREF && t.args_ref != LUA_REFNIL)
                lua_unref(main_L_, t.args_ref);
        }
        timer_heap_.clear();

        for (auto& m : microtasks_)
        {
            if (m.thread_ref != LUA_NOREF && m.thread_ref != LUA_REFNIL)
                lua_unref(main_L_, m.thread_ref);
            if (m.args_ref != LUA_NOREF && m.args_ref != LUA_REFNIL)
                lua_unref(main_L_, m.args_ref);
        }
        microtasks_.clear();

        for (auto& io : io_watchers_)
        {
            if (io.thread_ref != LUA_NOREF && io.thread_ref != LUA_REFNIL)
                lua_unref(main_L_, io.thread_ref);
            if (io.callback_ref != LUA_NOREF && io.callback_ref != LUA_REFNIL)
                lua_unref(main_L_, io.callback_ref);
        }
        io_watchers_.clear();
    }
}

Reactor* Reactor::get(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kReactorRegistryKey);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        void* mem = lua_newuserdatadtor(L, sizeof(Reactor), reactor_dtor);
        Reactor* r = new (mem) Reactor(L);
        lua_setfield(L, LUA_REGISTRYINDEX, kReactorRegistryKey);
        return r;
    }
    Reactor* r = static_cast<Reactor*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return r;
}

void Reactor::init_in_state(lua_State* L)
{
    (void)get(L);
}

bool Reactor::has_pending_work() const
{
    return !microtasks_.empty() || !timer_heap_.empty() || !io_watchers_.empty();
}

void Reactor::stop()
{
    stop_requested_ = true;
}

void Reactor::defer_thread(lua_State* L, lua_State* co, int nargs)
{
    lua_pushthread(co);
    if (co != L)
    {
        lua_xmove(co, L, 1);
    }
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    MicrotaskNode node;
    node.thread_ref = thread_ref;
    node.args_ref = LUA_NOREF;
    node.nargs = nargs;
    node.is_function = false;
    node.cancelled = false;

    microtasks_.push_back(node);
}

void Reactor::defer_function(lua_State* L, int func_idx, int nargs)
{
    lua_pushvalue(L, func_idx);
    int func_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    int args_ref = LUA_NOREF;
    if (nargs > 0)
    {
        lua_createtable(L, nargs, 0);
        for (int i = nargs; i >= 1; i--)
        {
            lua_pushvalue(L, -1 - i);
            lua_rawseti(L, -2, nargs - i + 1);
        }
        args_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    MicrotaskNode node;
    node.thread_ref = func_ref;
    node.args_ref = args_ref;
    node.nargs = nargs;
    node.is_function = true;
    node.cancelled = false;

    microtasks_.push_back(node);
}

int64_t Reactor::schedule_timer(uint64_t delay_ns, int thread_ref, int args_ref, int nargs, bool is_yield_wait, uint64_t interval_ns, bool is_callback)
{
    uint64_t start = now_ns();
    int64_t id = next_timer_id_++;

    TimerNode node;
    node.start_ns = start;
    node.expiry_ns = start + delay_ns;
    node.interval_ns = interval_ns;
    node.timer_id = id;
    node.thread_ref = thread_ref;
    node.args_ref = args_ref;
    node.nargs = nargs;
    node.is_yield_wait = is_yield_wait;
    node.is_callback = is_callback;
    node.cancelled = false;

    timer_heap_.push_back(node);
    std::push_heap(timer_heap_.begin(), timer_heap_.end(), std::greater<TimerNode>());

    return id;
}

void Reactor::cancel_timer(int64_t timer_id)
{
    for (auto& t : timer_heap_)
    {
        if (t.timer_id == timer_id)
        {
            t.cancelled = true;
            break;
        }
    }
}

void Reactor::cancel_thread(lua_State* co)
{
    for (auto& t : timer_heap_)
    {
        if (!t.cancelled && t.thread_ref != LUA_NOREF)
        {
            lua_rawgeti(main_L_, LUA_REGISTRYINDEX, t.thread_ref);
            if (lua_tothread(main_L_, -1) == co)
            {
                t.cancelled = true;
            }
            lua_pop(main_L_, 1);
        }
    }

    for (auto& m : microtasks_)
    {
        if (!m.cancelled && !m.is_function && m.thread_ref != LUA_NOREF)
        {
            lua_rawgeti(main_L_, LUA_REGISTRYINDEX, m.thread_ref);
            if (lua_tothread(main_L_, -1) == co)
            {
                m.cancelled = true;
            }
            lua_pop(main_L_, 1);
        }
    }

    for (auto& io : io_watchers_)
    {
        if (!io.cancelled && io.thread_ref != LUA_NOREF)
        {
            lua_rawgeti(main_L_, LUA_REGISTRYINDEX, io.thread_ref);
            if (lua_tothread(main_L_, -1) == co)
            {
                io.cancelled = true;
            }
            lua_pop(main_L_, 1);
        }
    }

    lua_resetthread(co);
}

void Reactor::register_poll(jaci_socket_t fd, int events, int thread_ref, int callback_ref, uint64_t timeout_ns, bool one_shot)
{
    uint64_t start = now_ns();
    IOWatcher watcher;
    watcher.fd = fd;
    watcher.events = events;
    watcher.thread_ref = thread_ref;
    watcher.callback_ref = callback_ref;
    watcher.start_ns = start;
    watcher.expiry_ns = timeout_ns > 0 ? (start + timeout_ns) : 0;
    watcher.one_shot = one_shot;
    watcher.cancelled = false;

    io_watchers_.push_back(watcher);
}

void Reactor::cancel_poll(jaci_socket_t fd)
{
    for (auto& io : io_watchers_)
    {
        if (io.fd == fd)
            io.cancelled = true;
    }
}

void Reactor::resume_thread_internal(lua_State* L, lua_State* co, int nargs, bool is_yield_wait, double elapsed_sec)
{
    if (is_yield_wait)
    {
        lua_pushnumber(co, elapsed_sec);
        nargs = 1;
    }

    int status = lua_resume(co, L, nargs);
    if (status != 0 && status != LUA_YIELD)
    {
        const char* err = lua_tostring(co, -1);
        if (!err) err = "unknown async error";
        std::string traceback = lua_debugtrace(co);
        fprintf(stderr, "Async task error: %s\nstacktrace:\n%s\n", err, traceback.c_str());
    }
}

void Reactor::invoke_callback_internal(lua_State* L, int callback_ref, int args_ref, int nargs)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        return;
    }

    if (args_ref != LUA_NOREF && nargs > 0)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, args_ref);
        for (int i = 1; i <= nargs; i++)
        {
            lua_rawgeti(L, -1, i);
        }
        lua_remove(L, -1 - nargs); // remove table
    }

    int status = lua_pcall(L, nargs, 0, 0);
    if (status != 0)
    {
        const char* err = lua_tostring(L, -1);
        if (!err) err = "callback error";
        fprintf(stderr, "Async callback error: %s\n", err);
        lua_pop(L, 1);
    }
}

void Reactor::process_microtasks(lua_State* L)
{
    if (microtasks_.empty())
        return;

    active_microtasks_.swap(microtasks_);

    for (size_t i = 0; i < active_microtasks_.size(); i++)
    {
        MicrotaskNode& m = active_microtasks_[i];
        if (m.cancelled)
        {
            if (m.thread_ref != LUA_NOREF) lua_unref(L, m.thread_ref);
            if (m.args_ref != LUA_NOREF) lua_unref(L, m.args_ref);
            continue;
        }

        if (m.is_function)
        {
            lua_State* co = lua_newthread(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, m.thread_ref);
            lua_xmove(L, co, 1); // Move function to new thread

            int actual_args = 0;
            if (m.args_ref != LUA_NOREF)
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, m.args_ref);
                for (int a = 1; a <= m.nargs; a++)
                {
                    lua_rawgeti(L, -1, a);
                    lua_xmove(L, co, 1);
                }
                lua_pop(L, 1); // pop args table
                actual_args = m.nargs;
            }

            resume_thread_internal(L, co, actual_args);
            lua_pop(L, 1); // pop co from L
        }
        else
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, m.thread_ref);
            lua_State* co = lua_tothread(L, -1);
            lua_pop(L, 1);

            if (co && (co->status == LUA_OK || co->status == LUA_YIELD))
            {
                int actual_args = m.nargs;
                if (m.args_ref != LUA_NOREF)
                {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, m.args_ref);
                    for (int a = 1; a <= m.nargs; a++)
                    {
                        lua_rawgeti(L, -1, a);
                        lua_xmove(L, co, 1);
                    }
                    lua_pop(L, 1);
                    actual_args = m.nargs;
                }

                resume_thread_internal(L, co, actual_args);
            }
        }

        if (m.thread_ref != LUA_NOREF) lua_unref(L, m.thread_ref);
        if (m.args_ref != LUA_NOREF) lua_unref(L, m.args_ref);
    }

    active_microtasks_.clear();
}

void Reactor::process_expired_timers(lua_State* L, uint64_t current_ns)
{
    while (!timer_heap_.empty() && timer_heap_.front().expiry_ns <= current_ns)
    {
        std::pop_heap(timer_heap_.begin(), timer_heap_.end(), std::greater<TimerNode>());
        TimerNode timer = timer_heap_.back();
        timer_heap_.pop_back();

        if (timer.cancelled)
        {
            if (timer.thread_ref != LUA_NOREF) lua_unref(L, timer.thread_ref);
            if (timer.args_ref != LUA_NOREF) lua_unref(L, timer.args_ref);
            continue;
        }

        if (timer.interval_ns > 0)
        {
            // Repeating timer: re-insert into heap
            TimerNode next_node = timer;
            next_node.expiry_ns = current_ns + timer.interval_ns;
            next_node.start_ns = current_ns;
            timer_heap_.push_back(next_node);
            std::push_heap(timer_heap_.begin(), timer_heap_.end(), std::greater<TimerNode>());
        }

        if (timer.is_callback)
        {
            invoke_callback_internal(L, timer.thread_ref, timer.args_ref, timer.nargs);
        }
        else
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, timer.thread_ref);
            lua_State* co = lua_tothread(L, -1);
            lua_pop(L, 1);

            if (co && (co->status == LUA_OK || co->status == LUA_YIELD))
            {
                double elapsed = (double)(current_ns - timer.start_ns) / 1e9;
                int actual_args = timer.nargs;

                if (timer.is_yield_wait)
                {
                    lua_pushnumber(co, elapsed);
                    actual_args = 1;
                }
                else if (timer.args_ref != LUA_NOREF)
                {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, timer.args_ref);
                    for (int a = 1; a <= timer.nargs; a++)
                    {
                        lua_rawgeti(L, -1, a);
                        lua_xmove(L, co, 1);
                    }
                    lua_pop(L, 1);
                    actual_args = timer.nargs;
                }

                resume_thread_internal(L, co, actual_args, false);
            }
        }

        if (timer.interval_ns == 0)
        {
            if (timer.thread_ref != LUA_NOREF) lua_unref(L, timer.thread_ref);
            if (timer.args_ref != LUA_NOREF) lua_unref(L, timer.args_ref);
        }
    }
}

void Reactor::poll_io(lua_State* L, int timeout_ms)
{
    if (io_watchers_.empty())
        return;

    // Filter out cancelled watchers
    io_watchers_.erase(
        std::remove_if(io_watchers_.begin(), io_watchers_.end(),
            [L](const IOWatcher& w) {
                if (w.cancelled)
                {
                    if (w.thread_ref != LUA_NOREF) lua_unref(L, w.thread_ref);
                    if (w.callback_ref != LUA_NOREF) lua_unref(L, w.callback_ref);
                    return true;
                }
                return false;
            }),
        io_watchers_.end());

    if (io_watchers_.empty())
        return;

    size_t count = io_watchers_.size();
#if defined(_WIN32)
    std::vector<WSAPOLLFD> fds(count);
#else
    std::vector<struct pollfd> fds(count);
#endif

    for (size_t i = 0; i < count; i++)
    {
        fds[i].fd = io_watchers_[i].fd;
        fds[i].events = (short)io_watchers_[i].events;
        fds[i].revents = 0;
    }

#if defined(_WIN32)
    int poll_res = WSAPoll(fds.data(), (ULONG)count, timeout_ms);
#else
    int poll_res = poll(fds.data(), (nfds_t)count, timeout_ms);
#endif

    uint64_t current_ns = now_ns();

    std::vector<size_t> ready_indices;
    for (size_t i = 0; i < count; i++)
    {
        bool ready = (poll_res > 0 && fds[i].revents != 0);
        bool timed_out = (io_watchers_[i].expiry_ns > 0 && current_ns >= io_watchers_[i].expiry_ns);

        if (ready || timed_out)
        {
            ready_indices.push_back(i);
        }
    }

    for (size_t idx : ready_indices)
    {
        IOWatcher& watcher = io_watchers_[idx];
        if (watcher.cancelled)
            continue;

        bool ready = (fds[idx].revents != 0);
        bool timed_out = (watcher.expiry_ns > 0 && current_ns >= watcher.expiry_ns);

        if (watcher.thread_ref != LUA_NOREF)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, watcher.thread_ref);
            lua_State* co = lua_tothread(L, -1);
            lua_pop(L, 1);

            if (co && (co->status == LUA_OK || co->status == LUA_YIELD))
            {
                lua_pushboolean(co, ready);
                lua_pushboolean(co, timed_out);
                resume_thread_internal(L, co, 2);
            }
        }
        else if (watcher.callback_ref != LUA_NOREF)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, watcher.callback_ref);
            lua_pushboolean(L, ready);
            lua_pushboolean(L, timed_out);
            lua_pcall(L, 2, 0, 0);
        }

        if (watcher.one_shot)
        {
            watcher.cancelled = true;
            if (watcher.thread_ref != LUA_NOREF) { lua_unref(L, watcher.thread_ref); watcher.thread_ref = LUA_NOREF; }
            if (watcher.callback_ref != LUA_NOREF) { lua_unref(L, watcher.callback_ref); watcher.callback_ref = LUA_NOREF; }
        }
    }
}

int Reactor::step(lua_State* L, int64_t max_wait_ms)
{
    main_L_ = L;

    // 1. Process microtasks
    process_microtasks(L);

    // 2. Process expired timers
    uint64_t current_ns = now_ns();
    process_expired_timers(L, current_ns);

    // 3. Determine wait duration for I/O poll
    int timeout_ms = 0;
    if (!microtasks_.empty())
    {
        timeout_ms = 0; // Don't block if there are new microtasks
    }
    else if (!timer_heap_.empty())
    {
        uint64_t next_expiry = timer_heap_.front().expiry_ns;
        uint64_t diff = next_expiry > current_ns ? (next_expiry - current_ns) : 0;
        timeout_ms = (int)(diff / 1000000);
        if (max_wait_ms >= 0 && timeout_ms > max_wait_ms)
            timeout_ms = (int)max_wait_ms;
    }
    else
    {
        timeout_ms = (max_wait_ms >= 0) ? (int)max_wait_ms : (io_watchers_.empty() ? 0 : 50);
    }

    // 4. Poll I/O or sleep
    if (!io_watchers_.empty())
    {
        poll_io(L, timeout_ms);
    }
    else if (timeout_ms > 0 && microtasks_.empty())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    }

    // 5. Final microtask flush
    process_microtasks(L);

    return (int)(microtasks_.size() + timer_heap_.size() + io_watchers_.size());
}

int Reactor::run(lua_State* L)
{
    main_L_ = L;
    running_ = true;
    stop_requested_ = false;

    while (!stop_requested_ && has_pending_work())
    {
        step(L, -1);
    }

    running_ = false;
    return 0;
}

void Reactor::settle_promise(lua_State* L, PromiseData* promise, PromiseState state, int nresults, int results_ref)
{
    if (promise->state != PromiseState::Pending)
        return;

    promise->state = state;
    promise->nresults = nresults;
    promise->results_ref = results_ref;

    // Wake up any coroutines suspended on task.await(promise)
    for (int thread_ref : promise->awaiting_thread_refs)
    {
        if (thread_ref != LUA_NOREF && thread_ref != LUA_REFNIL)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, thread_ref);
            lua_State* co = lua_tothread(L, -1);
            lua_pop(L, 1);

            if (co && (co->status == LUA_OK || co->status == LUA_YIELD))
            {
                if (state == PromiseState::Fulfilled)
                {
                    // Push results onto co stack
                    if (results_ref != LUA_NOREF && nresults > 0)
                    {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, results_ref);
                        for (int i = 1; i <= nresults; i++)
                        {
                            lua_rawgeti(L, -1, i);
                            lua_xmove(L, co, 1);
                        }
                        lua_pop(L, 1);
                    }
                    resume_thread_internal(L, co, nresults);
                }
                else
                {
                    // Rejected: push error onto co stack and resume with error
                    if (results_ref != LUA_NOREF)
                    {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, results_ref);
                        lua_xmove(L, co, 1);
                    }
                    else
                    {
                        lua_pushstring(co, "promise rejected");
                    }
                    resume_thread_internal(L, co, 1);
                }
            }
            lua_unref(L, thread_ref);
        }
    }
    promise->awaiting_thread_refs.clear();

    // Schedule chained callbacks
    for (const auto& cb : promise->callbacks)
    {
        int handler_ref = (state == PromiseState::Fulfilled) ? cb.on_fulfilled_ref : cb.on_rejected_ref;
        if (handler_ref != LUA_NOREF && handler_ref != LUA_REFNIL)
        {
            // Defer execution of handler
            lua_rawgeti(L, LUA_REGISTRYINDEX, handler_ref);
            int func_idx = lua_gettop(L);

            int nargs = nresults;
            if (results_ref != LUA_NOREF)
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, results_ref);
                if (state == PromiseState::Fulfilled)
                {
                    for (int i = 1; i <= nresults; i++)
                    {
                        lua_rawgeti(L, func_idx + 1, i);
                    }
                }
                else
                {
                    lua_pushvalue(L, func_idx + 1);
                    nargs = 1;
                }
                lua_remove(L, func_idx + 1); // remove table
            }

            defer_function(L, func_idx, nargs);
            lua_pop(L, 1 + nargs);
        }
        else if (cb.next_promise_ref != LUA_NOREF)
        {
            // Propagate unhandled outcome to next promise
            lua_rawgeti(L, LUA_REGISTRYINDEX, cb.next_promise_ref);
            PromiseData* next_p = static_cast<PromiseData*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (next_p)
            {
                settle_promise(L, next_p, state, nresults, results_ref);
            }
        }

        if (cb.on_fulfilled_ref != LUA_NOREF) lua_unref(L, cb.on_fulfilled_ref);
        if (cb.on_rejected_ref != LUA_NOREF) lua_unref(L, cb.on_rejected_ref);
        if (cb.next_promise_ref != LUA_NOREF) lua_unref(L, cb.next_promise_ref);
    }
    promise->callbacks.clear();
}

void Reactor::attach_promise_continuation(lua_State* L, PromiseData* promise, int on_fulfilled_ref, int on_rejected_ref, int next_promise_ref)
{
    if (promise->state == PromiseState::Pending)
    {
        PromiseCallback cb;
        cb.on_fulfilled_ref = on_fulfilled_ref;
        cb.on_rejected_ref = on_rejected_ref;
        cb.next_promise_ref = next_promise_ref;
        promise->callbacks.push_back(cb);
    }
    else
    {
        int handler_ref = (promise->state == PromiseState::Fulfilled) ? on_fulfilled_ref : on_rejected_ref;
        if (handler_ref != LUA_NOREF && handler_ref != LUA_REFNIL)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, handler_ref);
            int func_idx = lua_gettop(L);

            int nargs = promise->nresults;
            if (promise->results_ref != LUA_NOREF)
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, promise->results_ref);
                if (promise->state == PromiseState::Fulfilled)
                {
                    for (int i = 1; i <= promise->nresults; i++)
                    {
                        lua_rawgeti(L, func_idx + 1, i);
                    }
                }
                else
                {
                    lua_pushvalue(L, func_idx + 1);
                    nargs = 1;
                }
                lua_remove(L, func_idx + 1);
            }

            defer_function(L, func_idx, nargs);
            lua_pop(L, 1 + nargs);
        }
        else if (next_promise_ref != LUA_NOREF)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, next_promise_ref);
            PromiseData* next_p = static_cast<PromiseData*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (next_p)
            {
                settle_promise(L, next_p, promise->state, promise->nresults, promise->results_ref);
            }
        }

        if (on_fulfilled_ref != LUA_NOREF) lua_unref(L, on_fulfilled_ref);
        if (on_rejected_ref != LUA_NOREF) lua_unref(L, on_rejected_ref);
        if (next_promise_ref != LUA_NOREF) lua_unref(L, next_promise_ref);
    }
}

void Reactor::await_promise(lua_State* L, PromiseData* promise, lua_State* co)
{
    lua_pushthread(co);
    if (co != L)
    {
        lua_xmove(co, L, 1);
    }
    int thread_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    promise->awaiting_thread_refs.push_back(thread_ref);
}

} // namespace Jaci
