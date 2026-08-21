// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "lua.h"
#include "lualib.h"

#include <vector>
#include <deque>
#include <chrono>
#include <cstdint>
#include <memory>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
typedef SOCKET jaci_socket_t;
#define JACI_INVALID_SOCKET INVALID_SOCKET
#else
#include <poll.h>
#include <unistd.h>
typedef int jaci_socket_t;
#define JACI_INVALID_SOCKET (-1)
#endif

namespace Jaci
{

// Monotonic clock in nanoseconds
inline uint64_t now_ns()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

inline double now_seconds()
{
    return (double)now_ns() / 1e9;
}

// Timer node stored in min-heap priority queue
struct TimerNode
{
    uint64_t expiry_ns;
    uint64_t interval_ns;    // 0 for one-shot, >0 for repeating
    int64_t timer_id;
    int thread_ref;          // LUA_REGISTRYINDEX reference to thread or callback
    int args_ref;            // LUA_REGISTRYINDEX reference to argument array or LUA_NOREF
    int nargs;
    bool is_yield_wait;      // If true, passes elapsed seconds as argument to resume
    uint64_t start_ns;
    bool is_callback;        // True if thread_ref is a function callback, not a thread
    bool cancelled;

    bool operator>(const TimerNode& other) const
    {
        return expiry_ns > other.expiry_ns;
    }
};

// Microtask node for deferred/immediate task execution
struct MicrotaskNode
{
    int thread_ref;          // LUA_REGISTRYINDEX reference to thread or function
    int args_ref;            // LUA_REGISTRYINDEX reference to arguments or LUA_NOREF
    int nargs;
    bool is_function;        // True if thread_ref is a function to be executed in a new thread
    bool cancelled;
};

// I/O Watcher for socket/file descriptor polling
struct IOWatcher
{
    jaci_socket_t fd;
    int events;              // POLLIN, POLLOUT
    int thread_ref;          // LUA_REGISTRYINDEX reference to waiting thread
    int callback_ref;        // Optional callback function reference
    uint64_t expiry_ns;      // Optional timeout (0 for none)
    uint64_t start_ns;
    bool one_shot;
    bool cancelled;
};

// Promise states
enum class PromiseState : uint8_t
{
    Pending = 0,
    Fulfilled = 1,
    Rejected = 2
};

// Callback registered on a promise
struct PromiseCallback
{
    int on_fulfilled_ref;    // LUA_REGISTRYINDEX ref to onFulfilled function (or LUA_NOREF)
    int on_rejected_ref;     // LUA_REGISTRYINDEX ref to onRejected function (or LUA_NOREF)
    int next_promise_ref;    // LUA_REGISTRYINDEX ref to chained Promise (or LUA_NOREF)
};

// Promise native data
struct PromiseData
{
    PromiseState state;
    int results_ref;         // LUA_REGISTRYINDEX ref to results array or error object
    int nresults;
    std::vector<PromiseCallback> callbacks;
    std::vector<int> awaiting_thread_refs; // LUA_REGISTRYINDEX refs to threads awaiting this promise
};

// Channel waiter
struct ChannelWaiter
{
    int thread_ref;          // LUA_REGISTRYINDEX ref to waiting thread
    int value_ref;           // LUA_REGISTRYINDEX ref to value (for senders) or LUA_NOREF (for receivers)
};

// Channel native data
struct ChannelData
{
    size_t capacity;
    bool closed;
    std::deque<int> buffer_refs;            // LUA_REGISTRYINDEX refs to buffered values
    std::deque<ChannelWaiter> send_waiters; // Threads waiting to send
    std::deque<ChannelWaiter> recv_waiters; // Threads waiting to receive
};

// Timer handle userdata
struct TimerHandleData
{
    int64_t timer_id;
    uint64_t interval_ns;
    int callback_ref;
    bool repeating;
    bool active;
};

// Native Reactor Engine
class Reactor
{
public:
    explicit Reactor(lua_State* L);
    ~Reactor();

    // Disable copy
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    // Static singleton / lookup per Lua state
    static Reactor* get(lua_State* L);
    static void init_in_state(lua_State* L);

    // Main reactor loop methods
    int step(lua_State* L, int64_t max_wait_ms = 0);
    int run(lua_State* L);
    void stop();
    bool is_running() const { return running_; }
    bool has_pending_work() const;

    // Microtask / Deferral queue
    void defer_thread(lua_State* L, lua_State* co, int nargs = 0);
    void defer_function(lua_State* L, int func_idx, int nargs = 0);

    // Timers
    int64_t schedule_timer(uint64_t delay_ns, int thread_ref, int args_ref, int nargs, bool is_yield_wait, uint64_t interval_ns = 0, bool is_callback = false);
    void cancel_timer(int64_t timer_id);
    void cancel_thread(lua_State* co);

    // I/O Polling
    void register_poll(jaci_socket_t fd, int events, int thread_ref, int callback_ref, uint64_t timeout_ns, bool one_shot = true);
    void cancel_poll(jaci_socket_t fd);

    // Promise management
    void settle_promise(lua_State* L, PromiseData* promise, PromiseState state, int nresults, int results_ref);
    void attach_promise_continuation(lua_State* L, PromiseData* promise, int on_fulfilled_ref, int on_rejected_ref, int next_promise_ref);
    void await_promise(lua_State* L, PromiseData* promise, lua_State* co);

    // Stats
    size_t pending_timers_count() const { return timer_heap_.size(); }
    size_t pending_microtasks_count() const { return microtasks_.size(); }
    size_t pending_io_count() const { return io_watchers_.size(); }

private:
    lua_State* main_L_;
    bool running_;
    bool stop_requested_;
    int64_t next_timer_id_;

    // Min-heap for timers
    std::vector<TimerNode> timer_heap_;

    // Microtask queue (double-buffered)
    std::vector<MicrotaskNode> microtasks_;
    std::vector<MicrotaskNode> active_microtasks_;

    // I/O Watchers
    std::vector<IOWatcher> io_watchers_;

    // Internal helpers
    void process_expired_timers(lua_State* L, uint64_t current_ns);
    void process_microtasks(lua_State* L);
    void poll_io(lua_State* L, int timeout_ms);
    void resume_thread_internal(lua_State* L, lua_State* co, int nargs, bool is_yield_wait = false, double elapsed_sec = 0.0);
    void invoke_callback_internal(lua_State* L, int callback_ref, int args_ref, int nargs);
};

} // namespace Jaci
