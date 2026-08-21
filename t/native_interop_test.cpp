// This file is part of the Luau programming language and is licensed under
// MIT License; see LICENSE.txt for details.
// Copyright (c) 2026 Julia Klee, Roblox Corporation, Lua.org/PUC-Rio.
//
// t_native_interop_test driver:
//   * builds a Lua state with full CodeGen support,
//   * registers a battery of C closures to exercise native-Call dispatch
//     inside loops,
//   * exposes an install_interrupt(limit) helper that installs / disarms
//     an interrupt hook and returns the hit count,
//   * loads and executes t/native_loop_bench.luau,
//   * prints benchmark timings and exits non-zero on any correctness
//     failure.

#include "lua.h"
#include "lualib.h"
#include "luacodegen.h"
#include "luacode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <chrono>

namespace {

// ---------------------------------------------------------------------------
// Interrupt harness
// ---------------------------------------------------------------------------
struct InterruptState
{
    int limit = 0;       // 0 means disarmed
    int hits  = 0;
    bool errored = false;
};

static InterruptState g_interrupt;

static void interruptHook(lua_State* L, int /*gc*/)
{
    // Note: native CodeGen passes -1 for loop/call interrupts; >= 0 is a GC
    // safepoint which we ignore for the counter but still service safely.
    // InterruptLimit from the research is for the bytecode VM; native just
    // uses cb.interrupt directly.
    if (g_interrupt.limit <= 0)
        return;
    ++g_interrupt.hits;
    if (g_interrupt.hits >= g_interrupt.limit && !g_interrupt.errored)
    {
        g_interrupt.errored = true;
        luaL_error(L, "interrupt limit reached (%d)", g_interrupt.limit);
    }
}

// install_interrupt(limit): sets the interrupt hook.
//   limit == 0: disarm the hook and return the total observed hit count.
//   limit >  0: reset hit counter, arm the hook; returns previous hit count.
static int lua_install_interrupt(lua_State* L)
{
    lua_Integer limit = luaL_checkinteger(L, 1);
    int prev = g_interrupt.hits;

    lua_Callbacks* cb = lua_callbacks(L);
    if (limit <= 0)
    {
        cb->interrupt = nullptr;
        g_interrupt.limit = 0;
        lua_pushinteger(L, prev);
        return 1;
    }

    g_interrupt.limit   = int(limit);
    g_interrupt.hits    = 0;
    g_interrupt.errored = false;
    cb->interrupt       = interruptHook;
    lua_pushinteger(L, prev);
    return 1;
}

// ---------------------------------------------------------------------------
// Native / C closures exposed to the benchmark script
// ---------------------------------------------------------------------------

// c_noop0() -> nil
static int c_noop0(lua_State* L)
{
    return 0;
}

// c_id(x) -> x
static int c_id(lua_State* L)
{
    luaL_checkany(L, 1);
    return 1;
}

// c_add(a, b) -> a + b (numeric, exact int arithmetic matches luau ref formula)
static int c_add(lua_State* L)
{
    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_checknumber(L, 2);
    lua_pushnumber(L, a + b);
    return 1;
}

// c_sum(arr, n) -> number, sum of arr[1..n] as numbers
static int c_sum(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer n = luaL_checkinteger(L, 2);
    lua_Number sum = 0.0;
    for (lua_Integer i = 1; i <= n; ++i)
    {
        lua_rawgeti(L, 1, i);
        sum += luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pushnumber(L, sum);
    return 1;
}

// c_transform(buf, n): buf is a numeric table; multiply buf[i] in-place
// by (1.0 + i * 1e-9).  Returns nothing.  Stresses memory reads/writes
// plus argument binding so the benchmark can't hoist the call.
static int c_transform(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer n = luaL_checkinteger(L, 2);
    for (lua_Integer i = 1; i <= n; ++i)
    {
        lua_rawgeti(L, 1, i);
        lua_Number v = luaL_checknumber(L, -1);
        lua_pop(L, 1);
        v *= (1.0 + double(i) * 1e-9);
        lua_pushnumber(L, v);
        lua_rawseti(L, 1, i);
    }
    return 0;
}

// c_mulret() -> 1, 2, 3
static int c_mulret(lua_State* L)
{
    lua_pushinteger(L, 1);
    lua_pushinteger(L, 2);
    lua_pushinteger(L, 3);
    return 3;
}

// ---------------------------------------------------------------------------
// State + test harness helpers
// ---------------------------------------------------------------------------

static std::string readFile(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs)
    {
        fprintf(stderr, "ERROR: cannot open '%s'\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static bool getFieldBool(lua_State* L, const char* name)
{
    lua_getfield(L, -1, name);
    bool r = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return r;
}

static lua_Number getFieldNum(lua_State* L, const char* name)
{
    lua_getfield(L, -1, name);
    lua_Number r = luaL_optnumber(L, -1, 0.0);
    lua_pop(L, 1);
    return r;
}

static lua_Integer getFieldInt(lua_State* L, const char* name)
{
    lua_getfield(L, -1, name);
    lua_Integer r = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> L(luaL_newstate(), lua_close);
    if (!L)
    {
        fprintf(stderr, "ERROR: luaL_newstate failed\n");
        return 3;
    }
    luaL_openlibs(L.get());
    lua_pushcfunction(L.get(), luaopen_os, "os");
    lua_call(L.get(), 0, 0);

    // Bind native CodeGen if supported; the benchmark should still be
    // meaningful without it but correctness is more interesting with CodeGen
    // on.  We don't fail unsupported; we just emit a notice.
    bool codegen = false;
    if (luau_codegen_supported())
    {
        luau_codegen_create(L.get());
        codegen = true;
    }
    else
    {
        fprintf(stderr, "NOTE: luau_codegen_supported() == 0; benchmark will "
                        "run under bytecode VM.\n");
    }

    // Expose native C closures as globals.
    struct Binding
    {
        const char* name;
        lua_CFunction fn;
    };
    const Binding bindings[] = {
        {"c_noop0",         c_noop0},
        {"c_id",            c_id},
        {"c_add",           c_add},
        {"c_sum",           c_sum},
        {"c_transform",     c_transform},
        {"c_mulret",        c_mulret},
        {"install_interrupt", lua_install_interrupt},
    };
    for (const auto& b : bindings)
    {
        lua_pushcfunction(L.get(), b.fn, b.name);
        lua_setglobal(L.get(), b.name);
    }

    // Resolve bench script path.  argv[0] might be absolute or relative;
    // try ./t/native_loop_bench.luau, then t/native_loop_bench.luau next to
    // CWD, then relative to the exe dir.
    std::string script = "t/native_loop_bench.luau";
    auto exists = [](const std::string& p) {
        std::ifstream f(p);
        return f.good();
    };
    if (argc >= 2 && argv[1][0])
        script = argv[1];
    if (!exists(script))
    {
        std::string alt = std::string("./") + script;
        if (exists(alt)) script = alt;
    }
    std::string source = readFile(script);

    // Compile (luacompile) then load.  We use luau_compile/luau_load so
    // that CodeGen compilation hooks fire for the main chunk too.
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
    if (!bytecode)
    {
        fprintf(stderr, "ERROR: luau_compile failed for %s\n", script.c_str());
        return 4;
    }
    int rc = luau_load(L.get(), "@t/native_loop_bench.luau", bytecode, bytecodeSize, 0);
    free(bytecode);
    if (rc != LUA_OK)
    {
        fprintf(stderr, "ERROR: luau_load failed: %s\n", lua_tostring(L.get(), -1));
        return 5;
    }

    // Now CodeGen-compile the main chunk (and inner funcs) before executing.
    if (codegen)
        luau_codegen_compile(L.get(), -1);

    // Optional: pass argument N as first arg to the script via arg table.
    {
        lua_newtable(L.get());
        for (int i = 2; i < argc; ++i)
        {
            lua_pushstring(L.get(), argv[i]);
            lua_rawseti(L.get(), -2, i - 1);
        }
        lua_setglobal(L.get(), "arg");
    }

    auto t0 = std::chrono::steady_clock::now();
    rc = lua_pcall(L.get(), 0, LUA_MULTRET, 0);
    auto t1 = std::chrono::steady_clock::now();
    double wallSec = std::chrono::duration<double>(t1 - t0).count();

    if (rc != LUA_OK)
    {
        fprintf(stderr, "ERROR: lua_pcall failed: %s\n", lua_tostring(L.get(), -1));
        return 6;
    }

    // Expect exactly one table return.
    if (lua_gettop(L.get()) != 1 || lua_type(L.get(), -1) != LUA_TTABLE)
    {
        fprintf(stderr, "ERROR: script did not return a result table\n");
        return 7;
    }

    lua_Number for_ms    = getFieldNum(L.get(), "for_ms");
    lua_Number while_ms  = getFieldNum(L.get(), "while_ms");
    lua_Number rep_ms    = getFieldNum(L.get(), "repeat_ms");
    lua_Number ipairs_ms = getFieldNum(L.get(), "ipairs_ms");
    lua_Number empty_ms  = getFieldNum(L.get(), "empty_ms");
    lua_Integer csum    = getFieldInt(L.get(), "checksum");
    lua_Integer ips     = getFieldInt(L.get(), "iters_per_sec");
    lua_Integer ihits   = getFieldInt(L.get(), "interrupt_hits");
    lua_Integer n       = getFieldInt(L.get(), "n");
    bool match           = getFieldBool(L.get(), "checks_match");
    bool iok             = getFieldBool(L.get(), "interrupt_ok");

    printf("native_loop_bench (n = %lld, codegen = %s):\n", static_cast<long long>(n), codegen ? "yes" : "no");
    printf("  for_ms        = %.4f\n", for_ms);
    printf("  while_ms      = %.4f\n", while_ms);
    printf("  repeat_ms     = %.4f\n", rep_ms);
    printf("  ipairs_ms     = %.4f\n", ipairs_ms);
    printf("  empty_ms      = %.4f\n", empty_ms);
    printf("  iters_per_sec = %lld\n", static_cast<long long>(ips));
    printf("  checksum      = %lld\n", static_cast<long long>(csum));
    printf("  checks_match  = %s\n", match ? "true" : "false");
    printf("  interrupt_ok  = %s (hits = %lld)\n", iok ? "true" : "false", static_cast<long long>(ihits));
    printf("  wall_total    = %.4f s\n", wallSec);

    int failures = 0;
    if (!match)
    {
        fprintf(stderr, "FAIL: loop checksums do not match reference\n");
        ++failures;
    }
    if (!iok)
    {
        fprintf(stderr, "FAIL: interrupt did not fire inside native loops (hits=%lld)\n", static_cast<long long>(ihits));
        ++failures;
    }
    if (ihits < 200)
    {
        fprintf(stderr, "FAIL: interrupt hit count too low (expected >= 200, got %lld)\n", static_cast<long long>(ihits));
        ++failures;
    }

    return failures == 0 ? 0 : 1;
}
