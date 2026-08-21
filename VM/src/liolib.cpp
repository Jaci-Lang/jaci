// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lualib.h"
#include "lcommon.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUA_FILEHANDLE "FILE*"

#define IO_INPUT 1
#define IO_OUTPUT 2

struct LuaFileHandle
{
    FILE* f;
    bool isPipe;
    bool isStandard;
};

static LuaFileHandle* tofilehandle(lua_State* L, int findex)
{
    LuaFileHandle* handle = (LuaFileHandle*)luaL_checkudata(L, findex, LUA_FILEHANDLE);
    if (!handle)
        luaL_typeerror(L, findex, "FILE*");
    return handle;
}

static FILE* tofile(lua_State* L, int findex)
{
    LuaFileHandle* handle = tofilehandle(L, findex);
    if (handle->f == NULL)
        luaL_error(L, "attempt to use a closed file");
    return handle->f;
}

static LuaFileHandle* newfilehandle(lua_State* L, FILE* f, bool isPipe, bool isStandard)
{
    LuaFileHandle* handle = (LuaFileHandle*)lua_newuserdata(L, sizeof(LuaFileHandle));
    handle->f = f;
    handle->isPipe = isPipe;
    handle->isStandard = isStandard;

    luaL_getmetatable(L, LUA_FILEHANDLE);
    lua_setmetatable(L, -2);
    return handle;
}

static int aux_close(lua_State* L)
{
    LuaFileHandle* handle = tofilehandle(L, 1);
    if (handle->f == NULL)
        return 0;

    FILE* f = handle->f;
    handle->f = NULL;

    if (handle->isStandard)
    {
        lua_pushboolean(L, true);
        return 1;
    }

    int res = 0;
    if (handle->isPipe)
    {
#if defined(_WIN32)
        res = _pclose(f);
#else
        res = pclose(f);
#endif
    }
    else
    {
        res = fclose(f);
    }

    if (res == 0)
    {
        lua_pushboolean(L, true);
        return 1;
    }
    else
    {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        lua_pushinteger(L, res);
        return 3;
    }
}

static int io_gc(lua_State* L)
{
    LuaFileHandle* handle = (LuaFileHandle*)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    if (handle && handle->f && !handle->isStandard)
    {
        FILE* f = handle->f;
        handle->f = NULL;
        if (handle->isPipe)
        {
#if defined(_WIN32)
            _pclose(f);
#else
            pclose(f);
#endif
        }
        else
        {
            fclose(f);
        }
    }
    return 0;
}

static int io_tostring(lua_State* L)
{
    LuaFileHandle* handle = tofilehandle(L, 1);
    if (handle->f == NULL)
        lua_pushliteral(L, "file (closed)");
    else
        lua_pushfstring(L, "file (%p)", handle->f);
    return 1;
}

static int read_number(lua_State* L, FILE* f)
{
    double d;
    if (fscanf(f, "%lf", &d) == 1)
    {
        lua_pushnumber(L, d);
        return 1;
    }
    return 0;
}

static int read_line(lua_State* L, FILE* f)
{
    luaL_Strbuf b;
    luaL_buffinit(L, &b);
    int c;
    bool readAny = false;
    while ((c = fgetc(f)) != EOF)
    {
        readAny = true;
        if (c == '\n')
            break;
        if (c == '\r')
        {
            int next = fgetc(f);
            if (next != '\n' && next != EOF)
                ungetc(next, f);
            break;
        }
        luaL_addchar(&b, (char)c);
    }

    if (!readAny && c == EOF)
        return 0;

    luaL_pushresult(&b);
    return 1;
}

static int read_chars(lua_State* L, FILE* f, size_t n)
{
    if (n == 0)
    {
        lua_pushliteral(L, "");
        return 1;
    }

    luaL_Strbuf b;
    char* buff = luaL_buffinitsize(L, &b, n);
    size_t nr = fread(buff, 1, n, f);
    if (nr == 0)
    {
        return 0;
    }

    luaL_pushresultsize(&b, nr);
    return 1;
}

static int read_all(lua_State* L, FILE* f)
{
    luaL_Strbuf b;
    luaL_buffinit(L, &b);
    char temp[4096];
    size_t nr;
    while ((nr = fread(temp, 1, sizeof(temp), f)) > 0)
    {
        luaL_addlstring(&b, temp, nr);
    }
    luaL_pushresult(&b);
    return 1;
}

static int g_read(lua_State* L, FILE* f, int first)
{
    int top = lua_gettop(L);
    int nargs = top - first + 1;
    int success = 1;
    int nresults = 0;

    if (nargs <= 0)
    {
        success = read_line(L, f);
        nresults = 1;
    }
    else
    {
        luaL_checkstack(L, nargs + LUA_MINSTACK, "too many arguments to read");
        for (int i = first; i <= top; i++)
        {
            if (lua_type(L, i) == LUA_TNUMBER)
            {
                size_t l = (size_t)lua_tointeger(L, i);
                success = read_chars(L, f, l);
            }
            else
            {
                const char* p = lua_tostring(L, i);
                if (!p)
                    luaL_argerror(L, i, "invalid format");

                if (*p == '*')
                    p++;

                switch (*p)
                {
                case 'n':
                    success = read_number(L, f);
                    break;
                case 'l':
                    success = read_line(L, f);
                    break;
                case 'a':
                    read_all(L, f);
                    success = 1;
                    break;
                default:
                    luaL_argerror(L, i, "invalid format");
                }
            }

            if (!success)
            {
                lua_pushnil(L);
                nresults++;
                break;
            }
            nresults++;
        }
    }

    return nresults;
}

static int g_write(lua_State* L, FILE* f, int arg)
{
    int nargs = lua_gettop(L);
    int status = 1;
    for (int i = arg; i <= nargs; i++)
    {
        if (lua_type(L, i) == LUA_TNUMBER)
        {
            double n = lua_tonumber(L, i);
            status = status && (fprintf(f, "%.17g", n) > 0);
        }
        else if (lua_isbuffer(L, i))
        {
            size_t len = 0;
            const char* s = (const char*)luaL_checkbuffer(L, i, &len);
            status = status && (fwrite(s, 1, len, f) == len);
        }
        else
        {
            size_t len = 0;
            const char* s = luaL_checklstring(L, i, &len);
            status = status && (fwrite(s, 1, len, f) == len);
        }
    }

    if (status)
    {
        return 1; // returns file / true
    }
    else
    {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
}

static int file_read(lua_State* L)
{
    FILE* f = tofile(L, 1);
    return g_read(L, f, 2);
}

static int file_write(lua_State* L)
{
    FILE* f = tofile(L, 1);
    int res = g_write(L, f, 2);
    if (res == 1)
    {
        lua_pushvalue(L, 1); // return file handle for chaining
        return 1;
    }
    return res;
}

static int file_seek(lua_State* L)
{
    static const int mode[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    static const char* const modenames[] = {"set", "cur", "end", NULL};

    FILE* f = tofile(L, 1);
    int op = luaL_checkoption(L, 2, "cur", modenames);
    int64_t offset = luaL_optinteger64(L, 3, 0);

#if defined(_WIN32)
    int res = _fseeki64(f, offset, mode[op]);
    if (res == 0)
    {
        int64_t pos = _ftelli64(f);
        lua_pushinteger64(L, pos);
        return 1;
    }
#else
    int res = fseeko(f, (off_t)offset, mode[op]);
    if (res == 0)
    {
        off_t pos = ftello(f);
        lua_pushinteger64(L, (int64_t)pos);
        return 1;
    }
#endif

    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
}

static int file_flush(lua_State* L)
{
    FILE* f = tofile(L, 1);
    if (fflush(f) == 0)
    {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
}

static int file_lines_iterator(lua_State* L)
{
    FILE* f = tofile(L, lua_upvalueindex(1));
    int success = read_line(L, f);
    if (success)
        return 1;
    return 0;
}

static int file_lines(lua_State* L)
{
    tofile(L, 1);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, file_lines_iterator, "lines_iterator", 1);
    return 1;
}

static int io_open(lua_State* L)
{
    const char* filename = luaL_checkstring(L, 1);
    const char* mode = luaL_optstring(L, 2, "r");

    FILE* f = fopen(filename, mode);
    if (f == NULL)
    {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: %s", filename, strerror(errno));
        return 2;
    }

    newfilehandle(L, f, false, false);
    return 1;
}

static int io_popen(lua_State* L)
{
    const char* cmd = luaL_checkstring(L, 1);
    const char* mode = luaL_optstring(L, 2, "r");

#if defined(_WIN32)
    FILE* f = _popen(cmd, mode);
#else
    FILE* f = popen(cmd, mode);
#endif

    if (f == NULL)
    {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: %s", cmd, strerror(errno));
        return 2;
    }

    newfilehandle(L, f, true, false);
    return 1;
}

static int io_tmpfile(lua_State* L)
{
    FILE* f = tmpfile();
    if (f == NULL)
    {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }

    newfilehandle(L, f, false, false);
    return 1;
}

static FILE* getiofile(lua_State* L, int findex)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, findex);
    LuaFileHandle* handle = (LuaFileHandle*)lua_touserdata(L, -1);
    if (!handle || !handle->f)
        luaL_error(L, "standard file is closed or invalid");
    lua_pop(L, 1);
    return handle->f;
}

static int io_read(lua_State* L)
{
    return g_read(L, getiofile(L, IO_INPUT), 1);
}

static int io_write(lua_State* L)
{
    return g_write(L, getiofile(L, IO_OUTPUT), 1);
}

static int io_flush(lua_State* L)
{
    FILE* f = getiofile(L, IO_OUTPUT);
    if (fflush(f) == 0)
    {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
}

static int io_input(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, IO_INPUT);
        return 1;
    }

    if (lua_isstring(L, 1))
    {
        const char* filename = lua_tostring(L, 1);
        FILE* f = fopen(filename, "r");
        if (!f)
            luaL_error(L, "cannot open file '%s': %s", filename, strerror(errno));
        newfilehandle(L, f, false, false);
    }
    else
    {
        tofile(L, 1);
        lua_pushvalue(L, 1);
    }

    lua_rawseti(L, LUA_REGISTRYINDEX, IO_INPUT);
    lua_rawgeti(L, LUA_REGISTRYINDEX, IO_INPUT);
    return 1;
}

static int io_output(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, IO_OUTPUT);
        return 1;
    }

    if (lua_isstring(L, 1))
    {
        const char* filename = lua_tostring(L, 1);
        FILE* f = fopen(filename, "w");
        if (!f)
            luaL_error(L, "cannot open file '%s': %s", filename, strerror(errno));
        newfilehandle(L, f, false, false);
    }
    else
    {
        tofile(L, 1);
        lua_pushvalue(L, 1);
    }

    lua_rawseti(L, LUA_REGISTRYINDEX, IO_OUTPUT);
    lua_rawgeti(L, LUA_REGISTRYINDEX, IO_OUTPUT);
    return 1;
}

static int io_lines_file_iterator(lua_State* L)
{
    LuaFileHandle* handle = tofilehandle(L, lua_upvalueindex(1));
    if (handle->f == NULL)
        return 0;

    int success = read_line(L, handle->f);
    if (success)
        return 1;

    // auto close file when EOF is reached
    if (!handle->isStandard)
    {
        fclose(handle->f);
        handle->f = NULL;
    }
    return 0;
}

static int io_lines(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, IO_INPUT);
        return file_lines(L);
    }

    const char* filename = luaL_checkstring(L, 1);
    FILE* f = fopen(filename, "r");
    if (!f)
        luaL_error(L, "cannot open file '%s': %s", filename, strerror(errno));

    newfilehandle(L, f, false, false);
    lua_pushcclosure(L, io_lines_file_iterator, "lines_iterator", 1);
    return 1;
}

static int io_type(lua_State* L)
{
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TUSERDATA)
    {
        lua_pushnil(L);
        return 1;
    }

    lua_getmetatable(L, 1);
    luaL_getmetatable(L, LUA_FILEHANDLE);
    int isFile = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);

    if (!isFile)
    {
        lua_pushnil(L);
        return 1;
    }

    LuaFileHandle* handle = (LuaFileHandle*)lua_touserdata(L, 1);
    if (handle->f == NULL)
        lua_pushliteral(L, "closed file");
    else
        lua_pushliteral(L, "file");

    return 1;
}

static int io_close(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, IO_OUTPUT);
    }
    return aux_close(L);
}

static const luaL_Reg flib[] = {
    {"close", aux_close},
    {"flush", file_flush},
    {"lines", file_lines},
    {"read", file_read},
    {"seek", file_seek},
    {"write", file_write},
    {"__tostring", io_tostring},
    {"__gc", io_gc},
    {NULL, NULL},
};

static const luaL_Reg iolib[] = {
    {"close", io_close},
    {"flush", io_flush},
    {"input", io_input},
    {"lines", io_lines},
    {"open", io_open},
    {"output", io_output},
    {"popen", io_popen},
    {"read", io_read},
    {"tmpfile", io_tmpfile},
    {"type", io_type},
    {"write", io_write},
    {NULL, NULL},
};

static void createmeta(lua_State* L)
{
    luaL_newmetatable(L, LUA_FILEHANDLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, flib);
    lua_pop(L, 1);
}

int luaopen_io(lua_State* L)
{
    createmeta(L);

    luaL_register(L, LUA_IOLIBNAME, iolib);

    // standard files
    newfilehandle(L, stdin, false, true);
    lua_pushvalue(L, -1);
    lua_rawseti(L, LUA_REGISTRYINDEX, IO_INPUT);
    lua_setfield(L, -2, "stdin");

    newfilehandle(L, stdout, false, true);
    lua_pushvalue(L, -1);
    lua_rawseti(L, LUA_REGISTRYINDEX, IO_OUTPUT);
    lua_setfield(L, -2, "stdout");

    newfilehandle(L, stderr, false, true);
    lua_setfield(L, -2, "stderr");

    return 1;
}
