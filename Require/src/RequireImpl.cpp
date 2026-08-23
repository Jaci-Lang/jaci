// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "RequireImpl.h"

#include "Navigation.h"

#include "Luau/RequireNavigator.h"
#include "Luau/Require.h"

#include "lua.h"
#include "lualib.h"
#include <stdlib.h>
#include <stdio.h>

LUAU_FASTFLAGVARIABLE(LuauCyclicRequireShortCircuit)

namespace Luau::Require
{

// Stores explicitly registered modules.
static const char* registeredCacheTableKey = "_REGISTEREDMODULES";

// Stores the results of require calls.
static const char* requiredCacheTableKey = "_MODULES";

// Fast-path resolution cache mapping composite require key (requirerChunkname \0 path) to cacheKey.
static const char* resolvedPathCacheTableKey = "_RESOLVED_REQUIRES";

// Sentinel address used as a unique registry key for the shared placeholder metatable.
static char cyclicPlaceholderMetatableSentinel = 0;

// Tracks which placeholders were actually returned to a cyclic requirer.
static const char* cyclicPlaceholderProvidedKey = "_CYCLIC_PLACEHOLDER_PROVIDED";

struct ResolvedRequire
{
    static ResolvedRequire fromErrorHandler(const RuntimeErrorHandler& errorHandler)
    {
        return {ResolvedRequire::Status::ErrorReported, "", "", "", errorHandler.getReportedError()};
    }

    static ResolvedRequire fromErrorMessage(const char* message)
    {
        return {ResolvedRequire::Status::ErrorReported, "", "", "", message};
    }

    enum class Status
    {
        Cached,
        ModuleRead,
        ErrorReported
    };

    Status status;
    std::string chunkname;
    std::string loadname;
    std::string cacheKey;
    std::string error;
};

static bool isCached(lua_State* L, const std::string& key)
{
    luaL_findtable(L, LUA_REGISTRYINDEX, requiredCacheTableKey, 1);
    lua_getfield(L, -1, key.c_str());
    bool cached = !lua_isnil(L, -1);

    if (FFlag::LuauCyclicRequireShortCircuit && cached && lua_istable(L, -1))
    {
        // Check if the cached value is a placeholder (has the shared placeholder metatable).
        if (lua_getmetatable(L, -1) == 1)
        {
            lua_rawgetp(L, LUA_REGISTRYINDEX, &cyclicPlaceholderMetatableSentinel);
            if (lua_rawequal(L, -1, -2) == 1)
            {
                // A cyclic require is accessing this placeholder — mark it as provided.
                luaL_findtable(L, LUA_REGISTRYINDEX, cyclicPlaceholderProvidedKey, 1);
                lua_pushboolean(L, 1);
                lua_setfield(L, -2, key.c_str());
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // pop both metatables
        }
    }

    lua_pop(L, 2);

    return cached;
}

// Checks if the given cache key is present in _MODULES and pushes the cached module to top of stack if so.
static bool checkCachedModule(lua_State* L, const char* cacheKey)
{
    luaL_findtable(L, LUA_REGISTRYINDEX, requiredCacheTableKey, 1);
    lua_getfield(L, -1, cacheKey);
    bool cached = !lua_isnil(L, -1);

    if (FFlag::LuauCyclicRequireShortCircuit && cached && lua_istable(L, -1))
    {
        if (lua_getmetatable(L, -1) == 1)
        {
            lua_rawgetp(L, LUA_REGISTRYINDEX, &cyclicPlaceholderMetatableSentinel);
            if (lua_rawequal(L, -1, -2) == 1)
            {
                luaL_findtable(L, LUA_REGISTRYINDEX, cyclicPlaceholderProvidedKey, 1);
                lua_pushboolean(L, 1);
                lua_setfield(L, -2, cacheKey);
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
        }
    }

    if (cached)
    {
        lua_remove(L, -2);
        return true;
    }

    lua_pop(L, 2);
    return false;
}

static ResolvedRequire resolveRequire(luarequire_Configuration* lrc, lua_State* L, void* ctx, const char* requirerChunkname, std::string path)
{
    if (getenv("KLEDBG_REQUIRE"))
    {
        fprintf(stderr, "[resolveRequire] chunkname='%s' path='%s' lrc=%p allowed_fp=%p\n", requirerChunkname ? requirerChunkname : "(null)", path.c_str(), (void*)lrc, (void*)(lrc->is_require_allowed));
        fflush(stderr);
    }
    bool allowed = lrc->is_require_allowed(L, ctx, requirerChunkname);
    if (getenv("KLEDBG_REQUIRE"))
    {
        fprintf(stderr, "[resolveRequire] allowed=%d\n", (int)allowed);
        fflush(stderr);
    }
    if (!allowed)
        return ResolvedRequire::fromErrorMessage("require is not supported in this context");

    RuntimeNavigationContext navigationContext{lrc, L, ctx, requirerChunkname};
    RuntimeErrorHandler errorHandler{path};

    Navigator navigator(navigationContext, errorHandler);

    // Updates navigationContext while navigating through the given path.
    Navigator::Status status = navigator.navigate(std::move(path));
    if (status == Navigator::Status::ErrorReported)
        return ResolvedRequire::fromErrorHandler(errorHandler);

    if (!navigationContext.isModulePresent())
        return ResolvedRequire::fromErrorMessage("no module present at resolved path");

    std::optional<std::string> cacheKey = navigationContext.getCacheKey();
    if (!cacheKey)
        return ResolvedRequire::fromErrorMessage("could not get cache key for module");

    if (isCached(L, *cacheKey))
    {
        // Put cached result on top of stack before returning.
        lua_getfield(L, LUA_REGISTRYINDEX, requiredCacheTableKey);
        lua_getfield(L, -1, cacheKey->c_str());
        lua_remove(L, -2);

        return ResolvedRequire{ResolvedRequire::Status::Cached, "", "", std::move(*cacheKey), ""};
    }

    std::optional<std::string> chunkname = navigationContext.getChunkname();
    if (!chunkname)
        return ResolvedRequire::fromErrorMessage("could not get chunkname for module");

    std::optional<std::string> loadname = navigationContext.getLoadname();
    if (!loadname)
        return ResolvedRequire::fromErrorMessage("could not get loadname for module");

    return ResolvedRequire{
        ResolvedRequire::Status::ModuleRead,
        std::move(*chunkname),
        std::move(*loadname),
        std::move(*cacheKey),
    };
}

static int checkRegisteredModules(lua_State* L, const char* path)
{
    size_t pathLen = strlen(path);
    char pathLowerBuf[256];
    char* pathLower = pathLowerBuf;
    std::string pathLowerHeap;
    if (pathLen >= sizeof(pathLowerBuf))
    {
        pathLowerHeap.resize(pathLen);
        pathLower = &pathLowerHeap[0];
    }
    for (size_t i = 0; i < pathLen; ++i)
    {
        char c = path[i];
        if (c >= 'A' && c <= 'Z')
            c += ('a' - 'A');
        pathLower[i] = c;
    }
    pathLower[pathLen] = '\0';

    luaL_findtable(L, LUA_REGISTRYINDEX, registeredCacheTableKey, 1);
    lua_getfield(L, -1, pathLower);
    if (getenv("KLEDBG_REQUIRE"))
        fprintf(stderr, "[checkRegistered] path='%s' found=%d\n", path, !lua_isnil(L, -1) ? 1 : 0);
    if (!lua_isnil(L, -1))
    {
        // Check if the registered value is a cyclic placeholder (not yet resolved).
        // If so, fall through to resolveRequire instead of returning the placeholder.
        if (!lua_istable(L, -1) || lua_getmetatable(L, -1) == 0)
        {
            lua_remove(L, -2);
            return 1;
        }
        lua_rawgetp(L, LUA_REGISTRYINDEX, &cyclicPlaceholderMetatableSentinel);
        if (lua_rawequal(L, -1, -2) != 1)
        {
            // Not a cyclic placeholder; the registered module is fully resolved.
            lua_pop(L, 1); // pop metatable sentinel
            lua_remove(L, -2);
            return 1;
        }
        // It is a cyclic placeholder; fall through to resolveRequire.
        lua_pop(L, 3); // pop registered value, cache table, sentinel
        return 0;
    }
    lua_pop(L, 2);

    // Support @std/<library> or bare standard library module imports (e.g. @std/fs, net, task)
    if (pathLen > 5 && memcmp(pathLower, "@std/", 5) == 0)
    {
        const char* libName = pathLower + 5;
        lua_getglobal(L, libName);
        if (!lua_isnil(L, -1))
            return 1;
        lua_pop(L, 1);
    }
    else
    {
        static const char* kStdLibs[] = {
            "fs", "io", "ffi", "json", "hash", "crypto", "process", "net", "task",
            "math", "table", "string", "coroutine", "bit32", "utf8", "os", "debug",
            "buffer", "vector", "class", "integer", nullptr
        };
        for (int i = 0; kStdLibs[i]; ++i)
        {
            if (strcmp(pathLower, kStdLibs[i]) == 0)
            {
                lua_getglobal(L, kStdLibs[i]);
                if (!lua_isnil(L, -1))
                    return 1;
                lua_pop(L, 1);
                break;
            }
        }
    }

    return 0;
}

static int CyclicDependencyIndexError(lua_State* L)
{
    const char* key = lua_tostring(L, 2);
    luaL_error(L, "Cannot access the exported field '%s' because it has a cyclic dependency on its requiring module", key ? key : "unknown");
}

static int CyclicDependencyNewIndexError(lua_State* L)
{
    const char* key = lua_tostring(L, 2);
    luaL_error(L, "Cannot set the exported field '%s' because it has a cyclic dependency on its requiring module", key ? key : "unknown");
}

// Returns the shared placeholder metatable, creating it on first use.
static void pushCyclicPlaceholderMetatable(lua_State* L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &cyclicPlaceholderMetatableSentinel);
    if (!lua_isnil(L, -1))
        return;
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, CyclicDependencyIndexError, "CyclicDependencyIndexError");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, CyclicDependencyNewIndexError, "CyclicDependencyNewIndexError");
    lua_setfield(L, -2, "__newindex");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");

    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &cyclicPlaceholderMetatableSentinel);
}

void lockPlaceholder(lua_State* L, int idx)
{
    idx = lua_absindex(L, idx);
    pushCyclicPlaceholderMetatable(L);
    lua_setmetatable(L, idx);
    lua_setreadonly(L, idx, 1);
}

void createPlaceholder(lua_State* L)
{
    const char* cacheKey = luaL_checkstring(L, 2);

    lua_newtable(L);
    lockPlaceholder(L, -1);

    luaL_findtable(L, LUA_REGISTRYINDEX, requiredCacheTableKey, 1);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, cacheKey);
    lua_pop(L, 2);
}

void populatePlaceholder(lua_State* L, int placeholderIdx, int resultIdx)
{
    placeholderIdx = lua_absindex(L, placeholderIdx);
    resultIdx = lua_absindex(L, resultIdx);

    // Unfreeze so we can write to the placeholder
    lua_setreadonly(L, placeholderIdx, 0);

    // Copy all fields from the result table into the placeholder
    for (int iter = 0; (iter = lua_rawiter(L, resultIdx, iter)) != -1;)
    {
        // key at -2, value at -1
        lua_rawset(L, placeholderIdx);
    }

    // Copy the metatable from the result (if any)
    if (lua_getmetatable(L, resultIdx) == 0)
        lua_pushnil(L);
    lua_setmetatable(L, placeholderIdx);

    // Freeze the populated placeholder.
    lua_setreadonly(L, placeholderIdx, 1);
}

// Fixed stack slots below the load results:
//   (1) path, (2) cacheKey, (3) chunkname, (4) loadname
static const int kRequireStackValues = 4;

int lua_requirecont(lua_State* L, int status)
{
    LUAU_ASSERT(lua_gettop(L) >= kRequireStackValues);
    const int numResults = lua_gettop(L) - kRequireStackValues;
    const char* cacheKey = luaL_checkstring(L, 2);

    if (numResults > 1)
        luaL_error(L, "module must return a single value");

    if (FFlag::LuauCyclicRequireShortCircuit && numResults == 1)
    {
        const int resultIdx = kRequireStackValues + 1;

        // Check if the placeholder was actually provided to a cyclic requirer.
        luaL_findtable(L, LUA_REGISTRYINDEX, cyclicPlaceholderProvidedKey, 1);
        lua_getfield(L, -1, cacheKey);
        bool wasProvided = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        // Clear the placeholder from the cache to reset the state.
        lua_pushnil(L);
        lua_setfield(L, -2, cacheKey);
        lua_pop(L, 1);

        if (wasProvided)
        {
            LUAU_ASSERT(lua_istable(L, resultIdx));

            // Retrieve the placeholder from the cache.
            luaL_findtable(L, LUA_REGISTRYINDEX, requiredCacheTableKey, 1);
            lua_getfield(L, -1, cacheKey);

            // Populate the placeholder with the module's result.
            // Cyclic importers already hold the placeholder, so they see the result.
            populatePlaceholder(L, -1, resultIdx);

            // Replace the result with the populated placeholder so the initial caller
            // gets the same object that cyclic importers and future cache hits receive.
            lua_replace(L, resultIdx);
            lua_pop(L, 1);
        }
        else
        {
            // Cache the result normally (no cycle occurred).
            luaL_findtable(L, LUA_REGISTRYINDEX, requiredCacheTableKey, 1);
            lua_pushvalue(L, resultIdx);
            lua_setfield(L, -2, cacheKey);
            lua_pop(L, 1);
        }
    }
    else if (numResults == 1)
    {
        // Initial stack state
        // (-1) result
        lua_getfield(L, LUA_REGISTRYINDEX, requiredCacheTableKey);
        // (-2) result, (-1) cache table

        lua_pushvalue(L, -2);
        // (-3) result, (-2) cache table, (-1) result

        lua_setfield(L, -2, cacheKey);
        // (-2) result, (-1) cache table

        lua_pop(L, 1);
        // (-1) result
    }

    return numResults;
}

int lua_requireinternal(lua_State* L, const char* requirerChunkname)
{
    // Discard extra arguments, we only use path
    lua_settop(L, 1);

    luarequire_Configuration* lrc = static_cast<luarequire_Configuration*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!lrc)
        luaL_error(L, "unable to find require configuration");

    void* ctx = lua_tolightuserdata(L, lua_upvalueindex(2));

    // (1) path
    const char* path = luaL_checkstring(L, 1);

    if (checkRegisteredModules(L, path) == 1)
        return 1;

    // Fast-path resolution cache: check if (requirerChunkname, path) was already resolved and cached.
    size_t chunkLen = requirerChunkname ? strlen(requirerChunkname) : 0;
    size_t pathLen = strlen(path);
    char compositeKeyBuf[512];
    char* compositeKey = compositeKeyBuf;
    std::string compositeKeyHeap;
    size_t compositeLen = chunkLen + 1 + pathLen;
    if (compositeLen >= sizeof(compositeKeyBuf))
    {
        compositeKeyHeap.resize(compositeLen);
        compositeKey = &compositeKeyHeap[0];
    }
    if (chunkLen > 0)
        memcpy(compositeKey, requirerChunkname, chunkLen);
    compositeKey[chunkLen] = '\0';
    memcpy(compositeKey + chunkLen + 1, path, pathLen);

    luaL_findtable(L, LUA_REGISTRYINDEX, resolvedPathCacheTableKey, 1);
    lua_pushlstring(L, compositeKey, compositeLen);
    lua_rawget(L, -2);

    if (lua_isstring(L, -1))
    {
        const char* cachedKey = lua_tostring(L, -1);
        if (checkCachedModule(L, cachedKey))
        {
            // Cached module is on top of stack.
            // Stack: (1) path, (2) _RESOLVED_REQUIRES, (3) cachedKey, (4) module
            lua_replace(L, 1);
            lua_settop(L, 1);
            return 1;
        }
    }
    lua_pop(L, 2); // pop rawget result and _RESOLVED_REQUIRES table

    // ResolvedRequire will be destroyed and any string will be pinned to Luau stack, so that luaL_error doesn't need destructors
    bool resolveError = false;

    {
        ResolvedRequire resolvedRequire = resolveRequire(lrc, L, ctx, requirerChunkname, path);

        if (resolvedRequire.status == ResolvedRequire::Status::Cached)
        {
            // Store mapping into resolved path cache
            luaL_findtable(L, LUA_REGISTRYINDEX, resolvedPathCacheTableKey, 1);
            lua_pushlstring(L, compositeKey, compositeLen);
            lua_pushstring(L, resolvedRequire.cacheKey.c_str());
            lua_rawset(L, -3);
            lua_pop(L, 1);

            return 1;
        }

        if (resolvedRequire.status == ResolvedRequire::Status::ErrorReported)
        {
            lua_pushstring(L, resolvedRequire.error.c_str());
            resolveError = true;
        }
        else
        {
            // Store mapping into resolved path cache
            luaL_findtable(L, LUA_REGISTRYINDEX, resolvedPathCacheTableKey, 1);
            lua_pushlstring(L, compositeKey, compositeLen);
            lua_pushstring(L, resolvedRequire.cacheKey.c_str());
            lua_rawset(L, -3);
            lua_pop(L, 1);

            // (1) path, ..., cacheKey, chunkname, loadname
            lua_pushstring(L, resolvedRequire.cacheKey.c_str());
            lua_pushstring(L, resolvedRequire.chunkname.c_str());
            lua_pushstring(L, resolvedRequire.loadname.c_str());
        }
    }

    if (resolveError)
        lua_error(L); // Error already on top of the stack

    const char* chunkname = lua_tostring(L, 3);
    const char* loadname = lua_tostring(L, 4);

    LUAU_ASSERT(lua_gettop(L) == kRequireStackValues);

    int numResults = lrc->load(L, ctx, path, chunkname, loadname);
    if (numResults == -1)
    {
        if (lua_gettop(L) != kRequireStackValues)
            luaL_error(L, "stack cannot be modified when require yields");

        return lua_yield(L, 0);
    }

    return lua_requirecont(L, LUA_OK);
}

int lua_proxyrequire(lua_State* L)
{
    const char* requirerChunkname = luaL_checkstring(L, 2);
    return lua_requireinternal(L, requirerChunkname);
}

int lua_require(lua_State* L)
{
    lua_Debug ar;
    int level = 1;

    do
    {
        if (!lua_getinfo(L, level++, "s", &ar))
            luaL_error(L, "require is not supported in this context");
    } while (ar.what[0] == 'C');

    if (getenv("KLEDBG_REQUIRE"))
        fprintf(stderr, "[lua_require] source='%s' calling_requireinternal\n", ar.source ? ar.source : "(null)");

    int result = lua_requireinternal(L, ar.source);
    if (getenv("KLEDBG_REQUIRE"))
        fprintf(stderr, "[lua_require] returned %d\n", result);
    return result;
}

int registerModuleImpl(lua_State* L)
{
    if (lua_gettop(L) != 2)
        luaL_error(L, "expected 2 arguments: aliased require path and desired result");

    size_t len;
    const char* path = luaL_checklstring(L, 1, &len);
    std::string_view pathView(path, len);
    if (pathView.empty() || pathView[0] != '@')
        luaL_argerrorL(L, 1, "path must begin with '@'");

    // Make path lowercase to ensure case-insensitive matching.
    char pathLowerBuf[256];
    char* pathLower = pathLowerBuf;
    std::string pathLowerHeap;
    if (len >= sizeof(pathLowerBuf))
    {
        pathLowerHeap.resize(len);
        pathLower = &pathLowerHeap[0];
    }
    for (size_t i = 0; i < len; ++i)
    {
        char c = path[i];
        if (c >= 'A' && c <= 'Z')
            c += ('a' - 'A');
        pathLower[i] = c;
    }
    pathLower[len] = '\0';

    lua_pushlstring(L, pathLower, len);
    lua_replace(L, 1);

    luaL_findtable(L, LUA_REGISTRYINDEX, registeredCacheTableKey, 1);
    // (1) path, (2) result, (3) cache table

    lua_insert(L, 1);
    // (1) cache table, (2) path, (3) result

    lua_settable(L, 1);
    // (1) cache table

    lua_pop(L, 1);

    return 0;
}

int clearCacheEntry(lua_State* L)
{
    const char* cacheKey = luaL_checkstring(L, 1);
    luaL_findtable(L, LUA_REGISTRYINDEX, requiredCacheTableKey, 1);
    lua_pushnil(L);
    lua_setfield(L, -2, cacheKey);
    lua_pop(L, 1);
    return 0;
}

int clearCache(lua_State* L)
{
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, requiredCacheTableKey);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, resolvedPathCacheTableKey);
    return 0;
}

} // namespace Luau::Require
