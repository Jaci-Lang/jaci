// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
//
// Single binary = engine executable + embedded VFS + entry module.
//
// The embedded files are registered with the process VFS layer (VfsLayer) and
// the entry is executed through the ordinary require stack (ReplRequirer +
// VfsNavigator + Require::Navigator) that the REPL, luau-analyze and the LSP
// use. There is no second navigation implementation and no module-name
// reconciliation: a binary resolves modules exactly the way the REPL does.
//
// Build discovery walks the static require graph, resolving every require
// string with the same Navigator the type checker uses, so the bundler embeds
// precisely what the runtime would resolve. Explicit --embed entries cover
// modules reachable only through dynamically-loaded chunks; `klur build`
// passes the project and toolchain package directories for that.
#include "Luau/SingleBinaryCompiler.h"

#include "Luau/Ast.h"
#include "Luau/AnalyzeRequirer.h"
#include "Luau/CodeGen.h"
#include "Luau/CodeGenOptions.h"
#include "Luau/Compiler.h"
#include "Luau/FileUtils.h"
#include "Luau/Parser.h"
#include "Luau/ReplRequirer.h"
#include "Luau/Require.h"
#include "Luau/RequireNavigator.h"
#include "Luau/VfsCompress.h"
#include "Luau/VfsLayer.h"
#include "Luau/VfsNavigator.h"
#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <process.h> // _getpid()
#include <sys/stat.h> // _stat64
#define getpid _getpid
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace Luau
{

namespace
{

// A module in the bundle: one entry per canonical absolute path.
struct DiscoveredModule
{
    std::string absolutePath;
    std::string source;
};

// A non-module file embedded for the application (images, data, ...).
struct DiscoveredAsset
{
    std::string relativePath; // as specified at build time; the runtime lookup key
    std::string absolutePath; // build-time absolute path; the VFS registration key
    std::string data;
};

static const char kMagicHeader[8] = {'J', 'A', 'C', 'I', 'P', 'K', 'G', '\0'};
static const char kMagicTrailer[8] = {'J', 'A', 'C', 'I', 'P', 'K', 'G', '\0'};
static const size_t kTrailerSize = 24; // uint64_t offset + uint64_t size + 8 bytes magic
static const uint32_t kPayloadVersion = 2;

// Payload flags
static const uint32_t kFlagCodegen = 1;
static const uint32_t kFlagCompressed = 8;

static void writeUint32(std::string& buf, uint32_t val)
{
    buf.push_back(static_cast<char>(val & 0xFF));
    buf.push_back(static_cast<char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<char>((val >> 16) & 0xFF));
    buf.push_back(static_cast<char>((val >> 24) & 0xFF));
}

static void writeInt32(std::string& buf, int32_t val)
{
    writeUint32(buf, static_cast<uint32_t>(val));
}

static void writeUint64(std::string& buf, uint64_t val)
{
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<char>((val >> (i * 8)) & 0xFF));
}

static void writeString(std::string& buf, const std::string& str)
{
    writeUint32(buf, static_cast<uint32_t>(str.size()));
    buf.append(str);
}

static uint32_t readUint32(const unsigned char*& ptr, const unsigned char* end)
{
    if (ptr + 4 > end)
        return 0;
    uint32_t val = static_cast<uint32_t>(ptr[0]) |
                   (static_cast<uint32_t>(ptr[1]) << 8) |
                   (static_cast<uint32_t>(ptr[2]) << 16) |
                   (static_cast<uint32_t>(ptr[3]) << 24);
    ptr += 4;
    return val;
}

static int32_t readInt32(const unsigned char*& ptr, const unsigned char* end)
{
    return static_cast<int32_t>(readUint32(ptr, end));
}

static uint64_t readUint64(const unsigned char*& ptr, const unsigned char* end)
{
    if (ptr + 8 > end)
        return 0;
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
        val |= static_cast<uint64_t>(ptr[i]) << (i * 8);
    ptr += 8;
    return val;
}

static std::string readString(const unsigned char*& ptr, const unsigned char* end)
{
    uint32_t size = readUint32(ptr, end);
    if (ptr + size > end)
    {
        // Corrupt payload: advance to the end so the parse terminates.
        ptr = end;
        return std::string();
    }
    std::string result(reinterpret_cast<const char*>(ptr), size);
    ptr += size;
    return result;
}

static std::string getTempRunnerPath()
{
#if defined(_WIN32)
    char tempDir[MAX_PATH] = {};
    DWORD len = GetTempPathA(sizeof(tempDir), tempDir);
    std::string dir = (len > 0 && len < sizeof(tempDir)) ? std::string(tempDir) : ".";
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/')
        dir += "\\";
    char path[MAX_PATH * 2] = {};
    snprintf(path, sizeof(path), "%sjaci_single_binary_%d.tmp", dir.c_str(), static_cast<int>(getpid()));
    return std::string(path);
#else
    const char* tmp = getenv("TMPDIR");
    if (!tmp || *tmp == '\0')
        tmp = "/tmp";
    char path[512] = {};
    snprintf(path, sizeof(path), "%s/jaci_single_binary_%d.tmp", tmp, static_cast<int>(getpid()));
    return std::string(path);
#endif
}

// Collects string-literal require() calls from a parsed module.
class RequireVisitor : public AstVisitor
{
public:
    std::vector<std::string> requirePaths;

    bool visit(AstExprCall* call) override
    {
        if (AstExprGlobal* global = call->func->as<AstExprGlobal>())
        {
            if (global->name == "require" && call->args.size == 1)
            {
                if (AstExprConstantString* str = call->args.data[0]->as<AstExprConstantString>())
                {
                    requirePaths.emplace_back(str->value.data, str->value.size);
                }
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Discovery: walk the static require graph, resolving each require with the
// real navigator. Whatever luau-analyze resolves, the bundler embeds.
// ---------------------------------------------------------------------------

std::optional<std::string> resolveRequirePath(const std::string& requirerAbsPath, const std::string& specifier)
{
    FileNavigationContext navigationContext{requirerAbsPath};

    Luau::Require::ErrorHandler nullErrorHandler{};
    Luau::Require::Navigator navigator(navigationContext, nullErrorHandler);
    if (navigator.navigate(specifier) != Luau::Require::Navigator::Status::Success)
        return std::nullopt;

    if (!navigationContext.isModulePresent())
        return std::nullopt;

    return navigationContext.getIdentifier();
}

bool isLuauModuleFile(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    std::string suffix = (dot != std::string::npos) ? path.substr(dot) : "";
    return suffix == ".luau" || suffix == ".lua";
}

bool discoverModules(
    const std::string& entryAbsPath,
    const std::vector<std::string>& embedPaths,
    std::vector<DiscoveredModule>& outModules,
    std::vector<DiscoveredAsset>& outAssets
)
{
    std::map<std::string, size_t> moduleIndex; // canonical absolute path -> index
    std::set<std::string> visitedEmbeds;
    std::vector<std::string> toVisit;

    auto addModule = [&](const std::string& absPath) -> bool
    {
        if (moduleIndex.count(absPath))
            return false;
        moduleIndex[absPath] = outModules.size();
        outModules.push_back(DiscoveredModule{absPath, {}});
        toVisit.push_back(absPath);
        return true;
    };

    if (!addModule(entryAbsPath))
        return false;

    // Explicit embeds: files and directories the solver wants carried in the
    // binary regardless of the static require graph (dynamically-loaded
    // modules). The engine does not decide what a package layout is; it only
    // embeds what it is told to.
    for (const std::string& spec : embedPaths)
    {
        if (spec.empty())
            continue;

        std::string resolved = spec;
        if (!isAbsolutePath(resolved))
        {
            if (auto cwd = getCurrentWorkingDirectory())
                resolved = normalizePath(*cwd + "/" + resolved);
            else
                continue;
        }
        resolved = normalizePath(resolved);

        if (isFile(resolved))
        {
            addModule(resolved);
        }
        else if (isDirectory(resolved))
        {
            traverseDirectory(
                resolved,
                [&](const std::string& filePath)
                {
                    if (!isLuauModuleFile(filePath))
                        return;
                    std::string absPath = normalizePath(filePath);
                    if (visitedEmbeds.insert(absPath).second)
                        addModule(absPath);
                });
        }
        else
        {
            fprintf(stderr, "SingleBinaryCompiler: Warning: embed path '%s' not found\n", spec.c_str());
        }
    }

    // Static require graph, resolved with the real navigator.
    size_t head = 0;
    while (head < toVisit.size())
    {
        std::string currentPath = toVisit[head++];

        std::optional<std::string> source = readFile(currentPath);
        if (!source)
        {
            fprintf(stderr, "SingleBinaryCompiler: Warning: Could not read module '%s'\n", currentPath.c_str());
            continue;
        }
        outModules[moduleIndex[currentPath]].source = *source;

        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        Luau::ParseOptions popts;
        Luau::ParseResult parseResult = Luau::Parser::parse(source->data(), source->size(), names, allocator, popts);
        if (!parseResult.root)
            continue;

        RequireVisitor visitor;
        parseResult.root->visit(&visitor);

        for (const std::string& specifier : visitor.requirePaths)
        {
            std::optional<std::string> resolved = resolveRequirePath(currentPath, specifier);
            if (!resolved)
            {
                // The navigator (shared with the REPL and the type checker)
                // cannot resolve this specifier from this requirer, so the
                // binary would fail at runtime exactly like the REPL. Report
                // it instead of silently shipping a broken binary.
                fprintf(stderr, "SingleBinaryCompiler: Warning: cannot resolve require('%s') from '%s'; the module will be missing at runtime\n",
                        specifier.c_str(), currentPath.c_str());
                continue;
            }
            addModule(*resolved);
        }
    }

    // Carry the config files the resolution depended on.
    //
    // Alias resolution reads .luaurc / .config.luau by walking up from each
    // requirer. A binary running outside the project tree would lose those
    // configs, so embed every config file on the walk-up paths of the
    // embedded modules. At runtime the embedded configs shadow the on-disk
    // ones, which is the correct reproducible-binary semantics.
    std::set<std::string> embeddedConfigs;
    for (const auto& mod : outModules)
    {
        std::optional<std::string> dir = getParentPath(mod.absolutePath);
        while (dir)
        {
            for (const char* name : {Luau::kConfigName, Luau::kLuauConfigName})
            {
                std::string cfg = *dir + "/" + name;
                if (isFile(cfg) && embeddedConfigs.insert(cfg).second)
                {
                    if (auto contents = readFile(cfg))
                        outAssets.push_back(DiscoveredAsset{cfg, cfg, *contents});
                }
            }
            dir = getParentPath(*dir);
        }
    }

    // Rebase toolchain packages onto the entry's directory.
    //
    // A package tree rooted at X/<pkgdir> is reachable at runtime only from
    // requirers whose directory is under X (the walk-up checks <level>/<pkgdir>
    // on the way from the requirer to the root). Modules resolved through the
    // build-time toolchain root (next to the engine that does the building)
    // are generally not reachable from the entry, because the runtime binary
    // lives somewhere else entirely. Re-rooting such trees at the entry's
    // directory keeps every package in the binary reachable from every module
    // in it, from any working directory, regardless of where the binary is
    // later copied to.
    if (auto entryDir = getParentPath(entryAbsPath))
    {
        static const char* kPkgDirs[] = {"klur_modules", "luau_packages", "packages", "node_modules"};

        for (auto& mod : outModules)
        {
            const std::string& path = mod.absolutePath;
            if (path.size() > entryDir->size() && path.compare(0, entryDir->size(), *entryDir) == 0 &&
                (path[entryDir->size()] == '/'))
                continue; // already under the entry's directory

            // Find the deepest package-directory component of the path.
            std::string pkgDir;
            std::string tail;
            size_t pkgPos = std::string::npos;
            std::vector<std::string> components;
            size_t start = 0;
            while (start <= path.size())
            {
                size_t slash = path.find('/', start);
                std::string comp = (slash == std::string::npos) ? path.substr(start) : path.substr(start, slash - start);
                components.push_back(comp);
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
            for (size_t i = 0; i < components.size(); ++i)
            {
                for (const char* pdir : kPkgDirs)
                {
                    if (components[i] == pdir)
                    {
                        pkgDir = pdir;
                        pkgPos = i;
                        for (size_t j = i + 1; j < components.size(); ++j)
                        {
                            if (!tail.empty())
                                tail += "/";
                            tail += components[j];
                        }
                        break;
                    }
                }
            }
            if (pkgDir.empty() || tail.empty())
                continue; // not a package file (plain module: relative navigation handles it)

            // X = the path prefix up to (not including) the package directory.
            std::string x;
            for (size_t i = 0; i < pkgPos; ++i)
            {
                if (!components[i].empty())
                {
                    if (!x.empty())
                        x += "/";
                    x += components[i];
                }
            }
            // Reachable when the entry's directory is under X (X an ancestor of it).
            if (entryDir->size() > x.size() && x.size() > 0 && entryDir->compare(0, x.size(), x) == 0 &&
                (entryDir->size() == x.size() || (*entryDir)[x.size()] == '/'))
                continue;

            std::string newPath = *entryDir + "/" + pkgDir + "/" + tail;
            mod.absolutePath = newPath;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Payload (format version 2)
// ---------------------------------------------------------------------------

std::string serializePayload(
    const std::string& entryAbsPath,
    const std::vector<DiscoveredModule>& modules,
    const std::vector<DiscoveredAsset>& assets,
    const SingleBinaryOptions& options
)
{
    std::string body;
    writeString(body, entryAbsPath);
    writeInt32(body, options.optimizationLevel);
    writeInt32(body, options.debugLevel);

    writeUint32(body, static_cast<uint32_t>(modules.size()));
    for (const auto& mod : modules)
    {
        writeString(body, mod.absolutePath);
        writeString(body, mod.source);
    }

    writeUint32(body, static_cast<uint32_t>(assets.size()));
    for (const auto& asset : assets)
    {
        writeString(body, asset.relativePath);
        writeString(body, asset.absolutePath);
        writeString(body, asset.data);
    }

    std::string payload;
    payload.append(kMagicHeader, 8);
    writeUint32(payload, kPayloadVersion);

    uint32_t flags = (options.codegen ? kFlagCodegen : 0);
    if (options.compress)
    {
        flags |= kFlagCompressed;
        writeUint32(payload, flags);
        writeUint64(payload, body.size());
        payload.append(Luau::Vfs::compress(body));
    }
    else
    {
        writeUint32(payload, flags);
        payload.append(body);
    }

    return payload;
}

// ---------------------------------------------------------------------------
// Packaging: base executable + payload + trailer
// ---------------------------------------------------------------------------

bool compileDirectBundle(
    const SingleBinaryOptions& options,
    const std::string& entryAbsPath,
    const std::vector<DiscoveredModule>& modules,
    const std::vector<DiscoveredAsset>& assets
)
{
    std::string stubPath = options.customStubPath;
    if (stubPath.empty())
    {
        if (const char* envStub = getenv("JACI_RUNNER_STUB"))
            stubPath = envStub;
    }
    // Explicit environment overrides only: JACI_ROOT (source tree) and
    // JACI_BUILD (build tree). No machine-specific defaults: the running
    // executable itself (below) is the portable fallback base.
    if (stubPath.empty() || !isFile(stubPath))
    {
        if (const char* envBuild = getenv("JACI_BUILD"))
        {
            if (isFile(envBuild))
            {
                stubPath = envBuild;
            }
            else if (isDirectory(envBuild))
            {
                static const char* kExeNames[] = {"luau", "luau.exe"};
                for (const char* name : kExeNames)
                {
                    std::string c = std::string(envBuild) + "/" + name;
                    if (isFile(c))
                    {
                        stubPath = c;
                        break;
                    }
                }
            }
        }
        if (const char* envRoot = getenv("JACI_ROOT"); envRoot && (stubPath.empty() || !isFile(stubPath)))
        {
            static const char* kCandidates[] = {
                "luau", "luau.exe", "build/luau", "build/luau.exe"
            };
            for (const char* cand : kCandidates)
            {
                std::string c = std::string(envRoot) + "/" + cand;
                if (isFile(c))
                {
                    stubPath = c;
                    break;
                }
            }
        }
    }
    if (stubPath.empty() || !isFile(stubPath))
    {
        if (auto exe = getExecutablePath())
            stubPath = *exe;
    }

    if (stubPath.empty() || !isFile(stubPath))
    {
        fprintf(stderr, "SingleBinaryCompiler: Could not locate base executable for standalone packaging.\n");
        return false;
    }

    std::optional<std::string> baseExeData = readFile(stubPath);
    if (!baseExeData || baseExeData->empty())
    {
        fprintf(stderr, "SingleBinaryCompiler: Failed to read base executable '%s'\n", stubPath.c_str());
        return false;
    }

    // If the base executable already has an appended bundle, strip it to start from a clean base.
    size_t baseSize = baseExeData->size();
    if (baseSize >= kTrailerSize)
    {
        const unsigned char* endPtr = reinterpret_cast<const unsigned char*>(baseExeData->data()) + baseSize;
        const unsigned char* trailerPtr = endPtr - kTrailerSize;
        if (memcmp(trailerPtr + 16, kMagicTrailer, 8) == 0)
        {
            const unsigned char* p = trailerPtr;
            uint64_t prevOffset = readUint64(p, endPtr);
            if (prevOffset < baseSize)
                baseSize = static_cast<size_t>(prevOffset);
        }
    }

    std::string finalBaseData = baseExeData->substr(0, baseSize);
    if (options.strip)
    {
        std::string tempStubPath = getTempRunnerPath() + ".stub";
        FILE* sfp = fopen(tempStubPath.c_str(), "wb");
        if (sfp)
        {
            fwrite(finalBaseData.data(), 1, finalBaseData.size(), sfp);
            fclose(sfp);
#if defined(_WIN32)
            std::string stripCmd = "strip -s \"" + tempStubPath + "\" >nul 2>nul";
#else
            std::string stripCmd = "strip -s \"" + tempStubPath + "\" >/dev/null 2>&1";
#endif
            if (system(stripCmd.c_str()) == 0)
            {
                if (auto stripped = readFile(tempStubPath))
                {
                    if (!stripped->empty())
                    {
                        finalBaseData = std::move(*stripped);
                        baseSize = finalBaseData.size();
                    }
                }
            }
            remove(tempStubPath.c_str());
        }
    }

    std::string payload = serializePayload(entryAbsPath, modules, assets, options);
    std::string outPath = options.outputBinaryPath.empty() ? "a.out" : options.outputBinaryPath;

    std::ofstream of(outPath, std::ios::binary | std::ios::trunc);
    if (!of)
    {
        fprintf(stderr, "SingleBinaryCompiler: Failed to open output binary '%s' for writing\n", outPath.c_str());
        return false;
    }

    if (finalBaseData.size() > 0)
    {
        of.write(finalBaseData.data(), finalBaseData.size());
    }

    uint64_t payloadOffset = static_cast<uint64_t>(baseSize);
    of.write(payload.data(), payload.size());

    unsigned char trailer[kTrailerSize] = {};
    uint64_t payloadSize = static_cast<uint64_t>(payload.size());
    memcpy(trailer, &payloadOffset, 8);
    memcpy(trailer + 8, &payloadSize, 8);
    memcpy(trailer + 16, kMagicTrailer, 8);
    of.write(reinterpret_cast<char*>(trailer), kTrailerSize);
    of.close();

#ifndef _WIN32
    // The output is a standalone executable; make it runnable in place.
    chmod(outPath.c_str(), 0755);
#endif

    if (options.verbose)
    {
        printf("SingleBinaryCompiler: Packaged standalone binary '%s' (base: %zu bytes, payload: %zu bytes)\n",
               outPath.c_str(), baseSize, payload.size());
    }

    return true;
}

// ---------------------------------------------------------------------------
// Embedded runtime: the payload registers its files with the VFS layer and
// runs the entry through the ordinary require stack.
// ---------------------------------------------------------------------------

static int g_rtOptLevel = 1;
static int g_rtDbgLevel = 1;
static bool g_rtCodegen = false;

static Luau::CompileOptions rtCompileOptions()
{
    Luau::CompileOptions copts;
    copts.optimizationLevel = g_rtOptLevel;
    copts.debugLevel = g_rtDbgLevel;
    return copts;
}

static bool rtCoverageActive()
{
    return false;
}

static bool rtCodegenEnabled()
{
    return g_rtCodegen && Luau::CodeGen::isSupported();
}

static void rtCoverageTrack(lua_State*, int)
{
}

static bool rtCountersActive()
{
    return false;
}

static void rtCountersTrack(lua_State*, int)
{
}

// loadstring is not part of the standard base library (it is a REPL-only
// convenience in vanilla Luau). The embedded runtime does not run the REPL,
// so register it explicitly for runtime code that needs dynamic compilation
// (Packagefile loading, the klur test runner).
static int rt_loadstring(lua_State* L)
{
    size_t l = 0;
    const char* s = luaL_checklstring(L, 1, &l);
    const char* chunkname = luaL_optstring(L, 2, s);

    lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

    Luau::CompileOptions copts = rtCompileOptions();
    std::string bytecode = Luau::compile(std::string(s, l), copts);
    if (luau_load(L, chunkname, bytecode.data(), bytecode.size(), 0) == 0)
        return 1;

    lua_pushnil(L);
    lua_insert(L, -2); // put error message after nil
    return 2;
}

// fs.* overrides for the embedded runtime. The engine's native fs module hits
// the operating system directly; these route the calls through the VFS-aware
// FileUtils utilities so embedded files are visible to application code.
static int rt_fs_readfile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    std::optional<std::string> fileData = readFile(pathStr);
    if (!fileData)
        luaL_error(L, "fs.readfile: cannot open file: %s", pathStr);
    lua_pushlstring(L, fileData->data(), fileData->size());
    return 1;
}

static int rt_fs_exists(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    lua_pushboolean(L, isFile(pathStr) || isDirectory(pathStr));
    return 1;
}

static int rt_fs_isfile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    lua_pushboolean(L, isFile(pathStr));
    return 1;
}

static int rt_fs_isdir(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    lua_pushboolean(L, isDirectory(pathStr));
    return 1;
}

static int rt_fs_stat(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);

    // Embedded files: size from the stored data.
    if (std::optional<std::string> data = VfsLayer::readEmbeddedFile(pathStr))
    {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, static_cast<double>(data->size()));
        lua_setfield(L, -2, "size");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "isFile");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "isDirectory");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "exists");
        return 1;
    }

    if (isFile(pathStr))
    {
        double size = 0;
#ifdef _WIN32
        struct _stat64 st;
        if (_stat64(pathStr, &st) == 0)
            size = static_cast<double>(st.st_size);
#else
        struct stat st;
        if (stat(pathStr, &st) == 0)
            size = static_cast<double>(st.st_size);
#endif
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, size);
        lua_setfield(L, -2, "size");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "isFile");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "isDirectory");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "exists");
        return 1;
    }
    if (isDirectory(pathStr))
    {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, 0);
        lua_setfield(L, -2, "size");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "isFile");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "isDirectory");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "exists");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

} // namespace

std::optional<int> SingleBinaryCompiler::checkAndRunBundledPayload(int argc, char** argv)
{
    // An explicit engine-level build request (luau --build entry -o out) must
    // reach the normal CLI even on a payload-bearing binary. This is what lets
    // `klur build` inside a single binary re-invoke itself as the engine.
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] && (strcmp(argv[i], "--build") == 0 || strcmp(argv[i], "--bundle") == 0 || strcmp(argv[i], "-b") == 0))
            return std::nullopt;
    }

    std::string exePath;
    if (auto p = getExecutablePath())
        exePath = *p;
    if (exePath.empty() && argc > 0 && argv[0])
        exePath = argv[0];

    if (exePath.empty() || !isFile(exePath))
        return std::nullopt;

    FILE* fp = fopen(exePath.c_str(), "rb");
    if (!fp)
        return std::nullopt;

    fseek(fp, 0, SEEK_END);
    long fileLength = ftell(fp);
    if (fileLength < static_cast<long>(kTrailerSize + 32))
    {
        fclose(fp);
        return std::nullopt;
    }

    fseek(fp, fileLength - kTrailerSize, SEEK_SET);
    unsigned char trailerBuf[kTrailerSize];
    if (fread(trailerBuf, 1, kTrailerSize, fp) != kTrailerSize)
    {
        fclose(fp);
        return std::nullopt;
    }

    if (memcmp(trailerBuf + 16, kMagicTrailer, 8) != 0)
    {
        fclose(fp);
        return std::nullopt;
    }

    const unsigned char* tp = trailerBuf;
    const unsigned char* tpEnd = trailerBuf + kTrailerSize;
    uint64_t payloadOffset = readUint64(tp, tpEnd);
    uint64_t payloadSize = readUint64(tp, tpEnd);

    if (payloadOffset + payloadSize + kTrailerSize != static_cast<uint64_t>(fileLength))
    {
        fclose(fp);
        return std::nullopt;
    }

    if (payloadOffset >= static_cast<uint64_t>(fileLength))
    {
        fclose(fp);
        return std::nullopt;
    }

    fseek(fp, static_cast<long>(payloadOffset), SEEK_SET);
    std::string payloadData(static_cast<size_t>(payloadSize), '\0');
    if (fread(&payloadData[0], 1, payloadSize, fp) != payloadSize)
    {
        fclose(fp);
        return std::nullopt;
    }
    fclose(fp);

    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(payloadData.data());
    const unsigned char* end = ptr + payloadData.size();

    if (ptr + 8 > end || memcmp(ptr, kMagicHeader, 8) != 0)
        return std::nullopt;
    ptr += 8;

    uint32_t version = readUint32(ptr, end);
    if (version != kPayloadVersion)
    {
        // Unknown or legacy payload: behave as a plain engine binary.
        return std::nullopt;
    }

    uint32_t flags = readUint32(ptr, end);
    bool enableCodegen = (flags & kFlagCodegen) != 0;
    bool isCompressed = (flags & kFlagCompressed) != 0;

    // Must outlive the parse below: ptr/end point into this buffer.
    std::string decompressedData;
    if (isCompressed)
    {
        uint64_t uncompressedSize = readUint64(ptr, end);
        size_t remainingCompressedSize = static_cast<size_t>(end - ptr);
        std::string_view compressedView(reinterpret_cast<const char*>(ptr), remainingCompressedSize);
        decompressedData = Luau::Vfs::decompress(compressedView, static_cast<size_t>(uncompressedSize));
        ptr = reinterpret_cast<const unsigned char*>(decompressedData.data());
        end = ptr + decompressedData.size();
    }

    std::string entryAbsPath = readString(ptr, end);
    int optLevel = readInt32(ptr, end);
    int dbgLevel = readInt32(ptr, end);

    uint32_t numModules = readUint32(ptr, end);
    // Each module record carries at least two zero-length strings (8 bytes);
    // a count beyond the remaining buffer means a corrupt payload. Fail fast
    // instead of looping on out-of-range reads.
    if (numModules > static_cast<uint32_t>((end - ptr) / 8))
    {
        fprintf(stderr, "Jaci Single Binary: Corrupt payload (module count)\n");
        return 1;
    }
    std::vector<DiscoveredModule> modules;
    modules.reserve(numModules);
    for (uint32_t i = 0; i < numModules; ++i)
    {
        DiscoveredModule m;
        m.absolutePath = readString(ptr, end);
        m.source = readString(ptr, end);
        modules.push_back(std::move(m));
    }

    uint32_t numAssets = readUint32(ptr, end);
    // Each asset record carries at least three zero-length strings (12 bytes).
    if (numAssets > static_cast<uint32_t>((end - ptr) / 12))
    {
        fprintf(stderr, "Jaci Single Binary: Corrupt payload (asset count)\n");
        return 1;
    }
    std::vector<DiscoveredAsset> assets;
    assets.reserve(numAssets);
    for (uint32_t i = 0; i < numAssets; ++i)
    {
        DiscoveredAsset a;
        a.relativePath = readString(ptr, end);
        a.absolutePath = readString(ptr, end);
        a.data = readString(ptr, end);
        assets.push_back(std::move(a));
    }

    if (entryAbsPath.empty())
    {
        fprintf(stderr, "Jaci Single Binary: Invalid payload (empty entry path)\n");
        return 1;
    }

    // Register the embedded files. Modules are visible at their canonical
    // absolute paths (what the navigator resolves to); assets are also
    // visible under their build-time relative path, the application's
    // usual lookup key, so reads work from any working directory.
    std::vector<std::pair<std::string, std::string>> embedded;
    embedded.reserve(modules.size() * 2);
    for (const auto& m : modules)
        embedded.emplace_back(m.absolutePath, m.source);
    for (const auto& a : assets)
    {
        embedded.emplace_back(a.absolutePath, a.data);
        if (a.relativePath != a.absolutePath)
            embedded.emplace_back(a.relativePath, a.data);
    }
    VfsLayer::setEmbeddedFiles(std::move(embedded));

    // Initialize the global Luau state.
    lua_State* GL = luaL_newstate();
    if (!GL)
    {
        fprintf(stderr, "Jaci Single Binary: Failed to initialize Luau VM\n");
        return 1;
    }

    if (enableCodegen && Luau::CodeGen::isSupported())
        Luau::CodeGen::create(GL);

    luaL_openlibs(GL);

    lua_pushcfunction(GL, rt_loadstring, "loadstring");
    lua_setglobal(GL, "loadstring");
    lua_pushcfunction(GL, rt_loadstring, "load");
    lua_setglobal(GL, "load");

    // Route fs.* through the VFS-aware file utilities.
    lua_getglobal(GL, "fs");
    if (lua_istable(GL, -1))
    {
        lua_pushcfunction(GL, rt_fs_readfile, "fs.readfile");
        lua_setfield(GL, -2, "readfile");
        lua_pushcfunction(GL, rt_fs_readfile, "fs.readFile");
        lua_setfield(GL, -2, "readFile");
        lua_pushcfunction(GL, rt_fs_exists, "fs.exists");
        lua_setfield(GL, -2, "exists");
        lua_pushcfunction(GL, rt_fs_isfile, "fs.isfile");
        lua_setfield(GL, -2, "isfile");
        lua_pushcfunction(GL, rt_fs_isfile, "fs.isFile");
        lua_setfield(GL, -2, "isFile");
        lua_pushcfunction(GL, rt_fs_isdir, "fs.isdir");
        lua_setfield(GL, -2, "isdir");
        lua_pushcfunction(GL, rt_fs_isdir, "fs.isDir");
        lua_setfield(GL, -2, "isDir");
        lua_pushcfunction(GL, rt_fs_stat, "fs.stat");
        lua_setfield(GL, -2, "stat");
    }
    lua_pop(GL, 1);

    g_rtOptLevel = optLevel;
    g_rtDbgLevel = dbgLevel;
    g_rtCodegen = enableCodegen;

    // The ordinary require stack, exactly as the REPL installs it.
    ReplRequirer requirer{
        rtCompileOptions,
        rtCoverageActive,
        rtCodegenEnabled,
        rtCoverageTrack,
        rtCountersActive,
        rtCountersTrack,
    };
    luaopen_require(GL, requireConfigInit, &requirer);

    // Program arguments: argv[1..] are the program's arguments (argv[0] is
    // the executable itself, never a program argument).
    int programArgc = argc > 1 ? argc - 1 : 0;
    char** programArgv = argv + 1;

    lua_getglobal(GL, "process");
    if (lua_istable(GL, -1))
    {
        lua_createtable(GL, programArgc, 0);
        for (int i = 0; i < programArgc; ++i)
        {
            lua_pushstring(GL, programArgv[i]);
            lua_rawseti(GL, -2, i + 1);
        }
        lua_setfield(GL, -2, "args");
    }
    lua_pop(GL, 1);

    // Run the entry on a fresh resumable thread, exactly like the CLI's
    // file-run path (runCode): the main state stays the main state, the entry
    // is a coroutine the task scheduler can drive, and require from the entry
    // resolves against its @-prefixed absolute chunkname.
    lua_State* L = lua_newthread(GL);
    luaL_sandboxthread(L);

    // arg: [0] is the entry (the "script"), [1..] the program arguments.
    lua_createtable(L, programArgc + 1, 0);
    lua_pushstring(L, entryAbsPath.c_str());
    lua_rawseti(L, -2, 0);
    for (int i = 0; i < programArgc; ++i)
    {
        lua_pushstring(L, programArgv[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setglobal(L, "arg");

    // Load the entry chunk.
    std::optional<std::string> entrySource = readFile(entryAbsPath);
    if (!entrySource)
    {
        fprintf(stderr, "Jaci Single Binary: Error reading entry module '%s'\n", entryAbsPath.c_str());
        lua_close(GL);
        VfsLayer::setEmbeddedFiles({});
        return 1;
    }

    Luau::CompileOptions copts = rtCompileOptions();
    std::string bytecode = Luau::compile(*entrySource, copts);
    std::string chunkName = "@" + entryAbsPath;
    int status = luau_load(L, chunkName.c_str(), bytecode.data(), bytecode.size(), 0);
    if (status != 0)
    {
        fprintf(stderr, "Jaci Single Binary: Error loading entry chunk: %s\n", lua_tostring(L, -1));
        lua_close(GL);
        VfsLayer::setEmbeddedFiles({});
        return 1;
    }

    if (enableCodegen && Luau::CodeGen::isSupported())
    {
        Luau::CodeGen::CompilationOptions nativeOpts;
        Luau::CodeGen::compile(L, -1, nativeOpts);
    }

    for (int i = 0; i < programArgc; ++i)
        lua_pushstring(L, programArgv[i]);

    status = lua_resume(L, NULL, programArgc);
    if (status == 0 || status == LUA_YIELD)
    {
        // Pump the task scheduler: task.spawn/defer work scheduled by the
        // entry complete here, mirroring the CLI's file-run path.
        luaL_runtasks(GL);
        if (status == LUA_YIELD && lua_status(L) == LUA_OK)
            status = 0;
    }

    if (status != 0)
    {
        const char* msg = (status == LUA_YIELD) ? "thread yielded unexpectedly" : lua_tostring(L, -1);
        fprintf(stderr, "Jaci Single Binary: Runtime error: %s\n", msg ? msg : "unknown");
        lua_close(GL);
        VfsLayer::setEmbeddedFiles({});
        return 1;
    }

    lua_close(GL);
    VfsLayer::setEmbeddedFiles({});
    return 0;
}

bool SingleBinaryCompiler::compile(const SingleBinaryOptions& options)
{
    if (options.entryFilePath.empty())
    {
        fprintf(stderr, "SingleBinaryCompiler: No entry file specified\n");
        return false;
    }

    std::string entryAbsPath = normalizePath(options.entryFilePath);
    if (!isAbsolutePath(entryAbsPath))
    {
        if (auto cwd = getCurrentWorkingDirectory())
            entryAbsPath = normalizePath(*cwd + "/" + entryAbsPath);
    }

    std::optional<std::string> entrySource = readFile(entryAbsPath);
    if (!entrySource)
    {
        fprintf(stderr, "SingleBinaryCompiler: Cannot read entry file '%s'\n", entryAbsPath.c_str());
        return false;
    }

    std::vector<DiscoveredModule> modules;
    std::vector<DiscoveredAsset> assets;

    if (!discoverModules(entryAbsPath, options.embedPaths, modules, assets))
    {
        fprintf(stderr, "SingleBinaryCompiler: Failed to discover modules\n");
        return false;
    }

    // Application assets: files and directories embedded verbatim, addressed
    // at runtime by their build-time relative path.
    std::set<std::string> visitedAssetPaths;
    for (const std::string& assetSpec : options.assetPaths)
    {
        if (assetSpec.empty())
            continue;

        std::string resolved = assetSpec;
        if (!isAbsolutePath(resolved))
        {
            if (auto cwd = getCurrentWorkingDirectory())
                resolved = normalizePath(*cwd + "/" + resolved);
            else
                continue;
        }
        resolved = normalizePath(resolved);

        auto addAssetFile = [&](const std::string& absPath, const std::string& relPath)
        {
            if (visitedAssetPaths.count(absPath))
                return;
            visitedAssetPaths.insert(absPath);

            std::optional<std::string> content = readFile(absPath);
            if (!content)
            {
                fprintf(stderr, "SingleBinaryCompiler: Warning: Could not read asset '%s'\n", absPath.c_str());
                return;
            }
            assets.push_back(DiscoveredAsset{relPath, absPath, *content});
        };

        if (isFile(resolved))
        {
            addAssetFile(resolved, assetSpec);
        }
        else if (isDirectory(resolved))
        {
            traverseDirectory(resolved, [&](const std::string& filePath)
                              {
                                  std::string rel = filePath;
                                  if (rel.size() >= resolved.size() && rel.substr(0, resolved.size()) == resolved)
                                  {
                                      std::string sub = rel.substr(resolved.size());
                                      if (!sub.empty() && (sub[0] == '/' || sub[0] == '\\'))
                                          sub = sub.substr(1);
                                      rel = normalizePath(assetSpec + "/" + sub);
                                  }
                                  addAssetFile(normalizePath(filePath), rel);
                              });
        }
        else
        {
            fprintf(stderr, "SingleBinaryCompiler: Warning: Asset path '%s' not found\n", assetSpec.c_str());
        }
    }

    if (options.verbose)
    {
        printf("SingleBinaryCompiler: Bundled %zu module(s) and %zu asset(s) for entry '%s'\n",
               modules.size(), assets.size(), entryAbsPath.c_str());
        for (const auto& m : modules)
            printf("  [module] %s (%zu bytes source)\n", m.absolutePath.c_str(), m.source.size());
        for (const auto& a : assets)
            printf("  [asset]  %s (%zu bytes)\n", a.relativePath.c_str(), a.data.size());
    }

    return compileDirectBundle(options, entryAbsPath, modules, assets);
}

} // namespace Luau
