// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/ReplRequirer.h"

#include "Luau/CodeGen.h"
#include "Luau/CodeGenOptions.h"
#include "Luau/FileUtils.h"
#include "Luau/Require.h"
#include "Luau/VfsNavigator.h"

#include "lua.h"
#include "lualib.h"

#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <dlfcn.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

LUAU_FASTFLAG(LuauCyclicRequireShortCircuit)

static luarequire_WriteResult write(std::string_view contents, char* buffer, size_t bufferSize, size_t* sizeOut)
{
    size_t nullTerminatedSize = contents.size() + 1;

    if (bufferSize < nullTerminatedSize)
    {
        *sizeOut = nullTerminatedSize;
        return luarequire_WriteResult::WRITE_BUFFER_TOO_SMALL;
    }

    *sizeOut = nullTerminatedSize;
    memcpy(buffer, contents.data(), contents.size());
    buffer[contents.size()] = '\0';
    return luarequire_WriteResult::WRITE_SUCCESS;
}

static luarequire_WriteResult write(const std::optional<std::string>& contents, char* buffer, size_t bufferSize, size_t* sizeOut)
{
    if (!contents)
        return luarequire_WriteResult::WRITE_FAILURE;
    return write(std::string_view(*contents), buffer, bufferSize, sizeOut);
}

static luarequire_NavigateResult convert(NavigationStatus status)
{
    if (status == NavigationStatus::Success)
        return NAVIGATE_SUCCESS;
    else if (status == NavigationStatus::Ambiguous)
        return NAVIGATE_AMBIGUOUS;
    else
        return NAVIGATE_NOT_FOUND;
}

static luarequire_ConfigStatus convert(VfsNavigator::ConfigStatus status)
{
    if (status == VfsNavigator::ConfigStatus::Ambiguous)
        return CONFIG_AMBIGUOUS;
    else if (status == VfsNavigator::ConfigStatus::PresentJson)
        return CONFIG_PRESENT_JSON;
    else if (status == VfsNavigator::ConfigStatus::PresentLuau)
        return CONFIG_PRESENT_LUAU;
    else
        return CONFIG_ABSENT;
}

static bool is_require_allowed(lua_State* L, void* ctx, const char* requirer_chunkname)
{
    std::string_view chunkname = requirer_chunkname;
    return chunkname == "=stdin" || chunkname == "=eval" || (!chunkname.empty() && chunkname[0] == '@');
}

static luarequire_NavigateResult reset(lua_State* L, void* ctx, const char* requirer_chunkname)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);

    std::string_view chunkname = requirer_chunkname;
    if (chunkname == "=stdin" || chunkname == "=eval")
        return convert(req->vfs.resetToStdIn());
    else if (!chunkname.empty() && chunkname[0] == '@')
        return convert(req->vfs.resetToPath(std::string(chunkname.substr(1))));

    return NAVIGATE_NOT_FOUND;
}

static luarequire_NavigateResult jump_to_alias(lua_State* L, void* ctx, const char* path)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);

    if (!isAbsolutePath(path))
        return NAVIGATE_NOT_FOUND;

    return convert(req->vfs.resetToPath(path));
}

static luarequire_NavigateResult to_parent(lua_State* L, void* ctx)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return convert(req->vfs.toParent());
}

static luarequire_NavigateResult to_child(lua_State* L, void* ctx, const char* name)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return convert(req->vfs.toChild(name));
}

static bool reset_is_directory_module(lua_State* L, void* ctx)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return req->vfs.requirerIsDirectoryModule();
}

static bool is_module_present(lua_State* L, void* ctx)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return isFile(req->vfs.getFilePath());
}

static luarequire_WriteResult get_chunkname(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    const std::string& fp = req->vfs.getFilePath();
    size_t nullTerminatedSize = fp.size() + 2; // '@' + fp + '\0'
    if (buffer_size < nullTerminatedSize)
    {
        *size_out = nullTerminatedSize;
        return luarequire_WriteResult::WRITE_BUFFER_TOO_SMALL;
    }
    *size_out = nullTerminatedSize;
    buffer[0] = '@';
    memcpy(buffer + 1, fp.data(), fp.size());
    buffer[nullTerminatedSize - 1] = '\0';
    return luarequire_WriteResult::WRITE_SUCCESS;
}

static luarequire_WriteResult get_loadname(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return write(std::string_view(req->vfs.getAbsoluteFilePath()), buffer, buffer_size, size_out);
}

static luarequire_WriteResult get_cache_key(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return write(std::string_view(req->vfs.getAbsoluteFilePath()), buffer, buffer_size, size_out);
}

static luarequire_ConfigStatus get_config_status(lua_State* L, void* ctx)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return convert(req->vfs.getConfigStatus());
}

static luarequire_WriteResult get_config(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return write(req->vfs.getConfig(), buffer, buffer_size, size_out);
}

// Load a native shared library module via dlopen/LoadLibrary and invoke luaopen_<name>.
static int loadNativeModule(lua_State* L, const char* loadname)
{
    // Derive the C entry-point symbol from the path.
    // This mirrors VfsNavigator::getNativeEntryPoint() logic.
    std::string name = loadname;

    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);

    if (name.size() > 3 && name.substr(0, 3) == "lib")
        name = name.substr(3);

    // Strip known native suffixes.
    static const char* kNativeSuffixes[] = {".so", ".dylib", ".dll", nullptr};
    for (int i = 0; kNativeSuffixes[i]; ++i)
    {
        std::string_view suf = kNativeSuffixes[i];
        if (name.size() >= suf.size() && name.substr(name.size() - suf.size()) == suf)
        {
            name = name.substr(0, name.size() - suf.size());
            break;
        }
    }

    // Replace non-identifier characters with underscores.
    for (char& c : name)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            c = '_';
    }

    std::string entryPoint = "luaopen_" + name;

#if !defined(_WIN32)
    void* handle = dlopen(loadname, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        luaL_error(L, "cannot open native module '%s': %s", loadname, dlerror());

    typedef int (*LuaOpenFn)(lua_State*);
    LuaOpenFn fn = reinterpret_cast<LuaOpenFn>(dlsym(handle, entryPoint.c_str()));
    if (!fn)
        luaL_error(L, "native module '%s' has no entry point '%s': %s", loadname, entryPoint.c_str(), dlerror());
#else
    HMODULE handle = LoadLibraryA(loadname);
    if (!handle)
        luaL_error(L, "cannot open native module '%s' (error %lu)", loadname, GetLastError());

    typedef int (*LuaOpenFn)(lua_State*);
    LuaOpenFn fn = reinterpret_cast<LuaOpenFn>((void*)GetProcAddress(handle, entryPoint.c_str()));
    if (!fn)
        luaL_error(L, "native module '%s' has no entry point '%s' (error %lu)", loadname, entryPoint.c_str(), GetLastError());
#endif

    return fn(L);
}

static int load(lua_State* L, void* ctx, const char* path, const char* chunkname, const char* loadname)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);

    // Detect native shared library by extension.
    static const char* kNativeSuffixes[] = {".so", ".dylib", ".dll", nullptr};
    bool isNative = false;
    std::string_view loadnameView = loadname;
    for (int i = 0; kNativeSuffixes[i]; ++i)
    {
        std::string_view suf = kNativeSuffixes[i];
        if (loadnameView.size() >= suf.size() && loadnameView.substr(loadnameView.size() - suf.size()) == suf)
        {
            isNative = true;
            break;
        }
    }

    if (isNative)
        return loadNativeModule(L, loadname);

    // module needs to run in a new thread, isolated from the rest
    // note: we create ML on main thread so that it doesn't inherit environment of L
    lua_State* GL = lua_mainthread(L);
    lua_State* ML = lua_newthread(GL);
    lua_xmove(GL, L, 1);

    // new thread needs to have the globals sandboxed
    luaL_sandboxthread(ML);

    bool hadContents = false;
    int status = LUA_OK;

    // Handle C++ RAII objects in a scope which doesn't cause a Luau error
    {
        std::optional<std::string> contents = readFile(loadname);
        hadContents = contents.has_value();

        if (contents)
        {
            // now we can compile & run module on the new thread
            std::string bytecode = Luau::compile(*contents, req->copts());
            status = luau_load(ML, chunkname, bytecode.data(), bytecode.size(), 0);
        }
    }

    if (!hadContents)
        luaL_error(L, "could not read file '%s'", loadname);

    if (status == 0)
    {
        if (FFlag::LuauCyclicRequireShortCircuit && lua_usesexport(ML, -1) != 0)
            luarequire_createplaceholder(L);

        if (req->codegenEnabled())
        {
            Luau::CodeGen::CompilationOptions nativeOptions;

            if (req->countersActive())
                nativeOptions.recordCounters = true;

            Luau::CodeGen::compile(ML, -1, nativeOptions);
        }

        if (req->coverageActive())
            req->coverageTrack(ML, -1);

        if (req->countersActive())
            req->countersTrack(ML, -1);

        int status = lua_resume(ML, L, 0);

        if (status == 0)
        {
            if (lua_gettop(ML) != 1)
                luaL_error(L, "module must return a single value");
        }
        else if (status == LUA_YIELD)
        {
            luaL_error(L, "module can not yield");
        }
        else if (!lua_isstring(ML, -1))
        {
            luaL_error(L, "unknown error while running module");
        }
        else
        {
            luaL_error(L, "error while running module: %s", lua_tostring(ML, -1));
        }
    }
    else
    {
        const char* error = lua_isstring(ML, -1) ? lua_tostring(ML, -1) : "unknown compile error";
        luaL_error(L, "error while compiling module: %s", error);
    }

    // add ML result to L stack
    lua_xmove(ML, L, 1);

    // remove ML thread from L stack
    lua_remove(L, -2);

    // added one value to L stack: module result
    return 1;
}

// toAliasFallback is called by the require navigator when an alias is not found
// in any .luaurc config. For bare package specifiers (e.g. require("mylib")),
// the navigator calls this with the bare package name. We delegate to
// VfsNavigator::toBarePackage which searches luau_packages/, packages/,
// and node_modules/ directories walking up the filesystem hierarchy.
static luarequire_NavigateResult alias_fallback(lua_State* L, void* ctx, const char* aliasUnprefixed)
{
    ReplRequirer* req = static_cast<ReplRequirer*>(ctx);
    return convert(req->vfs.toBarePackage(aliasUnprefixed));
}

void requireConfigInit(luarequire_Configuration* config)
{
    if (config == nullptr)
        return;

    config->is_require_allowed = is_require_allowed;
    config->reset = reset;
    config->jump_to_alias = jump_to_alias;
    config->to_parent = to_parent;
    config->to_child = to_child;
    config->reset_is_directory_module = reset_is_directory_module;
    config->is_module_present = is_module_present;
    config->get_config_status = get_config_status;
    config->get_chunkname = get_chunkname;
    config->get_loadname = get_loadname;
    config->get_cache_key = get_cache_key;
    config->get_config = get_config;
    config->load = load;
    config->to_alias_fallback = alias_fallback;
}

ReplRequirer::ReplRequirer(
    CompileOptions copts,
    BoolCheck coverageActive,
    BoolCheck codegenEnabled,
    Coverage coverageTrack,
    BoolCheck countersActive,
    Coverage countersTrack
)
    : copts(copts)
    , coverageActive(coverageActive)
    , codegenEnabled(codegenEnabled)
    , coverageTrack(coverageTrack)
    , countersActive(countersActive)
    , countersTrack(countersTrack)
{
}
