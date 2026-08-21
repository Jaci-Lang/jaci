// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define FFI_LIBRARY_MT "FFI_Library"
#define FFI_SYMBOL_MT "FFI_Symbol"

#define LUA_FFILIBNAME "ffi"

enum FFI_Type
{
    FFI_VOID,
    FFI_I8,
    FFI_U8,
    FFI_I16,
    FFI_U16,
    FFI_I32,
    FFI_U32,
    FFI_I64,
    FFI_U64,
    FFI_F32,
    FFI_F64,
    FFI_PTR,
    FFI_STR,
    FFI_BUF
};

struct FFI_Library
{
    void* handle;
    const char* path;
};

struct FFI_Symbol
{
    void* func;
    FFI_Library* lib;
    const char* name;
    FFI_Type retType;
    int argCount;
    FFI_Type argTypes[16];
};

static FFI_Type parseType(lua_State* L, const char* name)
{
    if (strcmp(name, "void") == 0) return FFI_VOID;
    if (strcmp(name, "i8") == 0) return FFI_I8;
    if (strcmp(name, "u8") == 0) return FFI_U8;
    if (strcmp(name, "i16") == 0) return FFI_I16;
    if (strcmp(name, "u16") == 0) return FFI_U16;
    if (strcmp(name, "i32") == 0) return FFI_I32;
    if (strcmp(name, "u32") == 0) return FFI_U32;
    if (strcmp(name, "i64") == 0) return FFI_I64;
    if (strcmp(name, "u64") == 0) return FFI_U64;
    if (strcmp(name, "f32") == 0) return FFI_F32;
    if (strcmp(name, "f64") == 0) return FFI_F64;
    if (strcmp(name, "ptr") == 0) return FFI_PTR;
    if (strcmp(name, "str") == 0) return FFI_STR;
    if (strcmp(name, "buf") == 0) return FFI_BUF;
    luaL_error(L, "unknown type: %s", name);
    return FFI_VOID;
}

static size_t typeSize(FFI_Type type)
{
    switch (type)
    {
    case FFI_VOID: return 0;
    case FFI_I8: return 1;
    case FFI_U8: return 1;
    case FFI_I16: return 2;
    case FFI_U16: return 2;
    case FFI_I32: return 4;
    case FFI_U32: return 4;
    case FFI_I64: return 8;
    case FFI_U64: return 8;
    case FFI_F32: return 4;
    case FFI_F64: return 8;
    case FFI_PTR: return sizeof(void*);
    case FFI_STR: return sizeof(const char*);
    case FFI_BUF: return sizeof(void*);
    default: return 0;
    }
}

static int ffi_open(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    
    void* handle = nullptr;
#if defined(_WIN32)
    handle = (void*)LoadLibraryA(path);
    if (!handle)
    {
        luaL_error(L, "error loading library: %u", GetLastError());
    }
#else
    handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
    {
        luaL_error(L, "error loading library: %s", dlerror());
    }
#endif

    FFI_Library* lib = (FFI_Library*)lua_newuserdata(L, sizeof(FFI_Library));
    lib->handle = handle;
    lib->path = strdup(path);

    luaL_getmetatable(L, FFI_LIBRARY_MT);
    lua_setmetatable(L, -2);

    return 1;
}

static int ffi_library_gc(lua_State* L)
{
    FFI_Library* lib = (FFI_Library*)lua_touserdata(L, 1);
    if (lib->handle)
    {
#if defined(_WIN32)
        FreeLibrary((HMODULE)lib->handle);
#else
        dlclose(lib->handle);
#endif
        lib->handle = nullptr;
    }
    if (lib->path)
    {
        free((void*)lib->path);
        lib->path = nullptr;
    }
    return 0;
}

static int ffi_library_tostring(lua_State* L)
{
    FFI_Library* lib = (FFI_Library*)luaL_checkudata(L, 1, FFI_LIBRARY_MT);
    if (lib->handle)
        lua_pushfstring(L, "ffi.library (%s)", lib->path);
    else
        lua_pushstring(L, "ffi.library (closed)");
    return 1;
}

static int ffi_sym(lua_State* L)
{
    FFI_Library* lib = (FFI_Library*)luaL_checkudata(L, 1, FFI_LIBRARY_MT);
    const char* name = luaL_checkstring(L, 2);
    const char* retTypeName = luaL_checkstring(L, 3);
    
    if (!lib->handle)
        luaL_error(L, "library is closed");

    void* func = nullptr;
#if defined(_WIN32)
    func = (void*)GetProcAddress((HMODULE)lib->handle, name);
#else
    func = dlsym(lib->handle, name);
#endif

    if (!func)
    {
#if defined(_WIN32)
        luaL_error(L, "symbol not found: %u", GetLastError());
#else
        luaL_error(L, "symbol not found: %s", dlerror());
#endif
    }

    int n = lua_gettop(L) - 3;
    if (n > 16)
        luaL_error(L, "maximum 16 arguments supported");

    FFI_Symbol* sym = (FFI_Symbol*)lua_newuserdata(L, sizeof(FFI_Symbol));
    sym->func = func;
    sym->lib = lib;
    sym->name = strdup(name);
    sym->retType = parseType(L, retTypeName);
    sym->argCount = n;
    
    for (int i = 0; i < n; i++)
    {
        sym->argTypes[i] = parseType(L, luaL_checkstring(L, 4 + i));
    }

    luaL_getmetatable(L, FFI_SYMBOL_MT);
    lua_setmetatable(L, -2);

    return 1;
}

static int ffi_symbol_gc(lua_State* L)
{
    FFI_Symbol* sym = (FFI_Symbol*)lua_touserdata(L, 1);
    if (sym->name)
    {
        free((void*)sym->name);
        sym->name = nullptr;
    }
    return 0;
}

static int ffi_symbol_tostring(lua_State* L)
{
    FFI_Symbol* sym = (FFI_Symbol*)luaL_checkudata(L, 1, FFI_SYMBOL_MT);
    lua_pushfstring(L, "ffi.symbol (%s)", sym->name ? sym->name : "unknown");
    return 1;
}

typedef intptr_t (*ffi_fn0)();
typedef intptr_t (*ffi_fn1)(intptr_t);
typedef intptr_t (*ffi_fn2)(intptr_t, intptr_t);
typedef intptr_t (*ffi_fn3)(intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn4)(intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn5)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn6)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn7)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn8)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn9)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn10)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn11)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn12)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn13)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn14)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn15)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn16)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);

typedef double (*ffi_fnd0)();
typedef double (*ffi_fnd1)(intptr_t);
typedef double (*ffi_fnd2)(intptr_t, intptr_t);
typedef double (*ffi_fnd3)(intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd4)(intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd5)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd6)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd7)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd8)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd9)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd10)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd11)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd12)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd13)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd14)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd15)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd16)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);

typedef double (*ffi_fnd_d1)(double);
typedef double (*ffi_fnd_d2)(double, double);
typedef double (*ffi_fnd_d3)(double, double, double);
typedef double (*ffi_fnd_d4)(double, double, double, double);

union DoubleCast {
    double d;
    intptr_t i;
};

union FloatCast {
    float f;
    int32_t i;
};

static int ffi_symbol_call(lua_State* L)
{
    FFI_Symbol* sym = (FFI_Symbol*)luaL_checkudata(L, 1, FFI_SYMBOL_MT);
    
    intptr_t args[16] = {0};
    bool allDouble = true;
    
    for (int i = 0; i < sym->argCount; i++)
    {
        int lua_arg = i + 2;
        switch (sym->argTypes[i])
        {
        case FFI_I8:
        case FFI_U8:
        case FFI_I16:
        case FFI_U16:
        case FFI_I32:
        case FFI_U32:
        case FFI_I64:
        case FFI_U64:
            args[i] = (intptr_t)luaL_checknumber(L, lua_arg);
            allDouble = false;
            break;
        case FFI_F32:
        {
            float v = (float)luaL_checknumber(L, lua_arg);
            FloatCast fc;
            fc.i = 0;
            fc.f = v;
            args[i] = fc.i;
            allDouble = false;
            break;
        }
        case FFI_F64:
        {
            double v = (double)luaL_checknumber(L, lua_arg);
            DoubleCast dc;
            dc.d = v;
            args[i] = dc.i;
            break;
        }
        case FFI_PTR:
        {
            if (lua_islightuserdata(L, lua_arg))
            {
                args[i] = (intptr_t)lua_touserdata(L, lua_arg);
            }
            else if (lua_isbuffer(L, lua_arg))
            {
                size_t len;
                args[i] = (intptr_t)lua_tobuffer(L, lua_arg, &len);
            }
            else
            {
                args[i] = (intptr_t)luaL_checknumber(L, lua_arg);
            }
            allDouble = false;
            break;
        }
        case FFI_STR:
        {
            args[i] = (intptr_t)luaL_checkstring(L, lua_arg);
            allDouble = false;
            break;
        }
        case FFI_BUF:
        {
            size_t len;
            args[i] = (intptr_t)luaL_checkbuffer(L, lua_arg, &len);
            allDouble = false;
            break;
        }
        default:
            args[i] = 0;
            allDouble = false;
            break;
        }
    }
    
    bool retDouble = (sym->retType == FFI_F32 || sym->retType == FFI_F64);
    
    intptr_t retI = 0;
    double retD = 0.0;
    
    if (sym->argCount > 0 && allDouble && retDouble)
    {
        double dargs[4] = {0.0};
        for(int i = 0; i < sym->argCount && i < 4; i++) {
            DoubleCast dc;
            dc.i = args[i];
            dargs[i] = dc.d;
        }
        switch(sym->argCount) {
            case 1: retD = ((ffi_fnd_d1)sym->func)(dargs[0]); break;
            case 2: retD = ((ffi_fnd_d2)sym->func)(dargs[0], dargs[1]); break;
            case 3: retD = ((ffi_fnd_d3)sym->func)(dargs[0], dargs[1], dargs[2]); break;
            case 4: retD = ((ffi_fnd_d4)sym->func)(dargs[0], dargs[1], dargs[2], dargs[3]); break;
            default: luaL_error(L, "all-double dispatch up to 4 args supported");
        }
    }
    else if (retDouble)
    {
        switch (sym->argCount)
        {
        case 0: retD = ((ffi_fnd0)sym->func)(); break;
        case 1: retD = ((ffi_fnd1)sym->func)(args[0]); break;
        case 2: retD = ((ffi_fnd2)sym->func)(args[0], args[1]); break;
        case 3: retD = ((ffi_fnd3)sym->func)(args[0], args[1], args[2]); break;
        case 4: retD = ((ffi_fnd4)sym->func)(args[0], args[1], args[2], args[3]); break;
        case 5: retD = ((ffi_fnd5)sym->func)(args[0], args[1], args[2], args[3], args[4]); break;
        case 6: retD = ((ffi_fnd6)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5]); break;
        case 7: retD = ((ffi_fnd7)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6]); break;
        case 8: retD = ((ffi_fnd8)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]); break;
        case 9: retD = ((ffi_fnd9)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]); break;
        case 10: retD = ((ffi_fnd10)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]); break;
        case 11: retD = ((ffi_fnd11)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]); break;
        case 12: retD = ((ffi_fnd12)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]); break;
        case 13: retD = ((ffi_fnd13)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]); break;
        case 14: retD = ((ffi_fnd14)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]); break;
        case 15: retD = ((ffi_fnd15)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]); break;
        case 16: retD = ((ffi_fnd16)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15]); break;
        }
    }
    else
    {
        switch (sym->argCount)
        {
        case 0: retI = ((ffi_fn0)sym->func)(); break;
        case 1: retI = ((ffi_fn1)sym->func)(args[0]); break;
        case 2: retI = ((ffi_fn2)sym->func)(args[0], args[1]); break;
        case 3: retI = ((ffi_fn3)sym->func)(args[0], args[1], args[2]); break;
        case 4: retI = ((ffi_fn4)sym->func)(args[0], args[1], args[2], args[3]); break;
        case 5: retI = ((ffi_fn5)sym->func)(args[0], args[1], args[2], args[3], args[4]); break;
        case 6: retI = ((ffi_fn6)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5]); break;
        case 7: retI = ((ffi_fn7)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6]); break;
        case 8: retI = ((ffi_fn8)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]); break;
        case 9: retI = ((ffi_fn9)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]); break;
        case 10: retI = ((ffi_fn10)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]); break;
        case 11: retI = ((ffi_fn11)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]); break;
        case 12: retI = ((ffi_fn12)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]); break;
        case 13: retI = ((ffi_fn13)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]); break;
        case 14: retI = ((ffi_fn14)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]); break;
        case 15: retI = ((ffi_fn15)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]); break;
        case 16: retI = ((ffi_fn16)sym->func)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15]); break;
        }
    }

    switch (sym->retType)
    {
    case FFI_VOID: return 0;
    case FFI_I8: lua_pushnumber(L, (double)(int8_t)retI); return 1;
    case FFI_U8: lua_pushnumber(L, (double)(uint8_t)retI); return 1;
    case FFI_I16: lua_pushnumber(L, (double)(int16_t)retI); return 1;
    case FFI_U16: lua_pushnumber(L, (double)(uint16_t)retI); return 1;
    case FFI_I32: lua_pushnumber(L, (double)(int32_t)retI); return 1;
    case FFI_U32: lua_pushnumber(L, (double)(uint32_t)retI); return 1;
    case FFI_I64: lua_pushnumber(L, (double)(int64_t)retI); return 1;
    case FFI_U64: lua_pushnumber(L, (double)(uint64_t)retI); return 1;
    case FFI_F32: lua_pushnumber(L, (double)(float)retD); return 1;
    case FFI_F64: lua_pushnumber(L, retD); return 1;
    case FFI_PTR: lua_pushlightuserdata(L, (void*)retI); return 1;
    case FFI_STR: lua_pushstring(L, (const char*)retI); return 1;
    case FFI_BUF: lua_pushlightuserdata(L, (void*)retI); return 1;
    default: return 0;
    }
}

static int ffi_cast(lua_State* L)
{
    const char* typeName = luaL_checkstring(L, 1);
    FFI_Type type = parseType(L, typeName);
    
    switch(type)
    {
        case FFI_I32:
            lua_pushnumber(L, (double)(int32_t)luaL_checknumber(L, 2));
            return 1;
        case FFI_I64:
            lua_pushnumber(L, (double)(int64_t)luaL_checknumber(L, 2));
            return 1;
        case FFI_F32:
            lua_pushnumber(L, (double)(float)luaL_checknumber(L, 2));
            return 1;
        case FFI_F64:
            lua_pushnumber(L, luaL_checknumber(L, 2));
            return 1;
        case FFI_PTR:
        {
            if (lua_isbuffer(L, 2))
            {
                size_t len;
                lua_pushlightuserdata(L, lua_tobuffer(L, 2, &len));
                return 1;
            }
            if (lua_islightuserdata(L, 2))
            {
                lua_pushvalue(L, 2);
                return 1;
            }
            if (lua_type(L, 2) == LUA_TNUMBER)
            {
                lua_pushlightuserdata(L, (void*)(intptr_t)lua_tonumber(L, 2));
                return 1;
            }
            luaL_error(L, "invalid cast to ptr");
            return 1;
        }
        default:
            luaL_error(L, "cast not supported for type");
            return 0;
    }
}

static int ffi_ptr(lua_State* L)
{
    size_t len;
    void* data = luaL_checkbuffer(L, 1, &len);
    lua_pushlightuserdata(L, data);
    return 1;
}

static int ffi_sizeof(lua_State* L)
{
    const char* typeName = luaL_checkstring(L, 1);
    FFI_Type type = parseType(L, typeName);
    lua_pushnumber(L, (double)typeSize(type));
    return 1;
}

static const luaL_Reg ffilib[] = {
    {"open", ffi_open},
    {"sym", ffi_sym},
    {"cast", ffi_cast},
    {"ptr", ffi_ptr},
    {"sizeof", ffi_sizeof},
    {nullptr, nullptr}
};

LUALIB_API int luaopen_ffi(lua_State* L)
{
    luaL_newmetatable(L, FFI_LIBRARY_MT);
    lua_pushcfunction(L, ffi_library_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, ffi_library_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    luaL_newmetatable(L, FFI_SYMBOL_MT);
    lua_pushcfunction(L, ffi_symbol_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, ffi_symbol_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, ffi_symbol_call, "__call");
    lua_setfield(L, -2, "__call");
    lua_pop(L, 1);

    luaL_register(L, LUA_FFILIBNAME, ffilib);
    
    lua_pushlightuserdata(L, nullptr);
    lua_setfield(L, -2, "nullptr");

    return 1;
}
