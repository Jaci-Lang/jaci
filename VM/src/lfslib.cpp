// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lualib.h"
#include "lcommon.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <system_error>
#include <chrono>

#include <stdio.h>
#include <string.h>

namespace fs = std::filesystem;

static int fs_readfile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    fs::path p(pathStr);

    std::error_code ec;
    if (!fs::exists(p, ec) || fs::is_directory(p, ec))
    {
        luaL_error(L, "fs.readfile: file not found or is a directory: %s", pathStr);
    }

    FILE* f = fopen(pathStr, "rb");
    if (!f)
    {
        luaL_error(L, "fs.readfile: cannot open file: %s", pathStr);
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        luaL_error(L, "fs.readfile: cannot seek file: %s", pathStr);
    }

    long sz = ftell(f);
    if (sz < 0)
    {
        fclose(f);
        luaL_error(L, "fs.readfile: cannot determine file size: %s", pathStr);
    }

    rewind(f);

    std::string contents;
    contents.resize((size_t)sz);

    if (sz > 0)
    {
        size_t readBytes = fread(&contents[0], 1, (size_t)sz, f);
        if (readBytes != (size_t)sz)
        {
            fclose(f);
            luaL_error(L, "fs.readfile: failed to read complete file: %s", pathStr);
        }
    }

    fclose(f);
    lua_pushlstring(L, contents.data(), contents.size());
    return 1;
}

static int fs_writefile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    size_t dataLen = 0;
    const char* data = NULL;

    if (lua_type(L, 2) == LUA_TSTRING)
    {
        data = luaL_checklstring(L, 2, &dataLen);
    }
    else if (lua_isbuffer(L, 2))
    {
        data = (const char*)luaL_checkbuffer(L, 2, &dataLen);
    }
    else
    {
        luaL_typeerror(L, 2, "string or buffer");
    }

    FILE* f = fopen(pathStr, "wb");
    if (!f)
    {
        luaL_error(L, "fs.writefile: cannot open file for writing: %s", pathStr);
    }

    if (dataLen > 0)
    {
        size_t written = fwrite(data, 1, dataLen, f);
        if (written != dataLen)
        {
            fclose(f);
            luaL_error(L, "fs.writefile: failed to write all data to: %s", pathStr);
        }
    }

    fclose(f);
    return 0;
}

static int fs_appendfile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    size_t dataLen = 0;
    const char* data = NULL;

    if (lua_type(L, 2) == LUA_TSTRING)
    {
        data = luaL_checklstring(L, 2, &dataLen);
    }
    else if (lua_isbuffer(L, 2))
    {
        data = (const char*)luaL_checkbuffer(L, 2, &dataLen);
    }
    else
    {
        luaL_typeerror(L, 2, "string or buffer");
    }

    FILE* f = fopen(pathStr, "ab");
    if (!f)
    {
        luaL_error(L, "fs.appendfile: cannot open file for appending: %s", pathStr);
    }

    if (dataLen > 0)
    {
        size_t written = fwrite(data, 1, dataLen, f);
        if (written != dataLen)
        {
            fclose(f);
            luaL_error(L, "fs.appendfile: failed to append data to: %s", pathStr);
        }
    }

    fclose(f);
    return 0;
}

static int fs_removefile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    fs::path p(pathStr);

    std::error_code ec;
    if (fs::is_directory(p, ec))
    {
        luaL_error(L, "fs.removefile: path is a directory: %s", pathStr);
    }

    if (!fs::remove(p, ec) || ec)
    {
        luaL_error(L, "fs.removefile: failed to remove file: %s (%s)", pathStr, ec.message().c_str());
    }

    return 0;
}

static int fs_removedir(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    bool recursive = luaL_optboolean(L, 2, false);
    fs::path p(pathStr);

    std::error_code ec;
    if (!fs::is_directory(p, ec))
    {
        luaL_error(L, "fs.removedir: path is not a directory: %s", pathStr);
    }

    if (recursive)
    {
        fs::remove_all(p, ec);
        if (ec)
            luaL_error(L, "fs.removedir: failed to remove directory recursively: %s (%s)", pathStr, ec.message().c_str());
    }
    else
    {
        if (!fs::remove(p, ec) || ec)
            luaL_error(L, "fs.removedir: failed to remove directory: %s (%s)", pathStr, ec.message().c_str());
    }

    return 0;
}

static int fs_mkdir(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    bool recursive = luaL_optboolean(L, 2, true);
    fs::path p(pathStr);

    std::error_code ec;
    if (recursive)
    {
        fs::create_directories(p, ec);
    }
    else
    {
        fs::create_directory(p, ec);
    }

    if (ec)
    {
        luaL_error(L, "fs.mkdir: failed to create directory: %s (%s)", pathStr, ec.message().c_str());
    }

    return 0;
}

static int fs_list(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    fs::path p(pathStr);

    std::error_code ec;
    if (!fs::is_directory(p, ec))
    {
        luaL_error(L, "fs.list: path is not a directory: %s", pathStr);
    }

    lua_newtable(L);
    int index = 1;

    for (const auto& entry : fs::directory_iterator(p, ec))
    {
        if (ec)
            break;

        std::string filename = entry.path().filename().string();
        lua_pushlstring(L, filename.data(), filename.size());
        lua_rawseti(L, -2, index++);
    }

    if (ec)
    {
        luaL_error(L, "fs.list: failed while reading directory: %s (%s)", pathStr, ec.message().c_str());
    }

    return 1;
}

static int fs_isfile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    fs::path p(pathStr);

    std::error_code ec;
    bool isRegular = fs::is_regular_file(p, ec);
    lua_pushboolean(L, !ec && isRegular);
    return 1;
}

static int fs_isdir(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    fs::path p(pathStr);

    std::error_code ec;
    bool isDir = fs::is_directory(p, ec);
    lua_pushboolean(L, !ec && isDir);
    return 1;
}

static int fs_exists(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    fs::path p(pathStr);

    std::error_code ec;
    bool ex = fs::exists(p, ec);
    lua_pushboolean(L, !ec && ex);
    return 1;
}

static int fs_stat(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    fs::path p(pathStr);

    std::error_code ec;
    bool exists = fs::exists(p, ec);
    if (ec || !exists)
    {
        lua_pushnil(L);
        return 1;
    }

    bool isFile = fs::is_regular_file(p, ec);
    bool isDir = fs::is_directory(p, ec);

    uintmax_t size = 0;
    if (isFile)
    {
        size = fs::file_size(p, ec);
        if (ec)
            size = 0;
    }

    double modTime = 0.0;
    auto ftime = fs::last_write_time(p, ec);
    if (!ec)
    {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        modTime = (double)std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
    }

    lua_createtable(L, 0, 5);

    lua_pushboolean(L, true);
    lua_setfield(L, -2, "exists");

    lua_pushboolean(L, isFile);
    lua_setfield(L, -2, "isFile");

    lua_pushboolean(L, isDir);
    lua_setfield(L, -2, "isDirectory");

    lua_pushnumber(L, (double)size);
    lua_setfield(L, -2, "size");

    lua_pushnumber(L, modTime);
    lua_setfield(L, -2, "modified");

    return 1;
}

static int fs_copy(lua_State* L)
{
    const char* fromStr = luaL_checkstring(L, 1);
    const char* toStr = luaL_checkstring(L, 2);
    bool overwrite = luaL_optboolean(L, 3, true);

    fs::path from(fromStr);
    fs::path to(toStr);

    std::error_code ec;
    fs::copy_options options = fs::copy_options::recursive;
    if (overwrite)
        options |= fs::copy_options::overwrite_existing;
    else
        options |= fs::copy_options::skip_existing;

    fs::copy(from, to, options, ec);
    if (ec)
    {
        luaL_error(L, "fs.copy: failed to copy from '%s' to '%s' (%s)", fromStr, toStr, ec.message().c_str());
    }

    return 0;
}

static int fs_move(lua_State* L)
{
    const char* fromStr = luaL_checkstring(L, 1);
    const char* toStr = luaL_checkstring(L, 2);

    fs::path from(fromStr);
    fs::path to(toStr);

    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec)
    {
        luaL_error(L, "fs.move: failed to move from '%s' to '%s' (%s)", fromStr, toStr, ec.message().c_str());
    }

    return 0;
}

static int fs_cwd(lua_State* L)
{
    std::error_code ec;
    fs::path current = fs::current_path(ec);
    if (ec)
    {
        lua_pushliteral(L, ".");
        return 1;
    }

    std::string s = current.string();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

static const luaL_Reg fslib[] = {
    {"readfile", fs_readfile},
    {"writefile", fs_writefile},
    {"appendfile", fs_appendfile},
    {"removefile", fs_removefile},
    {"removedir", fs_removedir},
    {"mkdir", fs_mkdir},
    {"list", fs_list},
    {"isfile", fs_isfile},
    {"isdir", fs_isdir},
    {"exists", fs_exists},
    {"stat", fs_stat},
    {"copy", fs_copy},
    {"move", fs_move},
    {"cwd", fs_cwd},

    // CamelCase aliases
    {"readFile", fs_readfile},
    {"writeFile", fs_writefile},
    {"appendFile", fs_appendfile},
    {"removeFile", fs_removefile},
    {"removeDir", fs_removedir},
    {"makeDir", fs_mkdir},
    {"readDir", fs_list},
    {"isFile", fs_isfile},
    {"isDir", fs_isdir},

    {NULL, NULL},
};

int luaopen_fs(lua_State* L)
{
    luaL_register(L, LUA_FSLIBNAME, fslib);
    return 1;
}
