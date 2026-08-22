// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Repl.h"

#include "Luau/CodeGenOptions.h"
#include "Luau/Common.h"
#include "lua.h"
#include "lualib.h"

#include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/Parser.h"
#include "Luau/TimeTrace.h"
#include "Luau/Counters.h"
#include "Luau/Coverage.h"
#include "Luau/FileUtils.h"
#include "Luau/Flags.h"
#include "Luau/JitInliner.h"
#include "Luau/LspServer.h"
#include "Luau/Profiler.h"
#include "Luau/ReplRequirer.h"
#include "Luau/Require.h"
#include "Luau/SingleBinaryCompiler.h"

#include "isocline.h"

#include <memory>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

namespace Color
{
static bool enabled()
{
    static int isColor = -1;
    if (isColor == -1)
    {
        const char* noColor = getenv("NO_COLOR");
        if (noColor && *noColor)
            isColor = 0;
        else
        {
#if !defined(_WIN32)
            isColor = isatty(fileno(stderr)) ? 1 : 0;
#else
            isColor = 1;
#endif
        }
    }
    return isColor == 1;
}

static const char* red() { return enabled() ? "\033[1;31m" : ""; }
static const char* green() { return enabled() ? "\033[1;32m" : ""; }
static const char* yellow() { return enabled() ? "\033[1;33m" : ""; }
static const char* cyan() { return enabled() ? "\033[1;36m" : ""; }
static const char* bold() { return enabled() ? "\033[1m" : ""; }
static const char* dim() { return enabled() ? "\033[2m" : ""; }
static const char* reset() { return enabled() ? "\033[0m" : ""; }
} // namespace Color

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef __linux__
#include <unistd.h>
#endif

#ifdef CALLGRIND
#include <valgrind/callgrind.h>
#endif

#include <locale.h>
#include <signal.h>

LUAU_FASTFLAG(DebugLuauTimeTracing)

constexpr int MaxTraversalLimit = 50;

static bool codegen = false;
static bool codegenCold = false;
static bool jitInliner = false;
static int program_argc = 0;
char** program_argv = nullptr;

// Ctrl-C handling
static void sigintCallback(lua_State* L, int gc)
{
    if (gc >= 0)
        return;

    lua_callbacks(L)->interrupt = NULL;

    lua_rawcheckstack(L, 1); // reserve space for error string
    luaL_error(L, "Execution interrupted");
}

static lua_State* replState = NULL;

#ifdef _WIN32
BOOL WINAPI sigintHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT && replState)
        lua_callbacks(replState)->interrupt = &sigintCallback;
    return TRUE;
}
#else
static void sigintHandler(int signum)
{
    if (signum == SIGINT && replState)
        lua_callbacks(replState)->interrupt = &sigintCallback;
}
#endif

struct GlobalOptions
{
    int optimizationLevel = 1;
    int debugLevel = 1;
} globalOptions;

static Luau::CompileOptions copts()
{
    Luau::CompileOptions result = {};
    result.optimizationLevel = globalOptions.optimizationLevel;
    result.debugLevel = globalOptions.debugLevel;
    result.typeInfoLevel = 1;
    result.coverageLevel = coverageActive() ? 2 : 0;

    return result;
}

static int lua_loadstring(lua_State* L)
{
    size_t l = 0;
    const char* s = luaL_checklstring(L, 1, &l);
    const char* chunkname = luaL_optstring(L, 2, s);

    lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

    std::string bytecode = Luau::compile(std::string(s, l), copts());
    if (luau_load(L, chunkname, bytecode.data(), bytecode.size(), 0) == 0)
        return 1;

    lua_pushnil(L);
    lua_insert(L, -2); // put before error message
    return 2;          // return nil plus error message
}

static int lua_collectgarbage(lua_State* L)
{
    const char* option = luaL_optstring(L, 1, "collect");

    if (strcmp(option, "collect") == 0)
    {
        lua_gc(L, LUA_GCCOLLECT, 0);
        return 0;
    }

    if (strcmp(option, "count") == 0)
    {
        int c = lua_gc(L, LUA_GCCOUNT, 0);
        lua_pushnumber(L, c);
        return 1;
    }

    luaL_error(L, "collectgarbage must be called with 'count' or 'collect'");
}

#ifdef CALLGRIND
static int lua_callgrind(lua_State* L)
{
    const char* option = luaL_checkstring(L, 1);

    if (strcmp(option, "running") == 0)
    {
        int r = RUNNING_ON_VALGRIND;
        lua_pushboolean(L, r);
        return 1;
    }

    if (strcmp(option, "zero") == 0)
    {
        CALLGRIND_ZERO_STATS;
        return 0;
    }

    if (strcmp(option, "dump") == 0)
    {
        const char* name = luaL_checkstring(L, 2);

        CALLGRIND_DUMP_STATS_AT(name);
        return 0;
    }

    luaL_error(L, "callgrind must be called with one of 'running', 'zero', 'dump'");
}
#endif

void* createCliRequireContext(lua_State* L)
{
    void* ctx = lua_newuserdatadtor(
        L,
        sizeof(ReplRequirer),
        [](void* ptr)
        {
            static_cast<ReplRequirer*>(ptr)->~ReplRequirer();
        }
    );

    if (!ctx)
        luaL_error(L, "unable to allocate ReplRequirer");

    ctx = new (ctx) ReplRequirer{
        copts,
        coverageActive,
        []()
        {
            return codegen;
        },
        coverageTrack,
        countersActive,
        countersTrack
    };

    // Store ReplRequirer in the registry to keep it alive for the lifetime of
    // this lua_State. Memory address is used as a key to avoid collisions.
    lua_pushlightuserdata(L, ctx);
    lua_insert(L, -2);
    lua_settable(L, LUA_REGISTRYINDEX);

    return ctx;
}

static std::string getFilePath(const char* name);

static int lua_loadfile(lua_State* L)
{
    const char* filename = luaL_checkstring(L, 1);
    const char* chunkname = luaL_optstring(L, 2, filename);

    std::string path = getFilePath(filename);
    std::optional<std::string> source = readFile(path.empty() ? filename : path);
    if (!source)
    {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open %s", filename);
        return 2;
    }

    lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

    std::string bytecode = Luau::compile(*source, copts());
    if (luau_load(L, chunkname, bytecode.data(), bytecode.size(), 0) == 0)
        return 1;

    lua_pushnil(L);
    lua_insert(L, -2); // put before error message
    return 2;          // return nil plus error message
}

static int lua_dofile(lua_State* L)
{
    const char* filename = luaL_optstring(L, 1, NULL);
    std::optional<std::string> source;
    std::string chunkname;

    if (filename)
    {
        std::string path = getFilePath(filename);
        source = readFile(path.empty() ? filename : path);
        if (!source)
            luaL_error(L, "cannot open %s", filename);
        chunkname = "@" + normalizePath(filename);
    }
    else
    {
        source = readStdin();
        if (!source)
            luaL_error(L, "cannot read stdin");
        chunkname = "=stdin";
    }

    lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

    std::string bytecode = Luau::compile(*source, copts());
    if (luau_load(L, chunkname.c_str(), bytecode.data(), bytecode.size(), 0) != 0)
        lua_error(L);

    if (codegen)
    {
        Luau::CodeGen::CompilationOptions nativeOptions;
        if (codegenCold)
            nativeOptions.flags = Luau::CodeGen::CodeGen_ColdFunctions;
        if (countersActive())
            nativeOptions.recordCounters = true;
        Luau::CodeGen::compile(L, -1, nativeOptions);
    }

    int n = lua_gettop(L) - 1;
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - n;
}

void setupState(lua_State* L)
{
    if (codegen)
        Luau::CodeGen::create(L);

    if (jitInliner)
        Luau::JitInliner::setup(L);

    luaL_openlibs(L);

    static const luaL_Reg funcs[] = {
        {"loadstring", lua_loadstring},
        {"loadfile", lua_loadfile},
        {"dofile", lua_dofile},
        {"collectgarbage", lua_collectgarbage},
#ifdef CALLGRIND
        {"callgrind", lua_callgrind},
#endif
        {NULL, NULL},
    };

    lua_pushvalue(L, LUA_GLOBALSINDEX);
    luaL_register(L, NULL, funcs);
    lua_pop(L, 1);

    luaopen_require(L, requireConfigInit, createCliRequireContext(L));
}

void setupArguments(lua_State* L, int argc, char** argv)
{
    for (int i = 0; i < argc; ++i)
        lua_pushstring(L, argv[i]);
}

std::string runCode(lua_State* L, const std::string& source)
{
    std::string bytecode = Luau::compile(source, copts());

    if (luau_load(L, "=stdin", bytecode.data(), bytecode.size(), 0) != 0)
    {
        size_t len;
        const char* msg = lua_tolstring(L, -1, &len);

        std::string error(msg, len);
        lua_pop(L, 1);

        return error;
    }

    lua_State* T = lua_newthread(L);

    lua_pushvalue(L, -2);
    lua_remove(L, -3);
    lua_xmove(L, T, 1);

    int status = lua_resume(T, NULL, 0);

    if (status == 0)
    {
        int n = lua_gettop(T);

        if (n)
        {
            luaL_checkstack(T, LUA_MINSTACK, "too many results to print");
            lua_getglobal(T, "_PRETTYPRINT");
            // If _PRETTYPRINT is nil, then use the standard print function instead
            if (lua_isnil(T, -1))
            {
                lua_pop(T, 1);
                lua_getglobal(T, "print");
            }
            lua_insert(T, 1);
            lua_pcall(T, n, 0, 0);
        }

        lua_pop(L, 1);
        return std::string();
    }
    else
    {
        std::string error;

        if (status == LUA_YIELD)
        {
            error = "thread yielded unexpectedly";
        }
        else if (const char* str = lua_tostring(T, -1))
        {
            error = str;
        }

        error += "\nstack backtrace:\n";
        error += lua_debugtrace(T);

        lua_pop(L, 1);
        return error;
    }
}

// Replaces the top of the lua stack with the metatable __index for the value
// if it exists.  Returns true iff __index exists.
static bool tryReplaceTopWithIndex(lua_State* L)
{
    if (luaL_getmetafield(L, -1, "__index"))
    {
        // Remove the table leaving __index on the top of stack
        lua_remove(L, -2);
        return true;
    }
    return false;
}


// This function is similar to lua_gettable, but it avoids calling any
// lua callback functions (e.g. __index) which might modify the Lua VM state.
static void safeGetTable(lua_State* L, int tableIndex)
{
    lua_pushvalue(L, tableIndex); // Duplicate the table

    // The loop invariant is that the table to search is at -1
    // and the key is at -2.
    for (int loopCount = 0;; loopCount++)
    {
        lua_pushvalue(L, -2); // Duplicate the key
        lua_rawget(L, -2);    // Try to find the key
        if (!lua_isnil(L, -1) || loopCount >= MaxTraversalLimit)
        {
            // Either the key has been found, and/or we have reached the max traversal limit
            break;
        }
        else
        {
            lua_pop(L, 1); // Pop the nil result
            if (!luaL_getmetafield(L, -1, "__index"))
            {
                lua_pushnil(L);
                break;
            }
            else if (lua_istable(L, -1))
            {
                // Replace the current table being searched with __index table
                lua_replace(L, -2);
            }
            else
            {
                lua_pop(L, 1); // Pop the value
                lua_pushnil(L);
                break;
            }
        }
    }

    lua_remove(L, -2); // Remove the table
    lua_remove(L, -2); // Remove the original key
}

// completePartialMatches finds keys that match the specified 'prefix'
// Note: the table/object to be searched must be on the top of the Lua stack
static void completePartialMatches(
    lua_State* L,
    bool completeOnlyFunctions,
    const std::string& editBuffer,
    std::string_view prefix,
    const AddCompletionCallback& addCompletionCallback
)
{
    for (int i = 0; i < MaxTraversalLimit && lua_istable(L, -1); i++)
    {
        // table, key
        lua_pushnil(L);

        // Loop over all the keys in the current table
        while (lua_next(L, -2) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                // table, key, value
                std::string_view key = lua_tostring(L, -2);
                int valueType = lua_type(L, -1);

                // If the last separator was a ':' (i.e. a method call) then only functions should be completed.
                bool requiredValueType = (!completeOnlyFunctions || valueType == LUA_TFUNCTION);

                if (!key.empty() && requiredValueType && Luau::startsWith(key, prefix))
                {
                    std::string completedComponent(key.substr(prefix.size()));
                    std::string completion(editBuffer + completedComponent);
                    if (valueType == LUA_TFUNCTION)
                    {
                        // Add an opening paren for function calls by default.
                        completion += "(";
                    }
                    addCompletionCallback(completion, std::string(key));
                }
            }
            lua_pop(L, 1);
        }

        // Replace the current table being searched with an __index table if one exists
        if (!tryReplaceTopWithIndex(L))
        {
            break;
        }
    }
}

static void completeIndexer(lua_State* L, const std::string& editBuffer, const AddCompletionCallback& addCompletionCallback)
{
    std::string_view lookup = editBuffer;
    bool completeOnlyFunctions = false;

    // Push the global variable table to begin the search
    lua_pushvalue(L, LUA_GLOBALSINDEX);

    for (;;)
    {
        size_t sep = lookup.find_first_of(".:");
        std::string_view prefix = lookup.substr(0, sep);

        if (sep == std::string_view::npos)
        {
            completePartialMatches(L, completeOnlyFunctions, editBuffer, prefix, addCompletionCallback);
            break;
        }
        else
        {
            // find the key in the table
            lua_pushlstring(L, prefix.data(), prefix.size());
            safeGetTable(L, -2);
            lua_remove(L, -2);

            if (lua_istable(L, -1) || tryReplaceTopWithIndex(L))
            {
                completeOnlyFunctions = lookup[sep] == ':';
                lookup.remove_prefix(sep + 1);
            }
            else
            {
                // Unable to search for keys, so stop searching
                break;
            }
        }
    }

    lua_pop(L, 1);
}

void getCompletions(lua_State* L, const std::string& editBuffer, const AddCompletionCallback& addCompletionCallback)
{
    completeIndexer(L, editBuffer, addCompletionCallback);
}

static void icGetCompletions(ic_completion_env_t* cenv, const char* editBuffer)
{
    auto* L = reinterpret_cast<lua_State*>(ic_completion_arg(cenv));

    getCompletions(
        L,
        std::string(editBuffer),
        [cenv](const std::string& completion, const std::string& display)
        {
            ic_add_completion_ex(cenv, completion.data(), display.data(), nullptr);
        }
    );
}

static bool isMethodOrFunctionChar(const char* s, long len)
{
    char c = *s;
    return len == 1 && (isalnum(c) || c == '.' || c == ':' || c == '_');
}

static void completeRepl(ic_completion_env_t* cenv, const char* editBuffer)
{
    ic_complete_word(cenv, editBuffer, icGetCompletions, isMethodOrFunctionChar);
}

static void loadHistory(const char* name)
{
    std::string path;

    if (const char* home = getenv("HOME"))
    {
        path = joinPaths(home, name);
    }
    else if (const char* userProfile = getenv("USERPROFILE"))
    {
        path = joinPaths(userProfile, name);
    }

    if (!path.empty())
        ic_set_history(path.c_str(), -1 /* default entries (= 200) */);
}

static void runReplImpl(lua_State* L)
{
    ic_set_default_completer(completeRepl, L);

    // Reset the locale to C
    setlocale(LC_ALL, "C");

    // Make brace matching easier to see
    ic_style_def("ic-bracematch", "teal");

    // Prevent auto insertion of braces
    ic_enable_brace_insertion(false);

    // Loads history from the given file; isocline automatically saves the history on process exit
    loadHistory(".luau_history");

    std::string buffer;

    for (;;)
    {
        const char* prompt = buffer.empty() ? "" : ">";
        std::unique_ptr<char, void (*)(void*)> line(ic_readline(prompt), free);
        if (!line)
            break;

        if (buffer.empty() && runCode(L, std::string("return ") + line.get()) == std::string())
        {
            ic_history_add(line.get());
            continue;
        }

        if (!buffer.empty())
            buffer += "\n";
        buffer += line.get();

        std::string error = runCode(L, buffer);

        if (error.length() >= 5 && error.compare(error.length() - 5, 5, "<eof>") == 0)
        {
            continue;
        }

        if (error.length())
        {
            if (Color::enabled())
                fprintf(stdout, "%s%s%s\n", Color::red(), error.c_str(), Color::reset());
            else
                fprintf(stdout, "%s\n", error.c_str());
        }

        ic_history_add(buffer.c_str());
        buffer.clear();
    }
}

static void runRepl()
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();

    setupState(L);

    // setup Ctrl+C handling
    replState = L;
#ifdef _WIN32
    SetConsoleCtrlHandler(sigintHandler, TRUE);
#else
    signal(SIGINT, sigintHandler);
#endif

    luaL_sandboxthread(L);
    runReplImpl(L);
}

static std::string getFilePath(const char* name)
{
    if (isFile(name))
        return name;

    std::string base = name;

    std::string luauPath = base + ".luau";
    if (isFile(luauPath))
        return luauPath;

    std::string luaPath = base + ".lua";
    if (isFile(luaPath))
        return luaPath;

    return "";
}

static bool runCode(const char* code, const char* chunkname, lua_State* GL, bool repl)
{
    // module needs to run in a new thread, isolated from the rest
    lua_State* L = lua_newthread(GL);

    // new thread needs to have the globals sandboxed
    luaL_sandboxthread(L);

    std::string bytecode = Luau::compile(code, copts());
    int status = 0;

    if (luau_load(L, chunkname, bytecode.data(), bytecode.size(), 0) == 0)
    {
        if (codegen)
        {
            Luau::CodeGen::CompilationOptions nativeOptions;
            if (codegenCold)
            {
                nativeOptions.flags = Luau::CodeGen::CodeGen_ColdFunctions;
            }

            if (countersActive())
                nativeOptions.recordCounters = true;

            Luau::CodeGen::compile(L, -1, nativeOptions);
        }

        if (coverageActive())
            coverageTrack(L, -1);

        if (countersActive())
            countersTrack(L, -1);

        setupArguments(L, program_argc, program_argv);
        status = lua_resume(L, NULL, program_argc);

        if (status == 0 || status == LUA_YIELD)
        {
            luaL_runtasks(GL);
            int curStatus = lua_status(L);
            if (status == LUA_YIELD && (curStatus == LUA_OK || curStatus == LUA_YIELD))
                status = 0;
        }
    }
    else
    {
        status = LUA_ERRSYNTAX;
    }

    if (status != 0)
    {
        std::string error;

        if (status == LUA_YIELD)
        {
            error = "thread yielded unexpectedly";
        }
        else if (const char* str = lua_tostring(L, -1))
        {
            error = str;
        }

        error += "\nstacktrace:\n";
        error += lua_debugtrace(L);

        if (Color::enabled())
            fprintf(stderr, "%s%s%s", Color::red(), error.c_str(), Color::reset());
        else
            fprintf(stderr, "%s", error.c_str());
    }

    if (repl)
        runReplImpl(L);

    return status == 0;
}

// `repl` is used it indicate if a repl should be started after executing the file.
static bool runFile(const char* name, lua_State* GL, bool repl)
{
    std::optional<std::string> source = readFile(getFilePath(name));
    if (!source)
    {
        fprintf(stderr, "Error opening %s\n", name);
        return false;
    }

    std::string chunkname = "@" + normalizePath(name);
    return runCode(source->c_str(), chunkname.c_str(), GL, repl);
}

static void displayHelp(const char* argv0)
{
    printf("Usage: %s [options] [file list] [-a] [arg list]\n", argv0);
    printf("\n");
    printf("When file list is omitted, an interactive REPL is started instead.\n");
    printf("\n");
    printf("Available options:\n");
    printf("  --coverage: collect code coverage while running the code and output results to coverage.out\n");
    printf("  --counters: collect native counters data while running the code and output results to callgrind.out\n");
    printf("  -h, --help: Display this usage message.\n");
    printf("  -i, --interactive: Run an interactive REPL after executing the last script specified.\n");
    printf("  -e, --eval <code>: Execute string code directly in Luau runtime\n");
    printf("  -O<n>: compile with optimization level n (default 1, n should be between 0 and 2).\n");
    printf("  -g<n>: compile with debug level n (default 1, n should be between 0 and 2).\n");
    printf("  --profile[=N]: profile the code using N Hz sampling (default 10000) and output results to profile.out\n");
    printf("  --timetrace: record compiler time tracing information into trace.json\n");
    printf("  --codegen: execute code using native code generation\n");
    printf("  --codegen-cold: execute code using native code generation, including any functions deemed not profitable to natively compile\n");
    printf("  --codegen-perf: execute code using native code generation and profile using perf (only on Linux)\n");
    printf("  --program-args,-a: declare start of arguments to be passed to the Luau program\n");
    printf("  --fflags=<flags>: comma-separated list of fast flags to enable/disable (--fflags=true,false,LuauFlag1=true,LuauFlag2=false).\n");
    printf("  --jit-inliner: enable JIT bytecode inliner\n");
    printf("  --lsp: start Language Server Protocol (LSP) mode over stdio\n");
    printf("  --build, --bundle, -b: compile entry file and transitive modules into standalone executable binary\n");
    printf("  --target=<arch>: specify target architecture or toolchain for --build/--bundle (e.g. linux-x64, linux-arm64, windows-x64, macos-arm64)\n");
    printf("  --include-assets=<path>: embed directory or file into single binary virtual filesystem\n");
    printf("  -o, --output=<file>: specify output binary path for --build/--bundle\n");
    printf("  -v, --verbose: enable verbose compiler output\n");
}

static int assertionHandler(const char* expr, const char* file, int line, const char* function)
{
    printf("%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
    return 1;
}

int replMain(int argc, char** argv)
{
    Luau::assertHandler() = assertionHandler;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    int profile = 0;
    bool coverage = false;
    bool interactive = false;
    bool codegenPerf = false;
    bool counters = false;
    bool buildMode = false;
    std::string outputFile;
    std::string targetArchitecture;
    std::vector<std::string> assetPaths;
    std::string evalCode;
    bool hasEval = false;
    bool verbose = false;
    std::vector<std::string> inputFiles;
    int program_args = argc;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            displayHelp(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
        {
            interactive = true;
        }
        else if (strncmp(argv[i], "-O", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
            {
                fprintf(stderr, "Error: Optimization level must be between 0 and 2 inclusive.\n");
                return 1;
            }
            globalOptions.optimizationLevel = level;
        }
        else if (strncmp(argv[i], "-g", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
            {
                fprintf(stderr, "Error: Debug level must be between 0 and 2 inclusive.\n");
                return 1;
            }
            globalOptions.debugLevel = level;
        }
        else if (strcmp(argv[i], "--profile") == 0)
        {
            profile = 10000; // default to 10 KHz
        }
        else if (strncmp(argv[i], "--profile=", 10) == 0)
        {
            profile = atoi(argv[i] + 10);
        }
        else if (strcmp(argv[i], "--codegen") == 0)
        {
            codegen = true;
        }
        else if (strcmp(argv[i], "--codegen-cold") == 0)
        {
            codegen = true;
            codegenCold = true;
        }
        else if (strcmp(argv[i], "--codegen-perf") == 0)
        {
            codegen = true;
            codegenPerf = true;
        }
        else if (strcmp(argv[i], "--coverage") == 0)
        {
            coverage = true;
        }
        else if (strcmp(argv[i], "--counters") == 0)
        {
            counters = true;
        }
        else if (strcmp(argv[i], "--timetrace") == 0)
        {
            FFlag::DebugLuauTimeTracing.value = true;
        }
        else if (strcmp(argv[i], "--jit-inliner") == 0)
        {
            jitInliner = true;
        }
        else if (strcmp(argv[i], "--lsp") == 0)
        {
            return Luau::runLspServer();
        }
        else if (strncmp(argv[i], "--fflags=", 9) == 0)
        {
            setLuauFlags(argv[i] + 9);
        }
        else if (strcmp(argv[i], "--program-args") == 0 || strcmp(argv[i], "-a") == 0)
        {
            program_args = i + 1;
            break;
        }
        else if (strcmp(argv[i], "--build") == 0 || strcmp(argv[i], "--bundle") == 0 || strcmp(argv[i], "-b") == 0)
        {
            buildMode = true;
        }
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
        {
            targetArchitecture = argv[++i];
        }
        else if (strncmp(argv[i], "--target=", 9) == 0)
        {
            targetArchitecture = argv[i] + 9;
        }
        else if (strcmp(argv[i], "--include-assets") == 0 && i + 1 < argc)
        {
            assetPaths.push_back(argv[++i]);
        }
        else if (strncmp(argv[i], "--include-assets=", 17) == 0)
        {
            assetPaths.push_back(argv[i] + 17);
        }
        else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc)
        {
            evalCode = argv[++i];
            hasEval = true;
        }
        else if (strncmp(argv[i], "-e", 2) == 0 && argv[i][2] != '\0')
        {
            evalCode = argv[i] + 2;
            hasEval = true;
        }
        else if (strcmp(argv[i], "--eval") == 0 && i + 1 < argc)
        {
            evalCode = argv[++i];
            hasEval = true;
        }
        else if (strncmp(argv[i], "--eval=", 7) == 0)
        {
            evalCode = argv[i] + 7;
            hasEval = true;
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
            outputFile = argv[++i];
        }
        else if (strncmp(argv[i], "-o", 2) == 0 && argv[i][2] != '\0')
        {
            outputFile = argv[i] + 2;
        }
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
        {
            outputFile = argv[++i];
        }
        else if (strncmp(argv[i], "--output=", 9) == 0)
        {
            outputFile = argv[i] + 9;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
        {
            verbose = true;
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "Error: Unrecognized option '%s'.\n\n", argv[i]);
            displayHelp(argv[0]);
            return 1;
        }
        else
        {
            inputFiles.push_back(argv[i]);
        }
    }

    if (buildMode)
    {
        if (inputFiles.empty())
        {
            fprintf(stderr, "Error: No entry file specified for --build/--bundle.\n");
            return 1;
        }

        Luau::SingleBinaryOptions opts;
        opts.entryFilePath = inputFiles[0];
        opts.outputBinaryPath = outputFile.empty() ? "a.out" : outputFile;
        opts.targetArchitecture = targetArchitecture;
        opts.assetPaths = assetPaths;
        opts.optimizationLevel = globalOptions.optimizationLevel;
        opts.debugLevel = globalOptions.debugLevel;
        opts.codegen = codegen;
        opts.verbose = verbose;

        return Luau::SingleBinaryCompiler::compile(opts) ? 0 : 1;
    }

    program_argc = argc - program_args;
    program_argv = &argv[program_args];


#if !defined(LUAU_ENABLE_TIME_TRACE)
    if (FFlag::DebugLuauTimeTracing)
    {
        fprintf(stderr, "To run with --timetrace, Luau has to be built with LUAU_ENABLE_TIME_TRACE enabled\n");
        return 1;
    }
#endif

    if (codegenPerf)
    {
#if __linux__
        char path[128];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", getpid());

        // note, there's no need to close the log explicitly as it will be closed when the process exits
        FILE* codegenPerfLog = fopen(path, "w");

        Luau::CodeGen::setPerfLog(
            codegenPerfLog,
            [](void* context, uintptr_t addr, unsigned size, const char* symbol)
            {
                FILE* outputFile = static_cast<FILE*>(context);
                fprintf(outputFile, "%016lx %08x %s\n", long(addr), size, symbol);
                fflush(outputFile);
            }
        );
#else
        fprintf(stderr, "--codegen-perf option is only supported on Linux\n");
        return 1;
#endif
    }

    std::vector<std::string> files;
    for (const std::string& input : inputFiles)
    {
        if (input == "-")
        {
            files.push_back("-");
        }
        else
        {
            std::string normalized = normalizePath(input);
            if (isDirectory(normalized))
            {
                traverseDirectory(
                    normalized,
                    [&](const std::string& name)
                    {
                        if (hasFileExtension(name, {".lua", ".luau"}))
                            files.push_back(name);
                    }
                );
            }
            else
            {
                files.push_back(normalized);
            }
        }
    }

    if (files.empty() && !hasEval)
    {
        runRepl();
        return 0;
    }
    else
    {
        std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
        lua_State* L = globalState.get();

        setupState(L);

        if (profile)
            profilerStart(L, profile);

        if (coverage)
            coverageInit(L);

        if (counters)
            countersInit(L);

        int failed = 0;

        if (hasEval)
        {
            failed += !runCode(evalCode.c_str(), "=eval", L, interactive && files.empty());
        }

        for (size_t i = 0; i < files.size(); ++i)
        {
            bool isLastFile = i == files.size() - 1;
            failed += !runFile(files[i].c_str(), L, interactive && isLastFile);
        }

        if (profile)
        {
            profilerStop();
            profilerDump("profile.out");
        }

        if (coverage)
            coverageDump("coverage.out");

        if (counters)
            countersDump("callgrind.out");

        return failed ? 1 : 0;
    }
}
