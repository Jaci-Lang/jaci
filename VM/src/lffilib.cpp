// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

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
#define FFI_C_MT "FFI_C"
#define FFI_STRUCT_MT "FFI_Struct"

#define LUA_FFILIBNAME "ffi"

enum FFI_TypeKind
{
    FFI_VOID = 0,
    FFI_BOOL,
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

struct FFI_FieldInfo
{
    std::string name;
    FFI_TypeKind type;
    size_t offset;
    size_t size;
};

struct FFI_StructDef
{
    std::string name;
    size_t totalSize;
    size_t alignment;
    std::vector<FFI_FieldInfo> fields;
    std::unordered_map<std::string, size_t> fieldMap;
};

struct FFI_TypeInfo
{
    FFI_TypeKind kind;
    size_t size;
    size_t align;
    std::string structName;
};

struct FFI_Library
{
    void* handle;
    char* path;
    bool isDefault;
};

struct FFI_Symbol
{
    void* func;
    FFI_Library* lib;
    char* name;
    FFI_TypeKind retType;
    int argCount;
    FFI_TypeKind argTypes[16];
};

struct FFI_State
{
    std::unordered_map<std::string, FFI_TypeInfo> typeAliases;
    std::unordered_map<std::string, FFI_StructDef> structDefs;
    std::unordered_map<std::string, FFI_Symbol> declaredSymbols;
    void* defaultLibHandle;
};

static FFI_State* g_ffiState = nullptr;

static void initFFITypes(FFI_State* s)
{
    auto addType = [s](const char* name, FFI_TypeKind kind, size_t size, size_t align) {
        s->typeAliases[name] = {kind, size, align, ""};
    };

    addType("void", FFI_VOID, 0, 1);
    addType("bool", FFI_BOOL, sizeof(bool), alignof(bool));
    addType("_Bool", FFI_BOOL, sizeof(bool), alignof(bool));

    addType("char", FFI_I8, 1, 1);
    addType("signed char", FFI_I8, 1, 1);
    addType("unsigned char", FFI_U8, 1, 1);
    addType("int8_t", FFI_I8, 1, 1);
    addType("uint8_t", FFI_U8, 1, 1);
    addType("i8", FFI_I8, 1, 1);
    addType("u8", FFI_U8, 1, 1);
    addType("byte", FFI_U8, 1, 1);

    addType("short", FFI_I16, 2, alignof(int16_t));
    addType("short int", FFI_I16, 2, alignof(int16_t));
    addType("signed short", FFI_I16, 2, alignof(int16_t));
    addType("unsigned short", FFI_U16, 2, alignof(uint16_t));
    addType("int16_t", FFI_I16, 2, alignof(int16_t));
    addType("uint16_t", FFI_U16, 2, alignof(uint16_t));
    addType("i16", FFI_I16, 2, alignof(int16_t));
    addType("u16", FFI_U16, 2, alignof(uint16_t));

    addType("int", FFI_I32, 4, alignof(int32_t));
    addType("signed int", FFI_I32, 4, alignof(int32_t));
    addType("unsigned int", FFI_U32, 4, alignof(uint32_t));
    addType("unsigned", FFI_U32, 4, alignof(uint32_t));
    addType("int32_t", FFI_I32, 4, alignof(int32_t));
    addType("uint32_t", FFI_U32, 4, alignof(uint32_t));
    addType("i32", FFI_I32, 4, alignof(int32_t));
    addType("u32", FFI_U32, 4, alignof(uint32_t));

    addType("long", sizeof(long) == 8 ? FFI_I64 : FFI_I32, sizeof(long), alignof(long));
    addType("unsigned long", sizeof(long) == 8 ? FFI_U64 : FFI_U32, sizeof(unsigned long), alignof(unsigned long));
    addType("long long", FFI_I64, 8, alignof(int64_t));
    addType("signed long long", FFI_I64, 8, alignof(int64_t));
    addType("unsigned long long", FFI_U64, 8, alignof(uint64_t));
    addType("int64_t", FFI_I64, 8, alignof(int64_t));
    addType("uint64_t", FFI_U64, 8, alignof(uint64_t));
    addType("i64", FFI_I64, 8, alignof(int64_t));
    addType("u64", FFI_U64, 8, alignof(uint64_t));

    addType("float", FFI_F32, sizeof(float), alignof(float));
    addType("f32", FFI_F32, sizeof(float), alignof(float));
    addType("double", FFI_F64, sizeof(double), alignof(double));
    addType("f64", FFI_F64, sizeof(double), alignof(double));

    addType("size_t", sizeof(size_t) == 8 ? FFI_U64 : FFI_U32, sizeof(size_t), alignof(size_t));
    addType("ssize_t", sizeof(size_t) == 8 ? FFI_I64 : FFI_I32, sizeof(size_t), alignof(size_t));
    addType("intptr_t", sizeof(intptr_t) == 8 ? FFI_I64 : FFI_I32, sizeof(intptr_t), alignof(intptr_t));
    addType("uintptr_t", sizeof(uintptr_t) == 8 ? FFI_U64 : FFI_U32, sizeof(uintptr_t), alignof(uintptr_t));
    addType("ptrdiff_t", sizeof(ptrdiff_t) == 8 ? FFI_I64 : FFI_I32, sizeof(ptrdiff_t), alignof(ptrdiff_t));

    addType("ptr", FFI_PTR, sizeof(void*), alignof(void*));
    addType("void*", FFI_PTR, sizeof(void*), alignof(void*));
    addType("void *", FFI_PTR, sizeof(void*), alignof(void*));
    addType("str", FFI_STR, sizeof(const char*), alignof(const char*));
    addType("const char*", FFI_STR, sizeof(const char*), alignof(const char*));
    addType("const char *", FFI_STR, sizeof(const char*), alignof(const char*));
    addType("char*", FFI_STR, sizeof(char*), alignof(char*));
    addType("char *", FFI_STR, sizeof(char*), alignof(char*));
    addType("buf", FFI_BUF, sizeof(void*), alignof(void*));
}

static FFI_TypeKind resolveTypeKind(lua_State* L, const char* name, size_t* outSize = nullptr, size_t* outAlign = nullptr)
{
    if (!g_ffiState)
    {
        g_ffiState = new FFI_State();
        initFFITypes(g_ffiState);
    }

    std::string cleanName = name;
    while (!cleanName.empty() && isspace(cleanName.front())) cleanName.erase(cleanName.begin());
    while (!cleanName.empty() && isspace(cleanName.back())) cleanName.pop_back();

    if (cleanName.back() == '*')
    {
        if (cleanName == "const char*" || cleanName == "char*" || cleanName == "const char *" || cleanName == "char *")
        {
            if (outSize) *outSize = sizeof(const char*);
            if (outAlign) *outAlign = alignof(const char*);
            return FFI_STR;
        }
        if (outSize) *outSize = sizeof(void*);
        if (outAlign) *outAlign = alignof(void*);
        return FFI_PTR;
    }

    auto it = g_ffiState->typeAliases.find(cleanName);
    if (it != g_ffiState->typeAliases.end())
    {
        if (outSize) *outSize = it->second.size;
        if (outAlign) *outAlign = it->second.align;
        return it->second.kind;
    }

    if (L)
        luaL_error(L, "unknown FFI type: '%s'", name);
    return FFI_VOID;
}

#if defined(__x86_64__) && !defined(_WIN32)
static void ffi_invoke_native_x64(
    void* func,
    int numGpr, const uint64_t* gprArgs,
    int numSse, const double* sseArgs,
    int numStack, const uint64_t* stackArgs,
    FFI_TypeKind retKind,
    uint64_t* outInt,
    double* outDouble
)
{
    register uint64_t r_rax __asm__("rax") = (uint64_t)numSse;
    register uint64_t r_rdi __asm__("rdi") = numGpr > 0 ? gprArgs[0] : 0;
    register uint64_t r_rsi __asm__("rsi") = numGpr > 1 ? gprArgs[1] : 0;
    register uint64_t r_rdx __asm__("rdx") = numGpr > 2 ? gprArgs[2] : 0;
    register uint64_t r_rcx __asm__("rcx") = numGpr > 3 ? gprArgs[3] : 0;
    register uint64_t r_r8  __asm__("r8")  = numGpr > 4 ? gprArgs[4] : 0;
    register uint64_t r_r9  __asm__("r9")  = numGpr > 5 ? gprArgs[5] : 0;

    register double r_xmm0 __asm__("xmm0") = numSse > 0 ? sseArgs[0] : 0.0;
    register double r_xmm1 __asm__("xmm1") = numSse > 1 ? sseArgs[1] : 0.0;
    register double r_xmm2 __asm__("xmm2") = numSse > 2 ? sseArgs[2] : 0.0;
    register double r_xmm3 __asm__("xmm3") = numSse > 3 ? sseArgs[3] : 0.0;
    register double r_xmm4 __asm__("xmm4") = numSse > 4 ? sseArgs[4] : 0.0;
    register double r_xmm5 __asm__("xmm5") = numSse > 5 ? sseArgs[5] : 0.0;
    register double r_xmm6 __asm__("xmm6") = numSse > 6 ? sseArgs[6] : 0.0;
    register double r_xmm7 __asm__("xmm7") = numSse > 7 ? sseArgs[7] : 0.0;

    __asm__ __volatile__(
        "pushq %%rbp\n\t"
        "movq %%rsp, %%rbp\n\t"
        "andq $-16, %%rsp\n\t"
        "call *%[fn]\n\t"
        "movq %%rbp, %%rsp\n\t"
        "popq %%rbp\n\t"
        : "+r"(r_rax), "+x"(r_xmm0)
        : [fn]"r"(func),
          "r"(r_rdi), "r"(r_rsi), "r"(r_rdx), "r"(r_rcx), "r"(r_r8), "r"(r_r9),
          "x"(r_xmm1), "x"(r_xmm2), "x"(r_xmm3), "x"(r_xmm4), "x"(r_xmm5), "x"(r_xmm6), "x"(r_xmm7)
        : "r10", "r11", "memory"
    );

    if (outInt) *outInt = r_rax;
    if (outDouble) *outDouble = r_xmm0;
}
#endif

typedef intptr_t (*ffi_fn0)();
typedef intptr_t (*ffi_fn1)(intptr_t);
typedef intptr_t (*ffi_fn2)(intptr_t, intptr_t);
typedef intptr_t (*ffi_fn3)(intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn4)(intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn5)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn6)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn7)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t (*ffi_fn8)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);

typedef double (*ffi_fnd0)();
typedef double (*ffi_fnd1)(intptr_t);
typedef double (*ffi_fnd2)(intptr_t, intptr_t);
typedef double (*ffi_fnd3)(intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd4)(intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd5)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef double (*ffi_fnd6)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);

typedef double (*ffi_fnd_d1)(double);
typedef double (*ffi_fnd_d2)(double, double);
typedef double (*ffi_fnd_d3)(double, double, double);
typedef double (*ffi_fnd_d4)(double, double, double, double);

static void execute_call(
    void* func,
    int argCount,
    const FFI_TypeKind* argTypes,
    const intptr_t* gprValues,
    const double* sseValues,
    FFI_TypeKind retType,
    uint64_t* outInt,
    double* outDouble
)
{
#if defined(__x86_64__) && !defined(_WIN32)
    uint64_t gprArgs[6] = {0};
    double sseArgs[8] = {0.0};
    int gprCount = 0;
    int sseCount = 0;

    for (int i = 0; i < argCount; ++i)
    {
        if (argTypes[i] == FFI_F32 || argTypes[i] == FFI_F64)
        {
            if (sseCount < 8)
                sseArgs[sseCount++] = sseValues[i];
        }
        else
        {
            if (gprCount < 6)
                gprArgs[gprCount++] = (uint64_t)gprValues[i];
        }
    }

    ffi_invoke_native_x64(func, gprCount, gprArgs, sseCount, sseArgs, 0, nullptr, retType, outInt, outDouble);
#else
    bool retDouble = (retType == FFI_F32 || retType == FFI_F64);
    bool allDouble = true;
    for (int i = 0; i < argCount; ++i)
    {
        if (argTypes[i] != FFI_F32 && argTypes[i] != FFI_F64)
        {
            allDouble = false;
            break;
        }
    }

    if (argCount > 0 && allDouble && retDouble)
    {
        double retD = 0.0;
        switch (argCount)
        {
        case 1: retD = ((ffi_fnd_d1)func)(sseValues[0]); break;
        case 2: retD = ((ffi_fnd_d2)func)(sseValues[0], sseValues[1]); break;
        case 3: retD = ((ffi_fnd_d3)func)(sseValues[0], sseValues[1], sseValues[2]); break;
        case 4: retD = ((ffi_fnd_d4)func)(sseValues[0], sseValues[1], sseValues[2], sseValues[3]); break;
        default: retD = ((ffi_fnd_d1)func)(sseValues[0]); break;
        }
        *outDouble = retD;
        *outInt = 0;
    }
    else if (retDouble)
    {
        double retD = 0.0;
        switch (argCount)
        {
        case 0: retD = ((ffi_fnd0)func)(); break;
        case 1: retD = ((ffi_fnd1)func)(gprValues[0]); break;
        case 2: retD = ((ffi_fnd2)func)(gprValues[0], gprValues[1]); break;
        case 3: retD = ((ffi_fnd3)func)(gprValues[0], gprValues[1], gprValues[2]); break;
        case 4: retD = ((ffi_fnd4)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3]); break;
        case 5: retD = ((ffi_fnd5)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3], gprValues[4]); break;
        case 6: retD = ((ffi_fnd6)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3], gprValues[4], gprValues[5]); break;
        default: retD = 0.0; break;
        }
        *outDouble = retD;
        *outInt = 0;
    }
    else
    {
        intptr_t retI = 0;
        switch (argCount)
        {
        case 0: retI = ((ffi_fn0)func)(); break;
        case 1: retI = ((ffi_fn1)func)(gprValues[0]); break;
        case 2: retI = ((ffi_fn2)func)(gprValues[0], gprValues[1]); break;
        case 3: retI = ((ffi_fn3)func)(gprValues[0], gprValues[1], gprValues[2]); break;
        case 4: retI = ((ffi_fn4)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3]); break;
        case 5: retI = ((ffi_fn5)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3], gprValues[4]); break;
        case 6: retI = ((ffi_fn6)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3], gprValues[4], gprValues[5]); break;
        case 7: retI = ((ffi_fn7)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3], gprValues[4], gprValues[5], gprValues[6]); break;
        case 8: retI = ((ffi_fn8)func)(gprValues[0], gprValues[1], gprValues[2], gprValues[3], gprValues[4], gprValues[5], gprValues[6], gprValues[7]); break;
        default: retI = 0; break;
        }
        *outInt = (uint64_t)retI;
        *outDouble = 0.0;
    }
#endif
}

static int pushFFIReturnValue(lua_State* L, FFI_TypeKind retType, uint64_t retI, double retD)
{
    switch (retType)
    {
    case FFI_VOID:
        return 0;
    case FFI_BOOL:
        lua_pushboolean(L, (retI != 0));
        return 1;
    case FFI_I8:
        lua_pushnumber(L, (double)(int8_t)retI);
        return 1;
    case FFI_U8:
        lua_pushnumber(L, (double)(uint8_t)retI);
        return 1;
    case FFI_I16:
        lua_pushnumber(L, (double)(int16_t)retI);
        return 1;
    case FFI_U16:
        lua_pushnumber(L, (double)(uint16_t)retI);
        return 1;
    case FFI_I32:
        lua_pushnumber(L, (double)(int32_t)retI);
        return 1;
    case FFI_U32:
        lua_pushnumber(L, (double)(uint32_t)retI);
        return 1;
    case FFI_I64:
        lua_pushnumber(L, (double)(int64_t)retI);
        return 1;
    case FFI_U64:
        lua_pushnumber(L, (double)(uint64_t)retI);
        return 1;
    case FFI_F32:
        lua_pushnumber(L, (double)(float)retD);
        return 1;
    case FFI_F64:
        lua_pushnumber(L, retD);
        return 1;
    case FFI_PTR:
        lua_pushlightuserdata(L, (void*)retI);
        return 1;
    case FFI_STR:
        if ((void*)retI == nullptr)
            lua_pushnil(L);
        else
            lua_pushstring(L, (const char*)retI);
        return 1;
    case FFI_BUF:
        lua_pushlightuserdata(L, (void*)retI);
        return 1;
    default:
        return 0;
    }
}

static int ffi_symbol_call(lua_State* L)
{
    FFI_Symbol* sym = (FFI_Symbol*)luaL_checkudata(L, 1, FFI_SYMBOL_MT);

    intptr_t gprArgs[16] = {0};
    double sseArgs[16] = {0.0};

    for (int i = 0; i < sym->argCount; i++)
    {
        int lua_arg = i + 2;
        switch (sym->argTypes[i])
        {
        case FFI_BOOL:
            gprArgs[i] = lua_toboolean(L, lua_arg) ? 1 : 0;
            break;
        case FFI_I8:
        case FFI_U8:
        case FFI_I16:
        case FFI_U16:
        case FFI_I32:
        case FFI_U32:
        case FFI_I64:
        case FFI_U64:
            gprArgs[i] = (intptr_t)luaL_checknumber(L, lua_arg);
            break;
        case FFI_F32:
        {
            float v = (float)luaL_checknumber(L, lua_arg);
            sseArgs[i] = (double)v;
            break;
        }
        case FFI_F64:
        {
            sseArgs[i] = (double)luaL_checknumber(L, lua_arg);
            break;
        }
        case FFI_PTR:
        {
            if (lua_isnil(L, lua_arg))
            {
                gprArgs[i] = 0;
            }
            else if (lua_islightuserdata(L, lua_arg))
            {
                gprArgs[i] = (intptr_t)lua_touserdata(L, lua_arg);
            }
            else if (lua_isbuffer(L, lua_arg))
            {
                size_t len;
                gprArgs[i] = (intptr_t)lua_tobuffer(L, lua_arg, &len);
            }
            else if (lua_type(L, lua_arg) == LUA_TNUMBER)
            {
                gprArgs[i] = (intptr_t)lua_tonumber(L, lua_arg);
            }
            else
            {
                gprArgs[i] = (intptr_t)lua_touserdata(L, lua_arg);
            }
            break;
        }
        case FFI_STR:
        {
            if (lua_isnil(L, lua_arg))
                gprArgs[i] = 0;
            else
                gprArgs[i] = (intptr_t)luaL_checkstring(L, lua_arg);
            break;
        }
        case FFI_BUF:
        {
            size_t len;
            gprArgs[i] = (intptr_t)luaL_checkbuffer(L, lua_arg, &len);
            break;
        }
        default:
            gprArgs[i] = 0;
            break;
        }
    }

    uint64_t retI = 0;
    double retD = 0.0;

    execute_call(sym->func, sym->argCount, sym->argTypes, gprArgs, sseArgs, sym->retType, &retI, &retD);

    return pushFFIReturnValue(L, sym->retType, retI, retD);
}

static int ffi_symbol_gc(lua_State* L)
{
    FFI_Symbol* sym = (FFI_Symbol*)lua_touserdata(L, 1);
    if (sym->name)
    {
        free(sym->name);
        sym->name = nullptr;
    }
    return 0;
}

static int ffi_symbol_tostring(lua_State* L)
{
    FFI_Symbol* sym = (FFI_Symbol*)luaL_checkudata(L, 1, FFI_SYMBOL_MT);
    lua_pushfstring(L, "ffi.symbol (%s)", sym->name ? sym->name : "anonymous");
    return 1;
}

static FFI_Symbol* createFFISymbol(
    lua_State* L,
    void* func,
    FFI_Library* lib,
    const char* name,
    FFI_TypeKind retType,
    int argCount,
    const FFI_TypeKind* argTypes
)
{
    FFI_Symbol* sym = (FFI_Symbol*)lua_newuserdata(L, sizeof(FFI_Symbol));
    sym->func = func;
    sym->lib = lib;
    sym->name = name ? strdup(name) : nullptr;
    sym->retType = retType;
    sym->argCount = argCount;
    for (int i = 0; i < argCount; ++i)
        sym->argTypes[i] = argTypes[i];

    luaL_getmetatable(L, FFI_SYMBOL_MT);
    lua_setmetatable(L, -2);
    return sym;
}

static void* resolveNativeSymbol(void* handle, const char* name)
{
    if (!name) return nullptr;
#if defined(_WIN32)
    if (!handle)
    {
        HMODULE hMod = GetModuleHandleA(NULL);
        void* p = (void*)GetProcAddress(hMod, name);
        if (p) return p;
        HMODULE hCrt = GetModuleHandleA("msvcrt.dll");
        if (!hCrt) hCrt = LoadLibraryA("msvcrt.dll");
        if (hCrt) return (void*)GetProcAddress(hCrt, name);
        return nullptr;
    }
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    if (!handle)
    {
        void* p = dlsym(RTLD_DEFAULT, name);
        if (p) return p;
        static void* s_libc = dlopen("libc.so.6", RTLD_LAZY);
        if (!s_libc) s_libc = dlopen("libc.so", RTLD_LAZY);
        if (s_libc)
        {
            p = dlsym(s_libc, name);
            if (p) return p;
        }
        static void* s_libm = dlopen("libm.so.6", RTLD_LAZY);
        if (!s_libm) s_libm = dlopen("libm.so", RTLD_LAZY);
        if (s_libm)
        {
            p = dlsym(s_libm, name);
            if (p) return p;
        }
        return nullptr;
    }
    return dlsym(handle, name);
#endif
}

enum FFI_SecurityMode
{
    FFI_SEC_PERMISSIVE = 0,
    FFI_SEC_STRICT,
    FFI_SEC_DISABLED
};

static FFI_SecurityMode g_ffiSecurityMode = FFI_SEC_PERMISSIVE;
static std::vector<std::string> g_allowedLibraries;
static std::vector<std::string> g_deniedLibraries;

static bool isLibraryAllowed(const char* path)
{
    if (g_ffiSecurityMode == FFI_SEC_DISABLED)
        return false;
    if (!path) return true; // Default / libc
    std::string s(path);
    for (const auto& d : g_deniedLibraries)
    {
        if (s.find(d) != std::string::npos)
            return false;
    }
    if (g_ffiSecurityMode == FFI_SEC_STRICT && !g_allowedLibraries.empty())
    {
        for (const auto& a : g_allowedLibraries)
        {
            if (s.find(a) != std::string::npos)
                return true;
        }
        return false;
    }
    return true;
}

static bool isPointerSafe(const void* ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    return addr >= 0x10000;
}

static int ffi_open(lua_State* L)
{
    const char* path = nullptr;
    bool isDefault = false;

    if (lua_isnoneornil(L, 1))
    {
        isDefault = true;
    }
    else
    {
        path = luaL_checkstring(L, 1);
    }

    if (!isLibraryAllowed(path))
    {
        luaL_error(L, "FFI security policy denied loading library '%s'", path ? path : "default");
    }

    void* handle = nullptr;
    if (!isDefault)
    {
#if defined(_WIN32)
        handle = (void*)LoadLibraryA(path);
        if (!handle)
            luaL_error(L, "error loading library '%s': %u", path, GetLastError());
#else
        handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (!handle)
            luaL_error(L, "error loading library '%s': %s", path, dlerror());
#endif
    }

    FFI_Library* lib = (FFI_Library*)lua_newuserdata(L, sizeof(FFI_Library));
    lib->handle = handle;
    lib->path = path ? strdup(path) : strdup("default");
    lib->isDefault = isDefault;

    luaL_getmetatable(L, FFI_LIBRARY_MT);
    lua_setmetatable(L, -2);

    return 1;
}

static int ffi_library_gc(lua_State* L)
{
    FFI_Library* lib = (FFI_Library*)lua_touserdata(L, 1);
    if (lib->handle && !lib->isDefault)
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
        free(lib->path);
        lib->path = nullptr;
    }
    return 0;
}

static int ffi_library_tostring(lua_State* L)
{
    FFI_Library* lib = (FFI_Library*)luaL_checkudata(L, 1, FFI_LIBRARY_MT);
    if (lib->path)
        lua_pushfstring(L, "ffi.library (%s)", lib->path);
    else
        lua_pushstring(L, "ffi.library (closed)");
    return 1;
}

static int ffi_library_sym(lua_State* L)
{
    FFI_Library* lib = (FFI_Library*)luaL_checkudata(L, 1, FFI_LIBRARY_MT);
    const char* name = luaL_checkstring(L, 2);
    const char* retTypeName = luaL_checkstring(L, 3);

    void* func = resolveNativeSymbol(lib->handle, name);
    if (!func)
    {
#if defined(_WIN32)
        luaL_error(L, "symbol not found '%s': %u", name, GetLastError());
#else
        luaL_error(L, "symbol not found '%s': %s", name, dlerror());
#endif
    }

    int n = lua_gettop(L) - 3;
    if (n > 16)
        luaL_error(L, "maximum 16 arguments supported");

    FFI_TypeKind retType = resolveTypeKind(L, retTypeName);
    FFI_TypeKind argTypes[16];
    for (int i = 0; i < n; i++)
    {
        argTypes[i] = resolveTypeKind(L, luaL_checkstring(L, 4 + i));
    }

    createFFISymbol(L, func, lib, name, retType, n, argTypes);
    return 1;
}

static int ffi_library_index(lua_State* L)
{
    FFI_Library* lib = (FFI_Library*)luaL_checkudata(L, 1, FFI_LIBRARY_MT);
    const char* key = luaL_checkstring(L, 2);

    if (strcmp(key, "sym") == 0)
    {
        lua_pushcfunction(L, ffi_library_sym, "sym");
        return 1;
    }
    if (strcmp(key, "close") == 0)
    {
        lua_pushcfunction(L, ffi_library_gc, "close");
        return 1;
    }

    if (g_ffiState)
    {
        auto it = g_ffiState->declaredSymbols.find(key);
        if (it != g_ffiState->declaredSymbols.end())
        {
            const FFI_Symbol& decl = it->second;
            void* func = resolveNativeSymbol(lib->handle, key);
            if (func)
            {
                createFFISymbol(L, func, lib, key, decl.retType, decl.argCount, decl.argTypes);
                return 1;
            }
        }
    }

    void* func = resolveNativeSymbol(lib->handle, key);
    if (func)
    {
        FFI_TypeKind noArgs[1] = {FFI_VOID};
        createFFISymbol(L, func, lib, key, FFI_PTR, 0, noArgs);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}

static int ffi_c_index(lua_State* L)
{
    const char* key = luaL_checkstring(L, 2);

    if (g_ffiState)
    {
        auto it = g_ffiState->declaredSymbols.find(key);
        if (it != g_ffiState->declaredSymbols.end())
        {
            const FFI_Symbol& decl = it->second;
            void* func = resolveNativeSymbol(nullptr, key);
            if (func)
            {
                createFFISymbol(L, func, nullptr, key, decl.retType, decl.argCount, decl.argTypes);
                return 1;
            }
        }
    }

    void* func = resolveNativeSymbol(nullptr, key);
    if (func)
    {
        FFI_TypeKind noArgs[1] = {FFI_VOID};
        createFFISymbol(L, func, nullptr, key, FFI_PTR, 0, noArgs);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}

static void parseCDefDeclarations(lua_State* L, const char* cdefStr)
{
    if (!g_ffiState)
    {
        g_ffiState = new FFI_State();
        initFFITypes(g_ffiState);
    }

    const char* p = cdefStr;
    while (*p)
    {
        while (*p && (isspace(*p) || *p == ';')) p++;
        if (!*p) break;

        if (p[0] == '/' && p[1] == '/')
        {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        const char* stmtStart = p;
        while (*p && *p != ';') p++;
        size_t stmtLen = p - stmtStart;
        if (*p == ';') p++;

        std::string stmt(stmtStart, stmtLen);
        while (!stmt.empty() && isspace(stmt.front())) stmt.erase(stmt.begin());
        while (!stmt.empty() && isspace(stmt.back())) stmt.pop_back();
        if (stmt.empty()) continue;

        if (stmt.rfind("typedef", 0) == 0)
        {
            std::string body = stmt.substr(7);
            while (!body.empty() && isspace(body.front())) body.erase(body.begin());
            size_t lastSpace = body.rfind(' ');
            if (lastSpace != std::string::npos)
            {
                std::string targetType = body.substr(0, lastSpace);
                std::string alias = body.substr(lastSpace + 1);
                while (!alias.empty() && isspace(alias.front())) alias.erase(alias.begin());
                while (!targetType.empty() && isspace(targetType.back())) targetType.pop_back();

                size_t sz = 0, al = 0;
                FFI_TypeKind kind = resolveTypeKind(nullptr, targetType.c_str(), &sz, &al);
                if (kind != FFI_VOID || targetType == "void")
                {
                    g_ffiState->typeAliases[alias] = {kind, sz, al, ""};
                }
            }
            continue;
        }

        size_t parenOpen = stmt.find('(');
        size_t parenClose = stmt.find(')', parenOpen);
        if (parenOpen != std::string::npos && parenClose != std::string::npos)
        {
            std::string beforeParen = stmt.substr(0, parenOpen);
            while (!beforeParen.empty() && isspace(beforeParen.back())) beforeParen.pop_back();

            size_t fnNameIdx = beforeParen.rfind(' ');
            if (fnNameIdx != std::string::npos)
            {
                std::string retTypeStr = beforeParen.substr(0, fnNameIdx);
                std::string fnName = beforeParen.substr(fnNameIdx + 1);

                while (!fnName.empty() && (fnName.front() == '*' || isspace(fnName.front())))
                {
                    if (fnName.front() == '*') retTypeStr += "*";
                    fnName.erase(fnName.begin());
                }

                if (!fnName.empty())
                {
                    FFI_TypeKind retKind = resolveTypeKind(nullptr, retTypeStr.c_str());

                    std::string argsStr = stmt.substr(parenOpen + 1, parenClose - parenOpen - 1);
                    std::vector<FFI_TypeKind> argKinds;

                    size_t astart = 0;
                    while (astart < argsStr.size())
                    {
                        size_t aend = argsStr.find(',', astart);
                        if (aend == std::string::npos) aend = argsStr.size();

                        std::string oneArg = argsStr.substr(astart, aend - astart);
                        while (!oneArg.empty() && isspace(oneArg.front())) oneArg.erase(oneArg.begin());
                        while (!oneArg.empty() && isspace(oneArg.back())) oneArg.pop_back();

                        if (!oneArg.empty() && oneArg != "void" && oneArg != "...")
                        {
                            size_t argSpace = oneArg.rfind(' ');
                            std::string argType = oneArg;
                            if (argSpace != std::string::npos)
                            {
                                argType = oneArg.substr(0, argSpace);
                            }
                            argKinds.push_back(resolveTypeKind(nullptr, argType.c_str()));
                        }

                        astart = aend + 1;
                    }

                    FFI_Symbol symDecl;
                    symDecl.func = nullptr;
                    symDecl.lib = nullptr;
                    symDecl.name = strdup(fnName.c_str());
                    symDecl.retType = retKind;
                    symDecl.argCount = (int)argKinds.size();
                    for (size_t i = 0; i < argKinds.size() && i < 16; ++i)
                    {
                        symDecl.argTypes[i] = argKinds[i];
                    }

                    g_ffiState->declaredSymbols[fnName] = symDecl;
                }
            }
        }
    }
}

static int ffi_cdef(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    parseCDefDeclarations(L, s);
    return 0;
}

static int ffi_sym(lua_State* L)
{
    return ffi_library_sym(L);
}

static int ffi_cast(lua_State* L)
{
    const char* typeName = luaL_checkstring(L, 1);
    FFI_TypeKind type = resolveTypeKind(L, typeName);

    switch (type)
    {
    case FFI_BOOL:
        lua_pushboolean(L, lua_toboolean(L, 2));
        return 1;
    case FFI_I8:
        lua_pushnumber(L, (double)(int8_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_U8:
        lua_pushnumber(L, (double)(uint8_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_I16:
        lua_pushnumber(L, (double)(int16_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_U16:
        lua_pushnumber(L, (double)(uint16_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_I32:
        lua_pushnumber(L, (double)(int32_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_U32:
        lua_pushnumber(L, (double)(uint32_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_I64:
        lua_pushnumber(L, (double)(int64_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_U64:
        lua_pushnumber(L, (double)(uint64_t)luaL_checknumber(L, 2));
        return 1;
    case FFI_F32:
        lua_pushnumber(L, (double)(float)luaL_checknumber(L, 2));
        return 1;
    case FFI_F64:
        lua_pushnumber(L, luaL_checknumber(L, 2));
        return 1;
    case FFI_PTR:
    {
        if (lua_isnil(L, 2))
        {
            lua_pushlightuserdata(L, nullptr);
            return 1;
        }
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
    case FFI_STR:
    {
        if (lua_islightuserdata(L, 2))
        {
            const char* s = (const char*)lua_touserdata(L, 2);
            if (s) lua_pushstring(L, s); else lua_pushnil(L);
            return 1;
        }
        if (lua_isstring(L, 2))
        {
            lua_pushvalue(L, 2);
            return 1;
        }
        luaL_error(L, "invalid cast to str");
        return 1;
    }
    default:
        luaL_error(L, "cast not supported for type '%s'", typeName);
        return 0;
    }
}

static int ffi_ptr(lua_State* L)
{
    if (lua_isbuffer(L, 1))
    {
        size_t len;
        void* data = luaL_checkbuffer(L, 1, &len);
        lua_pushlightuserdata(L, data);
        return 1;
    }
    if (lua_isstring(L, 1))
    {
        const char* s = lua_tostring(L, 1);
        lua_pushlightuserdata(L, (void*)s);
        return 1;
    }
    if (lua_islightuserdata(L, 1))
    {
        lua_pushvalue(L, 1);
        return 1;
    }
    luaL_typeerror(L, 1, "buffer, string, or pointer");
    return 1;
}

static int ffi_string(lua_State* L)
{
    void* ptr = nullptr;
    if (lua_islightuserdata(L, 1))
    {
        ptr = lua_touserdata(L, 1);
    }
    else if (lua_isbuffer(L, 1))
    {
        size_t blen;
        ptr = luaL_checkbuffer(L, 1, &blen);
    }
    else if (lua_type(L, 1) == LUA_TNUMBER)
    {
        ptr = (void*)(intptr_t)lua_tonumber(L, 1);
    }
    else
    {
        luaL_typeerror(L, 1, "pointer or buffer");
    }

    if (!ptr)
    {
        lua_pushnil(L);
        return 1;
    }

    if (!lua_isnoneornil(L, 2))
    {
        size_t len = (size_t)luaL_checkinteger(L, 2);
        lua_pushlstring(L, (const char*)ptr, len);
    }
    else
    {
        lua_pushstring(L, (const char*)ptr);
    }
    return 1;
}

static int ffi_copy(lua_State* L)
{
    void* dst = nullptr;
    const void* src = nullptr;

    if (lua_islightuserdata(L, 1))
        dst = lua_touserdata(L, 1);
    else if (lua_isbuffer(L, 1))
    {
        size_t l;
        dst = luaL_checkbuffer(L, 1, &l);
    }
    else
        luaL_typeerror(L, 1, "pointer or buffer");

    size_t srclen = 0;
    if (lua_islightuserdata(L, 2))
        src = lua_touserdata(L, 2);
    else if (lua_isbuffer(L, 2))
        src = luaL_checkbuffer(L, 2, &srclen);
    else if (lua_isstring(L, 2))
        src = luaL_checklstring(L, 2, &srclen);
    else
        luaL_typeerror(L, 2, "pointer, buffer, or string");

    size_t count = (size_t)luaL_optinteger(L, 3, (int)srclen);
    if (!dst || !src || count == 0)
        return 0;

    memcpy(dst, src, count);
    return 0;
}

static int ffi_fill(lua_State* L)
{
    void* dst = nullptr;
    if (lua_islightuserdata(L, 1))
        dst = lua_touserdata(L, 1);
    else if (lua_isbuffer(L, 1))
    {
        size_t l;
        dst = luaL_checkbuffer(L, 1, &l);
    }
    else
        luaL_typeerror(L, 1, "pointer or buffer");

    size_t count = (size_t)luaL_checkinteger(L, 2);
    int val = luaL_optinteger(L, 3, 0);

    if (dst && count > 0)
    {
        memset(dst, val, count);
    }
    return 0;
}

static void* getPtrFromArg(lua_State* L, int idx, size_t* outBufLen = nullptr)
{
    if (lua_islightuserdata(L, idx))
    {
        if (outBufLen) *outBufLen = SIZE_MAX;
        return lua_touserdata(L, idx);
    }
    if (lua_isbuffer(L, idx))
    {
        size_t l = 0;
        void* p = luaL_checkbuffer(L, idx, &l);
        if (outBufLen) *outBufLen = l;
        return p;
    }
    if (lua_type(L, idx) == LUA_TNUMBER)
    {
        if (outBufLen) *outBufLen = SIZE_MAX;
        return (void*)(intptr_t)lua_tonumber(L, idx);
    }
    luaL_typeerror(L, idx, "pointer or buffer");
    return nullptr;
}

static int ffi_read(lua_State* L)
{
    size_t bufLen = 0;
    void* base = getPtrFromArg(L, 1, &bufLen);
    size_t offset = (size_t)luaL_optinteger(L, 2, 0);
    const char* typeName = luaL_checkstring(L, 3);

    size_t tsize = 0, talign = 0;
    FFI_TypeKind kind = resolveTypeKind(L, typeName, &tsize, &talign);

    if (!base)
    {
        lua_pushnil(L);
        return 1;
    }

    uint8_t* ptr = (uint8_t*)base + offset;

    switch (kind)
    {
    case FFI_BOOL:
        lua_pushboolean(L, *ptr != 0);
        return 1;
    case FFI_I8:
        lua_pushnumber(L, (double)*(int8_t*)ptr);
        return 1;
    case FFI_U8:
        lua_pushnumber(L, (double)*(uint8_t*)ptr);
        return 1;
    case FFI_I16:
    {
        int16_t v;
        memcpy(&v, ptr, 2);
        lua_pushnumber(L, (double)v);
        return 1;
    }
    case FFI_U16:
    {
        uint16_t v;
        memcpy(&v, ptr, 2);
        lua_pushnumber(L, (double)v);
        return 1;
    }
    case FFI_I32:
    {
        int32_t v;
        memcpy(&v, ptr, 4);
        lua_pushnumber(L, (double)v);
        return 1;
    }
    case FFI_U32:
    {
        uint32_t v;
        memcpy(&v, ptr, 4);
        lua_pushnumber(L, (double)v);
        return 1;
    }
    case FFI_I64:
    {
        int64_t v;
        memcpy(&v, ptr, 8);
        lua_pushnumber(L, (double)v);
        return 1;
    }
    case FFI_U64:
    {
        uint64_t v;
        memcpy(&v, ptr, 8);
        lua_pushnumber(L, (double)v);
        return 1;
    }
    case FFI_F32:
    {
        float v;
        memcpy(&v, ptr, 4);
        lua_pushnumber(L, (double)v);
        return 1;
    }
    case FFI_F64:
    {
        double v;
        memcpy(&v, ptr, 8);
        lua_pushnumber(L, v);
        return 1;
    }
    case FFI_PTR:
    case FFI_BUF:
    {
        void* v;
        memcpy(&v, ptr, sizeof(void*));
        lua_pushlightuserdata(L, v);
        return 1;
    }
    case FFI_STR:
    {
        const char* s;
        memcpy(&s, ptr, sizeof(const char*));
        if (s) lua_pushstring(L, s); else lua_pushnil(L);
        return 1;
    }
    default:
        lua_pushnil(L);
        return 1;
    }
}

static int ffi_write(lua_State* L)
{
    size_t bufLen = 0;
    void* base = getPtrFromArg(L, 1, &bufLen);
    size_t offset = (size_t)luaL_optinteger(L, 2, 0);
    const char* typeName = luaL_checkstring(L, 3);

    size_t tsize = 0, talign = 0;
    FFI_TypeKind kind = resolveTypeKind(L, typeName, &tsize, &talign);

    if (!base) return 0;
    uint8_t* ptr = (uint8_t*)base + offset;

    switch (kind)
    {
    case FFI_BOOL:
        *ptr = lua_toboolean(L, 4) ? 1 : 0;
        break;
    case FFI_I8:
        *(int8_t*)ptr = (int8_t)luaL_checknumber(L, 4);
        break;
    case FFI_U8:
        *(uint8_t*)ptr = (uint8_t)luaL_checknumber(L, 4);
        break;
    case FFI_I16:
    {
        int16_t v = (int16_t)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 2);
        break;
    }
    case FFI_U16:
    {
        uint16_t v = (uint16_t)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 2);
        break;
    }
    case FFI_I32:
    {
        int32_t v = (int32_t)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 4);
        break;
    }
    case FFI_U32:
    {
        uint32_t v = (uint32_t)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 4);
        break;
    }
    case FFI_I64:
    {
        int64_t v = (int64_t)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 8);
        break;
    }
    case FFI_U64:
    {
        uint64_t v = (uint64_t)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 8);
        break;
    }
    case FFI_F32:
    {
        float v = (float)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 4);
        break;
    }
    case FFI_F64:
    {
        double v = (double)luaL_checknumber(L, 4);
        memcpy(ptr, &v, 8);
        break;
    }
    case FFI_PTR:
    case FFI_BUF:
    {
        void* v = nullptr;
        if (lua_islightuserdata(L, 4))
            v = lua_touserdata(L, 4);
        else if (lua_isbuffer(L, 4))
        {
            size_t l;
            v = lua_tobuffer(L, 4, &l);
        }
        else if (lua_type(L, 4) == LUA_TNUMBER)
            v = (void*)(intptr_t)lua_tonumber(L, 4);
        memcpy(ptr, &v, sizeof(void*));
        break;
    }
    case FFI_STR:
    {
        const char* s = lua_isnil(L, 4) ? nullptr : luaL_checkstring(L, 4);
        memcpy(ptr, &s, sizeof(const char*));
        break;
    }
    default:
        break;
    }
    return 0;
}

static int ffi_sizeof(lua_State* L)
{
    if (lua_istable(L, 1))
    {
        lua_getfield(L, 1, "size");
        if (lua_isnumber(L, -1)) return 1;
        lua_pop(L, 1);
    }
    const char* typeName = luaL_checkstring(L, 1);
    size_t sz = 0, al = 0;
    resolveTypeKind(L, typeName, &sz, &al);
    lua_pushnumber(L, (double)sz);
    return 1;
}

static int ffi_alignof(lua_State* L)
{
    if (lua_istable(L, 1))
    {
        lua_getfield(L, 1, "align");
        if (lua_isnumber(L, -1)) return 1;
        lua_pop(L, 1);
    }
    const char* typeName = luaL_checkstring(L, 1);
    size_t sz = 0, al = 0;
    resolveTypeKind(L, typeName, &sz, &al);
    lua_pushnumber(L, (double)al);
    return 1;
}

static int ffi_errno(lua_State* L)
{
    if (!lua_isnoneornil(L, 1))
    {
        int newErr = luaL_checkinteger(L, 1);
        errno = newErr;
#if defined(_WIN32)
        SetLastError((DWORD)newErr);
#endif
        return 0;
    }
#if defined(_WIN32)
    DWORD err = GetLastError();
    if (err == 0 && errno != 0) err = (DWORD)errno;
    lua_pushinteger(L, (int)err);
#else
    lua_pushinteger(L, errno);
#endif
    return 1;
}

static int ffi_new(lua_State* L)
{
    const char* typeName = luaL_checkstring(L, 1);
    size_t count = (size_t)luaL_optinteger(L, 2, 1);
    if (count < 1) count = 1;

    size_t sz = 0, al = 0;
    resolveTypeKind(L, typeName, &sz, &al);
    size_t totalBytes = sz * count;

    lua_pushnumber(L, (double)totalBytes);
    lua_getglobal(L, "buffer");
    lua_getfield(L, -1, "create");
    lua_pushnumber(L, (double)totalBytes);
    lua_call(L, 1, 1);
    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}

static int ffi_struct(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    size_t currentOffset = 0;
    size_t maxAlign = 1;

    lua_createtable(L, 0, 8);
    lua_createtable(L, 0, 8);

    int nfields = (int)lua_objlen(L, 1);
    for (int i = 1; i <= nfields; ++i)
    {
        lua_rawgeti(L, 1, i);
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            continue;
        }

        lua_rawgeti(L, -1, 1);
        const char* fieldName = luaL_checkstring(L, -1);
        lua_pop(L, 1);

        lua_rawgeti(L, -1, 2);
        const char* fieldType = luaL_checkstring(L, -1);
        lua_pop(L, 1);

        size_t fsize = 0, falign = 0;
        resolveTypeKind(L, fieldType, &fsize, &falign);
        if (falign == 0) falign = 1;
        if (falign > maxAlign) maxAlign = falign;

        size_t padding = (falign - (currentOffset % falign)) % falign;
        currentOffset += padding;

        lua_createtable(L, 0, 4);
        lua_pushnumber(L, (double)currentOffset);
        lua_setfield(L, -2, "offset");
        lua_pushnumber(L, (double)fsize);
        lua_setfield(L, -2, "size");
        lua_pushstring(L, fieldType);
        lua_setfield(L, -2, "type");

        lua_setfield(L, -3, fieldName);

        currentOffset += fsize;
        lua_pop(L, 1);
    }

    size_t structPadding = (maxAlign - (currentOffset % maxAlign)) % maxAlign;
    currentOffset += structPadding;

    lua_setfield(L, -2, "fields");
    lua_pushnumber(L, (double)currentOffset);
    lua_setfield(L, -2, "size");
    lua_pushnumber(L, (double)maxAlign);
    lua_setfield(L, -2, "align");

    return 1;
}

static int ffi_mode(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
    {
        switch (g_ffiSecurityMode)
        {
        case FFI_SEC_STRICT: lua_pushstring(L, "strict"); break;
        case FFI_SEC_DISABLED: lua_pushstring(L, "disabled"); break;
        default: lua_pushstring(L, "permissive"); break;
        }
        return 1;
    }
    const char* m = luaL_checkstring(L, 1);
    if (strcmp(m, "strict") == 0) g_ffiSecurityMode = FFI_SEC_STRICT;
    else if (strcmp(m, "disabled") == 0) g_ffiSecurityMode = FFI_SEC_DISABLED;
    else if (strcmp(m, "permissive") == 0) g_ffiSecurityMode = FFI_SEC_PERMISSIVE;
    else luaL_error(L, "invalid FFI mode '%s' (expected 'permissive', 'strict', or 'disabled')", m);
    return 0;
}

static int ffi_allow_library(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    g_allowedLibraries.push_back(path);
    return 0;
}

static int ffi_deny_library(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    g_deniedLibraries.push_back(path);
    return 0;
}

static int ffi_is_safe(lua_State* L)
{
    if (!lua_islightuserdata(L, 1) && !lua_isbuffer(L, 1))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    const void* ptr = lua_islightuserdata(L, 1) ? lua_touserdata(L, 1) : lua_tobuffer(L, 1, nullptr);
    lua_pushboolean(L, isPointerSafe(ptr));
    return 1;
}

static int ffi_istype(lua_State* L)
{
    const char* typeName = luaL_checkstring(L, 1);
    FFI_TypeKind kind = resolveTypeKind(nullptr, typeName);
    bool match = false;
    switch (kind)
    {
    case FFI_BOOL: match = lua_isboolean(L, 2); break;
    case FFI_I8: case FFI_U8: case FFI_I16: case FFI_U16:
    case FFI_I32: case FFI_U32: case FFI_I64: case FFI_U64:
    case FFI_F32: case FFI_F64: match = lua_isnumber(L, 2); break;
    case FFI_STR: match = lua_isstring(L, 2); break;
    case FFI_BUF: match = lua_isbuffer(L, 2); break;
    case FFI_PTR: match = lua_islightuserdata(L, 2) || lua_isnil(L, 2); break;
    default: match = false; break;
    }
    lua_pushboolean(L, match ? 1 : 0);
    return 1;
}

static const luaL_Reg ffilib[] = {
    {"open", ffi_open},
    {"cdef", ffi_cdef},
    {"sym", ffi_sym},
    {"cast", ffi_cast},
    {"ptr", ffi_ptr},
    {"string", ffi_string},
    {"copy", ffi_copy},
    {"fill", ffi_fill},
    {"read", ffi_read},
    {"write", ffi_write},
    {"sizeof", ffi_sizeof},
    {"alignof", ffi_alignof},
    {"errno", ffi_errno},
    {"new", ffi_new},
    {"struct", ffi_struct},
    {"mode", ffi_mode},
    {"allow_library", ffi_allow_library},
    {"allowLibrary", ffi_allow_library},
    {"deny_library", ffi_deny_library},
    {"denyLibrary", ffi_deny_library},
    {"is_safe", ffi_is_safe},
    {"isSafe", ffi_is_safe},
    {"istype", ffi_istype},
    {nullptr, nullptr}
};

LUALIB_API int luaopen_ffi(lua_State* L)
{
    if (!g_ffiState)
    {
        g_ffiState = new FFI_State();
        initFFITypes(g_ffiState);
    }

    luaL_newmetatable(L, FFI_LIBRARY_MT);
    lua_pushcfunction(L, ffi_library_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, ffi_library_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, ffi_library_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newmetatable(L, FFI_SYMBOL_MT);
    lua_pushcfunction(L, ffi_symbol_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, ffi_symbol_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, ffi_symbol_call, "__call");
    lua_setfield(L, -2, "__call");
    lua_pop(L, 1);

    luaL_newmetatable(L, FFI_C_MT);
    lua_pushcfunction(L, ffi_c_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_register(L, LUA_FFILIBNAME, ffilib);

    lua_pushlightuserdata(L, nullptr);
    lua_setfield(L, -2, "nullptr");

    lua_newtable(L);
    luaL_getmetatable(L, FFI_C_MT);
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "C");

    return 1;
}
