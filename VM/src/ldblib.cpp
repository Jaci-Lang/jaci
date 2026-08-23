// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "lualib.h"

#include "lvm.h"
#include "lapi.h"
#include "lstate.h"
#include "lobject.h"
#include "lfunc.h"
#include "lstring.h"
#include "ltable.h"
#include "lgc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

static int absIndex(lua_State* L, int idx)
{
    return (idx > 0 || idx <= LUA_REGISTRYINDEX) ? idx : lua_gettop(L) + idx + 1;
}

static lua_State* getthread(lua_State* L, int* arg)
{
    if (lua_isthread(L, 1))
    {
        *arg = 1;
        return lua_tothread(L, 1);
    }
    else
    {
        *arg = 0;
        return L;
    }
}

static int db_info(lua_State* L)
{
    int arg;
    lua_State* L1 = getthread(L, &arg);
    int l1top = 0;

    // if L1 != L, L1 can be in any state, and therefore there are no guarantees about its stack space
    if (L != L1)
    {
        // for 'f' option, we reserve one slot and we also record the stack top
        lua_rawcheckstack(L1, 1);

        l1top = lua_gettop(L1);
    }

    int level;
    if (lua_isnumber(L, arg + 1))
    {
        level = (int)lua_tointeger(L, arg + 1);
        luaL_argcheck(L, level >= 0, arg + 1, "level can't be negative");
    }
    else if (arg == 0 && lua_isfunction(L, 1))
    {
        // convert absolute index to relative index
        level = -lua_gettop(L);
    }
    else
        luaL_argerror(L, arg + 1, "function or level expected");

    const char* options = luaL_checkstring(L, arg + 2);

    lua_Debug ar;
    if (!lua_getinfo(L1, level, options, &ar))
        return 0;

    int results = 0;
    bool occurs[26] = {};

    for (const char* it = options; *it; ++it)
    {
        if (unsigned(*it - 'a') < 26)
        {
            if (occurs[*it - 'a'])
            {
                // restore stack state of another thread as 'f' option might not have been visited yet
                if (L != L1)
                    lua_settop(L1, l1top);

                luaL_argerror(L, arg + 2, "duplicate option");
            }
            occurs[*it - 'a'] = true;
        }

        switch (*it)
        {
        case 's':
            lua_pushstring(L, ar.short_src);
            results++;
            break;

        case 'l':
            lua_pushinteger(L, ar.currentline);
            results++;
            break;

        case 'n':
            lua_pushstring(L, ar.name ? ar.name : "");
            results++;
            break;

        case 'f':
            if (L1 == L)
                lua_pushvalue(L, -1 - results); // function is right before results
            else
                lua_xmove(L1, L, 1); // function is at top of L1
            results++;
            break;

        case 'a':
            lua_pushinteger(L, ar.nparams);
            lua_pushboolean(L, ar.isvararg);
            results += 2;
            break;

        default:
            // restore stack state of another thread as 'f' option might not have been visited yet
            if (L != L1)
                lua_settop(L1, l1top);

            luaL_argerror(L, arg + 2, "invalid option");
        }
    }

    return results;
}

static int db_getlocal(lua_State* L)
{
    int arg;
    lua_State* L1 = getthread(L, &arg);
    int level = luaL_checkinteger(L, arg + 1);
    int n = luaL_checkinteger(L, arg + 2);
    luaL_argcheck(L, level >= 0, arg + 1, "level can't be negative");
    luaL_argcheck(L, n > 0, arg + 2, "index must be positive");

    const char* name = lua_getlocal(L1, level, n);
    if (name)
    {
        if (L != L1)
            lua_xmove(L1, L, 1);
        lua_pushstring(L, name);
        lua_insert(L, -2);
        return 2;
    }
    return 0;
}

static int db_setlocal(lua_State* L)
{
    int arg;
    lua_State* L1 = getthread(L, &arg);
    int level = (int)luaL_checkinteger(L, arg + 1);
    int n = (int)luaL_checkinteger(L, arg + 2);
    luaL_checkany(L, arg + 3);
    luaL_argcheck(L, level >= 0, arg + 1, "level can't be negative");
    luaL_argcheck(L, n > 0, arg + 2, "index must be positive");

    lua_settop(L, arg + 3);
    if (L != L1)
        lua_xmove(L, L1, 1);

    const char* name = lua_setlocal(L1, level, n);
    if (name)
    {
        lua_pushstring(L, name);
        return 1;
    }
    return 0;
}

static int db_getlocals(lua_State* L)
{
    int arg;
    lua_State* L1 = getthread(L, &arg);
    int level = (int)luaL_checkinteger(L, arg + 1);
    luaL_argcheck(L, level >= 0, arg + 1, "level can't be negative");

    lua_newtable(L);
    int n = 1;
    while (const char* name = lua_getlocal(L1, level, n))
    {
        if (L != L1)
            lua_xmove(L1, L, 1);
        lua_setfield(L, -2, name);
        n++;
    }
    return 1;
}

static int db_getupvalue(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int n = 0;
    if (lua_isnumber(L, 2))
    {
        n = (int)lua_tointeger(L, 2);
        luaL_argcheck(L, n > 0, 2, "index must be positive");
    }
    else if (lua_isstring(L, 2))
    {
        const char* target = lua_tostring(L, 2);
        int idx = 1;
        while (const char* uname = lua_getupvalue(L, 1, idx))
        {
            if (strcmp(uname, target) == 0)
            {
                lua_pushstring(L, uname);
                lua_insert(L, -2);
                return 2;
            }
            lua_pop(L, 1);
            idx++;
        }
        return 0;
    }
    else
    {
        luaL_argerror(L, 2, "number or string expected");
    }

    const char* name = lua_getupvalue(L, 1, n);
    if (name)
    {
        lua_pushstring(L, name);
        lua_insert(L, -2);
        return 2;
    }
    return 0;
}

static int db_setupvalue(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_checkany(L, 3);

    int n = 0;
    if (lua_isnumber(L, 2))
    {
        n = (int)lua_tointeger(L, 2);
        luaL_argcheck(L, n > 0, 2, "index must be positive");
    }
    else if (lua_isstring(L, 2))
    {
        const char* target = lua_tostring(L, 2);
        int idx = 1;
        while (const char* uname = lua_getupvalue(L, 1, idx))
        {
            lua_pop(L, 1);
            if (strcmp(uname, target) == 0)
            {
                n = idx;
                break;
            }
            idx++;
        }
        if (n == 0)
            return 0;
    }
    else
    {
        luaL_argerror(L, 2, "number or string expected");
    }

    lua_settop(L, 3);
    const char* name = lua_setupvalue(L, 1, n);
    if (name)
    {
        lua_pushstring(L, name);
        return 1;
    }
    return 0;
}

static int db_getupvalues(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_newtable(L);
    int n = 1;
    while (const char* name = lua_getupvalue(L, 1, n))
    {
        lua_setfield(L, -2, name);
        n++;
    }
    return 1;
}

static int db_getmetatable(lua_State* L)
{
    luaL_checkany(L, 1);
    if (!lua_getmetatable(L, 1))
        lua_pushnil(L);
    return 1;
}

static int db_setmetatable(lua_State* L)
{
    int t = lua_type(L, 2);
    luaL_argcheck(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table expected");
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
}

static int db_getregistry(lua_State* L)
{
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    return 1;
}

static int db_getfenv(lua_State* L)
{
    luaL_checkany(L, 1);
    lua_getfenv(L, 1);
    return 1;
}

static int db_setfenv(lua_State* L)
{
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);
    if (lua_setfenv(L, 1) == 0)
        luaL_error(L, "'setfenv' cannot change environment of given object");
    return 1;
}

static int db_getconstants(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const TValue* o = luaA_toobject(L, 1);
    Closure* cl = clvalue(o);
    if (cl->isC)
    {
        lua_newtable(L);
        return 1;
    }
    Proto* p = cl->l.p;
    lua_createtable(L, p->sizek, 0);
    for (int i = 0; i < p->sizek; ++i)
    {
        luaA_pushvalue(L, &p->k[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int db_getconstant(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int idx = (int)luaL_checkinteger(L, 2);
    const TValue* o = luaA_toobject(L, 1);
    Closure* cl = clvalue(o);
    if (cl->isC)
    {
        lua_pushnil(L);
        return 1;
    }
    Proto* p = cl->l.p;
    if (idx < 1 || idx > p->sizek)
    {
        lua_pushnil(L);
        return 1;
    }
    luaA_pushvalue(L, &p->k[idx - 1]);
    return 1;
}

static int db_getprotos(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const TValue* o = luaA_toobject(L, 1);
    Closure* cl = clvalue(o);
    if (cl->isC)
    {
        lua_newtable(L);
        return 1;
    }
    Proto* p = cl->l.p;
    lua_createtable(L, p->sizep, 0);
    for (int i = 0; i < p->sizep; ++i)
    {
        Closure* ncl = luaF_newLclosure(L, 0, cl->env, p->p[i]);
        TValue val;
        setclvalue(L, &val, ncl);
        luaA_pushvalue(L, &val);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int db_getproto(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int idx = (int)luaL_checkinteger(L, 2);
    const TValue* o = luaA_toobject(L, 1);
    Closure* cl = clvalue(o);
    if (cl->isC)
    {
        lua_pushnil(L);
        return 1;
    }
    Proto* p = cl->l.p;
    if (idx < 1 || idx > p->sizep)
    {
        lua_pushnil(L);
        return 1;
    }
    Closure* ncl = luaF_newLclosure(L, 0, cl->env, p->p[idx - 1]);
    TValue val;
    setclvalue(L, &val, ncl);
    luaA_pushvalue(L, &val);
    return 1;
}

static int db_getstack(lua_State* L)
{
    int arg;
    lua_State* L1 = getthread(L, &arg);
    int level = (int)luaL_checkinteger(L, arg + 1);
    luaL_argcheck(L, level >= 0, arg + 1, "level can't be negative");

    int numFrames = (int)(L1->ci - L1->base_ci);
    if (level >= numFrames)
        luaL_error(L, "level out of range");

    CallInfo* ci = L1->ci - level;
    if (ci->flags & LUA_CALLINFO_NATIVE)
    {
        if (lua_isnoneornil(L, arg + 2))
        {
            lua_newtable(L);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    int stackSize = (int)(ci->top - ci->base);
    if (stackSize < 0)
        stackSize = 0;

    if (!lua_isnoneornil(L, arg + 2))
    {
        int idx = (int)luaL_checkinteger(L, arg + 2);
        if (idx < 1 || idx > stackSize)
        {
            lua_pushnil(L);
            return 1;
        }
        luaA_pushvalue(L, ci->base + (idx - 1));
        if (L != L1)
            lua_xmove(L1, L, 1);
        return 1;
    }

    lua_createtable(L, stackSize, 0);
    for (int i = 0; i < stackSize; ++i)
    {
        luaA_pushvalue(L, ci->base + i);
        if (L != L1)
            lua_xmove(L1, L, 1);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int db_dumpstack(lua_State* L)
{
    int arg;
    lua_State* L1 = getthread(L, &arg);
    int startLevel = (int)luaL_optinteger(L, arg + 1, (L == L1) ? 1 : 0);

    lua_newtable(L);
    int count = 1;
    lua_Debug ar;

    for (int level = startLevel; lua_getinfo(L1, level, "slna", &ar); ++level)
    {
        lua_createtable(L, 0, 6);

        lua_pushinteger(L, level);
        lua_setfield(L, -2, "level");

        lua_pushstring(L, ar.short_src);
        lua_setfield(L, -2, "source");

        lua_pushinteger(L, ar.currentline);
        lua_setfield(L, -2, "line");

        lua_pushstring(L, ar.name ? ar.name : "");
        lua_setfield(L, -2, "name");

        lua_pushinteger(L, ar.nparams);
        lua_setfield(L, -2, "nparams");

        lua_pushboolean(L, ar.isvararg);
        lua_setfield(L, -2, "isvararg");

        lua_rawseti(L, -2, count++);
    }

    return 1;
}

static int db_traceback(lua_State* L)
{
    int arg;
    lua_State* L1 = getthread(L, &arg);
    const char* msg = luaL_optstring(L, arg + 1, NULL);
    int level = (int)luaL_optinteger(L, arg + 2, (L == L1) ? 1 : 0);
    luaL_argcheck(L, level >= 0, arg + 2, "level can't be negative");

    luaL_traceback(L, L1, msg, level);

    return 1;
}

static int db_setwarnhandler(lua_State* L)
{
    int t = lua_type(L, 1);
    luaL_argcheck(L, t == LUA_TNIL || t == LUA_TFUNCTION, 1, "nil or function expected");
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "_WARN_HANDLER");
    return 0;
}

static int db_getwarnhandler(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "_WARN_HANDLER");
    return 1;
}

static int global_strict_index(lua_State* L)
{
    const char* key = luaL_tolstring(L, 2, NULL);
    lua_pop(L, 1);
    lua_getglobal(L, "warn");
    if (lua_isfunction(L, -1))
    {
        lua_pushfstring(L, "Warning: attempt to access undeclared global variable '%s'", key ? key : "?");
        lua_call(L, 1, 0);
    }
    else
    {
        lua_pop(L, 1);
        fprintf(stderr, "Warning: attempt to access undeclared global variable '%s'\n", key ? key : "?");
        fflush(stderr);
    }
    lua_pushnil(L);
    return 1;
}

static int global_strict_newindex(lua_State* L)
{
    const char* key = luaL_tolstring(L, 2, NULL);
    lua_pop(L, 1);
    lua_getglobal(L, "warn");
    if (lua_isfunction(L, -1))
    {
        lua_pushfstring(L, "Warning: assignment to undeclared global variable '%s'", key ? key : "?");
        lua_call(L, 1, 0);
    }
    else
    {
        lua_pop(L, 1);
        fprintf(stderr, "Warning: assignment to undeclared global variable '%s'\n", key ? key : "?");
        fflush(stderr);
    }
    lua_rawset(L, 1);
    return 0;
}

static int db_setglobalwarning(lua_State* L)
{
    bool enabled = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
    lua_pushvalue(L, LUA_GLOBALSINDEX);

    if (enabled)
    {
        if (!lua_getmetatable(L, -1))
        {
            lua_newtable(L);
        }
        lua_pushcfunction(L, global_strict_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, global_strict_newindex, "__newindex");
        lua_setfield(L, -2, "__newindex");
        lua_setmetatable(L, -2);
    }
    else
    {
        if (lua_getmetatable(L, -1))
        {
            lua_pushnil(L);
            lua_setfield(L, -2, "__index");
            lua_pushnil(L);
            lua_setfield(L, -2, "__newindex");
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 0;
}

struct InspectOptions
{
    int maxDepth = 5;
    std::string indent = "  ";
    bool showMetatables = false;
    bool sortKeys = true;
    bool compact = false;
    bool showFunctions = true;
};

static InspectOptions parseInspectOptions(lua_State* L, int idx)
{
    InspectOptions opts;
    if (lua_istable(L, idx))
    {
        lua_getfield(L, idx, "depth");
        if (lua_isnumber(L, -1))
            opts.maxDepth = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, idx, "indent");
        if (lua_isstring(L, -1))
            opts.indent = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, idx, "showMetatables");
        if (!lua_isnil(L, -1))
            opts.showMetatables = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, idx, "sortKeys");
        if (!lua_isnil(L, -1))
            opts.sortKeys = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, idx, "compact");
        if (!lua_isnil(L, -1))
            opts.compact = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, idx, "showFunctions");
        if (!lua_isnil(L, -1))
            opts.showFunctions = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    return opts;
}

static void escapeString(const char* s, size_t len, std::string& out)
{
    out += '"';
    for (size_t i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\0':
            out += "\\0";
            break;
        default:
            if (c < 32 || c >= 127)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\%03u", c);
                out += buf;
            }
            else
            {
                out += (char)c;
            }
            break;
        }
    }
    out += '"';
}

static bool isIdentifier(const std::string& s)
{
    if (s.empty())
        return false;
    if (!isalpha((unsigned char)s[0]) && s[0] != '_')
        return false;
    for (char c : s)
    {
        if (!isalnum((unsigned char)c) && c != '_')
            return false;
    }
    static const char* const keywords[] = {
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "if", "in", "local", "nil", "not", "or", "repeat",
        "return", "then", "true", "until", "while", nullptr
    };
    for (int i = 0; keywords[i]; ++i)
    {
        if (s == keywords[i])
            return false;
    }
    return true;
}

static void inspectValue(
    lua_State* L,
    int idx,
    int depth,
    const InspectOptions& opts,
    std::unordered_set<const void*>& visited,
    std::string& out
)
{
    int absIdx = absIndex(L, idx);
    int t = lua_type(L, absIdx);

    switch (t)
    {
    case LUA_TNIL:
        out += "nil";
        break;

    case LUA_TBOOLEAN:
        out += lua_toboolean(L, absIdx) ? "true" : "false";
        break;

    case LUA_TNUMBER:
    {
        double n = lua_tonumber(L, absIdx);
        if (std::isnan(n))
            out += "nan";
        else if (std::isinf(n))
            out += (n < 0 ? "-inf" : "inf");
        else
        {
            char buf[64];
            if (n == (double)(long long)n && !std::isinf(n))
                snprintf(buf, sizeof(buf), "%lld", (long long)n);
            else
                snprintf(buf, sizeof(buf), "%.14g", n);
            out += buf;
        }
        break;
    }

    case LUA_TSTRING:
    {
        size_t len = 0;
        const char* s = lua_tolstring(L, absIdx, &len);
        escapeString(s, len, out);
        break;
    }

    case LUA_TFUNCTION:
    {
        const void* ptr = lua_topointer(L, absIdx);
        lua_Debug ar;
        lua_pushvalue(L, absIdx);
        if (lua_getinfo(L, -lua_gettop(L), "snla", &ar))
        {
            char buf[256];
            if (ar.what && strcmp(ar.what, "C") == 0)
            {
                snprintf(buf, sizeof(buf), "function: %p (C)", ptr);
            }
            else
            {
                const char* name = (ar.name && *ar.name) ? ar.name : "<anonymous>";
                snprintf(buf, sizeof(buf), "function: %s (%s:%d)", name, ar.short_src ? ar.short_src : "?", ar.currentline > 0 ? ar.currentline : ar.linedefined);
            }
            out += buf;
        }
        else
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "function: %p", ptr);
            out += buf;
        }
        lua_pop(L, 1);
        break;
    }

    case LUA_TTHREAD:
    {
        const void* ptr = lua_topointer(L, absIdx);
        char buf[64];
        snprintf(buf, sizeof(buf), "thread: %p", ptr);
        out += buf;
        break;
    }

    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
    {
        const void* ptr = lua_topointer(L, absIdx);
        const char* typeName = luaL_typename(L, absIdx);
        char buf[128];
        snprintf(buf, sizeof(buf), "userdata: %p (%s)", ptr, typeName ? typeName : "userdata");
        out += buf;
        break;
    }

    case LUA_TBUFFER:
    {
        size_t len = lua_objlen(L, absIdx);
        char buf[64];
        snprintf(buf, sizeof(buf), "buffer(size: %zu)", len);
        out += buf;
        break;
    }

    case LUA_TVECTOR:
    {
        const float* v = lua_tovector(L, absIdx);
        char buf[128];
#if LUA_VECTOR_SIZE == 4
        if (v)
            snprintf(buf, sizeof(buf), "vector(%g, %g, %g, %g)", v[0], v[1], v[2], v[3]);
        else
            snprintf(buf, sizeof(buf), "vector(0, 0, 0, 0)");
#else
        if (v)
            snprintf(buf, sizeof(buf), "vector(%g, %g, %g)", v[0], v[1], v[2]);
        else
            snprintf(buf, sizeof(buf), "vector(0, 0, 0)");
#endif
        out += buf;
        break;
    }

    case LUA_TTABLE:
    {
        const void* ptr = lua_topointer(L, absIdx);
        if (visited.find(ptr) != visited.end())
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "<cycle: %p>", ptr);
            out += buf;
            return;
        }

        if (depth >= opts.maxDepth)
        {
            out += "{...}";
            return;
        }

        visited.insert(ptr);

        int arrLen = (int)lua_objlen(L, absIdx);

        struct Entry
        {
            std::string keyStr;
            std::string valStr;
            bool isArrayItem;
            int arrayIndex;
            int keyType;
            double keyNumber;
        };
        std::vector<Entry> entries;

        lua_pushnil(L);
        while (lua_next(L, absIdx) != 0)
        {
            int ktype = lua_type(L, -2);
            bool isArr = false;
            int arrIdx = 0;
            if (ktype == LUA_TNUMBER)
            {
                double d = lua_tonumber(L, -2);
                if (d >= 1.0 && d <= (double)arrLen && d == (double)(int)d)
                {
                    isArr = true;
                    arrIdx = (int)d;
                }
            }

            std::string valStr;
            inspectValue(L, -1, depth + 1, opts, visited, valStr);

            std::string keyStr;
            double keyNum = 0;
            if (ktype == LUA_TSTRING)
            {
                size_t slen = 0;
                const char* s = lua_tolstring(L, -2, &slen);
                std::string rawKey(s, slen);
                if (isIdentifier(rawKey))
                    keyStr = rawKey;
                else
                {
                    std::string esc;
                    escapeString(s, slen, esc);
                    keyStr = "[" + esc + "]";
                }
            }
            else if (ktype == LUA_TNUMBER)
            {
                keyNum = lua_tonumber(L, -2);
                char buf[64];
                if (keyNum == (double)(long long)keyNum)
                    snprintf(buf, sizeof(buf), "[%lld]", (long long)keyNum);
                else
                    snprintf(buf, sizeof(buf), "[%.14g]", keyNum);
                keyStr = buf;
            }
            else
            {
                std::string kEsc;
                inspectValue(L, -2, depth + 1, opts, visited, kEsc);
                keyStr = "[" + kEsc + "]";
            }

            entries.push_back({keyStr, valStr, isArr, arrIdx, ktype, keyNum});
            lua_pop(L, 1);
        }

        if (opts.showMetatables && lua_getmetatable(L, absIdx))
        {
            std::string mtStr;
            inspectValue(L, -1, depth + 1, opts, visited, mtStr);
            entries.push_back({"[\"<metatable>\"]", mtStr, false, 0, LUA_TSTRING, 0});
            lua_pop(L, 1);
        }

        if (opts.sortKeys)
        {
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
                if (a.isArrayItem && b.isArrayItem)
                    return a.arrayIndex < b.arrayIndex;
                if (a.isArrayItem != b.isArrayItem)
                    return a.isArrayItem;
                if (a.keyType == LUA_TNUMBER && b.keyType == LUA_TNUMBER)
                    return a.keyNumber < b.keyNumber;
                return a.keyStr < b.keyStr;
            });
        }

        if (entries.empty())
        {
            out += "{}";
        }
        else if (opts.compact)
        {
            out += "{";
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (i > 0)
                    out += ", ";
                if (entries[i].isArrayItem)
                    out += entries[i].valStr;
                else
                    out += entries[i].keyStr + " = " + entries[i].valStr;
            }
            out += "}";
        }
        else
        {
            out += "{\n";
            std::string curIndent;
            for (int i = 0; i < depth + 1; ++i)
                curIndent += opts.indent;
            std::string closeIndent;
            for (int i = 0; i < depth; ++i)
                closeIndent += opts.indent;

            for (size_t i = 0; i < entries.size(); ++i)
            {
                out += curIndent;
                if (entries[i].isArrayItem)
                    out += entries[i].valStr;
                else
                    out += entries[i].keyStr + " = " + entries[i].valStr;

                if (i + 1 < entries.size())
                    out += ",\n";
                else
                    out += "\n";
            }
            out += closeIndent + "}";
        }

        visited.erase(ptr);
        break;
    }

    default:
    {
        const char* typeName = luaL_typename(L, absIdx);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %p", typeName ? typeName : "unknown", lua_topointer(L, absIdx));
        out += buf;
        break;
    }
    }
}

static int db_inspect(lua_State* L)
{
    luaL_checkany(L, 1);
    InspectOptions opts = parseInspectOptions(L, 2);
    std::unordered_set<const void*> visited;
    std::string out;
    inspectValue(L, 1, 0, opts, visited, out);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int db_dump(lua_State* L)
{
    luaL_checkany(L, 1);
    InspectOptions opts = parseInspectOptions(L, 2);
    std::unordered_set<const void*> visited;
    std::string out;
    inspectValue(L, 1, 0, opts, visited, out);
    printf("%s\n", out.c_str());
    fflush(stdout);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static const luaL_Reg dblib[] = {
    {"info", db_info},
    {"getinfo", db_info},
    {"traceback", db_traceback},
    {"getlocal", db_getlocal},
    {"getlocals", db_getlocals},
    {"setlocal", db_setlocal},
    {"getupvalue", db_getupvalue},
    {"getupvalues", db_getupvalues},
    {"setupvalue", db_setupvalue},
    {"dumpstack", db_dumpstack},
    {"getmetatable", db_getmetatable},
    {"setmetatable", db_setmetatable},
    {"getregistry", db_getregistry},
    {"getfenv", db_getfenv},
    {"setfenv", db_setfenv},
    {"getconstants", db_getconstants},
    {"getconstant", db_getconstant},
    {"getprotos", db_getprotos},
    {"getproto", db_getproto},
    {"getstack", db_getstack},
    {"inspect", db_inspect},
    {"dump", db_dump},
    {"setwarnhandler", db_setwarnhandler},
    {"getwarnhandler", db_getwarnhandler},
    {"setglobalwarning", db_setglobalwarning},
    {"strictglobals", db_setglobalwarning},
    {NULL, NULL},
};

int luaopen_debug(lua_State* L)
{
    luaL_register(L, LUA_DBLIBNAME, dblib);
    return 1;
}
