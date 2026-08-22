// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee, Roblox Corporation, Lua.org/PUC-Rio.

#include "lualib.h"
#include "lcommon.h"
#include "ljni.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <cmath>
#include <locale.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#define JNI_OBJECT_MT "JNI_Object"
#define JNI_CLASS_MT "JNI_Class"
#define JNI_ARRAY_MT "JNI_Array"
#define JNI_TYPED_VALUE_MT "JNI_TypedValue"

#ifndef LUA_JNILIBNAME
#define LUA_JNILIBNAME "jni"
#endif

namespace {

// ---------------------------------------------------------------------------
// Dynamic Library Utilities
// ---------------------------------------------------------------------------
static void* osLoadLibrary(const char* path)
{
#if defined(_WIN32)
    return (void*)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
}

static void* osGetSymbol(void* handle, const char* symbol)
{
#if defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)handle, symbol);
#else
    return dlsym(handle, symbol);
#endif
}

static void osCloseLibrary(void* handle)
{
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

static bool fileExists(const std::string& path)
{
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode));
#endif
}

// ---------------------------------------------------------------------------
// Multiplatform JVM Auto-Discovery
// ---------------------------------------------------------------------------
static std::string detectJvmPath()
{
    // 1. Check JAVA_HOME environment variable
    const char* javaHome = getenv("JAVA_HOME");
    if (javaHome && javaHome[0] != '\0')
    {
        std::string jh(javaHome);
        if (jh.back() == '/' || jh.back() == '\\')
            jh.pop_back();

#if defined(_WIN32)
        std::vector<std::string> candidates = {
            jh + "\\bin\\server\\jvm.dll",
            jh + "\\jre\\bin\\server\\jvm.dll",
            jh + "\\bin\\client\\jvm.dll"
        };
#elif defined(__APPLE__)
        std::vector<std::string> candidates = {
            jh + "/lib/server/libjvm.dylib",
            jh + "/jre/lib/server/libjvm.dylib",
            jh + "/lib/libjvm.dylib"
        };
#else
        std::vector<std::string> candidates = {
            jh + "/lib/server/libjvm.so",
            jh + "/jre/lib/amd64/server/libjvm.so",
            jh + "/jre/lib/aarch64/server/libjvm.so",
            jh + "/jre/lib/i386/server/libjvm.so",
            jh + "/lib/client/libjvm.so",
            jh + "/lib/libjvm.so"
        };
#endif
        for (const auto& c : candidates)
        {
            if (fileExists(c))
                return c;
        }
    }

#if defined(__APPLE__)
    // Try macOS /usr/libexec/java_home tool
    FILE* pipe = popen("/usr/libexec/java_home 2>/dev/null", "r");
    if (pipe)
    {
        char buffer[1024];
        std::string home;
        if (fgets(buffer, sizeof(buffer), pipe))
        {
            home = buffer;
            while (!home.empty() && (home.back() == '\n' || home.back() == '\r'))
                home.pop_back();
        }
        pclose(pipe);
        if (!home.empty())
        {
            std::string dylib = home + "/lib/server/libjvm.dylib";
            if (fileExists(dylib)) return dylib;
            dylib = home + "/jre/lib/server/libjvm.dylib";
            if (fileExists(dylib)) return dylib;
        }
    }

    // Common macOS directories
    DIR* jvmDir = opendir("/Library/Java/JavaVirtualMachines");
    if (jvmDir)
    {
        struct dirent* entry;
        while ((entry = readdir(jvmDir)) != nullptr)
        {
            if (entry->d_name[0] == '.') continue;
            std::string p = std::string("/Library/Java/JavaVirtualMachines/") + entry->d_name + "/Contents/Home/lib/server/libjvm.dylib";
            if (fileExists(p))
            {
                closedir(jvmDir);
                return p;
            }
        }
        closedir(jvmDir);
    }
#elif defined(_WIN32)
    // Windows common installation paths
    const char* programFiles = getenv("ProgramFiles");
    if (programFiles)
    {
        std::vector<std::string> searchDirs = {
            std::string(programFiles) + "\\Java",
            std::string(programFiles) + "\\Eclipse Adoptium",
            std::string(programFiles) + "\\Microsoft",
            std::string(programFiles) + "\\Zulu"
        };
        for (const auto& sdir : searchDirs)
        {
            WIN32_FIND_DATAA findData;
            HANDLE hFind = FindFirstFileA((sdir + "\\*").c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE)
            {
                do {
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        if (strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)
                        {
                            std::string candidate = sdir + "\\" + findData.cFileName + "\\bin\\server\\jvm.dll";
                            if (fileExists(candidate))
                            {
                                FindClose(hFind);
                                return candidate;
                            }
                        }
                    }
                } while (FindNextFileA(hFind, &findData));
                FindClose(hFind);
            }
        }
    }
#else
    // Linux standard paths
    DIR* jvmDir = opendir("/usr/lib/jvm");
    if (jvmDir)
    {
        struct dirent* entry;
        while ((entry = readdir(jvmDir)) != nullptr)
        {
            if (entry->d_name[0] == '.') continue;
            std::string base = std::string("/usr/lib/jvm/") + entry->d_name;
            std::vector<std::string> candidates = {
                base + "/lib/server/libjvm.so",
                base + "/jre/lib/amd64/server/libjvm.so",
                base + "/jre/lib/aarch64/server/libjvm.so",
                base + "/lib/client/libjvm.so"
            };
            for (const auto& c : candidates)
            {
                if (fileExists(c))
                {
                    closedir(jvmDir);
                    return c;
                }
            }
        }
        closedir(jvmDir);
    }
#endif

    // Fallback names for dynamic linker search
#if defined(_WIN32)
    return "jvm.dll";
#elif defined(__APPLE__)
    return "libjvm.dylib";
#else
    return "libjvm.so";
#endif
}

// ---------------------------------------------------------------------------
// JNI Userdata Definitions
// ---------------------------------------------------------------------------
struct JObjectUserData {
    jobject object; // Global reference
    char* className;
};

struct JClassUserData {
    jclass clazz; // Global reference
    char* className;
};

struct JArrayUserData {
    jarray array; // Global reference
    char* elementTypeName;
    int length;
    bool isPrimitive;
    char primitiveType; // 'Z'=boolean, 'B'=byte, 'C'=char, 'S'=short, 'I'=int, 'J'=long, 'F'=float, 'D'=double, 'L'=object
};

struct JTypedValueUserData {
    char type; // 'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D', 'L'
    jvalue val;
};

// ---------------------------------------------------------------------------
// JVM Global Context
// ---------------------------------------------------------------------------
struct JvmContext {
    void* libHandle = nullptr;
    JavaVM* vm = nullptr;
    bool initialized = false;
    std::string loadedJvmPath;

    // Dynamically loaded entry points
    typedef jint (JNICALL *CreateJavaVM_t)(JavaVM**, void**, void*);
    typedef jint (JNICALL *GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
    typedef jint (JNICALL *GetDefaultJavaVMInitArgs_t)(void*);

    CreateJavaVM_t fnCreateJavaVM = nullptr;
    GetCreatedJavaVMs_t fnGetCreatedJavaVMs = nullptr;
    GetDefaultJavaVMInitArgs_t fnGetDefaultJavaVMInitArgs = nullptr;

    // Cached Global Class References
    jclass clsClass = nullptr;
    jclass clsMethod = nullptr;
    jclass clsConstructor = nullptr;
    jclass clsField = nullptr;
    jclass clsModifier = nullptr;
    jclass clsThrowable = nullptr;
    jclass clsStringWriter = nullptr;
    jclass clsPrintWriter = nullptr;
    jclass clsBoolean = nullptr;
    jclass clsByte = nullptr;
    jclass clsCharacter = nullptr;
    jclass clsShort = nullptr;
    jclass clsInteger = nullptr;
    jclass clsLong = nullptr;
    jclass clsFloat = nullptr;
    jclass clsDouble = nullptr;
    jclass clsString = nullptr;
    jclass clsObject = nullptr;
    jclass clsList = nullptr;
    jclass clsArrayList = nullptr;
    jclass clsMap = nullptr;
    jclass clsHashMap = nullptr;
    jclass clsSet = nullptr;
    jclass clsIterator = nullptr;
    jclass clsArray = nullptr;

    // Cached Method IDs
    jmethodID midClass_getName = nullptr;
    jmethodID midClass_getMethods = nullptr;
    jmethodID midClass_getConstructors = nullptr;
    jmethodID midClass_getFields = nullptr;
    jmethodID midClass_getSuperclass = nullptr;
    jmethodID midClass_getInterfaces = nullptr;
    jmethodID midClass_isArray = nullptr;
    jmethodID midClass_isPrimitive = nullptr;
    jmethodID midClass_isInterface = nullptr;
    jmethodID midClass_getComponentType = nullptr;
    jmethodID midClass_isAssignableFrom = nullptr;

    jmethodID midMethod_getName = nullptr;
    jmethodID midMethod_getParameterTypes = nullptr;
    jmethodID midMethod_getReturnType = nullptr;
    jmethodID midMethod_getModifiers = nullptr;
    jmethodID midMethod_invoke = nullptr;

    jmethodID midConstructor_getParameterTypes = nullptr;
    jmethodID midConstructor_getModifiers = nullptr;
    jmethodID midConstructor_newInstance = nullptr;

    jmethodID midField_getName = nullptr;
    jmethodID midField_getType = nullptr;
    jmethodID midField_getModifiers = nullptr;
    jmethodID midField_get = nullptr;
    jmethodID midField_set = nullptr;

    jmethodID midThrowable_printStackTrace = nullptr;
    jmethodID midThrowable_getMessage = nullptr;
    jmethodID midThrowable_toString = nullptr;
    jmethodID midStringWriter_new = nullptr;
    jmethodID midStringWriter_toString = nullptr;
    jmethodID midPrintWriter_new = nullptr;
    jmethodID midPrintWriter_flush = nullptr;

    jmethodID midArray_newInstance = nullptr;
    jmethodID midArray_getLength = nullptr;
    jmethodID midArray_get = nullptr;
    jmethodID midArray_set = nullptr;

    // Boxed helpers
    jmethodID midBoolean_valueOf = nullptr;
    jmethodID midBoolean_booleanValue = nullptr;
    jmethodID midByte_valueOf = nullptr;
    jmethodID midByte_byteValue = nullptr;
    jmethodID midCharacter_valueOf = nullptr;
    jmethodID midCharacter_charValue = nullptr;
    jmethodID midShort_valueOf = nullptr;
    jmethodID midShort_shortValue = nullptr;
    jmethodID midInteger_valueOf = nullptr;
    jmethodID midInteger_intValue = nullptr;
    jmethodID midLong_valueOf = nullptr;
    jmethodID midLong_longValue = nullptr;
    jmethodID midFloat_valueOf = nullptr;
    jmethodID midFloat_floatValue = nullptr;
    jmethodID midDouble_valueOf = nullptr;
    jmethodID midDouble_doubleValue = nullptr;

    // Collection helpers
    jmethodID midArrayList_new = nullptr;
    jmethodID midList_add = nullptr;
    jmethodID midList_size = nullptr;
    jmethodID midList_get = nullptr;
    jmethodID midHashMap_new = nullptr;
    jmethodID midMap_put = nullptr;
    jmethodID midMap_get = nullptr;
    jmethodID midMap_keySet = nullptr;
    jmethodID midSet_iterator = nullptr;
    jmethodID midIterator_hasNext = nullptr;
    jmethodID midIterator_next = nullptr;
};

static JvmContext g_jvm;

// ---------------------------------------------------------------------------
// String Helpers
// ---------------------------------------------------------------------------
static std::string normalizeClassName(const char* name)
{
    std::string s(name);
    for (char& c : s)
    {
        if (c == '.') c = '/';
    }
    return s;
}

static std::string dotClassName(const char* name)
{
    std::string s(name);
    for (char& c : s)
    {
        if (c == '/') c = '.';
    }
    return s;
}

static char* duplicateString(const char* s)
{
    if (!s) return nullptr;
    size_t len = strlen(s);
    char* copy = (char*)malloc(len + 1);
    if (copy)
        memcpy(copy, s, len + 1);
    return copy;
}

// ---------------------------------------------------------------------------
// Forward Declarations
// ---------------------------------------------------------------------------
static JNIEnv* getJNIEnv(lua_State* L);
static void checkJniException(lua_State* L, JNIEnv* env);
static std::string formatJniException(JNIEnv* env, jthrowable exc);
static void pushJObject(lua_State* L, JNIEnv* env, jobject obj, const char* className = nullptr);
static void pushJClass(lua_State* L, JNIEnv* env, jclass clazz, const char* className = nullptr);
static void pushJArray(lua_State* L, JNIEnv* env, jarray arr, const char* elementTypeName = nullptr, char primType = 0);
static jobject luauToJavaObject(lua_State* L, JNIEnv* env, int idx);
static void javaToLuauValue(lua_State* L, JNIEnv* env, jobject obj, jclass targetCls = nullptr);

// ---------------------------------------------------------------------------
// JNI Exception Handling
// ---------------------------------------------------------------------------
static std::string formatJniException(JNIEnv* env, jthrowable exc)
{
    std::string result;
    if (g_jvm.clsStringWriter && g_jvm.clsPrintWriter && g_jvm.midThrowable_printStackTrace)
    {
        jobject sw = env->NewObject(g_jvm.clsStringWriter, g_jvm.midStringWriter_new);
        jobject pw = env->NewObject(g_jvm.clsPrintWriter, g_jvm.midPrintWriter_new, sw);
        env->CallVoidMethod(exc, g_jvm.midThrowable_printStackTrace, pw);
        if (g_jvm.midPrintWriter_flush)
            env->CallVoidMethod(pw, g_jvm.midPrintWriter_flush);

        jstring str = (jstring)env->CallObjectMethod(sw, g_jvm.midStringWriter_toString);
        if (str)
        {
            const char* utf = env->GetStringUTFChars(str, nullptr);
            if (utf)
            {
                result = utf;
                env->ReleaseStringUTFChars(str, utf);
            }
            env->DeleteLocalRef(str);
        }
        env->DeleteLocalRef(pw);
        env->DeleteLocalRef(sw);
    }
    if (result.empty())
    {
        jclass excCls = env->GetObjectClass(exc);
        jmethodID midToString = env->GetMethodID(excCls, "toString", "()Ljava/lang/String;");
        jstring str = (jstring)env->CallObjectMethod(exc, midToString);
        if (str)
        {
            const char* utf = env->GetStringUTFChars(str, nullptr);
            if (utf)
            {
                result = utf;
                env->ReleaseStringUTFChars(str, utf);
            }
            env->DeleteLocalRef(str);
        }
        env->DeleteLocalRef(excCls);
    }
    return result.empty() ? "java.lang.Throwable" : result;
}

static void checkJniException(lua_State* L, JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return;

    jthrowable exc = env->ExceptionOccurred();
    env->ExceptionClear();

    if (!exc)
    {
        luaL_error(L, "Java Exception: unknown error occurred");
        return;
    }

    std::string msg = formatJniException(env, exc);
    env->DeleteLocalRef(exc);
    luaL_error(L, "Java Exception: %s", msg.c_str());
}

// ---------------------------------------------------------------------------
// JVM Initialization & Reflection Cache
// ---------------------------------------------------------------------------
static void cacheReflectionSymbols(JNIEnv* env)
{
    auto loadCls = [env](const char* name) -> jclass {
        jclass localCls = env->FindClass(name);
        if (!localCls)
        {
            env->ExceptionClear();
            return nullptr;
        }
        jclass globalCls = (jclass)env->NewGlobalRef(localCls);
        env->DeleteLocalRef(localCls);
        return globalCls;
    };

    g_jvm.clsClass = loadCls("java/lang/Class");
    g_jvm.clsMethod = loadCls("java/lang/reflect/Method");
    g_jvm.clsConstructor = loadCls("java/lang/reflect/Constructor");
    g_jvm.clsField = loadCls("java/lang/reflect/Field");
    g_jvm.clsModifier = loadCls("java/lang/reflect/Modifier");
    g_jvm.clsThrowable = loadCls("java/lang/Throwable");
    g_jvm.clsStringWriter = loadCls("java/io/StringWriter");
    g_jvm.clsPrintWriter = loadCls("java/io/PrintWriter");
    g_jvm.clsBoolean = loadCls("java/lang/Boolean");
    g_jvm.clsByte = loadCls("java/lang/Byte");
    g_jvm.clsCharacter = loadCls("java/lang/Character");
    g_jvm.clsShort = loadCls("java/lang/Short");
    g_jvm.clsInteger = loadCls("java/lang/Integer");
    g_jvm.clsLong = loadCls("java/lang/Long");
    g_jvm.clsFloat = loadCls("java/lang/Float");
    g_jvm.clsDouble = loadCls("java/lang/Double");
    g_jvm.clsString = loadCls("java/lang/String");
    g_jvm.clsObject = loadCls("java/lang/Object");
    g_jvm.clsList = loadCls("java/util/List");
    g_jvm.clsArrayList = loadCls("java/util/ArrayList");
    g_jvm.clsMap = loadCls("java/util/Map");
    g_jvm.clsHashMap = loadCls("java/util/HashMap");
    g_jvm.clsSet = loadCls("java/util/Set");
    g_jvm.clsIterator = loadCls("java/util/Iterator");
    g_jvm.clsArray = loadCls("java/lang/reflect/Array");

    if (g_jvm.clsClass)
    {
        g_jvm.midClass_getName = env->GetMethodID(g_jvm.clsClass, "getName", "()Ljava/lang/String;");
        g_jvm.midClass_getMethods = env->GetMethodID(g_jvm.clsClass, "getMethods", "()[Ljava/lang/reflect/Method;");
        g_jvm.midClass_getConstructors = env->GetMethodID(g_jvm.clsClass, "getConstructors", "()[Ljava/lang/reflect/Constructor;");
        g_jvm.midClass_getFields = env->GetMethodID(g_jvm.clsClass, "getFields", "()[Ljava/lang/reflect/Field;");
        g_jvm.midClass_getSuperclass = env->GetMethodID(g_jvm.clsClass, "getSuperclass", "()Ljava/lang/Class;");
        g_jvm.midClass_getInterfaces = env->GetMethodID(g_jvm.clsClass, "getInterfaces", "()[Ljava/lang/Class;");
        g_jvm.midClass_isArray = env->GetMethodID(g_jvm.clsClass, "isArray", "()Z");
        g_jvm.midClass_isPrimitive = env->GetMethodID(g_jvm.clsClass, "isPrimitive", "()Z");
        g_jvm.midClass_isInterface = env->GetMethodID(g_jvm.clsClass, "isInterface", "()Z");
        g_jvm.midClass_getComponentType = env->GetMethodID(g_jvm.clsClass, "getComponentType", "()Ljava/lang/Class;");
        g_jvm.midClass_isAssignableFrom = env->GetMethodID(g_jvm.clsClass, "isAssignableFrom", "(Ljava/lang/Class;)Z");
    }

    if (g_jvm.clsMethod)
    {
        g_jvm.midMethod_getName = env->GetMethodID(g_jvm.clsMethod, "getName", "()Ljava/lang/String;");
        g_jvm.midMethod_getParameterTypes = env->GetMethodID(g_jvm.clsMethod, "getParameterTypes", "()[Ljava/lang/Class;");
        g_jvm.midMethod_getReturnType = env->GetMethodID(g_jvm.clsMethod, "getReturnType", "()Ljava/lang/Class;");
        g_jvm.midMethod_getModifiers = env->GetMethodID(g_jvm.clsMethod, "getModifiers", "()I");
        g_jvm.midMethod_invoke = env->GetMethodID(g_jvm.clsMethod, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    }

    if (g_jvm.clsConstructor)
    {
        g_jvm.midConstructor_getParameterTypes = env->GetMethodID(g_jvm.clsConstructor, "getParameterTypes", "()[Ljava/lang/Class;");
        g_jvm.midConstructor_getModifiers = env->GetMethodID(g_jvm.clsConstructor, "getModifiers", "()I");
        g_jvm.midConstructor_newInstance = env->GetMethodID(g_jvm.clsConstructor, "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;");
    }

    if (g_jvm.clsField)
    {
        g_jvm.midField_getName = env->GetMethodID(g_jvm.clsField, "getName", "()Ljava/lang/String;");
        g_jvm.midField_getType = env->GetMethodID(g_jvm.clsField, "getType", "()Ljava/lang/Class;");
        g_jvm.midField_getModifiers = env->GetMethodID(g_jvm.clsField, "getModifiers", "()I");
        g_jvm.midField_get = env->GetMethodID(g_jvm.clsField, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
        g_jvm.midField_set = env->GetMethodID(g_jvm.clsField, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V");
    }

    if (g_jvm.clsThrowable)
    {
        g_jvm.midThrowable_printStackTrace = env->GetMethodID(g_jvm.clsThrowable, "printStackTrace", "(Ljava/io/PrintWriter;)V");
        g_jvm.midThrowable_getMessage = env->GetMethodID(g_jvm.clsThrowable, "getMessage", "()Ljava/lang/String;");
        g_jvm.midThrowable_toString = env->GetMethodID(g_jvm.clsThrowable, "toString", "()Ljava/lang/String;");
    }

    if (g_jvm.clsStringWriter)
    {
        g_jvm.midStringWriter_new = env->GetMethodID(g_jvm.clsStringWriter, "<init>", "()V");
        g_jvm.midStringWriter_toString = env->GetMethodID(g_jvm.clsStringWriter, "toString", "()Ljava/lang/String;");
    }

    if (g_jvm.clsPrintWriter)
    {
        g_jvm.midPrintWriter_new = env->GetMethodID(g_jvm.clsPrintWriter, "<init>", "(Ljava/io/Writer;)V");
        g_jvm.midPrintWriter_flush = env->GetMethodID(g_jvm.clsPrintWriter, "flush", "()V");
    }

    if (g_jvm.clsArray)
    {
        g_jvm.midArray_newInstance = env->GetStaticMethodID(g_jvm.clsArray, "newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;");
        g_jvm.midArray_getLength = env->GetStaticMethodID(g_jvm.clsArray, "getLength", "(Ljava/lang/Object;)I");
        g_jvm.midArray_get = env->GetStaticMethodID(g_jvm.clsArray, "get", "(Ljava/lang/Object;I)Ljava/lang/Object;");
        g_jvm.midArray_set = env->GetStaticMethodID(g_jvm.clsArray, "set", "(Ljava/lang/Object;ILjava/lang/Object;)V");
    }

    // Boxed method IDs
    if (g_jvm.clsBoolean)
    {
        g_jvm.midBoolean_valueOf = env->GetStaticMethodID(g_jvm.clsBoolean, "valueOf", "(Z)Ljava/lang/Boolean;");
        g_jvm.midBoolean_booleanValue = env->GetMethodID(g_jvm.clsBoolean, "booleanValue", "()Z");
    }
    if (g_jvm.clsByte)
    {
        g_jvm.midByte_valueOf = env->GetStaticMethodID(g_jvm.clsByte, "valueOf", "(B)Ljava/lang/Byte;");
        g_jvm.midByte_byteValue = env->GetMethodID(g_jvm.clsByte, "byteValue", "()B");
    }
    if (g_jvm.clsCharacter)
    {
        g_jvm.midCharacter_valueOf = env->GetStaticMethodID(g_jvm.clsCharacter, "valueOf", "(C)Ljava/lang/Character;");
        g_jvm.midCharacter_charValue = env->GetMethodID(g_jvm.clsCharacter, "charValue", "()C");
    }
    if (g_jvm.clsShort)
    {
        g_jvm.midShort_valueOf = env->GetStaticMethodID(g_jvm.clsShort, "valueOf", "(S)Ljava/lang/Short;");
        g_jvm.midShort_shortValue = env->GetMethodID(g_jvm.clsShort, "shortValue", "()S");
    }
    if (g_jvm.clsInteger)
    {
        g_jvm.midInteger_valueOf = env->GetStaticMethodID(g_jvm.clsInteger, "valueOf", "(I)Ljava/lang/Integer;");
        g_jvm.midInteger_intValue = env->GetMethodID(g_jvm.clsInteger, "intValue", "()I");
    }
    if (g_jvm.clsLong)
    {
        g_jvm.midLong_valueOf = env->GetStaticMethodID(g_jvm.clsLong, "valueOf", "(J)Ljava/lang/Long;");
        g_jvm.midLong_longValue = env->GetMethodID(g_jvm.clsLong, "longValue", "()J");
    }
    if (g_jvm.clsFloat)
    {
        g_jvm.midFloat_valueOf = env->GetStaticMethodID(g_jvm.clsFloat, "valueOf", "(F)Ljava/lang/Float;");
        g_jvm.midFloat_floatValue = env->GetMethodID(g_jvm.clsFloat, "floatValue", "()F");
    }
    if (g_jvm.clsDouble)
    {
        g_jvm.midDouble_valueOf = env->GetStaticMethodID(g_jvm.clsDouble, "valueOf", "(D)Ljava/lang/Double;");
        g_jvm.midDouble_doubleValue = env->GetMethodID(g_jvm.clsDouble, "doubleValue", "()D");
    }

    // Collections method IDs
    if (g_jvm.clsArrayList)
        g_jvm.midArrayList_new = env->GetMethodID(g_jvm.clsArrayList, "<init>", "()V");
    if (g_jvm.clsList)
    {
        g_jvm.midList_add = env->GetMethodID(g_jvm.clsList, "add", "(Ljava/lang/Object;)Z");
        g_jvm.midList_size = env->GetMethodID(g_jvm.clsList, "size", "()I");
        g_jvm.midList_get = env->GetMethodID(g_jvm.clsList, "get", "(I)Ljava/lang/Object;");
    }
    if (g_jvm.clsHashMap)
        g_jvm.midHashMap_new = env->GetMethodID(g_jvm.clsHashMap, "<init>", "()V");
    if (g_jvm.clsMap)
    {
        g_jvm.midMap_put = env->GetMethodID(g_jvm.clsMap, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
        g_jvm.midMap_get = env->GetMethodID(g_jvm.clsMap, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
        g_jvm.midMap_keySet = env->GetMethodID(g_jvm.clsMap, "keySet", "()Ljava/util/Set;");
    }
    if (g_jvm.clsSet)
        g_jvm.midSet_iterator = env->GetMethodID(g_jvm.clsSet, "iterator", "()Ljava/util/Iterator;");
    if (g_jvm.clsIterator)
    {
        g_jvm.midIterator_hasNext = env->GetMethodID(g_jvm.clsIterator, "hasNext", "()Z");
        g_jvm.midIterator_next = env->GetMethodID(g_jvm.clsIterator, "next", "()Ljava/lang/Object;");
    }
}

static bool initJvmInstance(const std::string& jvmPath, const std::vector<std::string>& userOptions, bool ignoreUnrecognized, std::string& err)
{
    if (g_jvm.initialized && g_jvm.vm)
        return true;

    if (!g_jvm.libHandle)
    {
        std::string path = jvmPath.empty() ? detectJvmPath() : jvmPath;
        g_jvm.libHandle = osLoadLibrary(path.c_str());
        if (!g_jvm.libHandle)
        {
            err = "Failed to load JVM library from '" + path + "'";
            return false;
        }
        g_jvm.loadedJvmPath = path;

        g_jvm.fnCreateJavaVM = (JvmContext::CreateJavaVM_t)osGetSymbol(g_jvm.libHandle, "JNI_CreateJavaVM");
        g_jvm.fnGetCreatedJavaVMs = (JvmContext::GetCreatedJavaVMs_t)osGetSymbol(g_jvm.libHandle, "JNI_GetCreatedJavaVMs");
        g_jvm.fnGetDefaultJavaVMInitArgs = (JvmContext::GetDefaultJavaVMInitArgs_t)osGetSymbol(g_jvm.libHandle, "JNI_GetDefaultJavaVMInitArgs");

        if (!g_jvm.fnCreateJavaVM)
        {
            err = "Failed to find JNI_CreateJavaVM in " + path;
            osCloseLibrary(g_jvm.libHandle);
            g_jvm.libHandle = nullptr;
            return false;
        }
    }

    // Check if JVM is already created in current process
    if (g_jvm.fnGetCreatedJavaVMs)
    {
        JavaVM* existingVm = nullptr;
        jsize numVms = 0;
        if (g_jvm.fnGetCreatedJavaVMs(&existingVm, 1, &numVms) == JNI_OK && numVms > 0 && existingVm)
        {
            g_jvm.vm = existingVm;
            g_jvm.initialized = true;
            JNIEnv* env = nullptr;
            g_jvm.vm->AttachCurrentThread((void**)&env, nullptr);
            if (env)
                cacheReflectionSymbols(env);
            return true;
        }
    }

    // Setup JavaVMInitArgs
    std::vector<JavaVMOption> options;
    options.reserve(userOptions.size());
    for (const auto& opt : userOptions)
    {
        JavaVMOption jopt;
        jopt.optionString = (char*)opt.c_str();
        jopt.extraInfo = nullptr;
        options.push_back(jopt);
    }

    JavaVMInitArgs args;
    args.version = JNI_VERSION_1_8;
    args.nOptions = (jint)options.size();
    args.options = options.data();
    args.ignoreUnrecognized = ignoreUnrecognized ? JNI_TRUE : JNI_FALSE;

    JNIEnv* env = nullptr;
    jint res = g_jvm.fnCreateJavaVM(&g_jvm.vm, (void**)&env, &args);
    if (res != JNI_OK || !g_jvm.vm || !env)
    {
        err = "JNI_CreateJavaVM failed with error code " + std::to_string(res);
        return false;
    }

    g_jvm.initialized = true;
    // JVM initialization (JNI_CreateJavaVM) resets LC_NUMERIC to system locale; restore C locale for Luau lexer
    setlocale(LC_NUMERIC, "C");
    cacheReflectionSymbols(env);
    return true;
}

static JNIEnv* getJNIEnv(lua_State* L)
{
    if (!g_jvm.initialized || !g_jvm.vm)
    {
        std::string err;
        if (!initJvmInstance("", {}, true, err))
        {
            luaL_error(L, "JVM not initialized: %s", err.c_str());
            return nullptr;
        }
    }

    JNIEnv* env = nullptr;
    jint res = g_jvm.vm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (res == JNI_EDETACHED)
    {
        res = g_jvm.vm->AttachCurrentThread((void**)&env, nullptr);
        if (res != JNI_OK || !env)
        {
            luaL_error(L, "Failed to attach current thread to JVM (error %d)", res);
            return nullptr;
        }
    }
    else if (res != JNI_OK || !env)
    {
        luaL_error(L, "Failed to obtain JNIEnv from JavaVM (error %d)", res);
        return nullptr;
    }
    return env;
}

// ---------------------------------------------------------------------------
// Type & Userdata Push Helpers
// ---------------------------------------------------------------------------
static void pushJObject(lua_State* L, JNIEnv* env, jobject obj, const char* className)
{
    if (!obj)
    {
        lua_pushnil(L);
        return;
    }

    JObjectUserData* ud = (JObjectUserData*)lua_newuserdata(L, sizeof(JObjectUserData));
    ud->object = env->NewGlobalRef(obj);
    if (className)
    {
        ud->className = duplicateString(className);
    }
    else
    {
        jclass cls = env->GetObjectClass(obj);
        jstring nameStr = (jstring)env->CallObjectMethod(cls, g_jvm.midClass_getName);
        if (nameStr)
        {
            const char* utf = env->GetStringUTFChars(nameStr, nullptr);
            ud->className = duplicateString(utf);
            env->ReleaseStringUTFChars(nameStr, utf);
            env->DeleteLocalRef(nameStr);
        }
        else
        {
            ud->className = duplicateString("java/lang/Object");
        }
        env->DeleteLocalRef(cls);
    }

    luaL_getmetatable(L, JNI_OBJECT_MT);
    lua_setmetatable(L, -2);
}

static void pushJClass(lua_State* L, JNIEnv* env, jclass clazz, const char* className)
{
    if (!clazz)
    {
        lua_pushnil(L);
        return;
    }

    JClassUserData* ud = (JClassUserData*)lua_newuserdata(L, sizeof(JClassUserData));
    ud->clazz = (jclass)env->NewGlobalRef(clazz);
    if (className)
    {
        ud->className = duplicateString(className);
    }
    else
    {
        jstring nameStr = (jstring)env->CallObjectMethod(clazz, g_jvm.midClass_getName);
        if (nameStr)
        {
            const char* utf = env->GetStringUTFChars(nameStr, nullptr);
            ud->className = duplicateString(utf);
            env->ReleaseStringUTFChars(nameStr, utf);
            env->DeleteLocalRef(nameStr);
        }
        else
        {
            ud->className = duplicateString("unknown");
        }
    }

    luaL_getmetatable(L, JNI_CLASS_MT);
    lua_setmetatable(L, -2);
}

static void pushJArray(lua_State* L, JNIEnv* env, jarray arr, const char* elementTypeName, char primType)
{
    if (!arr)
    {
        lua_pushnil(L);
        return;
    }

    JArrayUserData* ud = (JArrayUserData*)lua_newuserdata(L, sizeof(JArrayUserData));
    ud->array = (jarray)env->NewGlobalRef(arr);
    ud->length = env->GetArrayLength(arr);
    ud->primitiveType = primType;
    ud->isPrimitive = (primType != 0 && primType != 'L');
    ud->elementTypeName = duplicateString(elementTypeName ? elementTypeName : "java/lang/Object");

    luaL_getmetatable(L, JNI_ARRAY_MT);
    lua_setmetatable(L, -2);
}

static void pushJTypedValue(lua_State* L, JNIEnv* env, char type, jvalue val)
{
    JTypedValueUserData* ud = (JTypedValueUserData*)lua_newuserdata(L, sizeof(JTypedValueUserData));
    ud->type = type;
    if (type == 'L' && val.l)
        ud->val.l = env->NewGlobalRef(val.l);
    else
        ud->val = val;

    luaL_getmetatable(L, JNI_TYPED_VALUE_MT);
    lua_setmetatable(L, -2);
}

// ---------------------------------------------------------------------------
// Type Conversion Utilities (Luau <-> Java)
// ---------------------------------------------------------------------------
static jobject luauToJavaObject(lua_State* L, JNIEnv* env, int idx)
{
    int t = lua_type(L, idx);
    switch (t)
    {
    case LUA_TNIL:
        return nullptr;

    case LUA_TBOOLEAN: {
        jboolean b = lua_toboolean(L, idx) ? JNI_TRUE : JNI_FALSE;
        return env->CallStaticObjectMethod(g_jvm.clsBoolean, g_jvm.midBoolean_valueOf, b);
    }

    case LUA_TNUMBER: {
        double d = lua_tonumber(L, idx);
        if (d == std::floor(d) && d >= -2147483648.0 && d <= 2147483647.0)
            return env->CallStaticObjectMethod(g_jvm.clsInteger, g_jvm.midInteger_valueOf, (jint)d);
        else if (d == std::floor(d))
            return env->CallStaticObjectMethod(g_jvm.clsLong, g_jvm.midLong_valueOf, (jlong)d);
        else
            return env->CallStaticObjectMethod(g_jvm.clsDouble, g_jvm.midDouble_valueOf, (jdouble)d);
    }

    case LUA_TSTRING: {
        size_t len = 0;
        const char* str = lua_tolstring(L, idx, &len);
        return env->NewStringUTF(str);
    }

    case LUA_TBUFFER: {
        size_t len = 0;
        void* buf = lua_tobuffer(L, idx, &len);
        jbyteArray jarr = env->NewByteArray((jsize)len);
        if (jarr && len > 0)
            env->SetByteArrayRegion(jarr, 0, (jsize)len, (const jbyte*)buf);
        return jarr;
    }

    case LUA_TUSERDATA: {
        if (luaL_getmetafield(L, idx, "__name"))
        {
            const char* mtName = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (mtName && strcmp(mtName, JNI_OBJECT_MT) == 0)
            {
                JObjectUserData* ud = (JObjectUserData*)lua_touserdata(L, idx);
                return ud->object;
            }
            if (mtName && strcmp(mtName, JNI_CLASS_MT) == 0)
            {
                JClassUserData* ud = (JClassUserData*)lua_touserdata(L, idx);
                return ud->clazz;
            }
            if (mtName && strcmp(mtName, JNI_ARRAY_MT) == 0)
            {
                JArrayUserData* ud = (JArrayUserData*)lua_touserdata(L, idx);
                return ud->array;
            }
            if (mtName && strcmp(mtName, JNI_TYPED_VALUE_MT) == 0)
            {
                JTypedValueUserData* ud = (JTypedValueUserData*)lua_touserdata(L, idx);
                switch (ud->type)
                {
                case 'Z': return env->CallStaticObjectMethod(g_jvm.clsBoolean, g_jvm.midBoolean_valueOf, ud->val.z);
                case 'B': return env->CallStaticObjectMethod(g_jvm.clsByte, g_jvm.midByte_valueOf, ud->val.b);
                case 'C': return env->CallStaticObjectMethod(g_jvm.clsCharacter, g_jvm.midCharacter_valueOf, ud->val.c);
                case 'S': return env->CallStaticObjectMethod(g_jvm.clsShort, g_jvm.midShort_valueOf, ud->val.s);
                case 'I': return env->CallStaticObjectMethod(g_jvm.clsInteger, g_jvm.midInteger_valueOf, ud->val.i);
                case 'J': return env->CallStaticObjectMethod(g_jvm.clsLong, g_jvm.midLong_valueOf, ud->val.j);
                case 'F': return env->CallStaticObjectMethod(g_jvm.clsFloat, g_jvm.midFloat_valueOf, ud->val.f);
                case 'D': return env->CallStaticObjectMethod(g_jvm.clsDouble, g_jvm.midDouble_valueOf, ud->val.d);
                case 'L': return ud->val.l;
                }
            }
        }
        return nullptr;
    }

    case LUA_TTABLE: {
        // Determine if array-like or map-like
        int len = (int)lua_objlen(L, idx);
        if (len > 0)
        {
            jobject list = env->NewObject(g_jvm.clsArrayList, g_jvm.midArrayList_new);
            for (int i = 1; i <= len; ++i)
            {
                lua_rawgeti(L, idx, i);
                jobject item = luauToJavaObject(L, env, -1);
                env->CallBooleanMethod(list, g_jvm.midList_add, item);
                if (item) env->DeleteLocalRef(item);
                lua_pop(L, 1);
            }
            return list;
        }
        else
        {
            jobject map = env->NewObject(g_jvm.clsHashMap, g_jvm.midHashMap_new);
            lua_pushnil(L);
            while (lua_next(L, idx < 0 ? idx - 1 : idx) != 0)
            {
                jobject k = luauToJavaObject(L, env, -2);
                jobject v = luauToJavaObject(L, env, -1);
                env->CallObjectMethod(map, g_jvm.midMap_put, k, v);
                if (k) env->DeleteLocalRef(k);
                if (v) env->DeleteLocalRef(v);
                lua_pop(L, 1);
            }
            return map;
        }
    }

    default:
        return nullptr;
    }
}

static void javaToLuauValue(lua_State* L, JNIEnv* env, jobject obj, jclass targetCls)
{
    if (!obj)
    {
        lua_pushnil(L);
        return;
    }

    // String
    if (env->IsInstanceOf(obj, g_jvm.clsString))
    {
        const char* utf = env->GetStringUTFChars((jstring)obj, nullptr);
        if (utf)
        {
            lua_pushstring(L, utf);
            env->ReleaseStringUTFChars((jstring)obj, utf);
        }
        else
        {
            lua_pushliteral(L, "");
        }
        return;
    }

    // Boolean
    if (env->IsInstanceOf(obj, g_jvm.clsBoolean))
    {
        jboolean b = env->CallBooleanMethod(obj, g_jvm.midBoolean_booleanValue);
        lua_pushboolean(L, b ? 1 : 0);
        return;
    }

    // Number primitives (Byte, Short, Integer, Long, Float, Double)
    if (env->IsInstanceOf(obj, g_jvm.clsInteger))
    {
        jint v = env->CallIntMethod(obj, g_jvm.midInteger_intValue);
        lua_pushinteger(L, v);
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsLong))
    {
        jlong v = env->CallLongMethod(obj, g_jvm.midLong_longValue);
        lua_pushnumber(L, (double)v);
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsDouble))
    {
        jdouble v = env->CallDoubleMethod(obj, g_jvm.midDouble_doubleValue);
        lua_pushnumber(L, v);
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsFloat))
    {
        jfloat v = env->CallFloatMethod(obj, g_jvm.midFloat_floatValue);
        lua_pushnumber(L, (double)v);
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsShort))
    {
        jshort v = env->CallShortMethod(obj, g_jvm.midShort_shortValue);
        lua_pushinteger(L, v);
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsByte))
    {
        jbyte v = env->CallByteMethod(obj, g_jvm.midByte_byteValue);
        lua_pushinteger(L, v);
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsCharacter))
    {
        jchar v = env->CallCharMethod(obj, g_jvm.midCharacter_charValue);
        char buf[2] = { (char)v, '\0' };
        lua_pushstring(L, buf);
        return;
    }

    // Array
    jclass cls = targetCls ? targetCls : env->GetObjectClass(obj);
    if (env->CallBooleanMethod(cls, g_jvm.midClass_isArray))
    {
        jclass elemCls = (jclass)env->CallObjectMethod(cls, g_jvm.midClass_getComponentType);
        jboolean isPrim = env->CallBooleanMethod(elemCls, g_jvm.midClass_isPrimitive);
        char primChar = 'L';
        std::string elemName = "java/lang/Object";

        if (isPrim)
        {
            jstring nameStr = (jstring)env->CallObjectMethod(elemCls, g_jvm.midClass_getName);
            const char* utf = env->GetStringUTFChars(nameStr, nullptr);
            if (strcmp(utf, "boolean") == 0) primChar = 'Z';
            else if (strcmp(utf, "byte") == 0) primChar = 'B';
            else if (strcmp(utf, "char") == 0) primChar = 'C';
            else if (strcmp(utf, "short") == 0) primChar = 'S';
            else if (strcmp(utf, "int") == 0) primChar = 'I';
            else if (strcmp(utf, "long") == 0) primChar = 'J';
            else if (strcmp(utf, "float") == 0) primChar = 'F';
            else if (strcmp(utf, "double") == 0) primChar = 'D';
            elemName = utf;
            env->ReleaseStringUTFChars(nameStr, utf);
            env->DeleteLocalRef(nameStr);
        }
        else
        {
            jstring nameStr = (jstring)env->CallObjectMethod(elemCls, g_jvm.midClass_getName);
            if (nameStr)
            {
                const char* utf = env->GetStringUTFChars(nameStr, nullptr);
                elemName = utf;
                env->ReleaseStringUTFChars(nameStr, utf);
                env->DeleteLocalRef(nameStr);
            }
        }
        env->DeleteLocalRef(elemCls);
        if (!targetCls) env->DeleteLocalRef(cls);

        pushJArray(L, env, (jarray)obj, elemName.c_str(), primChar);
        return;
    }

    if (!targetCls) env->DeleteLocalRef(cls);

    // Default object wrapper
    pushJObject(L, env, obj);
}

// ---------------------------------------------------------------------------
// Method Matching and Dynamic Invocation Engine
// ---------------------------------------------------------------------------
static int scoreArgumentMatch(JNIEnv* env, lua_State* L, int argIdx, jclass paramCls)
{
    int ltype = lua_type(L, argIdx);
    jstring nameStr = (jstring)env->CallObjectMethod(paramCls, g_jvm.midClass_getName);
    const char* utf = env->GetStringUTFChars(nameStr, nullptr);
    std::string pName = utf ? utf : "";
    if (utf) env->ReleaseStringUTFChars(nameStr, utf);
    env->DeleteLocalRef(nameStr);

    jboolean isPrim = env->CallBooleanMethod(paramCls, g_jvm.midClass_isPrimitive);

    if (ltype == LUA_TNIL)
    {
        return isPrim ? -1 : 10;
    }

    if (ltype == LUA_TBOOLEAN)
    {
        if (pName == "boolean" || pName == "java.lang.Boolean") return 1;
        if (pName == "java.lang.Object") return 5;
        return -1;
    }

    if (ltype == LUA_TNUMBER)
    {
        double n = lua_tonumber(L, argIdx);
        bool isInt = (n == std::floor(n));

        if (pName == "int" || pName == "java.lang.Integer") return isInt ? 1 : 4;
        if (pName == "long" || pName == "java.lang.Long") return isInt ? 2 : 4;
        if (pName == "double" || pName == "java.lang.Double") return !isInt ? 1 : 3;
        if (pName == "float" || pName == "java.lang.Float") return !isInt ? 2 : 4;
        if (pName == "short" || pName == "java.lang.Short") return isInt ? 3 : 5;
        if (pName == "byte" || pName == "java.lang.Byte") return isInt ? 4 : 6;
        if (pName == "char" || pName == "java.lang.Character") return isInt ? 5 : 7;
        if (pName == "java.lang.Number" || pName == "java.lang.Object") return 8;
        return -1;
    }

    if (ltype == LUA_TSTRING)
    {
        if (pName == "java.lang.String") return 1;
        if (pName == "java.lang.CharSequence") return 2;
        if (pName == "char" || pName == "java.lang.Character") return 3;
        if (pName == "java.lang.Object") return 5;
        return -1;
    }

    if (ltype == LUA_TBUFFER)
    {
        if (pName == "[B" || pName == "java.nio.ByteBuffer") return 1;
        if (pName == "java.lang.Object") return 5;
        return -1;
    }

    if (ltype == LUA_TTABLE)
    {
        if (pName == "java.util.List" || pName == "java.util.ArrayList") return 2;
        if (pName == "java.util.Map" || pName == "java.util.HashMap") return 2;
        if (pName == "java.lang.Object") return 5;
        return -1;
    }

    if (ltype == LUA_TUSERDATA)
    {
        if (luaL_getmetafield(L, argIdx, "__name"))
        {
            const char* mtName = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (mtName && strcmp(mtName, JNI_OBJECT_MT) == 0)
            {
                JObjectUserData* ud = (JObjectUserData*)lua_touserdata(L, argIdx);
                if (ud && ud->object)
                {
                    jclass objCls = env->GetObjectClass(ud->object);
                    jboolean assignable = env->CallBooleanMethod(paramCls, g_jvm.midClass_isAssignableFrom, objCls);
                    env->DeleteLocalRef(objCls);
                    return assignable ? 1 : -1;
                }
            }
            if (mtName && strcmp(mtName, JNI_CLASS_MT) == 0)
            {
                JClassUserData* ud = (JClassUserData*)lua_touserdata(L, argIdx);
                if (ud && ud->clazz)
                {
                    jboolean assignable = env->CallBooleanMethod(paramCls, g_jvm.midClass_isAssignableFrom, g_jvm.clsClass);
                    return assignable ? 1 : -1;
                }
            }
            if (mtName && strcmp(mtName, JNI_ARRAY_MT) == 0)
            {
                JArrayUserData* ud = (JArrayUserData*)lua_touserdata(L, argIdx);
                if (ud && ud->array)
                {
                    jclass arrCls = env->GetObjectClass(ud->array);
                    jboolean assignable = env->CallBooleanMethod(paramCls, g_jvm.midClass_isAssignableFrom, arrCls);
                    env->DeleteLocalRef(arrCls);
                    return assignable ? 1 : -1;
                }
            }
        }
    }

    return -1;
}

static jobject convertLuauArgToParam(JNIEnv* env, lua_State* L, int argIdx, jclass paramCls)
{
    int ltype = lua_type(L, argIdx);
    jboolean isPrim = env->CallBooleanMethod(paramCls, g_jvm.midClass_isPrimitive);

    jstring nameStr = (jstring)env->CallObjectMethod(paramCls, g_jvm.midClass_getName);
    const char* utf = env->GetStringUTFChars(nameStr, nullptr);
    std::string pName = utf ? utf : "";
    if (utf) env->ReleaseStringUTFChars(nameStr, utf);
    env->DeleteLocalRef(nameStr);

    if (ltype == LUA_TNIL)
        return nullptr;

    if (ltype == LUA_TBOOLEAN)
    {
        jboolean b = lua_toboolean(L, argIdx) ? JNI_TRUE : JNI_FALSE;
        return env->CallStaticObjectMethod(g_jvm.clsBoolean, g_jvm.midBoolean_valueOf, b);
    }

    if (ltype == LUA_TNUMBER)
    {
        double n = lua_tonumber(L, argIdx);
        if (pName == "int" || pName == "java.lang.Integer")
            return env->CallStaticObjectMethod(g_jvm.clsInteger, g_jvm.midInteger_valueOf, (jint)n);
        if (pName == "long" || pName == "java.lang.Long")
            return env->CallStaticObjectMethod(g_jvm.clsLong, g_jvm.midLong_valueOf, (jlong)n);
        if (pName == "float" || pName == "java.lang.Float")
            return env->CallStaticObjectMethod(g_jvm.clsFloat, g_jvm.midFloat_valueOf, (jfloat)n);
        if (pName == "double" || pName == "java.lang.Double")
            return env->CallStaticObjectMethod(g_jvm.clsDouble, g_jvm.midDouble_valueOf, (jdouble)n);
        if (pName == "short" || pName == "java.lang.Short")
            return env->CallStaticObjectMethod(g_jvm.clsShort, g_jvm.midShort_valueOf, (jshort)n);
        if (pName == "byte" || pName == "java.lang.Byte")
            return env->CallStaticObjectMethod(g_jvm.clsByte, g_jvm.midByte_valueOf, (jbyte)n);
        if (pName == "char" || pName == "java.lang.Character")
            return env->CallStaticObjectMethod(g_jvm.clsCharacter, g_jvm.midCharacter_valueOf, (jchar)n);

        // Fallback to integer or double box
        if (n == std::floor(n))
            return env->CallStaticObjectMethod(g_jvm.clsLong, g_jvm.midLong_valueOf, (jlong)n);
        else
            return env->CallStaticObjectMethod(g_jvm.clsDouble, g_jvm.midDouble_valueOf, (jdouble)n);
    }

    if (ltype == LUA_TSTRING)
    {
        size_t len = 0;
        const char* s = lua_tolstring(L, argIdx, &len);
        if (pName == "char" || pName == "java.lang.Character")
        {
            jchar c = len > 0 ? (jchar)s[0] : 0;
            return env->CallStaticObjectMethod(g_jvm.clsCharacter, g_jvm.midCharacter_valueOf, c);
        }
        return env->NewStringUTF(s);
    }

    return luauToJavaObject(L, env, argIdx);
}

static std::string getClassSignature(JNIEnv* env, jclass cls)
{
    jboolean isPrim = env->CallBooleanMethod(cls, g_jvm.midClass_isPrimitive);
    if (isPrim)
    {
        jstring nameStr = (jstring)env->CallObjectMethod(cls, g_jvm.midClass_getName);
        const char* utf = env->GetStringUTFChars(nameStr, nullptr);
        std::string pName = utf ? utf : "";
        if (utf) env->ReleaseStringUTFChars(nameStr, utf);
        env->DeleteLocalRef(nameStr);

        if (pName == "boolean") return "Z";
        if (pName == "byte") return "B";
        if (pName == "char") return "C";
        if (pName == "short") return "S";
        if (pName == "int") return "I";
        if (pName == "long") return "J";
        if (pName == "float") return "F";
        if (pName == "double") return "D";
        if (pName == "void") return "V";
        return "V";
    }

    jstring nameStr = (jstring)env->CallObjectMethod(cls, g_jvm.midClass_getName);
    const char* utf = env->GetStringUTFChars(nameStr, nullptr);
    std::string name = utf ? utf : "";
    if (utf) env->ReleaseStringUTFChars(nameStr, utf);
    env->DeleteLocalRef(nameStr);

    if (!name.empty() && name[0] == '[')
    {
        for (char& c : name)
        {
            if (c == '.') c = '/';
        }
        return name;
    }

    for (char& c : name)
    {
        if (c == '.') c = '/';
    }
    return "L" + name + ";";
}

static std::string getMethodSignature(JNIEnv* env, jobject method, char* outRetType)
{
    jobjectArray paramTypes = (jobjectArray)env->CallObjectMethod(method, g_jvm.midMethod_getParameterTypes);
    jclass retType = (jclass)env->CallObjectMethod(method, g_jvm.midMethod_getReturnType);

    std::string sig = "(";
    int pCount = paramTypes ? env->GetArrayLength(paramTypes) : 0;
    for (int p = 0; p < pCount; ++p)
    {
        jclass pCls = (jclass)env->GetObjectArrayElement(paramTypes, p);
        sig += getClassSignature(env, pCls);
        env->DeleteLocalRef(pCls);
    }
    if (paramTypes) env->DeleteLocalRef(paramTypes);

    sig += ")";
    std::string retSig = getClassSignature(env, retType);
    sig += retSig;

    if (outRetType)
    {
        *outRetType = retSig.empty() ? 'V' : retSig[0];
    }
    env->DeleteLocalRef(retType);

    return sig;
}

static std::string getConstructorSignature(JNIEnv* env, jobject ctor)
{
    jobjectArray paramTypes = (jobjectArray)env->CallObjectMethod(ctor, g_jvm.midConstructor_getParameterTypes);

    std::string sig = "(";
    int pCount = paramTypes ? env->GetArrayLength(paramTypes) : 0;
    for (int p = 0; p < pCount; ++p)
    {
        jclass pCls = (jclass)env->GetObjectArrayElement(paramTypes, p);
        sig += getClassSignature(env, pCls);
        env->DeleteLocalRef(pCls);
    }
    if (paramTypes) env->DeleteLocalRef(paramTypes);

    sig += ")V";
    return sig;
}

static jvalue convertLuauArgToJValue(JNIEnv* env, lua_State* L, int argIdx, jclass paramCls)
{
    jvalue v;
    memset(&v, 0, sizeof(v));
    int ltype = lua_type(L, argIdx);
    jboolean isPrim = env->CallBooleanMethod(paramCls, g_jvm.midClass_isPrimitive);

    if (!isPrim)
    {
        v.l = luauToJavaObject(L, env, argIdx);
        return v;
    }

    jstring nameStr = (jstring)env->CallObjectMethod(paramCls, g_jvm.midClass_getName);
    const char* utf = env->GetStringUTFChars(nameStr, nullptr);
    std::string pName = utf ? utf : "";
    if (utf) env->ReleaseStringUTFChars(nameStr, utf);
    env->DeleteLocalRef(nameStr);

    if (pName == "boolean")
    {
        v.z = lua_toboolean(L, argIdx) ? JNI_TRUE : JNI_FALSE;
    }
    else if (pName == "byte")
    {
        v.b = (jbyte)lua_tointeger(L, argIdx);
    }
    else if (pName == "char")
    {
        if (ltype == LUA_TSTRING)
        {
            const char* s = lua_tostring(L, argIdx);
            v.c = s ? (jchar)s[0] : 0;
        }
        else
        {
            v.c = (jchar)lua_tointeger(L, argIdx);
        }
    }
    else if (pName == "short")
    {
        v.s = (jshort)lua_tointeger(L, argIdx);
    }
    else if (pName == "int")
    {
        v.i = (jint)lua_tointeger(L, argIdx);
    }
    else if (pName == "long")
    {
        v.j = (jlong)lua_tonumber(L, argIdx);
    }
    else if (pName == "float")
    {
        v.f = (jfloat)lua_tonumber(L, argIdx);
    }
    else if (pName == "double")
    {
        v.d = (jdouble)lua_tonumber(L, argIdx);
    }
    return v;
}

// ---------------------------------------------------------------------------
// Metamethods: JObject
// ---------------------------------------------------------------------------
static int jni_object_gc(lua_State* L)
{
    JObjectUserData* ud = (JObjectUserData*)lua_touserdata(L, 1);
    if (ud)
    {
        if (ud->object && g_jvm.initialized && g_jvm.vm)
        {
            JNIEnv* env = nullptr;
            if (g_jvm.vm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env)
                env->DeleteGlobalRef(ud->object);
            ud->object = nullptr;
        }
        if (ud->className)
        {
            free(ud->className);
            ud->className = nullptr;
        }
    }
    return 0;
}

static int jni_object_tostring(lua_State* L)
{
    JObjectUserData* ud = (JObjectUserData*)luaL_checkudata(L, 1, JNI_OBJECT_MT);
    if (!ud->object)
    {
        lua_pushliteral(L, "null");
        return 1;
    }
    JNIEnv* env = getJNIEnv(L);
    jclass cls = env->GetObjectClass(ud->object);
    jmethodID mid = env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
    jstring s = (jstring)env->CallObjectMethod(ud->object, mid);
    env->DeleteLocalRef(cls);
    if (s)
    {
        const char* utf = env->GetStringUTFChars(s, nullptr);
        lua_pushstring(L, utf ? utf : "");
        if (utf) env->ReleaseStringUTFChars(s, utf);
        env->DeleteLocalRef(s);
    }
    else
    {
        lua_pushliteral(L, "null");
    }
    return 1;
}

static int jni_object_eq(lua_State* L)
{
    JObjectUserData* ud1 = (JObjectUserData*)luaL_checkudata(L, 1, JNI_OBJECT_MT);
    if (!lua_isuserdata(L, 2))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    JObjectUserData* ud2 = (JObjectUserData*)lua_touserdata(L, 2);
    if (!ud2)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (ud1->object == ud2->object)
    {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (!ud1->object || !ud2->object)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    JNIEnv* env = getJNIEnv(L);
    jclass cls = env->GetObjectClass(ud1->object);
    jmethodID mid = env->GetMethodID(cls, "equals", "(Ljava/lang/Object;)Z");
    jboolean eq = env->CallBooleanMethod(ud1->object, mid, ud2->object);
    env->DeleteLocalRef(cls);
    lua_pushboolean(L, eq ? 1 : 0);
    return 1;
}

// Closure invoked when calling a method on a JObject
static int jni_invoke_instance_method(lua_State* L)
{
    // Upvalues: 1 = target object (JObject), 2 = method name (string)
    JObjectUserData* ud = (JObjectUserData*)lua_touserdata(L, lua_upvalueindex(1));
    const char* methodName = lua_tostring(L, lua_upvalueindex(2));

    if (!ud || !ud->object)
        luaL_error(L, "attempt to call method '%s' on null Java object", methodName);

    JNIEnv* env = getJNIEnv(L);
    jclass cls = env->GetObjectClass(ud->object);

    int numArgs = lua_gettop(L);
    // If called with colon syntax obj:method(...), first argument on stack might be the object itself
    int startArg = 1;
    if (numArgs >= 1 && lua_isuserdata(L, 1))
    {
        JObjectUserData* argUd = (JObjectUserData*)lua_touserdata(L, 1);
        if (argUd && argUd->object == ud->object)
            startArg = 2;
    }
    int actualArgs = numArgs - startArg + 1;

    jobjectArray methods = (jobjectArray)env->CallObjectMethod(cls, g_jvm.midClass_getMethods);
    checkJniException(L, env);

    int methodCount = env->GetArrayLength(methods);
    jobject bestMethod = nullptr;
    jobjectArray bestParamTypes = nullptr;
    int bestScore = 999999;

    for (int i = 0; i < methodCount; ++i)
    {
        jobject m = env->GetObjectArrayElement(methods, i);
        jstring mNameStr = (jstring)env->CallObjectMethod(m, g_jvm.midMethod_getName);
        const char* mNameUtf = env->GetStringUTFChars(mNameStr, nullptr);
        bool nameMatch = (mNameUtf && strcmp(mNameUtf, methodName) == 0);
        if (mNameUtf) env->ReleaseStringUTFChars(mNameStr, mNameUtf);
        env->DeleteLocalRef(mNameStr);

        if (nameMatch)
        {
            jobjectArray paramTypes = (jobjectArray)env->CallObjectMethod(m, g_jvm.midMethod_getParameterTypes);
            int pCount = env->GetArrayLength(paramTypes);
            if (pCount == actualArgs)
            {
                int currentScore = 0;
                bool match = true;
                for (int p = 0; p < pCount; ++p)
                {
                    jclass pCls = (jclass)env->GetObjectArrayElement(paramTypes, p);
                    int s = scoreArgumentMatch(env, L, startArg + p, pCls);
                    env->DeleteLocalRef(pCls);
                    if (s < 0)
                    {
                        match = false;
                        break;
                    }
                    currentScore += s;
                }
                if (match && currentScore < bestScore)
                {
                    if (bestParamTypes) env->DeleteLocalRef(bestParamTypes);
                    if (bestMethod) env->DeleteLocalRef(bestMethod);
                    bestScore = currentScore;
                    bestMethod = m;
                    bestParamTypes = paramTypes;
                    continue;
                }
            }
            env->DeleteLocalRef(paramTypes);
        }
        env->DeleteLocalRef(m);
    }
    env->DeleteLocalRef(methods);

    if (!bestMethod)
    {
        env->DeleteLocalRef(cls);
        luaL_error(L, "No matching Java method '%s' found on '%s' for %d arguments", methodName, ud->className, actualArgs);
        return 0;
    }

    char retTypeChar = 'V';
    std::string sig = getMethodSignature(env, bestMethod, &retTypeChar);
    jmethodID mid = env->GetMethodID(cls, methodName, sig.c_str());

    std::vector<jvalue> jargs(actualArgs);
    for (int p = 0; p < actualArgs; ++p)
    {
        jclass pCls = (jclass)env->GetObjectArrayElement(bestParamTypes, p);
        jargs[p] = convertLuauArgToJValue(env, L, startArg + p, pCls);
        env->DeleteLocalRef(pCls);
    }
    env->DeleteLocalRef(bestParamTypes);
    env->DeleteLocalRef(bestMethod);

    if (!mid)
    {
        checkJniException(L, env);
        env->DeleteLocalRef(cls);
        luaL_error(L, "Failed to get method ID for '%s' with signature '%s'", methodName, sig.c_str());
        return 0;
    }

    switch (retTypeChar)
    {
    case 'V': {
        env->CallVoidMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        return 0;
    }
    case 'Z': {
        jboolean res = env->CallBooleanMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        lua_pushboolean(L, res ? 1 : 0);
        return 1;
    }
    case 'B': {
        jbyte res = env->CallByteMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        lua_pushinteger(L, res);
        return 1;
    }
    case 'C': {
        jchar res = env->CallCharMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        char buf[2] = { (char)res, 0 };
        lua_pushstring(L, buf);
        return 1;
    }
    case 'S': {
        jshort res = env->CallShortMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        lua_pushinteger(L, res);
        return 1;
    }
    case 'I': {
        jint res = env->CallIntMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        lua_pushinteger(L, res);
        return 1;
    }
    case 'J': {
        jlong res = env->CallLongMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        lua_pushnumber(L, (double)res);
        return 1;
    }
    case 'F': {
        jfloat res = env->CallFloatMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        lua_pushnumber(L, (double)res);
        return 1;
    }
    case 'D': {
        jdouble res = env->CallDoubleMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        lua_pushnumber(L, res);
        return 1;
    }
    default: {
        jobject res = env->CallObjectMethodA(ud->object, mid, jargs.data());
        env->DeleteLocalRef(cls);
        checkJniException(L, env);
        javaToLuauValue(L, env, res);
        if (res) env->DeleteLocalRef(res);
        return 1;
    }
    }
}

static int jni_object_index(lua_State* L)
{
    JObjectUserData* ud = (JObjectUserData*)luaL_checkudata(L, 1, JNI_OBJECT_MT);
    const char* key = luaL_checkstring(L, 2);

    if (!ud->object)
        luaL_error(L, "attempt to index null Java object");

    // Special properties
    if (strcmp(key, "getClass") == 0 || strcmp(key, "class") == 0)
    {
        JNIEnv* env = getJNIEnv(L);
        jclass cls = env->GetObjectClass(ud->object);
        pushJClass(L, env, cls, ud->className);
        env->DeleteLocalRef(cls);
        return 1;
    }

    JNIEnv* env = getJNIEnv(L);
    jclass cls = env->GetObjectClass(ud->object);

    // 1. Check if it's a field
    jobjectArray fields = (jobjectArray)env->CallObjectMethod(cls, g_jvm.midClass_getFields);
    int fieldCount = fields ? env->GetArrayLength(fields) : 0;
    for (int i = 0; i < fieldCount; ++i)
    {
        jobject f = env->GetObjectArrayElement(fields, i);
        jstring fNameStr = (jstring)env->CallObjectMethod(f, g_jvm.midField_getName);
        const char* fNameUtf = env->GetStringUTFChars(fNameStr, nullptr);
        bool match = (fNameUtf && strcmp(fNameUtf, key) == 0);
        if (fNameUtf) env->ReleaseStringUTFChars(fNameStr, fNameUtf);
        env->DeleteLocalRef(fNameStr);

        if (match)
        {
            jobject val = env->CallObjectMethod(f, g_jvm.midField_get, ud->object);
            jclass fType = (jclass)env->CallObjectMethod(f, g_jvm.midField_getType);
            env->DeleteLocalRef(f);
            env->DeleteLocalRef(fields);
            env->DeleteLocalRef(cls);
            checkJniException(L, env);
            javaToLuauValue(L, env, val, fType);
            if (val) env->DeleteLocalRef(val);
            env->DeleteLocalRef(fType);
            return 1;
        }
        env->DeleteLocalRef(f);
    }
    if (fields) env->DeleteLocalRef(fields);
    env->DeleteLocalRef(cls);

    // 2. Return a method invoker closure with (ud, key) as upvalues
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, jni_invoke_instance_method, nullptr, 2);
    return 1;
}

static int jni_object_newindex(lua_State* L)
{
    JObjectUserData* ud = (JObjectUserData*)luaL_checkudata(L, 1, JNI_OBJECT_MT);
    const char* key = luaL_checkstring(L, 2);

    if (!ud->object)
        luaL_error(L, "attempt to modify field on null Java object");

    JNIEnv* env = getJNIEnv(L);
    jclass cls = env->GetObjectClass(ud->object);

    jobjectArray fields = (jobjectArray)env->CallObjectMethod(cls, g_jvm.midClass_getFields);
    int fieldCount = fields ? env->GetArrayLength(fields) : 0;
    bool found = false;
    for (int i = 0; i < fieldCount; ++i)
    {
        jobject f = env->GetObjectArrayElement(fields, i);
        jstring fNameStr = (jstring)env->CallObjectMethod(f, g_jvm.midField_getName);
        const char* fNameUtf = env->GetStringUTFChars(fNameStr, nullptr);
        bool match = (fNameUtf && strcmp(fNameUtf, key) == 0);
        if (fNameUtf) env->ReleaseStringUTFChars(fNameStr, fNameUtf);
        env->DeleteLocalRef(fNameStr);

        if (match)
        {
            jclass fType = (jclass)env->CallObjectMethod(f, g_jvm.midField_getType);
            jobject converted = convertLuauArgToParam(env, L, 3, fType);
            env->CallVoidMethod(f, g_jvm.midField_set, ud->object, converted);
            if (converted) env->DeleteLocalRef(converted);
            env->DeleteLocalRef(fType);
            env->DeleteLocalRef(f);
            found = true;
            break;
        }
        env->DeleteLocalRef(f);
    }
    if (fields) env->DeleteLocalRef(fields);
    env->DeleteLocalRef(cls);

    checkJniException(L, env);
    if (!found)
        luaL_error(L, "Field '%s' not found on Java class '%s'", key, ud->className);
    return 0;
}

// ---------------------------------------------------------------------------
// Metamethods: JClass
// ---------------------------------------------------------------------------
static int jni_class_gc(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)lua_touserdata(L, 1);
    if (ud)
    {
        if (ud->clazz && g_jvm.initialized && g_jvm.vm)
        {
            JNIEnv* env = nullptr;
            if (g_jvm.vm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env)
                env->DeleteGlobalRef(ud->clazz);
            ud->clazz = nullptr;
        }
        if (ud->className)
        {
            free(ud->className);
            ud->className = nullptr;
        }
    }
    return 0;
}

static int jni_class_tostring(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    std::string s = "class " + (ud->className ? dotClassName(ud->className) : "unknown");
    lua_pushstring(L, s.c_str());
    return 1;
}

// Closure invoked when constructing a new instance via Class:new(...) or Class(...)
static int jni_invoke_constructor(lua_State* L)
{
    // Upvalue 1 = target class (JClass)
    JClassUserData* ud = (JClassUserData*)lua_touserdata(L, lua_upvalueindex(1));
    if (!ud || !ud->clazz)
        luaL_error(L, "attempt to instantiate null Java class");

    JNIEnv* env = getJNIEnv(L);

    int numArgs = lua_gettop(L);
    int startArg = 1;
    if (numArgs >= 1 && lua_isuserdata(L, 1))
    {
        JClassUserData* argUd = (JClassUserData*)lua_touserdata(L, 1);
        if (argUd && argUd->clazz == ud->clazz)
            startArg = 2;
    }
    int actualArgs = numArgs - startArg + 1;

    jobjectArray ctors = (jobjectArray)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getConstructors);
    checkJniException(L, env);

    int ctorCount = env->GetArrayLength(ctors);
    jobject bestCtor = nullptr;
    jobjectArray bestParamTypes = nullptr;
    int bestScore = 999999;

    for (int i = 0; i < ctorCount; ++i)
    {
        jobject c = env->GetObjectArrayElement(ctors, i);
        jobjectArray paramTypes = (jobjectArray)env->CallObjectMethod(c, g_jvm.midConstructor_getParameterTypes);
        int pCount = env->GetArrayLength(paramTypes);

        if (pCount == actualArgs)
        {
            int currentScore = 0;
            bool match = true;
            for (int p = 0; p < pCount; ++p)
            {
                jclass pCls = (jclass)env->GetObjectArrayElement(paramTypes, p);
                int s = scoreArgumentMatch(env, L, startArg + p, pCls);
                env->DeleteLocalRef(pCls);
                if (s < 0)
                {
                    match = false;
                    break;
                }
                currentScore += s;
            }
            if (match && currentScore < bestScore)
            {
                if (bestParamTypes) env->DeleteLocalRef(bestParamTypes);
                if (bestCtor) env->DeleteLocalRef(bestCtor);
                bestScore = currentScore;
                bestCtor = c;
                bestParamTypes = paramTypes;
                continue;
            }
        }
        env->DeleteLocalRef(paramTypes);
        env->DeleteLocalRef(c);
    }
    env->DeleteLocalRef(ctors);

    if (!bestCtor)
        luaL_error(L, "No matching constructor found for Java class '%s' with %d arguments", ud->className, actualArgs);

    std::string sig = getConstructorSignature(env, bestCtor);
    jmethodID mid = env->GetMethodID(ud->clazz, "<init>", sig.c_str());

    std::vector<jvalue> jargs(actualArgs);
    for (int p = 0; p < actualArgs; ++p)
    {
        jclass pCls = (jclass)env->GetObjectArrayElement(bestParamTypes, p);
        jargs[p] = convertLuauArgToJValue(env, L, startArg + p, pCls);
        env->DeleteLocalRef(pCls);
    }
    env->DeleteLocalRef(bestParamTypes);
    env->DeleteLocalRef(bestCtor);

    if (!mid)
    {
        checkJniException(L, env);
        luaL_error(L, "Failed to get constructor ID for '%s' with signature '%s'", ud->className, sig.c_str());
        return 0;
    }

    jobject newObj = env->NewObjectA(ud->clazz, mid, jargs.data());
    checkJniException(L, env);

    pushJObject(L, env, newObj, ud->className);
    if (newObj) env->DeleteLocalRef(newObj);
    return 1;
}

// Closure invoked when calling a static method on a JClass
static int jni_invoke_static_method(lua_State* L)
{
    // Upvalues: 1 = target class (JClass), 2 = static method name (string)
    JClassUserData* ud = (JClassUserData*)lua_touserdata(L, lua_upvalueindex(1));
    const char* methodName = lua_tostring(L, lua_upvalueindex(2));

    if (!ud || !ud->clazz)
        luaL_error(L, "attempt to call static method '%s' on null Java class", methodName);

    JNIEnv* env = getJNIEnv(L);

    int numArgs = lua_gettop(L);
    int startArg = 1;
    if (numArgs >= 1 && lua_isuserdata(L, 1))
    {
        JClassUserData* argUd = (JClassUserData*)lua_touserdata(L, 1);
        if (argUd && argUd->clazz == ud->clazz)
            startArg = 2;
    }
    int actualArgs = numArgs - startArg + 1;

    jobjectArray methods = (jobjectArray)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getMethods);
    checkJniException(L, env);

    int methodCount = env->GetArrayLength(methods);
    jobject bestMethod = nullptr;
    jobjectArray bestParamTypes = nullptr;
    int bestScore = 999999;

    for (int i = 0; i < methodCount; ++i)
    {
        jobject m = env->GetObjectArrayElement(methods, i);
        jint modifiers = env->CallIntMethod(m, g_jvm.midMethod_getModifiers);
        // Modifier.isStatic: (modifiers & 8) != 0
        if ((modifiers & 8) != 0)
        {
            jstring mNameStr = (jstring)env->CallObjectMethod(m, g_jvm.midMethod_getName);
            const char* mNameUtf = env->GetStringUTFChars(mNameStr, nullptr);
            bool nameMatch = (mNameUtf && strcmp(mNameUtf, methodName) == 0);
            if (mNameUtf) env->ReleaseStringUTFChars(mNameStr, mNameUtf);
            env->DeleteLocalRef(mNameStr);

            if (nameMatch)
            {
                jobjectArray paramTypes = (jobjectArray)env->CallObjectMethod(m, g_jvm.midMethod_getParameterTypes);
                int pCount = env->GetArrayLength(paramTypes);
                if (pCount == actualArgs)
                {
                    int currentScore = 0;
                    bool match = true;
                    for (int p = 0; p < pCount; ++p)
                    {
                        jclass pCls = (jclass)env->GetObjectArrayElement(paramTypes, p);
                        int s = scoreArgumentMatch(env, L, startArg + p, pCls);
                        env->DeleteLocalRef(pCls);
                        if (s < 0)
                        {
                            match = false;
                            break;
                        }
                        currentScore += s;
                    }
                    if (match && currentScore < bestScore)
                    {
                        if (bestParamTypes) env->DeleteLocalRef(bestParamTypes);
                        if (bestMethod) env->DeleteLocalRef(bestMethod);
                        bestScore = currentScore;
                        bestMethod = m;
                        bestParamTypes = paramTypes;
                        continue;
                    }
                }
                env->DeleteLocalRef(paramTypes);
            }
        }
        env->DeleteLocalRef(m);
    }
    env->DeleteLocalRef(methods);

    if (!bestMethod)
        luaL_error(L, "No matching static method '%s' found on Java class '%s' for %d arguments", methodName, ud->className, actualArgs);

    char retTypeChar = 'V';
    std::string sig = getMethodSignature(env, bestMethod, &retTypeChar);
    jmethodID mid = env->GetStaticMethodID(ud->clazz, methodName, sig.c_str());

    std::vector<jvalue> jargs(actualArgs);
    for (int p = 0; p < actualArgs; ++p)
    {
        jclass pCls = (jclass)env->GetObjectArrayElement(bestParamTypes, p);
        jargs[p] = convertLuauArgToJValue(env, L, startArg + p, pCls);
        env->DeleteLocalRef(pCls);
    }
    env->DeleteLocalRef(bestParamTypes);
    env->DeleteLocalRef(bestMethod);

    if (!mid)
    {
        checkJniException(L, env);
        luaL_error(L, "Failed to get static method ID for '%s' with signature '%s'", methodName, sig.c_str());
        return 0;
    }

    switch (retTypeChar)
    {
    case 'V': {
        env->CallStaticVoidMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        return 0;
    }
    case 'Z': {
        jboolean res = env->CallStaticBooleanMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        lua_pushboolean(L, res ? 1 : 0);
        return 1;
    }
    case 'B': {
        jbyte res = env->CallStaticByteMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        lua_pushinteger(L, res);
        return 1;
    }
    case 'C': {
        jchar res = env->CallStaticCharMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        char buf[2] = { (char)res, 0 };
        lua_pushstring(L, buf);
        return 1;
    }
    case 'S': {
        jshort res = env->CallStaticShortMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        lua_pushinteger(L, res);
        return 1;
    }
    case 'I': {
        jint res = env->CallStaticIntMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        lua_pushinteger(L, res);
        return 1;
    }
    case 'J': {
        jlong res = env->CallStaticLongMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        lua_pushnumber(L, (double)res);
        return 1;
    }
    case 'F': {
        jfloat res = env->CallStaticFloatMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        lua_pushnumber(L, (double)res);
        return 1;
    }
    case 'D': {
        jdouble res = env->CallStaticDoubleMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        lua_pushnumber(L, res);
        return 1;
    }
    default: {
        jobject res = env->CallStaticObjectMethodA(ud->clazz, mid, jargs.data());
        checkJniException(L, env);
        javaToLuauValue(L, env, res);
        if (res) env->DeleteLocalRef(res);
        return 1;
    }
    }
}

// Built-in Reflection methods on JClass
static int jni_class_getName(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    lua_pushstring(L, ud->className ? dotClassName(ud->className).c_str() : "");
    return 1;
}

static int jni_class_getSuperclass(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jclass superCls = (jclass)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getSuperclass);
    if (superCls)
    {
        pushJClass(L, env, superCls);
        env->DeleteLocalRef(superCls);
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

static int jni_class_getInterfaces(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jobjectArray ifaces = (jobjectArray)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getInterfaces);
    int count = ifaces ? env->GetArrayLength(ifaces) : 0;
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i)
    {
        jclass iface = (jclass)env->GetObjectArrayElement(ifaces, i);
        pushJClass(L, env, iface);
        env->DeleteLocalRef(iface);
        lua_rawseti(L, -2, i + 1);
    }
    if (ifaces) env->DeleteLocalRef(ifaces);
    return 1;
}

static int jni_class_isArray(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jboolean res = env->CallBooleanMethod(ud->clazz, g_jvm.midClass_isArray);
    lua_pushboolean(L, res ? 1 : 0);
    return 1;
}

static int jni_class_isPrimitive(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jboolean res = env->CallBooleanMethod(ud->clazz, g_jvm.midClass_isPrimitive);
    lua_pushboolean(L, res ? 1 : 0);
    return 1;
}

static int jni_class_isInterface(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jboolean res = env->CallBooleanMethod(ud->clazz, g_jvm.midClass_isInterface);
    lua_pushboolean(L, res ? 1 : 0);
    return 1;
}

static int jni_class_isAssignableFrom(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JClassUserData* otherUd = (JClassUserData*)luaL_checkudata(L, 2, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jboolean res = env->CallBooleanMethod(ud->clazz, g_jvm.midClass_isAssignableFrom, otherUd->clazz);
    lua_pushboolean(L, res ? 1 : 0);
    return 1;
}

static int jni_class_getMethods(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jobjectArray methods = (jobjectArray)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getMethods);
    int count = methods ? env->GetArrayLength(methods) : 0;
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i)
    {
        jobject m = env->GetObjectArrayElement(methods, i);
        jstring mName = (jstring)env->CallObjectMethod(m, g_jvm.midMethod_getName);
        const char* utf = env->GetStringUTFChars(mName, nullptr);
        lua_pushstring(L, utf ? utf : "");
        if (utf) env->ReleaseStringUTFChars(mName, utf);
        env->DeleteLocalRef(mName);
        env->DeleteLocalRef(m);
        lua_rawseti(L, -2, i + 1);
    }
    if (methods) env->DeleteLocalRef(methods);
    return 1;
}

static int jni_class_getFields(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    JNIEnv* env = getJNIEnv(L);
    jobjectArray fields = (jobjectArray)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getFields);
    int count = fields ? env->GetArrayLength(fields) : 0;
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i)
    {
        jobject f = env->GetObjectArrayElement(fields, i);
        jstring fName = (jstring)env->CallObjectMethod(f, g_jvm.midField_getName);
        const char* utf = env->GetStringUTFChars(fName, nullptr);
        lua_pushstring(L, utf ? utf : "");
        if (utf) env->ReleaseStringUTFChars(fName, utf);
        env->DeleteLocalRef(fName);
        env->DeleteLocalRef(f);
        lua_rawseti(L, -2, i + 1);
    }
    if (fields) env->DeleteLocalRef(fields);
    return 1;
}

static int jni_class_index(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    const char* key = luaL_checkstring(L, 2);

    if (!ud->clazz)
        luaL_error(L, "attempt to index null Java class");

    // Constructor syntax Class.new(...)
    if (strcmp(key, "new") == 0)
    {
        lua_pushvalue(L, 1);
        lua_pushcclosure(L, jni_invoke_constructor, nullptr, 1);
        return 1;
    }

    // Builtin class reflection methods
    if (strcmp(key, "getName") == 0) { lua_pushcfunction(L, jni_class_getName, "getName"); return 1; }
    if (strcmp(key, "getSuperclass") == 0) { lua_pushcfunction(L, jni_class_getSuperclass, "getSuperclass"); return 1; }
    if (strcmp(key, "getInterfaces") == 0) { lua_pushcfunction(L, jni_class_getInterfaces, "getInterfaces"); return 1; }
    if (strcmp(key, "isArray") == 0) { lua_pushcfunction(L, jni_class_isArray, "isArray"); return 1; }
    if (strcmp(key, "isPrimitive") == 0) { lua_pushcfunction(L, jni_class_isPrimitive, "isPrimitive"); return 1; }
    if (strcmp(key, "isInterface") == 0) { lua_pushcfunction(L, jni_class_isInterface, "isInterface"); return 1; }
    if (strcmp(key, "isAssignableFrom") == 0) { lua_pushcfunction(L, jni_class_isAssignableFrom, "isAssignableFrom"); return 1; }
    if (strcmp(key, "getMethods") == 0) { lua_pushcfunction(L, jni_class_getMethods, "getMethods"); return 1; }
    if (strcmp(key, "getFields") == 0) { lua_pushcfunction(L, jni_class_getFields, "getFields"); return 1; }

    JNIEnv* env = getJNIEnv(L);

    // 1. Check if it's a static field
    jobjectArray fields = (jobjectArray)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getFields);
    int fieldCount = fields ? env->GetArrayLength(fields) : 0;
    for (int i = 0; i < fieldCount; ++i)
    {
        jobject f = env->GetObjectArrayElement(fields, i);
        jint modifiers = env->CallIntMethod(f, g_jvm.midField_getModifiers);
        if ((modifiers & 8) != 0) // static
        {
            jstring fNameStr = (jstring)env->CallObjectMethod(f, g_jvm.midField_getName);
            const char* fNameUtf = env->GetStringUTFChars(fNameStr, nullptr);
            bool match = (fNameUtf && strcmp(fNameUtf, key) == 0);
            if (fNameUtf) env->ReleaseStringUTFChars(fNameStr, fNameUtf);
            env->DeleteLocalRef(fNameStr);

            if (match)
            {
                jobject val = env->CallObjectMethod(f, g_jvm.midField_get, nullptr);
                jclass fType = (jclass)env->CallObjectMethod(f, g_jvm.midField_getType);
                env->DeleteLocalRef(f);
                env->DeleteLocalRef(fields);
                checkJniException(L, env);
                javaToLuauValue(L, env, val, fType);
                if (val) env->DeleteLocalRef(val);
                env->DeleteLocalRef(fType);
                return 1;
            }
        }
        env->DeleteLocalRef(f);
    }
    if (fields) env->DeleteLocalRef(fields);

    // 2. Return a static method closure with (ud, key) as upvalues
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, jni_invoke_static_method, nullptr, 2);
    return 1;
}

static int jni_class_newindex(lua_State* L)
{
    JClassUserData* ud = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
    const char* key = luaL_checkstring(L, 2);

    if (!ud->clazz)
        luaL_error(L, "attempt to modify static field on null Java class");

    JNIEnv* env = getJNIEnv(L);
    jobjectArray fields = (jobjectArray)env->CallObjectMethod(ud->clazz, g_jvm.midClass_getFields);
    int fieldCount = fields ? env->GetArrayLength(fields) : 0;
    bool found = false;
    for (int i = 0; i < fieldCount; ++i)
    {
        jobject f = env->GetObjectArrayElement(fields, i);
        jint modifiers = env->CallIntMethod(f, g_jvm.midField_getModifiers);
        if ((modifiers & 8) != 0) // static
        {
            jstring fNameStr = (jstring)env->CallObjectMethod(f, g_jvm.midField_getName);
            const char* fNameUtf = env->GetStringUTFChars(fNameStr, nullptr);
            bool match = (fNameUtf && strcmp(fNameUtf, key) == 0);
            if (fNameUtf) env->ReleaseStringUTFChars(fNameStr, fNameUtf);
            env->DeleteLocalRef(fNameStr);

            if (match)
            {
                jclass fType = (jclass)env->CallObjectMethod(f, g_jvm.midField_getType);
                jobject converted = convertLuauArgToParam(env, L, 3, fType);
                env->CallVoidMethod(f, g_jvm.midField_set, nullptr, converted);
                if (converted) env->DeleteLocalRef(converted);
                env->DeleteLocalRef(fType);
                env->DeleteLocalRef(f);
                found = true;
                break;
            }
        }
        env->DeleteLocalRef(f);
    }
    if (fields) env->DeleteLocalRef(fields);

    checkJniException(L, env);
    if (!found)
        luaL_error(L, "Static field '%s' not found on Java class '%s'", key, ud->className);
    return 0;
}

// ---------------------------------------------------------------------------
// Metamethods: JArray
// ---------------------------------------------------------------------------
static int jni_array_gc(lua_State* L)
{
    JArrayUserData* ud = (JArrayUserData*)lua_touserdata(L, 1);
    if (ud)
    {
        if (ud->array && g_jvm.initialized && g_jvm.vm)
        {
            JNIEnv* env = nullptr;
            if (g_jvm.vm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env)
                env->DeleteGlobalRef(ud->array);
            ud->array = nullptr;
        }
        if (ud->elementTypeName)
        {
            free(ud->elementTypeName);
            ud->elementTypeName = nullptr;
        }
    }
    return 0;
}

static int jni_array_len(lua_State* L)
{
    JArrayUserData* ud = (JArrayUserData*)luaL_checkudata(L, 1, JNI_ARRAY_MT);
    lua_pushinteger(L, ud->length);
    return 1;
}

static int jni_array_tostring(lua_State* L)
{
    JArrayUserData* ud = (JArrayUserData*)luaL_checkudata(L, 1, JNI_ARRAY_MT);
    std::string s = "[" + std::to_string(ud->length) + " items of " + (ud->elementTypeName ? dotClassName(ud->elementTypeName) : "Object") + "]";
    lua_pushstring(L, s.c_str());
    return 1;
}

static int jni_array_to_table(lua_State* L)
{
    JArrayUserData* ud = (JArrayUserData*)luaL_checkudata(L, 1, JNI_ARRAY_MT);
    JNIEnv* env = getJNIEnv(L);
    lua_createtable(L, ud->length, 0);

    for (int i = 0; i < ud->length; ++i)
    {
        switch (ud->primitiveType)
        {
        case 'Z': {
            jboolean b;
            env->GetBooleanArrayRegion((jbooleanArray)ud->array, i, 1, &b);
            lua_pushboolean(L, b ? 1 : 0);
            break;
        }
        case 'B': {
            jbyte b;
            env->GetByteArrayRegion((jbyteArray)ud->array, i, 1, &b);
            lua_pushinteger(L, b);
            break;
        }
        case 'C': {
            jchar c;
            env->GetCharArrayRegion((jcharArray)ud->array, i, 1, &c);
            char buf[2] = { (char)c, 0 };
            lua_pushstring(L, buf);
            break;
        }
        case 'S': {
            jshort s;
            env->GetShortArrayRegion((jshortArray)ud->array, i, 1, &s);
            lua_pushinteger(L, s);
            break;
        }
        case 'I': {
            jint val;
            env->GetIntArrayRegion((jintArray)ud->array, i, 1, &val);
            lua_pushinteger(L, val);
            break;
        }
        case 'J': {
            jlong val;
            env->GetLongArrayRegion((jlongArray)ud->array, i, 1, &val);
            lua_pushnumber(L, (double)val);
            break;
        }
        case 'F': {
            jfloat val;
            env->GetFloatArrayRegion((jfloatArray)ud->array, i, 1, &val);
            lua_pushnumber(L, (double)val);
            break;
        }
        case 'D': {
            jdouble val;
            env->GetDoubleArrayRegion((jdoubleArray)ud->array, i, 1, &val);
            lua_pushnumber(L, val);
            break;
        }
        default: {
            jobject elem = env->GetObjectArrayElement((jobjectArray)ud->array, i);
            javaToLuauValue(L, env, elem);
            if (elem) env->DeleteLocalRef(elem);
            break;
        }
        }
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int jni_array_to_buffer(lua_State* L)
{
    JArrayUserData* ud = (JArrayUserData*)luaL_checkudata(L, 1, JNI_ARRAY_MT);
    if (!ud->isPrimitive)
        luaL_error(L, "to_buffer is only supported on primitive Java arrays");

    JNIEnv* env = getJNIEnv(L);
    size_t elemSize = 1;
    switch (ud->primitiveType)
    {
    case 'Z': case 'B': elemSize = 1; break;
    case 'C': case 'S': elemSize = 2; break;
    case 'I': case 'F': elemSize = 4; break;
    case 'J': case 'D': elemSize = 8; break;
    }
    size_t totalBytes = elemSize * ud->length;

    lua_getglobal(L, "buffer");
    lua_getfield(L, -1, "create");
    lua_pushnumber(L, (double)totalBytes);
    lua_call(L, 1, 1);
    lua_remove(L, -2); // remove buffer library table

    void* dest = lua_tobuffer(L, -1, nullptr);
    if (dest && totalBytes > 0)
    {
        switch (ud->primitiveType)
        {
        case 'Z': env->GetBooleanArrayRegion((jbooleanArray)ud->array, 0, ud->length, (jboolean*)dest); break;
        case 'B': env->GetByteArrayRegion((jbyteArray)ud->array, 0, ud->length, (jbyte*)dest); break;
        case 'C': env->GetCharArrayRegion((jcharArray)ud->array, 0, ud->length, (jchar*)dest); break;
        case 'S': env->GetShortArrayRegion((jshortArray)ud->array, 0, ud->length, (jshort*)dest); break;
        case 'I': env->GetIntArrayRegion((jintArray)ud->array, 0, ud->length, (jint*)dest); break;
        case 'J': env->GetLongArrayRegion((jlongArray)ud->array, 0, ud->length, (jlong*)dest); break;
        case 'F': env->GetFloatArrayRegion((jfloatArray)ud->array, 0, ud->length, (jfloat*)dest); break;
        case 'D': env->GetDoubleArrayRegion((jdoubleArray)ud->array, 0, ud->length, (jdouble*)dest); break;
        }
    }
    return 1;
}

static int jni_array_get(lua_State* L)
{
    JArrayUserData* ud = (JArrayUserData*)luaL_checkudata(L, 1, JNI_ARRAY_MT);
    int idx = luaL_checkinteger(L, 2); // 1-based index
    if (idx < 1 || idx > ud->length)
        luaL_error(L, "Java array index out of bounds (index %d, length %d)", idx, ud->length);

    int jidx = idx - 1;
    JNIEnv* env = getJNIEnv(L);

    switch (ud->primitiveType)
    {
    case 'Z': {
        jboolean b;
        env->GetBooleanArrayRegion((jbooleanArray)ud->array, jidx, 1, &b);
        lua_pushboolean(L, b ? 1 : 0);
        return 1;
    }
    case 'B': {
        jbyte b;
        env->GetByteArrayRegion((jbyteArray)ud->array, jidx, 1, &b);
        lua_pushinteger(L, b);
        return 1;
    }
    case 'C': {
        jchar c;
        env->GetCharArrayRegion((jcharArray)ud->array, jidx, 1, &c);
        char buf[2] = { (char)c, 0 };
        lua_pushstring(L, buf);
        return 1;
    }
    case 'S': {
        jshort s;
        env->GetShortArrayRegion((jshortArray)ud->array, jidx, 1, &s);
        lua_pushinteger(L, s);
        return 1;
    }
    case 'I': {
        jint val;
        env->GetIntArrayRegion((jintArray)ud->array, jidx, 1, &val);
        lua_pushinteger(L, val);
        return 1;
    }
    case 'J': {
        jlong val;
        env->GetLongArrayRegion((jlongArray)ud->array, jidx, 1, &val);
        lua_pushnumber(L, (double)val);
        return 1;
    }
    case 'F': {
        jfloat val;
        env->GetFloatArrayRegion((jfloatArray)ud->array, jidx, 1, &val);
        lua_pushnumber(L, (double)val);
        return 1;
    }
    case 'D': {
        jdouble val;
        env->GetDoubleArrayRegion((jdoubleArray)ud->array, jidx, 1, &val);
        lua_pushnumber(L, val);
        return 1;
    }
    default: {
        jobject elem = env->GetObjectArrayElement((jobjectArray)ud->array, jidx);
        javaToLuauValue(L, env, elem);
        if (elem) env->DeleteLocalRef(elem);
        return 1;
    }
    }
}

static int jni_array_set(lua_State* L)
{
    JArrayUserData* ud = (JArrayUserData*)luaL_checkudata(L, 1, JNI_ARRAY_MT);
    int idx = luaL_checkinteger(L, 2); // 1-based
    if (idx < 1 || idx > ud->length)
        luaL_error(L, "Java array index out of bounds (index %d, length %d)", idx, ud->length);

    int jidx = idx - 1;
    JNIEnv* env = getJNIEnv(L);

    switch (ud->primitiveType)
    {
    case 'Z': {
        jboolean b = lua_toboolean(L, 3) ? JNI_TRUE : JNI_FALSE;
        env->SetBooleanArrayRegion((jbooleanArray)ud->array, jidx, 1, &b);
        break;
    }
    case 'B': {
        jbyte b = (jbyte)luaL_checkinteger(L, 3);
        env->SetByteArrayRegion((jbyteArray)ud->array, jidx, 1, &b);
        break;
    }
    case 'C': {
        const char* s = luaL_checkstring(L, 3);
        jchar c = s ? (jchar)s[0] : 0;
        env->SetCharArrayRegion((jcharArray)ud->array, jidx, 1, &c);
        break;
    }
    case 'S': {
        jshort s = (jshort)luaL_checkinteger(L, 3);
        env->SetShortArrayRegion((jshortArray)ud->array, jidx, 1, &s);
        break;
    }
    case 'I': {
        jint val = (jint)luaL_checkinteger(L, 3);
        env->SetIntArrayRegion((jintArray)ud->array, jidx, 1, &val);
        break;
    }
    case 'J': {
        jlong val = (jlong)luaL_checknumber(L, 3);
        env->SetLongArrayRegion((jlongArray)ud->array, jidx, 1, &val);
        break;
    }
    case 'F': {
        jfloat val = (jfloat)luaL_checknumber(L, 3);
        env->SetFloatArrayRegion((jfloatArray)ud->array, jidx, 1, &val);
        break;
    }
    case 'D': {
        jdouble val = (jdouble)luaL_checknumber(L, 3);
        env->SetDoubleArrayRegion((jdoubleArray)ud->array, jidx, 1, &val);
        break;
    }
    default: {
        jobject elem = luauToJavaObject(L, env, 3);
        env->SetObjectArrayElement((jobjectArray)ud->array, jidx, elem);
        if (elem) env->DeleteLocalRef(elem);
        break;
    }
    }
    return 0;
}

static int jni_array_index(lua_State* L)
{
    if (lua_isnumber(L, 2))
    {
        return jni_array_get(L);
    }
    const char* key = luaL_checkstring(L, 2);
    if (strcmp(key, "length") == 0)
    {
        JArrayUserData* ud = (JArrayUserData*)luaL_checkudata(L, 1, JNI_ARRAY_MT);
        lua_pushinteger(L, ud->length);
        return 1;
    }
    if (strcmp(key, "get") == 0) { lua_pushcfunction(L, jni_array_get, "get"); return 1; }
    if (strcmp(key, "set") == 0) { lua_pushcfunction(L, jni_array_set, "set"); return 1; }
    if (strcmp(key, "to_table") == 0 || strcmp(key, "toTable") == 0) { lua_pushcfunction(L, jni_array_to_table, "to_table"); return 1; }
    if (strcmp(key, "to_buffer") == 0 || strcmp(key, "toBuffer") == 0) { lua_pushcfunction(L, jni_array_to_buffer, "to_buffer"); return 1; }

    luaL_error(L, "Unknown field or method '%s' on JArray", key);
    return 0;
}

static int jni_array_newindex(lua_State* L)
{
    if (lua_isnumber(L, 2))
    {
        return jni_array_set(L);
    }
    luaL_error(L, "Cannot set non-numeric index on JArray");
    return 0;
}

// ---------------------------------------------------------------------------
// Metamethods: JTypedValue
// ---------------------------------------------------------------------------
static int jni_typed_value_gc(lua_State* L)
{
    JTypedValueUserData* ud = (JTypedValueUserData*)lua_touserdata(L, 1);
    if (ud && ud->type == 'L' && ud->val.l && g_jvm.initialized && g_jvm.vm)
    {
        JNIEnv* env = nullptr;
        if (g_jvm.vm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env)
            env->DeleteGlobalRef(ud->val.l);
        ud->val.l = nullptr;
    }
    return 0;
}

static int jni_typed_value_tostring(lua_State* L)
{
    JTypedValueUserData* ud = (JTypedValueUserData*)luaL_checkudata(L, 1, JNI_TYPED_VALUE_MT);
    char buf[64];
    switch (ud->type)
    {
    case 'Z': snprintf(buf, sizeof(buf), "jboolean(%s)", ud->val.z ? "true" : "false"); break;
    case 'B': snprintf(buf, sizeof(buf), "jbyte(%d)", (int)ud->val.b); break;
    case 'C': snprintf(buf, sizeof(buf), "jchar(%c)", (char)ud->val.c); break;
    case 'S': snprintf(buf, sizeof(buf), "jshort(%d)", (int)ud->val.s); break;
    case 'I': snprintf(buf, sizeof(buf), "jint(%d)", (int)ud->val.i); break;
    case 'J': snprintf(buf, sizeof(buf), "jlong(%lld)", (long long)ud->val.j); break;
    case 'F': snprintf(buf, sizeof(buf), "jfloat(%g)", (double)ud->val.f); break;
    case 'D': snprintf(buf, sizeof(buf), "jdouble(%g)", ud->val.d); break;
    case 'L': snprintf(buf, sizeof(buf), "jobject(%p)", ud->val.l); break;
    default: snprintf(buf, sizeof(buf), "jvalue(?)"); break;
    }
    lua_pushstring(L, buf);
    return 1;
}

// ---------------------------------------------------------------------------
// JNI Module Top-Level Functions
// ---------------------------------------------------------------------------
static int jni_init(lua_State* L)
{
    std::string jvmPath;
    std::vector<std::string> options;
    bool ignoreUnrecognized = true;

    if (lua_istable(L, 1))
    {
        // classpath
        lua_getfield(L, 1, "classpath");
        if (lua_isstring(L, -1))
        {
            options.push_back(std::string("-Djava.class.path=") + lua_tostring(L, -1));
        }
        else if (lua_istable(L, -1))
        {
            std::string cp;
            int n = (int)lua_objlen(L, -1);
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, -1, i);
                if (lua_isstring(L, -1))
                {
                    if (!cp.empty())
#if defined(_WIN32)
                        cp += ";";
#else
                        cp += ":";
#endif
                    cp += lua_tostring(L, -1);
                }
                lua_pop(L, 1);
            }
            if (!cp.empty())
                options.push_back(std::string("-Djava.class.path=") + cp);
        }
        lua_pop(L, 1);

        // options array
        lua_getfield(L, 1, "options");
        if (lua_istable(L, -1))
        {
            int n = (int)lua_objlen(L, -1);
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, -1, i);
                if (lua_isstring(L, -1))
                    options.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        // jvm_path
        lua_getfield(L, 1, "jvm_path");
        if (lua_isstring(L, -1))
            jvmPath = lua_tostring(L, -1);
        lua_pop(L, 1);

        // ignore_unrecognized
        lua_getfield(L, 1, "ignore_unrecognized");
        if (lua_isboolean(L, -1))
            ignoreUnrecognized = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    std::string err;
    bool ok = initJvmInstance(jvmPath, options, ignoreUnrecognized, err);
    if (!ok)
        luaL_error(L, "Failed to initialize JVM: %s", err.c_str());

    lua_pushboolean(L, 1);
    return 1;
}

static int jni_is_initialized(lua_State* L)
{
    lua_pushboolean(L, (g_jvm.initialized && g_jvm.vm) ? 1 : 0);
    return 1;
}

static int jni_destroy(lua_State* L)
{
    if (g_jvm.initialized && g_jvm.vm)
    {
        // Deleting cached global references
        JNIEnv* env = nullptr;
        if (g_jvm.vm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env)
        {
            auto del = [env](jclass& c) { if (c) { env->DeleteGlobalRef(c); c = nullptr; } };
            del(g_jvm.clsClass);
            del(g_jvm.clsMethod);
            del(g_jvm.clsConstructor);
            del(g_jvm.clsField);
            del(g_jvm.clsModifier);
            del(g_jvm.clsThrowable);
            del(g_jvm.clsStringWriter);
            del(g_jvm.clsPrintWriter);
            del(g_jvm.clsBoolean);
            del(g_jvm.clsByte);
            del(g_jvm.clsCharacter);
            del(g_jvm.clsShort);
            del(g_jvm.clsInteger);
            del(g_jvm.clsLong);
            del(g_jvm.clsFloat);
            del(g_jvm.clsDouble);
            del(g_jvm.clsString);
            del(g_jvm.clsObject);
            del(g_jvm.clsList);
            del(g_jvm.clsArrayList);
            del(g_jvm.clsMap);
            del(g_jvm.clsHashMap);
            del(g_jvm.clsSet);
            del(g_jvm.clsIterator);
            del(g_jvm.clsArray);
        }
        g_jvm.vm->DestroyJavaVM();
        g_jvm.vm = nullptr;
        g_jvm.initialized = false;
    }
    if (g_jvm.libHandle)
    {
        osCloseLibrary(g_jvm.libHandle);
        g_jvm.libHandle = nullptr;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int jni_get_version(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jint ver = env->GetVersion();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d (0x%08X)", (ver >> 16) & 0xFFFF, ver & 0xFFFF, ver);
    lua_pushstring(L, buf);
    return 1;
}

static int jni_find_jvm_path(lua_State* L)
{
    std::string path = g_jvm.loadedJvmPath.empty() ? detectJvmPath() : g_jvm.loadedJvmPath;
    lua_pushstring(L, path.c_str());
    return 1;
}

static int jni_attach_current_thread(lua_State* L)
{
    if (!g_jvm.initialized || !g_jvm.vm)
        luaL_error(L, "JVM is not initialized");
    JNIEnv* env = nullptr;
    jint res = g_jvm.vm->AttachCurrentThread((void**)&env, nullptr);
    lua_pushboolean(L, res == JNI_OK ? 1 : 0);
    return 1;
}

static int jni_detach_current_thread(lua_State* L)
{
    if (!g_jvm.initialized || !g_jvm.vm)
    {
        lua_pushboolean(L, 1);
        return 1;
    }
    jint res = g_jvm.vm->DetachCurrentThread();
    lua_pushboolean(L, res == JNI_OK ? 1 : 0);
    return 1;
}

static int jni_find_class(lua_State* L)
{
    const char* rawName = luaL_checkstring(L, 1);
    std::string norm = normalizeClassName(rawName);
    JNIEnv* env = getJNIEnv(L);

    jclass localCls = env->FindClass(norm.c_str());
    if (!localCls)
    {
        env->ExceptionClear();
        luaL_error(L, "Java class not found: %s", rawName);
        return 0;
    }

    pushJClass(L, env, localCls, norm.c_str());
    env->DeleteLocalRef(localCls);
    return 1;
}

static int jni_new(lua_State* L)
{
    // jni.new(className, ...args) or jni.new(JClass, ...args)
    if (lua_isstring(L, 1))
    {
        const char* rawName = lua_tostring(L, 1);
        std::string norm = normalizeClassName(rawName);
        JNIEnv* env = getJNIEnv(L);
        jclass localCls = env->FindClass(norm.c_str());
        if (!localCls)
        {
            env->ExceptionClear();
            luaL_error(L, "Java class not found: %s", rawName);
            return 0;
        }
        pushJClass(L, env, localCls, norm.c_str());
        env->DeleteLocalRef(localCls);
        lua_replace(L, 1); // replace className with JClass at index 1
    }

    luaL_checkudata(L, 1, JNI_CLASS_MT);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, jni_invoke_constructor, nullptr, 1);
    // Move closure before arguments
    lua_insert(L, 2);
    // Arguments start at 3
    int numArgs = lua_gettop(L) - 2;
    lua_call(L, numArgs, 1);
    return 1;
}

static int jni_array(lua_State* L)
{
    // jni.array(typeName, size_or_table)
    const char* typeName = luaL_checkstring(L, 1);
    JNIEnv* env = getJNIEnv(L);

    bool isTable = lua_istable(L, 2);
    int count = isTable ? (int)lua_objlen(L, 2) : luaL_checkinteger(L, 2);
    if (count < 0) count = 0;

    std::string tName(typeName);
    char primType = 0;

    if (tName == "boolean" || tName == "Z") primType = 'Z';
    else if (tName == "byte" || tName == "B") primType = 'B';
    else if (tName == "char" || tName == "C") primType = 'C';
    else if (tName == "short" || tName == "S") primType = 'S';
    else if (tName == "int" || tName == "I") primType = 'I';
    else if (tName == "long" || tName == "J") primType = 'J';
    else if (tName == "float" || tName == "F") primType = 'F';
    else if (tName == "double" || tName == "D") primType = 'D';

    jarray arr = nullptr;
    if (primType != 0)
    {
        switch (primType)
        {
        case 'Z': arr = (jarray)env->NewBooleanArray(count); break;
        case 'B': arr = (jarray)env->NewByteArray(count); break;
        case 'C': arr = (jarray)env->NewCharArray(count); break;
        case 'S': arr = (jarray)env->NewShortArray(count); break;
        case 'I': arr = (jarray)env->NewIntArray(count); break;
        case 'J': arr = (jarray)env->NewLongArray(count); break;
        case 'F': arr = (jarray)env->NewFloatArray(count); break;
        case 'D': arr = (jarray)env->NewDoubleArray(count); break;
        }
    }
    else
    {
        std::string norm = normalizeClassName(typeName);
        jclass elemCls = env->FindClass(norm.c_str());
        if (!elemCls)
        {
            env->ExceptionClear();
            luaL_error(L, "Class not found for Java array: %s", typeName);
            return 0;
        }
        arr = (jarray)env->NewObjectArray(count, elemCls, nullptr);
        env->DeleteLocalRef(elemCls);
    }

    if (!arr)
    {
        checkJniException(L, env);
        luaL_error(L, "Failed to allocate Java array of type '%s' size %d", typeName, count);
        return 0;
    }

    // If a table was passed, populate elements
    if (isTable && arr)
    {
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, 2, i);
            int jidx = i - 1;
            switch (primType)
            {
            case 'Z': {
                jboolean b = lua_toboolean(L, -1) ? JNI_TRUE : JNI_FALSE;
                env->SetBooleanArrayRegion((jbooleanArray)arr, jidx, 1, &b);
                break;
            }
            case 'B': {
                jbyte b = (jbyte)lua_tointeger(L, -1);
                env->SetByteArrayRegion((jbyteArray)arr, jidx, 1, &b);
                break;
            }
            case 'C': {
                const char* s = lua_tostring(L, -1);
                jchar c = s ? (jchar)s[0] : (jchar)lua_tointeger(L, -1);
                env->SetCharArrayRegion((jcharArray)arr, jidx, 1, &c);
                break;
            }
            case 'S': {
                jshort s = (jshort)lua_tointeger(L, -1);
                env->SetShortArrayRegion((jshortArray)arr, jidx, 1, &s);
                break;
            }
            case 'I': {
                jint val = (jint)lua_tointeger(L, -1);
                env->SetIntArrayRegion((jintArray)arr, jidx, 1, &val);
                break;
            }
            case 'J': {
                jlong val = (jlong)lua_tonumber(L, -1);
                env->SetLongArrayRegion((jlongArray)arr, jidx, 1, &val);
                break;
            }
            case 'F': {
                jfloat val = (jfloat)lua_tonumber(L, -1);
                env->SetFloatArrayRegion((jfloatArray)arr, jidx, 1, &val);
                break;
            }
            case 'D': {
                jdouble val = (jdouble)lua_tonumber(L, -1);
                env->SetDoubleArrayRegion((jdoubleArray)arr, jidx, 1, &val);
                break;
            }
            default: {
                jobject elem = luauToJavaObject(L, env, -1);
                env->SetObjectArrayElement((jobjectArray)arr, jidx, elem);
                if (elem) env->DeleteLocalRef(elem);
                break;
            }
            }
            lua_pop(L, 1);
        }
    }

    pushJArray(L, env, arr, typeName, primType);
    env->DeleteLocalRef(arr);
    return 1;
}

static int jni_wrap_buffer(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    JNIEnv* env = getJNIEnv(L);

    jobject directBuf = env->NewDirectByteBuffer(buf, (jlong)len);
    if (!directBuf)
    {
        checkJniException(L, env);
        luaL_error(L, "Failed to wrap buffer with NewDirectByteBuffer");
        return 0;
    }

    pushJObject(L, env, directBuf, "java/nio/ByteBuffer");
    env->DeleteLocalRef(directBuf);
    return 1;
}

static int jni_to_java(lua_State* L)
{
    luaL_checkany(L, 1);
    JNIEnv* env = getJNIEnv(L);
    jobject obj = luauToJavaObject(L, env, 1);
    if (obj)
    {
        pushJObject(L, env, obj);
        env->DeleteLocalRef(obj);
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

static void javaToLuauDeep(lua_State* L, JNIEnv* env, jobject obj)
{
    if (!obj)
    {
        lua_pushnil(L);
        return;
    }

    if (env->IsInstanceOf(obj, g_jvm.clsString))
    {
        const char* utf = env->GetStringUTFChars((jstring)obj, nullptr);
        lua_pushstring(L, utf ? utf : "");
        if (utf) env->ReleaseStringUTFChars((jstring)obj, utf);
        return;
    }

    if (env->IsInstanceOf(obj, g_jvm.clsBoolean))
    {
        jboolean b = env->CallBooleanMethod(obj, g_jvm.midBoolean_booleanValue);
        lua_pushboolean(L, b ? 1 : 0);
        return;
    }

    if (env->IsInstanceOf(obj, g_jvm.clsInteger))
    {
        lua_pushinteger(L, env->CallIntMethod(obj, g_jvm.midInteger_intValue));
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsLong))
    {
        lua_pushnumber(L, (double)env->CallLongMethod(obj, g_jvm.midLong_longValue));
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsDouble))
    {
        lua_pushnumber(L, env->CallDoubleMethod(obj, g_jvm.midDouble_doubleValue));
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsFloat))
    {
        lua_pushnumber(L, (double)env->CallFloatMethod(obj, g_jvm.midFloat_floatValue));
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsShort))
    {
        lua_pushinteger(L, env->CallShortMethod(obj, g_jvm.midShort_shortValue));
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsByte))
    {
        lua_pushinteger(L, env->CallByteMethod(obj, g_jvm.midByte_byteValue));
        return;
    }
    if (env->IsInstanceOf(obj, g_jvm.clsCharacter))
    {
        jchar v = env->CallCharMethod(obj, g_jvm.midCharacter_charValue);
        char buf[2] = { (char)v, '\0' };
        lua_pushstring(L, buf);
        return;
    }

    // List or Collection
    if (env->IsInstanceOf(obj, g_jvm.clsList))
    {
        int size = env->CallIntMethod(obj, g_jvm.midList_size);
        lua_createtable(L, size, 0);
        for (int i = 0; i < size; ++i)
        {
            jobject item = env->CallObjectMethod(obj, g_jvm.midList_get, i);
            javaToLuauDeep(L, env, item);
            if (item) env->DeleteLocalRef(item);
            lua_rawseti(L, -2, i + 1);
        }
        return;
    }

    // Map
    if (env->IsInstanceOf(obj, g_jvm.clsMap))
    {
        lua_newtable(L);
        jobject keySet = env->CallObjectMethod(obj, g_jvm.midMap_keySet);
        if (keySet)
        {
            jobject iter = env->CallObjectMethod(keySet, g_jvm.midSet_iterator);
            if (iter)
            {
                while (env->CallBooleanMethod(iter, g_jvm.midIterator_hasNext))
                {
                    jobject keyObj = env->CallObjectMethod(iter, g_jvm.midIterator_next);
                    jobject valObj = env->CallObjectMethod(obj, g_jvm.midMap_get, keyObj);

                    javaToLuauDeep(L, env, keyObj);
                    javaToLuauDeep(L, env, valObj);
                    lua_settable(L, -3);

                    if (keyObj) env->DeleteLocalRef(keyObj);
                    if (valObj) env->DeleteLocalRef(valObj);
                }
                env->DeleteLocalRef(iter);
            }
            env->DeleteLocalRef(keySet);
        }
        return;
    }

    // Default object userdata wrapper
    pushJObject(L, env, obj);
}

static int jni_to_luau(lua_State* L)
{
    if (lua_isnil(L, 1))
    {
        lua_pushnil(L);
        return 1;
    }
    JNIEnv* env = getJNIEnv(L);
    jobject obj = luauToJavaObject(L, env, 1);
    javaToLuauDeep(L, env, obj);
    return 1;
}

static int jni_instanceof(lua_State* L)
{
    JObjectUserData* ud = (JObjectUserData*)luaL_checkudata(L, 1, JNI_OBJECT_MT);
    if (!ud->object)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    JNIEnv* env = getJNIEnv(L);
    jclass targetCls = nullptr;
    bool needDelete = false;

    if (lua_isuserdata(L, 2))
    {
        JClassUserData* clsUd = (JClassUserData*)luaL_checkudata(L, 2, JNI_CLASS_MT);
        targetCls = clsUd->clazz;
    }
    else
    {
        const char* name = luaL_checkstring(L, 2);
        std::string norm = normalizeClassName(name);
        targetCls = env->FindClass(norm.c_str());
        needDelete = true;
        if (!targetCls)
        {
            env->ExceptionClear();
            lua_pushboolean(L, 0);
            return 1;
        }
    }

    jboolean res = env->IsInstanceOf(ud->object, targetCls);
    if (needDelete && targetCls)
        env->DeleteLocalRef(targetCls);

    lua_pushboolean(L, res ? 1 : 0);
    return 1;
}

static int jni_cast(lua_State* L)
{
    JObjectUserData* ud = (JObjectUserData*)luaL_checkudata(L, 1, JNI_OBJECT_MT);
    const char* targetName = nullptr;
    if (lua_isuserdata(L, 2))
    {
        JClassUserData* clsUd = (JClassUserData*)luaL_checkudata(L, 2, JNI_CLASS_MT);
        targetName = clsUd->className;
    }
    else
    {
        targetName = luaL_checkstring(L, 2);
    }
    std::string norm = normalizeClassName(targetName);
    JNIEnv* env = getJNIEnv(L);
    pushJObject(L, env, ud->object, norm.c_str());
    return 1;
}

// Explicit typed value creators for overload disambiguation
static int jni_jboolean(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    v.z = lua_toboolean(L, 1) ? JNI_TRUE : JNI_FALSE;
    pushJTypedValue(L, env, 'Z', v);
    return 1;
}

static int jni_jbyte(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    v.b = (jbyte)luaL_checkinteger(L, 1);
    pushJTypedValue(L, env, 'B', v);
    return 1;
}

static int jni_jchar(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    if (lua_isnumber(L, 1))
        v.c = (jchar)lua_tointeger(L, 1);
    else
    {
        const char* s = luaL_checkstring(L, 1);
        v.c = s ? (jchar)s[0] : 0;
    }
    pushJTypedValue(L, env, 'C', v);
    return 1;
}

static int jni_jshort(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    v.s = (jshort)luaL_checkinteger(L, 1);
    pushJTypedValue(L, env, 'S', v);
    return 1;
}

static int jni_jint(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    v.i = (jint)luaL_checkinteger(L, 1);
    pushJTypedValue(L, env, 'I', v);
    return 1;
}

static int jni_jlong(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    v.j = (jlong)luaL_checknumber(L, 1);
    pushJTypedValue(L, env, 'J', v);
    return 1;
}

static int jni_jfloat(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    v.f = (jfloat)luaL_checknumber(L, 1);
    pushJTypedValue(L, env, 'F', v);
    return 1;
}

static int jni_jdouble(lua_State* L)
{
    JNIEnv* env = getJNIEnv(L);
    jvalue v;
    v.d = (jdouble)luaL_checknumber(L, 1);
    pushJTypedValue(L, env, 'D', v);
    return 1;
}

static int jni_jstring(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    JNIEnv* env = getJNIEnv(L);
    jstring js = env->NewStringUTF(s);
    pushJObject(L, env, js, "java/lang/String");
    if (js) env->DeleteLocalRef(js);
    return 1;
}

// ---------------------------------------------------------------------------
// Direct JNI Signature-Based Call Helpers
// ---------------------------------------------------------------------------
static int jni_call_method(lua_State* L)
{
    // jni.call_method(obj, methodName, [sig], ...args)
    JObjectUserData* ud = (JObjectUserData*)luaL_checkudata(L, 1, JNI_OBJECT_MT);
    const char* methodName = luaL_checkstring(L, 2);

    if (lua_isstring(L, 3) && lua_tostring(L, 3)[0] == '(')
    {
        // Explicit JNI signature mode
        const char* sig = lua_tostring(L, 3);
        JNIEnv* env = getJNIEnv(L);
        jclass cls = env->GetObjectClass(ud->object);
        jmethodID mid = env->GetMethodID(cls, methodName, sig);
        env->DeleteLocalRef(cls);
        if (!mid)
        {
            checkJniException(L, env);
            luaL_error(L, "Method '%s' with signature '%s' not found on '%s'", methodName, sig, ud->className);
            return 0;
        }

        // Parse signature arguments
        std::vector<jvalue> jargs;
        int argIdx = 4;
        const char* p = sig + 1;
        while (*p && *p != ')')
        {
            jvalue val;
            char typeChar = *p;
            switch (typeChar)
            {
            case 'Z': val.z = lua_toboolean(L, argIdx++) ? JNI_TRUE : JNI_FALSE; break;
            case 'B': val.b = (jbyte)luaL_checkinteger(L, argIdx++); break;
            case 'C': {
                const char* s = luaL_checkstring(L, argIdx++);
                val.c = s ? (jchar)s[0] : 0;
                break;
            }
            case 'S': val.s = (jshort)luaL_checkinteger(L, argIdx++); break;
            case 'I': val.i = (jint)luaL_checkinteger(L, argIdx++); break;
            case 'J': val.j = (jlong)luaL_checknumber(L, argIdx++); break;
            case 'F': val.f = (jfloat)luaL_checknumber(L, argIdx++); break;
            case 'D': val.d = (jdouble)luaL_checknumber(L, argIdx++); break;
            case 'L': {
                val.l = luauToJavaObject(L, env, argIdx++);
                while (*p && *p != ';') ++p;
                break;
            }
            case '[': {
                val.l = luauToJavaObject(L, env, argIdx++);
                while (*p == '[') ++p;
                if (*p == 'L') { while (*p && *p != ';') ++p; }
                break;
            }
            }
            jargs.push_back(val);
            if (*p) ++p;
        }

        const char* retSig = strchr(sig, ')');
        char retType = retSig ? retSig[1] : 'V';

        switch (retType)
        {
        case 'V': {
            env->CallVoidMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            return 0;
        }
        case 'Z': {
            jboolean res = env->CallBooleanMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            lua_pushboolean(L, res ? 1 : 0);
            return 1;
        }
        case 'B': {
            jbyte res = env->CallByteMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            lua_pushinteger(L, res);
            return 1;
        }
        case 'C': {
            jchar res = env->CallCharMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            char buf[2] = { (char)res, 0 };
            lua_pushstring(L, buf);
            return 1;
        }
        case 'S': {
            jshort res = env->CallShortMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            lua_pushinteger(L, res);
            return 1;
        }
        case 'I': {
            jint res = env->CallIntMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            lua_pushinteger(L, res);
            return 1;
        }
        case 'J': {
            jlong res = env->CallLongMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            lua_pushnumber(L, (double)res);
            return 1;
        }
        case 'F': {
            jfloat res = env->CallFloatMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            lua_pushnumber(L, (double)res);
            return 1;
        }
        case 'D': {
            jdouble res = env->CallDoubleMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            lua_pushnumber(L, res);
            return 1;
        }
        default: {
            jobject res = env->CallObjectMethodA(ud->object, mid, jargs.data());
            checkJniException(L, env);
            javaToLuauValue(L, env, res);
            if (res) env->DeleteLocalRef(res);
            return 1;
        }
        }
    }

    // Dynamic overload resolution mode
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, jni_invoke_instance_method, nullptr, 2);
    lua_insert(L, 3);
    int totalArgs = lua_gettop(L) - 3;
    lua_call(L, totalArgs, 1);
    return 1;
}

static int jni_call_static(lua_State* L)
{
    // jni.call_static(clazz, methodName, [sig], ...args)
    jclass targetCls = nullptr;
    const char* clsName = nullptr;
    bool needDelete = false;

    if (lua_isuserdata(L, 1))
    {
        JClassUserData* clsUd = (JClassUserData*)luaL_checkudata(L, 1, JNI_CLASS_MT);
        targetCls = clsUd->clazz;
        clsName = clsUd->className;
    }
    else
    {
        clsName = luaL_checkstring(L, 1);
        std::string norm = normalizeClassName(clsName);
        JNIEnv* env = getJNIEnv(L);
        targetCls = env->FindClass(norm.c_str());
        needDelete = true;
        if (!targetCls)
        {
            env->ExceptionClear();
            luaL_error(L, "Class '%s' not found for call_static", clsName);
            return 0;
        }
    }

    const char* methodName = luaL_checkstring(L, 2);

    if (lua_isstring(L, 3) && lua_tostring(L, 3)[0] == '(')
    {
        const char* sig = lua_tostring(L, 3);
        JNIEnv* env = getJNIEnv(L);
        jmethodID mid = env->GetStaticMethodID(targetCls, methodName, sig);
        if (!mid)
        {
            checkJniException(L, env);
            if (needDelete) env->DeleteLocalRef(targetCls);
            luaL_error(L, "Static method '%s' with signature '%s' not found on '%s'", methodName, sig, clsName);
            return 0;
        }

        std::vector<jvalue> jargs;
        int argIdx = 4;
        const char* p = sig + 1;
        while (*p && *p != ')')
        {
            jvalue val;
            char typeChar = *p;
            switch (typeChar)
            {
            case 'Z': val.z = lua_toboolean(L, argIdx++) ? JNI_TRUE : JNI_FALSE; break;
            case 'B': val.b = (jbyte)luaL_checkinteger(L, argIdx++); break;
            case 'C': {
                const char* s = luaL_checkstring(L, argIdx++);
                val.c = s ? (jchar)s[0] : 0;
                break;
            }
            case 'S': val.s = (jshort)luaL_checkinteger(L, argIdx++); break;
            case 'I': val.i = (jint)luaL_checkinteger(L, argIdx++); break;
            case 'J': val.j = (jlong)luaL_checknumber(L, argIdx++); break;
            case 'F': val.f = (jfloat)luaL_checknumber(L, argIdx++); break;
            case 'D': val.d = (jdouble)luaL_checknumber(L, argIdx++); break;
            case 'L': {
                val.l = luauToJavaObject(L, env, argIdx++);
                while (*p && *p != ';') ++p;
                break;
            }
            case '[': {
                val.l = luauToJavaObject(L, env, argIdx++);
                while (*p == '[') ++p;
                if (*p == 'L') { while (*p && *p != ';') ++p; }
                break;
            }
            }
            jargs.push_back(val);
            if (*p) ++p;
        }

        const char* retSig = strchr(sig, ')');
        char retType = retSig ? retSig[1] : 'V';

        switch (retType)
        {
        case 'V': {
            env->CallStaticVoidMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            return 0;
        }
        case 'Z': {
            jboolean res = env->CallStaticBooleanMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            lua_pushboolean(L, res ? 1 : 0);
            return 1;
        }
        case 'B': {
            jbyte res = env->CallStaticByteMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            lua_pushinteger(L, res);
            return 1;
        }
        case 'C': {
            jchar res = env->CallStaticCharMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            char buf[2] = { (char)res, 0 };
            lua_pushstring(L, buf);
            return 1;
        }
        case 'S': {
            jshort res = env->CallStaticShortMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            lua_pushinteger(L, res);
            return 1;
        }
        case 'I': {
            jint res = env->CallStaticIntMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            lua_pushinteger(L, res);
            return 1;
        }
        case 'J': {
            jlong res = env->CallStaticLongMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            lua_pushnumber(L, (double)res);
            return 1;
        }
        case 'F': {
            jfloat res = env->CallStaticFloatMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            lua_pushnumber(L, (double)res);
            return 1;
        }
        case 'D': {
            jdouble res = env->CallStaticDoubleMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            lua_pushnumber(L, res);
            return 1;
        }
        default: {
            jobject res = env->CallStaticObjectMethodA(targetCls, mid, jargs.data());
            if (needDelete) env->DeleteLocalRef(targetCls);
            checkJniException(L, env);
            javaToLuauValue(L, env, res);
            if (res) env->DeleteLocalRef(res);
            return 1;
        }
        }
    }

    // Dynamic static overload resolution
    if (needDelete)
    {
        JNIEnv* env = getJNIEnv(L);
        pushJClass(L, env, targetCls, clsName);
        env->DeleteLocalRef(targetCls);
        lua_replace(L, 1);
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, jni_invoke_static_method, nullptr, 2);
    lua_insert(L, 3);
    int totalArgs = lua_gettop(L) - 3;
    lua_call(L, totalArgs, 1);
    return 1;
}

static int jni_with_local_frame(lua_State* L)
{
    int capacity = 64;
    int funcIdx = 1;

    if (lua_isnumber(L, 1))
    {
        capacity = luaL_checkinteger(L, 1);
        funcIdx = 2;
    }
    luaL_checktype(L, funcIdx, LUA_TFUNCTION);

    JNIEnv* env = getJNIEnv(L);
    if (env->PushLocalFrame(capacity) < 0)
    {
        checkJniException(L, env);
        luaL_error(L, "Out of memory in PushLocalFrame with capacity %d", capacity);
        return 0;
    }

    int topBefore = lua_gettop(L);
    lua_pushvalue(L, funcIdx);
    int status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != 0)
    {
        env->PopLocalFrame(nullptr);
        lua_error(L);
        return 0;
    }

    int nresults = lua_gettop(L) - topBefore;
    env->PopLocalFrame(nullptr);
    return nresults;
}

// ---------------------------------------------------------------------------
// Library Registration Table
// ---------------------------------------------------------------------------
static const luaL_Reg jnilib[] = {
    {"init", jni_init},
    {"is_initialized", jni_is_initialized},
    {"isInitialized", jni_is_initialized},
    {"destroy", jni_destroy},
    {"get_version", jni_get_version},
    {"getVersion", jni_get_version},
    {"find_jvm_path", jni_find_jvm_path},
    {"findJvmPath", jni_find_jvm_path},
    {"attach_current_thread", jni_attach_current_thread},
    {"attachCurrentThread", jni_attach_current_thread},
    {"detach_current_thread", jni_detach_current_thread},
    {"detachCurrentThread", jni_detach_current_thread},
    {"with_local_frame", jni_with_local_frame},
    {"withLocalFrame", jni_with_local_frame},
    {"local_frame", jni_with_local_frame},
    {"localFrame", jni_with_local_frame},
    {"find_class", jni_find_class},
    {"findClass", jni_find_class},
    {"class", jni_find_class},
    {"import", jni_find_class},
    {"new", jni_new},
    {"array", jni_array},
    {"wrap_buffer", jni_wrap_buffer},
    {"wrapBuffer", jni_wrap_buffer},
    {"to_java", jni_to_java},
    {"toJava", jni_to_java},
    {"to_luau", jni_to_luau},
    {"toLuau", jni_to_luau},
    {"instanceof", jni_instanceof},
    {"instanceOf", jni_instanceof},
    {"cast", jni_cast},
    {"call_method", jni_call_method},
    {"callMethod", jni_call_method},
    {"call_static", jni_call_static},
    {"callStatic", jni_call_static},
    {"jboolean", jni_jboolean},
    {"jbyte", jni_jbyte},
    {"jchar", jni_jchar},
    {"jshort", jni_jshort},
    {"jint", jni_jint},
    {"jlong", jni_jlong},
    {"jfloat", jni_jfloat},
    {"jdouble", jni_jdouble},
    {"jstring", jni_jstring},
    {nullptr, nullptr}
};

} // namespace

LUALIB_API int luaopen_jni(lua_State* L)
{
    // Metatable: JObject
    luaL_newmetatable(L, JNI_OBJECT_MT);
    lua_pushstring(L, JNI_OBJECT_MT);
    lua_setfield(L, -2, "__name");
    lua_pushcfunction(L, jni_object_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, jni_object_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, jni_object_eq, "__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, jni_object_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, jni_object_newindex, "__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);

    // Metatable: JClass
    luaL_newmetatable(L, JNI_CLASS_MT);
    lua_pushstring(L, JNI_CLASS_MT);
    lua_setfield(L, -2, "__name");
    lua_pushcfunction(L, jni_class_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, jni_class_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, jni_invoke_constructor, "__call");
    lua_setfield(L, -2, "__call");
    lua_pushcfunction(L, jni_class_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, jni_class_newindex, "__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);

    // Metatable: JArray
    luaL_newmetatable(L, JNI_ARRAY_MT);
    lua_pushstring(L, JNI_ARRAY_MT);
    lua_setfield(L, -2, "__name");
    lua_pushcfunction(L, jni_array_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, jni_array_len, "__len");
    lua_setfield(L, -2, "__len");
    lua_pushcfunction(L, jni_array_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, jni_array_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, jni_array_newindex, "__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);

    // Metatable: JTypedValue
    luaL_newmetatable(L, JNI_TYPED_VALUE_MT);
    lua_pushstring(L, JNI_TYPED_VALUE_MT);
    lua_setfield(L, -2, "__name");
    lua_pushcfunction(L, jni_typed_value_gc, "__gc");
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, jni_typed_value_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    luaL_register(L, LUA_JNILIBNAME, jnilib);

    lua_pushlightuserdata(L, nullptr);
    lua_setfield(L, -2, "null");

    return 1;
}
