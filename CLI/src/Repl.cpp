// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Repl.h"

#include "Luau/CodeGenOptions.h"
#include "Luau/CliPresentation.h"
#include "Luau/Common.h"
#include "Luau/Allocator.h"
#include "Luau/Lexer.h"
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
#include "JaciLogoData.h"

#include <array>
#include <memory>
#include <climits>
#include <string>
#include <string_view>
#include <unordered_set>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

namespace Color
{
static bool enabled()
{
    return Luau::Cli::colorEnabled(stderr);
}

static const char* red()
{
    return enabled() ? "\033[1;31m" : "";
}
static const char* green()
{
    return enabled() ? "\033[1;32m" : "";
}
static const char* yellow()
{
    return enabled() ? "\033[1;33m" : "";
}
static const char* cyan()
{
    return enabled() ? "\033[1;36m" : "";
}
static const char* bold()
{
    return enabled() ? "\033[1m" : "";
}
static const char* dim()
{
    return enabled() ? "\033[2m" : "";
}
static const char* reset()
{
    return enabled() ? "\033[0m" : "";
}
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

void setReplDebugLevel(int level)
{
    globalOptions.debugLevel = level;
}

int getReplDebugLevel()
{
    return globalOptions.debugLevel;
}

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

    std::string formattedChunkname;
    if (chunkname && chunkname[0] != '@' && chunkname[0] != '=')
        formattedChunkname = "@" + normalizePath(chunkname);
    else
        formattedChunkname = chunkname ? chunkname : ("@" + normalizePath(filename));

    std::string bytecode = Luau::compile(*source, copts());
    if (luau_load(L, formattedChunkname.c_str(), bytecode.data(), bytecode.size(), 0) == 0)
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

    lua_getglobal(L, "process");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, "args");
        if (lua_istable(L, -1))
        {
            for (int i = 0; i < argc; ++i)
            {
                lua_pushstring(L, argv[i]);
                lua_rawseti(L, -2, i + 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static std::string runCodeInternal(lua_State* L, const std::string& source, bool rememberResult)
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
            if (rememberResult)
            {
                lua_pushvalue(T, 1);
                lua_setglobal(T, "ans");
            }

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

std::string runCode(lua_State* L, const std::string& source)
{
    return runCodeInternal(L, source, false);
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

static size_t sourceOffset(const std::vector<size_t>& lineOffsets, const Luau::Position& position, size_t sourceSize)
{
    if (position.line >= lineOffsets.size())
        return sourceSize;

    return std::min(lineOffsets[position.line] + position.column, sourceSize);
}

static bool isOperator(Luau::Lexeme::Type type)
{
    if (type >= Luau::Lexeme::Equal && type <= Luau::Lexeme::DoubleColon)
        return true;
    if (type >= Luau::Lexeme::AddAssign && type <= Luau::Lexeme::ConcatAssign)
        return true;

    return type < Luau::Lexeme::Char_END && strchr("+-*/%^#=<>~", char(type));
}

std::vector<ReplHighlightSpan> getReplHighlightSpans(const std::string& source)
{
    static const std::unordered_set<std::string_view> builtins = {
        "assert",       "collectgarbage", "dofile",   "error",    "getmetatable", "ipairs", "loadfile", "loadstring", "next",
        "pairs",        "pcall",          "print",    "rawequal", "rawget",       "rawlen", "rawset",   "require",    "select",
        "setmetatable", "tonumber",       "tostring", "type",     "typeof",       "unpack", "xpcall",
    };
    static const std::unordered_set<std::string_view> builtinTypes = {
        "any",
        "boolean",
        "buffer",
        "function",
        "never",
        "nil",
        "number",
        "string",
        "table",
        "thread",
        "unknown",
        "userdata",
        "vector",
    };

    std::vector<size_t> lineOffsets = {0};
    for (size_t i = 0; i < source.size(); ++i)
        if (source[i] == '\n')
            lineOffsets.push_back(i + 1);

    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::Lexer lexer(source.data(), source.size(), names);
    lexer.setSkipComments(false);

    std::vector<ReplHighlightSpan> spans;
    bool functionName = false;

    for (;;)
    {
        const Luau::Lexeme& token = lexer.next();
        if (token.type == Luau::Lexeme::Eof)
            break;

        size_t start = sourceOffset(lineOffsets, token.location.begin, source.size());
        size_t end = sourceOffset(lineOffsets, token.location.end, source.size());
        if (end <= start)
            continue;

        std::optional<ReplHighlightKind> kind;

        if (token.type >= Luau::Lexeme::Reserved_BEGIN && token.type < Luau::Lexeme::Reserved_END)
        {
            kind = ReplHighlightKind::Keyword;
            if (token.type == Luau::Lexeme::ReservedFunction)
                functionName = true;
        }
        else if (
            token.type == Luau::Lexeme::RawString || token.type == Luau::Lexeme::QuotedString || token.type == Luau::Lexeme::InterpStringBegin ||
            token.type == Luau::Lexeme::InterpStringMid || token.type == Luau::Lexeme::InterpStringEnd ||
            token.type == Luau::Lexeme::InterpStringSimple
        )
            kind = ReplHighlightKind::String;
        else if (token.type == Luau::Lexeme::Number)
            kind = ReplHighlightKind::Number;
        else if (token.type == Luau::Lexeme::Comment || token.type == Luau::Lexeme::BlockComment)
            kind = ReplHighlightKind::Comment;
        else if (token.type == Luau::Lexeme::Attribute || token.type == Luau::Lexeme::AttributeOpen)
            kind = ReplHighlightKind::Attribute;
        else if (
            token.type == Luau::Lexeme::BrokenString || token.type == Luau::Lexeme::BrokenComment || token.type == Luau::Lexeme::BrokenUnicode ||
            token.type == Luau::Lexeme::BrokenInterpDoubleBrace || token.type == Luau::Lexeme::Error
        )
            kind = ReplHighlightKind::Error;
        else if (isOperator(token.type))
            kind = ReplHighlightKind::Operator;
        else if (token.type == Luau::Lexeme::Name)
        {
            std::string_view name(source.data() + start, end - start);
            size_t next = end;
            while (next < source.size() && isspace(static_cast<unsigned char>(source[next])))
                ++next;

            size_t previous = start;
            while (previous > 0 && isspace(static_cast<unsigned char>(source[previous - 1])))
                --previous;
            bool typePosition = previous > 0 && source[previous - 1] == ':';

            if (functionName || (next < source.size() && source[next] == '('))
                kind = ReplHighlightKind::Function;
            else if (builtins.count(name))
                kind = ReplHighlightKind::Builtin;
            else if (typePosition || builtinTypes.count(name))
                kind = ReplHighlightKind::Type;

            if (functionName && (next >= source.size() || (source[next] != '.' && source[next] != ':')))
                functionName = false;
        }

        if (kind)
            spans.push_back({start, end - start, *kind});
    }

    return spans;
}

static const char* highlightStyle(ReplHighlightKind kind)
{
    switch (kind)
    {
    case ReplHighlightKind::Keyword:
        return "jaci-keyword";
    case ReplHighlightKind::String:
        return "jaci-string";
    case ReplHighlightKind::Number:
        return "jaci-number";
    case ReplHighlightKind::Comment:
        return "jaci-comment";
    case ReplHighlightKind::Function:
        return "jaci-function";
    case ReplHighlightKind::Builtin:
        return "jaci-builtin";
    case ReplHighlightKind::Type:
        return "jaci-type";
    case ReplHighlightKind::Operator:
        return "jaci-operator";
    case ReplHighlightKind::Attribute:
        return "jaci-attribute";
    case ReplHighlightKind::Error:
        return "jaci-error";
    }

    return "";
}

static void highlightRepl(ic_highlight_env_t* henv, const char* input, void*)
{
    for (const ReplHighlightSpan& span : getReplHighlightSpans(input))
        ic_highlight(henv, long(span.start), long(span.length), highlightStyle(span.kind));
}

static void replaceAll(std::string& value, std::string_view pattern)
{
    for (size_t offset = value.find(pattern); offset != std::string::npos; offset = value.find(pattern, offset))
        value.erase(offset, pattern.size());
}

static void printJaciLogo()
{
    std::string logo = JaciAsciiLogo;
    replaceAll(logo, "[size=9px]");
    replaceAll(logo, "[font=monospace]");
    replaceAll(logo, "<span style=\"color:#7f7f7f\">");
    replaceAll(logo, "</span>");
    for (size_t offset = logo.find("&amp;"); offset != std::string::npos; offset = logo.find("&amp;", offset + 1))
        logo.replace(offset, 5, "&");

    ic_enable_color(Luau::Cli::colorEnabled(stdout));
    ic_print(logo.c_str());
    if (logo.empty() || logo.back() != '\n')
        fputc('\n', stdout);
}

static void printReplWelcome()
{
    printJaciLogo();
    ic_printf("[b color=#7aa2f7]Jaci %s[/] [color=#565f89]· interactive Luau[/]\n", JACI_VERSION);
    ic_println(
        "[color=#737aa2]Tab[/] complete  [color=#737aa2]Shift+Tab[/] new line  [color=#737aa2]:help[/] commands  [color=#737aa2]Ctrl+D[/] exit"
    );
    fputc('\n', stdout);
}

static const char* replValueColor(int type)
{
    if (!Luau::Cli::colorEnabled(stdout))
        return "";

    switch (type)
    {
    case LUA_TNIL:
    case LUA_TBOOLEAN:
        return "\033[1;35m";
    case LUA_TNUMBER:
        return "\033[38;2;255;158;100m";
    case LUA_TSTRING:
        return "\033[38;2;158;206;106m";
    case LUA_TFUNCTION:
        return "\033[1;38;2;122;162;247m";
    case LUA_TTABLE:
        return "\033[38;2;42;195;222m";
    default:
        return "\033[38;2;224;175;104m";
    }
}

static void printQuotedString(const char* value, size_t length)
{
    fputc('"', stdout);
    for (size_t i = 0; i < length; ++i)
    {
        switch (value[i])
        {
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        case '"':
        case '\\':
            fputc('\\', stdout);
            fputc(value[i], stdout);
            break;
        default:
            fputc(value[i], stdout);
            break;
        }
    }
    fputc('"', stdout);
}

static int luaReplPrettyPrint(lua_State* L)
{
    int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i)
    {
        if (i > 1)
            fputc('\t', stdout);

        int type = lua_type(L, i);
        fputs(replValueColor(type), stdout);

        if (type == LUA_TSTRING)
        {
            size_t length = 0;
            const char* value = lua_tolstring(L, i, &length);
            printQuotedString(value, length);
        }
        else
        {
            size_t length = 0;
            const char* value = luaL_tolstring(L, i, &length);
            fwrite(value, 1, length, stdout);
            lua_pop(L, 1);
        }

        if (Luau::Cli::colorEnabled(stdout))
            fputs("\033[0m", stdout);
    }
    fputc('\n', stdout);
    return 0;
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
    lua_pushcfunction(L, luaReplPrettyPrint, "_PRETTYPRINT");
    lua_setglobal(L, "_PRETTYPRINT");

    ic_set_default_completer(completeRepl, L);
    ic_set_default_highlighter(highlightRepl, nullptr);

    // Reset the locale to C
    setlocale(LC_ALL, "C");

    ic_style_def("jaci-keyword", "bold color=#bb9af7");
    ic_style_def("jaci-string", "color=#9ece6a");
    ic_style_def("jaci-number", "color=#ff9e64");
    ic_style_def("jaci-comment", "italic color=#565f89");
    ic_style_def("jaci-function", "bold color=#7aa2f7");
    ic_style_def("jaci-builtin", "color=#2ac3de");
    ic_style_def("jaci-type", "color=#e0af68");
    ic_style_def("jaci-operator", "color=#89ddff");
    ic_style_def("jaci-attribute", "color=#f7768e");
    ic_style_def("jaci-error", "underline color=#f7768e");
    ic_style_def("ic-bracematch", "bold color=#2ac3de");
    ic_style_def("ic-bracematch-error", "bold underline color=#f7768e");

    ic_enable_color(Luau::Cli::colorEnabled(stdout));
    ic_enable_highlight(true);
    ic_enable_brace_matching(true);
    ic_enable_brace_insertion(true);
    ic_enable_multiline(true);
    ic_enable_multiline_indent(false);
    ic_enable_auto_tab(true);
    ic_enable_completion_preview(true);
    ic_enable_hint(true);
    ic_set_hint_delay(150);
    ic_enable_inline_help(true);

    // Loads history from the given file; isocline automatically saves the history on process exit
    loadHistory(".luau_history");

    printReplWelcome();

    std::string buffer;

    for (;;)
    {
        if (buffer.empty())
            ic_set_prompt_marker("[b color=#7aa2f7]jaci> [/]", "[color=#565f89]   ·  [/]");
        else
            ic_set_prompt_marker("[color=#565f89]   ·  [/]", "[color=#565f89]   ·  [/]");

        std::unique_ptr<char, void (*)(void*)> line(ic_readline(""), free);
        if (!line)
            break;

        if (buffer.empty() && strcmp(line.get(), ":quit") == 0)
            break;
        if (buffer.empty() && strcmp(line.get(), ":clear") == 0)
        {
            fputs("\033[2J\033[H", stdout);
            printReplWelcome();
            continue;
        }
        if (buffer.empty() && strcmp(line.get(), ":help") == 0)
        {
            ic_println("[b color=#7aa2f7]Interactive commands[/]");
            ic_println("  [color=#e0af68]:help[/]   Show this guide");
            ic_println("  [color=#e0af68]:clear[/]  Clear the screen");
            ic_println("  [color=#e0af68]:quit[/]   Exit the REPL");
            ic_println("  [color=#737aa2]ans[/]     Read the last expression result");
            continue;
        }

        if (buffer.empty() && runCodeInternal(L, std::string("return ") + line.get(), true) == std::string())
        {
            ic_history_add(line.get());
            continue;
        }

        if (!buffer.empty())
            buffer += "\n";
        buffer += line.get();

        std::string error = runCodeInternal(L, buffer, true);

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

enum class CliCommand
{
    Legacy,
    Run,
    Build,
    Repl,
    Lsp,
};

static bool isOption(const char* argument, const char* option)
{
    size_t length = strlen(option);
    return strcmp(argument, option) == 0 || (strncmp(argument, option, length) == 0 && argument[length] == '=');
}

static bool isBuildOnlyOption(const char* argument)
{
    return strcmp(argument, "-b") == 0 || strcmp(argument, "-o") == 0 || strcmp(argument, "-v") == 0 || strcmp(argument, "-W") == 0 ||
           strcmp(argument, "-Os") == 0 || (strncmp(argument, "-o", 2) == 0 && argument[2]) || isOption(argument, "--build") ||
           isOption(argument, "--bundle") || isOption(argument, "--native-binary") || isOption(argument, "--output") ||
           isOption(argument, "--target") || isOption(argument, "--include-assets") || isOption(argument, "--embed") ||
           isOption(argument, "--windowed") || isOption(argument, "--gui") || isOption(argument, "--direct") || isOption(argument, "--bundle-mode") ||
           isOption(argument, "--strip") || isOption(argument, "--no-strip") || isOption(argument, "--compress") ||
           isOption(argument, "--no-compress") || isOption(argument, "--opt-size") || isOption(argument, "--compiler") ||
           isOption(argument, "--stub") || isOption(argument, "--verbose");
}

static bool isRunOnlyOption(const char* argument)
{
    return strcmp(argument, "-e") == 0 || (strncmp(argument, "-e", 2) == 0 && argument[2]) || strcmp(argument, "-i") == 0 ||
           strcmp(argument, "-a") == 0 || strcmp(argument, "--") == 0 || isOption(argument, "--eval") || isOption(argument, "--interactive") ||
           isOption(argument, "--program-args") || isOption(argument, "--coverage") || isOption(argument, "--profile") ||
           isOption(argument, "--counters") || isOption(argument, "--timetrace") || isOption(argument, "--codegen-perf");
}

static void displayHelp(const char* argv0)
{
    argv0 = Luau::Cli::executableName(argv0);
    Luau::Cli::title(stdout, "Jaci", "fast, standalone Luau runtime");

    Luau::Cli::heading(stdout, "Usage");
    char usage[1024];
    snprintf(usage, sizeof(usage), "%s <command> [options]", argv0);
    Luau::Cli::usage(stdout, usage);
    snprintf(usage, sizeof(usage), "%s <script> [options]", argv0);
    Luau::Cli::usage(stdout, usage);

    Luau::Cli::heading(stdout, "Commands");
    Luau::Cli::option(stdout, "run", "Run a script. Use '--' before arguments passed to the script.");
    Luau::Cli::option(stdout, "build", "Build a standalone executable from one entry script.");
    Luau::Cli::option(stdout, "repl", "Start an interactive Luau session.");
    Luau::Cli::option(stdout, "lsp", "Start the language server over standard input/output.");
    Luau::Cli::option(stdout, "help", "Print help for Jaci or one command.");

    Luau::Cli::heading(stdout, "Options");
    Luau::Cli::option(stdout, "--color=<mode>", "Set color output: auto, always, or never.");
    Luau::Cli::option(stdout, "--version", "Print the Jaci version.");
    Luau::Cli::option(stdout, "-h, --help", "Print this help.");

    Luau::Cli::heading(stdout, "Examples");
    snprintf(usage, sizeof(usage), "%s run app.luau -- --port 8080", argv0);
    Luau::Cli::usage(stdout, usage);
    snprintf(usage, sizeof(usage), "%s build app.luau --output app", argv0);
    Luau::Cli::usage(stdout, usage);
    snprintf(usage, sizeof(usage), "%s repl", argv0);
    Luau::Cli::usage(stdout, usage);
}

static void displayRunHelp(const char* argv0)
{
    argv0 = Luau::Cli::executableName(argv0);
    Luau::Cli::title(stdout, "Jaci Run", "run Luau scripts");
    Luau::Cli::heading(stdout, "Usage");
    char usage[1024];
    snprintf(usage, sizeof(usage), "%s run [options] <script ...> [-- program arguments]", argv0);
    Luau::Cli::usage(stdout, usage);
    Luau::Cli::heading(stdout, "Options");
    Luau::Cli::option(stdout, "-e, --eval <code>", "Run code passed on the command line.");
    Luau::Cli::option(stdout, "-i, --interactive", "Open the REPL after the script finishes.");
    Luau::Cli::option(stdout, "--codegen", "Run profitable functions as native code.");
    Luau::Cli::option(stdout, "-O<0|1|2>", "Set optimization level. Default: 1.");
    Luau::Cli::option(stdout, "-g<0|1|2>", "Set debug information level. Default: 1.");
    Luau::Cli::option(stdout, "--coverage", "Write code coverage to coverage.out.");
    Luau::Cli::option(stdout, "--profile[=<hz>]", "Write a sampling profile to profile.out. Default: 10000 Hz.");
    Luau::Cli::option(stdout, "--timetrace", "Write compiler timing data to trace.json.");
    Luau::Cli::option(stdout, "-h, --help", "Print help for run.");
}

static void displayBuildHelp(const char* argv0)
{
    argv0 = Luau::Cli::executableName(argv0);
    Luau::Cli::title(stdout, "Jaci Build", "build a standalone executable");
    Luau::Cli::heading(stdout, "Usage");
    char usage[1024];
    snprintf(usage, sizeof(usage), "%s build [options] <entry-script>", argv0);
    Luau::Cli::usage(stdout, usage);
    Luau::Cli::heading(stdout, "Options");
    Luau::Cli::option(stdout, "-o, --output <file>", "Set the output path. Default: a.out.");
    Luau::Cli::option(stdout, "--target <target>", "Set a target such as linux-x64 or windows-x64.");
    Luau::Cli::option(stdout, "--include-assets <path>", "Embed an asset file or directory.");
    Luau::Cli::option(stdout, "--embed <path>", "Embed modules outside the static require graph.");
    Luau::Cli::option(stdout, "--windowed", "Build a Windows application without a console window.");
    Luau::Cli::option(stdout, "--opt-size", "Optimize the standalone executable for size.");
    Luau::Cli::option(stdout, "-v, --verbose", "Show detailed build activity.");
    Luau::Cli::option(stdout, "-h, --help", "Print help for build.");
}

static void displayReplHelp(const char* argv0)
{
    argv0 = Luau::Cli::executableName(argv0);
    Luau::Cli::title(stdout, "Jaci REPL", "start an interactive Luau session");
    Luau::Cli::heading(stdout, "Usage");
    char usage[1024];
    snprintf(usage, sizeof(usage), "%s repl [options]", argv0);
    Luau::Cli::usage(stdout, usage);
    Luau::Cli::heading(stdout, "Options");
    Luau::Cli::option(stdout, "--codegen", "Run profitable functions as native code.");
    Luau::Cli::option(stdout, "-O<0|1|2>", "Set optimization level. Default: 1.");
    Luau::Cli::option(stdout, "-h, --help", "Print help for repl.");
    Luau::Cli::heading(stdout, "Inside the REPL");
    Luau::Cli::option(stdout, "Tab", "Complete names and functions.");
    Luau::Cli::option(stdout, "Shift+Tab", "Insert a new line.");
    Luau::Cli::option(stdout, "ans", "Read the last expression result.");
    Luau::Cli::option(stdout, ":help", "Show interactive commands.");
    Luau::Cli::option(stdout, ":clear", "Clear the screen.");
    Luau::Cli::option(stdout, ":quit", "Exit the REPL.");
}

static void displayLspHelp(const char* argv0)
{
    argv0 = Luau::Cli::executableName(argv0);
    Luau::Cli::title(stdout, "Jaci LSP", "serve the Language Server Protocol over standard input/output");
    Luau::Cli::heading(stdout, "Usage");
    char usage[1024];
    snprintf(usage, sizeof(usage), "%s lsp", argv0);
    Luau::Cli::usage(stdout, usage);
}

static void displayCommandHelp(CliCommand command, const char* argv0)
{
    if (command == CliCommand::Run)
        displayRunHelp(argv0);
    else if (command == CliCommand::Build)
        displayBuildHelp(argv0);
    else if (command == CliCommand::Repl)
        displayReplHelp(argv0);
    else if (command == CliCommand::Lsp)
        displayLspHelp(argv0);
    else
        displayHelp(argv0);
}

static int assertionHandler(const char* expr, const char* file, int line, const char* function)
{
    printf("%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
    return 1;
}

int replMain(int argc, char** argv)
{
    Luau::assertHandler() = assertionHandler;

    if (auto exitCode = Luau::SingleBinaryCompiler::checkAndRunBundledPayload(argc, argv))
        return *exitCode;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    CliCommand command = CliCommand::Legacy;
    int argumentStart = 1;

    auto commandFromName = [](const char* name) -> CliCommand
    {
        if (strcmp(name, "run") == 0)
            return CliCommand::Run;
        if (strcmp(name, "build") == 0)
            return CliCommand::Build;
        if (strcmp(name, "repl") == 0)
            return CliCommand::Repl;
        if (strcmp(name, "lsp") == 0)
            return CliCommand::Lsp;
        return CliCommand::Legacy;
    };

    if (argc > 1 && strcmp(argv[1], "help") == 0)
    {
        displayCommandHelp(argc > 2 ? commandFromName(argv[2]) : CliCommand::Legacy, argv[0]);
        return 0;
    }

    if (argc > 1)
    {
        command = commandFromName(argv[1]);
        if (command != CliCommand::Legacy)
            argumentStart = 2;
    }

    if (command == CliCommand::Lsp)
    {
        if (argc == 2)
            return Luau::runLspServer();
        if (argc == 3 && (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0))
        {
            displayLspHelp(argv[0]);
            return 0;
        }

        Luau::Cli::error(stderr, "lsp does not accept arguments");
        Luau::Cli::hint(stderr, "use 'luau lsp'");
        return 1;
    }

    int profile = 0;
    bool coverage = false;
    bool interactive = false;
    bool codegenPerf = false;
    bool counters = false;
    bool buildMode = command == CliCommand::Build;
    bool windowed = false;
    bool strip = true;
    bool compress = true;
    bool optimizeForSize = false;
    Luau::BundleMode bundleMode = Luau::BundleMode::Auto;
    std::string compilerCommand;
    std::string customStubPath;
    std::string outputFile;
    std::string targetArchitecture;
    std::vector<std::string> assetPaths;
    std::vector<std::string> embedPaths;
    std::string evalCode;
    bool hasEval = false;
    bool verbose = false;
    std::vector<std::string> inputFiles;
    int program_args = argc;

    for (int i = argumentStart; i < argc; i++)
    {
        if ((strcmp(argv[i], "-h") != 0 && strcmp(argv[i], "--help") != 0) &&
            ((command == CliCommand::Build && (isRunOnlyOption(argv[i]) || strcmp(argv[i], "--lsp") == 0)) ||
             ((command == CliCommand::Run || command == CliCommand::Repl) && isBuildOnlyOption(argv[i])) ||
             (command == CliCommand::Repl && isRunOnlyOption(argv[i])) || (command != CliCommand::Legacy && strcmp(argv[i], "--lsp") == 0)))
        {
            char message[256];
            snprintf(message, sizeof(message), "option '%s' does not apply to this command", argv[i]);
            Luau::Cli::error(stderr, message);
            Luau::Cli::hint(
                stderr,
                command == CliCommand::Build  ? "run 'luau build --help'"
                : command == CliCommand::Repl ? "run 'luau repl --help'"
                                              : "run 'luau run --help'"
            );
            return 1;
        }

        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            displayCommandHelp(command, argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--version") == 0)
        {
            printJaciLogo();
            printf("Jaci %s (Luau fork)\n", JACI_VERSION);
            return 0;
        }
        else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
        {
            interactive = true;
        }
        else if (strcmp(argv[i], "-Os") == 0)
        {
            optimizeForSize = true;
        }
        else if (strncmp(argv[i], "-O", 2) == 0)
        {
            if (strlen(argv[i]) != 3 || argv[i][2] < '0' || argv[i][2] > '2')
            {
                Luau::Cli::error(stderr, "invalid optimization level");
                Luau::Cli::hint(stderr, "use -O0, -O1, or -O2");
                return 1;
            }
            globalOptions.optimizationLevel = argv[i][2] - '0';
        }
        else if (strncmp(argv[i], "-g", 2) == 0)
        {
            if (strlen(argv[i]) != 3 || argv[i][2] < '0' || argv[i][2] > '2')
            {
                Luau::Cli::error(stderr, "invalid debug information level");
                Luau::Cli::hint(stderr, "use -g0, -g1, or -g2");
                return 1;
            }
            globalOptions.debugLevel = argv[i][2] - '0';
        }
        else if (strcmp(argv[i], "--profile") == 0)
        {
            profile = 10000; // default to 10 KHz
        }
        else if (strncmp(argv[i], "--profile=", 10) == 0)
        {
            char* end = nullptr;
            long frequency = strtol(argv[i] + 10, &end, 10);
            if (!argv[i][10] || !end || *end || frequency <= 0 || frequency > INT_MAX)
            {
                Luau::Cli::error(stderr, "invalid profiling frequency");
                Luau::Cli::hint(stderr, "use --profile or --profile=<positive hz>");
                return 1;
            }
            profile = int(frequency);
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
        else if (strncmp(argv[i], "--color=", 8) == 0)
        {
            if (!Luau::Cli::setColorMode(argv[i] + 8))
            {
                Luau::Cli::error(stderr, "invalid color mode");
                Luau::Cli::hint(stderr, "use --color=auto, --color=always, or --color=never");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--program-args") == 0 || strcmp(argv[i], "-a") == 0)
        {
            program_args = i + 1;
            break;
        }
        else if (strcmp(argv[i], "--") == 0)
        {
            program_args = i + 1;
            break;
        }
        else if (strcmp(argv[i], "--build") == 0 || strcmp(argv[i], "--bundle") == 0 || strcmp(argv[i], "-b") == 0)
        {
            buildMode = true;
        }
        else if (strcmp(argv[i], "--windowed") == 0 || strcmp(argv[i], "--gui") == 0 || strcmp(argv[i], "-W") == 0)
        {
            windowed = true;
        }
        else if (strcmp(argv[i], "--direct") == 0)
        {
            bundleMode = Luau::BundleMode::Direct;
        }
        else if (strncmp(argv[i], "--bundle-mode=", 14) == 0)
        {
            const char* mode = argv[i] + 14;
            if (strcmp(mode, "direct") == 0)
                bundleMode = Luau::BundleMode::Direct;
            else if (strcmp(mode, "native") == 0)
                bundleMode = Luau::BundleMode::Native;
            else if (strcmp(mode, "auto") == 0)
                bundleMode = Luau::BundleMode::Auto;
            else
            {
                Luau::Cli::error(stderr, "invalid standalone packaging mode");
                Luau::Cli::hint(stderr, "use --bundle-mode=auto, --bundle-mode=direct, or --bundle-mode=native");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--strip") == 0)
        {
            strip = true;
        }
        else if (strcmp(argv[i], "--no-strip") == 0)
        {
            strip = false;
        }
        else if (strcmp(argv[i], "--compress") == 0)
        {
            compress = true;
        }
        else if (strcmp(argv[i], "--no-compress") == 0)
        {
            compress = false;
        }
        else if (strcmp(argv[i], "--opt-size") == 0)
        {
            optimizeForSize = true;
        }
        else if (strcmp(argv[i], "--compiler") == 0 && i + 1 < argc)
        {
            compilerCommand = argv[++i];
        }
        else if (strncmp(argv[i], "--compiler=", 11) == 0)
        {
            compilerCommand = argv[i] + 11;
        }
        else if (strcmp(argv[i], "--stub") == 0 && i + 1 < argc)
        {
            customStubPath = argv[++i];
        }
        else if (strncmp(argv[i], "--stub=", 7) == 0)
        {
            customStubPath = argv[i] + 7;
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
        else if (strcmp(argv[i], "--embed") == 0 && i + 1 < argc)
        {
            embedPaths.push_back(argv[++i]);
        }
        else if (strncmp(argv[i], "--embed=", 8) == 0)
        {
            embedPaths.push_back(argv[i] + 8);
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
        else if (
            strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eval") == 0 || strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0 ||
            strcmp(argv[i], "--compiler") == 0 || strcmp(argv[i], "--stub") == 0 || strcmp(argv[i], "--target") == 0 ||
            strcmp(argv[i], "--include-assets") == 0 || strcmp(argv[i], "--embed") == 0
        )
        {
            char message[256];
            snprintf(message, sizeof(message), "option '%s' requires a value", argv[i]);
            Luau::Cli::error(stderr, message);
            Luau::Cli::hint(stderr, "run 'luau --help' to see the expected value");
            return 1;
        }
        else if (argv[i][0] == '-')
        {
            char message[256];
            snprintf(message, sizeof(message), "unknown option '%s'", argv[i]);
            Luau::Cli::error(stderr, message);
            Luau::Cli::hint(stderr, "run 'luau --help' to list supported options");
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
            Luau::Cli::error(stderr, "build requires one entry script");
            Luau::Cli::hint(stderr, "use 'luau build <entry-script>'");
            return 1;
        }
        if (inputFiles.size() > 1)
        {
            Luau::Cli::error(stderr, "build accepts exactly one entry script");
            Luau::Cli::hint(stderr, "use 'luau build <entry-script>'");
            return 1;
        }

        Luau::SingleBinaryOptions opts;
        opts.entryFilePath = inputFiles[0];
        opts.outputBinaryPath = outputFile.empty() ? "a.out" : outputFile;
        opts.targetArchitecture = targetArchitecture;
        opts.compilerCommand = compilerCommand;
        opts.customStubPath = customStubPath;
        opts.assetPaths = assetPaths;
        opts.embedPaths = embedPaths;
        opts.bundleMode = bundleMode;
        opts.optimizationLevel = globalOptions.optimizationLevel;
        opts.debugLevel = globalOptions.debugLevel;
        opts.codegen = codegen;
        opts.verbose = verbose;
        opts.windowed = windowed;
        opts.strip = strip;
        opts.compress = compress;
        opts.optimizeForSize = optimizeForSize;

        const bool showProgress = command == CliCommand::Build && !verbose;
        if (showProgress)
        {
            char detail[1024];
            snprintf(detail, sizeof(detail), "%s -> %s", opts.entryFilePath.c_str(), opts.outputBinaryPath.c_str());
            Luau::Cli::status(stderr, "Building", detail);
        }

        bool built = Luau::SingleBinaryCompiler::compile(opts);
        if (showProgress && built)
            Luau::Cli::success(stderr, opts.outputBinaryPath.c_str());
        return built ? 0 : 1;
    }

    if (command == CliCommand::Run && inputFiles.empty() && !hasEval)
    {
        Luau::Cli::error(stderr, "run requires a script or --eval code");
        Luau::Cli::hint(stderr, "use 'luau run <script>' or 'luau run --eval <code>'");
        return 1;
    }

    if (command == CliCommand::Repl && (!inputFiles.empty() || hasEval))
    {
        Luau::Cli::error(stderr, "repl does not accept scripts or --eval");
        Luau::Cli::hint(stderr, "use 'luau run <script>' to execute code");
        return 1;
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

    if (command == CliCommand::Run)
    {
        if (hasEval && files.empty())
            Luau::Cli::status(stderr, "Running", "command-line code");
        else
        {
            char detail[1024];
            snprintf(detail, sizeof(detail), "%zu script%s", files.size(), files.size() == 1 ? "" : "s");
            Luau::Cli::status(stderr, "Running", detail);
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
