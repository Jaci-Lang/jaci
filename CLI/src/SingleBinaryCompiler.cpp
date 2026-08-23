// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/SingleBinaryCompiler.h"

#include "Luau/Ast.h"
#include "Luau/CodeGen.h"
#include "Luau/CodeGenOptions.h"
#include "Luau/Compiler.h"
#include "Luau/FileUtils.h"
#include "Luau/Parser.h"
#include "Luau/Require.h"
#include "Luau/VfsCompress.h"
#include "Luau/VfsNavigator.h"
#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
#define getpid _getpid
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace Luau
{

namespace
{

struct DiscoveredModule
{
    std::string chunkName;
    std::string loadName;
    std::string absolutePath;
    std::string source;
    std::string bytecode;
};

struct DiscoveredAsset
{
    std::string path;
    std::string relativePath;
    std::string absolutePath;
    std::string data;
};

static const char kMagicHeader[8] = {'J', 'A', 'C', 'I', 'P', 'K', 'G', '\0'};
static const char kMagicTrailer[8] = {'J', 'A', 'C', 'I', 'P', 'K', 'G', '\0'};
static const size_t kTrailerSize = 24; // uint64_t offset + uint64_t size + 8 bytes magic

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
        val |= (static_cast<uint64_t>(ptr[i]) << (i * 8));
    ptr += 8;
    return val;
}

static std::string readString(const unsigned char*& ptr, const unsigned char* end)
{
    uint32_t len = readUint32(ptr, end);
    if (ptr + len > end)
        return "";
    std::string s(reinterpret_cast<const char*>(ptr), len);
    ptr += len;
    return s;
}

std::string getExecutablePath()
{
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH * 4] = {};
    DWORD len = GetModuleFileNameW(NULL, buffer, sizeof(buffer) / sizeof(buffer[0]));
    if (len > 0)
    {
        int sz = WideCharToMultiByte(CP_UTF8, 0, buffer, len, NULL, 0, NULL, NULL);
        std::string res(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, buffer, len, &res[0], sz, NULL, NULL);
        return normalizePath(res);
    }
#elif defined(__linux__)
    char buffer[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0)
        return normalizePath(std::string(buffer, len));
#elif defined(__APPLE__)
    char buffer[4096] = {};
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0)
        return normalizePath(std::string(buffer));
#endif
    if (const char* env = getenv("JACI_EXE"))
        return normalizePath(std::string(env));
    return "";
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
    snprintf(path, sizeof(path), "%sjaci_single_binary_%d.cpp", dir.c_str(), static_cast<int>(getpid()));
    return std::string(path);
#else
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    char path[512] = {};
    snprintf(path, sizeof(path), "%s/jaci_single_binary_%d.cpp", tmp, static_cast<int>(getpid()));
    return std::string(path);
#endif
}

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

std::string formatHexBytes(const std::string& data)
{
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < data.size(); ++i)
    {
        if (i % 16 == 0)
            oss << "\n    ";
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%02x, ", static_cast<unsigned char>(data[i]));
        oss << buf;
    }
    if (!data.empty())
        oss << "\n";
    oss << "}";
    return oss.str();
}

std::string escapeCString(const std::string& str)
{
    std::string result;
    for (char c : str)
    {
        if (c == '\\')
            result += "\\\\";
        else if (c == '\"')
            result += "\\\"";
        else if (c == '\n')
            result += "\\n";
        else if (c == '\r')
            result += "\\r";
        else if (c == '\t')
            result += "\\t";
        else
            result += c;
    }
    return result;
}

std::string generateRunnerCpp(
    const std::vector<DiscoveredModule>& modules,
    size_t entryIndex,
    const std::vector<DiscoveredAsset>& assets,
    const SingleBinaryOptions& options
)
{
    std::ostringstream out;

    out << "// Auto-generated by Jaci Single Binary Compiler\n";
    out << "// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.\n\n";

    out << "#include \"lua.h\"\n";
    out << "#include \"lualib.h\"\n";
    out << "#include \"Luau/CodeGen.h\"\n";
    out << "#include \"Luau/Compiler.h\"\n";
    out << "#include \"Luau/Require.h\"\n";
    out << "#include \"Luau/FileUtils.h\"\n\n";
    out << "#include <cstdio>\n";
    out << "#include <cstdlib>\n";
    out << "#include <cstring>\n";
    out << "#include <string>\n";
    out << "#include <string_view>\n";
    out << "#include <vector>\n\n";

    // Inlined high-performance zero-dependency LZ decompressor
    out << "static std::string decompressData(const unsigned char* comp, size_t compSz, size_t origSz)\n{\n";
    out << "    if (compSz == 0 || origSz == 0) return \"\";\n";
    out << "    if (comp[0] == '\\0') return std::string(reinterpret_cast<const char*>(comp + 1), compSz - 1);\n";
    out << "    const unsigned char* src = comp + 1;\n";
    out << "    const size_t srcLen = compSz - 1;\n";
    out << "    std::string outBuf;\n";
    out << "    outBuf.resize(origSz);\n";
    out << "    unsigned char* dst = reinterpret_cast<unsigned char*>(&outBuf[0]);\n";
    out << "    size_t ip = 0, op = 0;\n";
    out << "    while (ip < srcLen && op < origSz)\n    {\n";
    out << "        size_t litLen = src[ip++];\n";
    out << "        if (litLen == 255)\n        {\n";
    out << "            while (ip < srcLen && src[ip] == 255) { litLen += 255; ++ip; }\n";
    out << "            if (ip < srcLen) litLen += src[ip++];\n";
    out << "        }\n";
    out << "        if (litLen > 0)\n        {\n";
    out << "            if (ip + litLen > srcLen || op + litLen > origSz) break;\n";
    out << "            memcpy(&dst[op], &src[ip], litLen);\n";
    out << "            ip += litLen;\n";
    out << "            op += litLen;\n";
    out << "        }\n";
    out << "        if (ip + 2 > srcLen) break;\n";
    out << "        uint16_t offset = static_cast<uint16_t>(src[ip] | (src[ip + 1] << 8));\n";
    out << "        ip += 2;\n";
    out << "        if (offset == 0) break;\n";
    out << "        if (ip >= srcLen) break;\n";
    out << "        size_t matchLen = 4 + src[ip++];\n";
    out << "        if (matchLen == 4 + 255)\n        {\n";
    out << "            while (ip < srcLen && src[ip] == 255) { matchLen += 255; ++ip; }\n";
    out << "            if (ip < srcLen) matchLen += src[ip++];\n";
    out << "        }\n";
    out << "        if (offset > op || op + matchLen > origSz) break;\n";
    out << "        size_t ref = op - offset;\n";
    out << "        for (size_t i = 0; i < matchLen; ++i) dst[op + i] = dst[ref + i];\n";
    out << "        op += matchLen;\n";
    out << "    }\n";
    out << "    outBuf.resize(op);\n";
    out << "    return outBuf;\n";
    out << "}\n\n";

    // Embed bytecode arrays for modules
    for (size_t i = 0; i < modules.size(); ++i)
    {
        std::string payload = options.compress ? Luau::Vfs::compress(modules[i].bytecode) : modules[i].bytecode;
        out << "static const unsigned char kModuleData_" << i << "[] = "
            << formatHexBytes(payload) << ";\n\n";
    }

    out << "struct EmbeddedModuleRecord\n{\n";
    out << "    const char* chunkName;\n";
    out << "    const char* loadName;\n";
    out << "    const char* absolutePath;\n";
    out << "    const unsigned char* bytecode;\n";
    out << "    size_t bytecodeSize;\n";
    out << "    size_t originalSize;\n";
    out << "};\n\n";

    out << "static const EmbeddedModuleRecord kEmbeddedModules[] = {\n";
    for (size_t i = 0; i < modules.size(); ++i)
    {
        std::string payload = options.compress ? Luau::Vfs::compress(modules[i].bytecode) : modules[i].bytecode;
        size_t origSize = options.compress ? modules[i].bytecode.size() : 0;
        out << "    { \"" << escapeCString(modules[i].chunkName) << "\", \""
            << escapeCString(modules[i].loadName) << "\", \""
            << escapeCString(modules[i].absolutePath) << "\", "
            << "kModuleData_" << i << ", "
            << payload.size() << ", "
            << origSize << " },\n";
    }
    out << "};\n";
    out << "static const size_t kNumEmbeddedModules = " << modules.size() << ";\n";
    out << "static const size_t kEntryModuleIndex = " << entryIndex << ";\n\n";

    // Embed asset data arrays
    for (size_t i = 0; i < assets.size(); ++i)
    {
        std::string payload = options.compress ? Luau::Vfs::compress(assets[i].data) : assets[i].data;
        out << "static const unsigned char kAssetData_" << i << "[] = "
            << formatHexBytes(payload) << ";\n\n";
    }

    out << "struct EmbeddedAssetRecord\n{\n";
    out << "    const char* path;\n";
    out << "    const char* relativePath;\n";
    out << "    const char* absolutePath;\n";
    out << "    const unsigned char* data;\n";
    out << "    size_t size;\n";
    out << "    size_t originalSize;\n";
    out << "};\n\n";

    out << "static const EmbeddedAssetRecord kEmbeddedAssets[] = {\n";
    for (size_t i = 0; i < assets.size(); ++i)
    {
        std::string payload = options.compress ? Luau::Vfs::compress(assets[i].data) : assets[i].data;
        size_t origSize = options.compress ? assets[i].data.size() : 0;
        out << "    { \"" << escapeCString(assets[i].path) << "\", \""
            << escapeCString(assets[i].relativePath) << "\", \""
            << escapeCString(assets[i].absolutePath) << "\", "
            << "kAssetData_" << i << ", "
            << payload.size() << ", "
            << origSize << " },\n";
    }
    out << "};\n";
    out << "static const size_t kNumEmbeddedAssets = " << assets.size() << ";\n\n";

    out << "static const EmbeddedAssetRecord* findEmbeddedAsset(const char* name)\n{\n";
    out << "    if (!name || !*name || kNumEmbeddedAssets == 0) return nullptr;\n";
    out << "    std::string_view target(name);\n";
    out << "    if (target.size() >= 2 && target[0] == '.' && (target[1] == '/' || target[1] == '\\\\'))\n";
    out << "        target.remove_prefix(2);\n\n";
    out << "    for (size_t i = 0; i < kNumEmbeddedAssets; ++i)\n    {\n";
    out << "        std::string_view p = kEmbeddedAssets[i].path;\n";
    out << "        if (p.size() >= 2 && p[0] == '.' && (p[1] == '/' || p[1] == '\\\\')) p.remove_prefix(2);\n";
    out << "        std::string_view rp = kEmbeddedAssets[i].relativePath;\n";
    out << "        if (rp.size() >= 2 && rp[0] == '.' && (rp[1] == '/' || rp[1] == '\\\\')) rp.remove_prefix(2);\n";
    out << "        if (target == p || target == rp || target == kEmbeddedAssets[i].absolutePath)\n";
    out << "            return &kEmbeddedAssets[i];\n";
    out << "    }\n";
    out << "    for (size_t i = 0; i < kNumEmbeddedAssets; ++i)\n    {\n";
    out << "        std::string_view absPath(kEmbeddedAssets[i].absolutePath);\n";
    out << "        if (absPath.size() >= target.size() && absPath.substr(absPath.size() - target.size()) == target)\n";
    out << "            return &kEmbeddedAssets[i];\n";
    out << "    }\n";
    out << "    return nullptr;\n";
    out << "}\n\n";

    out << "static const EmbeddedModuleRecord* findEmbeddedModule(const char* name)\n{\n";
    out << "    if (!name) return nullptr;\n";
    out << "    std::string_view target(name);\n";
    out << "    if (!target.empty() && target[0] == '@')\n";
    out << "        target.remove_prefix(1);\n\n";
    out << "    for (size_t i = 0; i < kNumEmbeddedModules; ++i)\n    {\n";
    out << "        std::string_view chunk = kEmbeddedModules[i].chunkName;\n";
    out << "        if (!chunk.empty() && chunk[0] == '@') chunk.remove_prefix(1);\n";
    out << "        if (target == chunk || target == kEmbeddedModules[i].loadName || target == kEmbeddedModules[i].absolutePath)\n";
    out << "            return &kEmbeddedModules[i];\n";
    out << "    }\n";
    out << "    for (size_t i = 0; i < kNumEmbeddedModules; ++i)\n    {\n";
    out << "        std::string_view absPath(kEmbeddedModules[i].absolutePath);\n";
    out << "        if (absPath.size() >= target.size() && absPath.substr(absPath.size() - target.size()) == target)\n";
    out << "            return &kEmbeddedModules[i];\n";
    out << "    }\n";
    out << "    return nullptr;\n";
    out << "}\n\n";

    out << "/// Embedded require configuration\n";
    out << "struct EmbeddedRequireContext\n{\n";
    out << "    std::string currentPath;\n";
    out << "    const EmbeddedModuleRecord* currentModule = nullptr;\n";
    out << "};\n\n";

    out << "static bool embedded_is_require_allowed(lua_State* L, void* ctx, const char* requirer_chunkname)\n{\n";
    out << "    return true;\n";
    out << "}\n\n";

    out << "static luarequire_NavigateResult embedded_reset(lua_State* L, void* ctx, const char* requirer_chunkname)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    if (!requirer_chunkname) return NAVIGATE_NOT_FOUND;\n";
    out << "    std::string name = requirer_chunkname;\n";
    out << "    if (!name.empty() && name[0] == '@')\n";
    out << "        name = name.substr(1);\n";
    out << "    req->currentPath = name;\n";
    out << "    req->currentModule = findEmbeddedModule(name.c_str());\n";
    out << "    return NAVIGATE_SUCCESS;\n";
    out << "}\n\n";

    out << "static luarequire_NavigateResult embedded_jump_to_alias(lua_State* L, void* ctx, const char* path)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    if (!path) return NAVIGATE_NOT_FOUND;\n";
    out << "    req->currentPath = path;\n";
    out << "    req->currentModule = findEmbeddedModule(path);\n";
    out << "    return NAVIGATE_SUCCESS;\n";
    out << "}\n\n";

    out << "static luarequire_NavigateResult embedded_to_parent(lua_State* L, void* ctx)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    req->currentModule = nullptr;\n";
    out << "    size_t slash = req->currentPath.find_last_of('/');\n";
    out << "    if (slash == std::string::npos || slash == 0)\n";
    out << "        return NAVIGATE_NOT_FOUND;\n";
    out << "    req->currentPath = req->currentPath.substr(0, slash);\n";
    out << "    return NAVIGATE_SUCCESS;\n";
    out << "}\n\n";

    out << "static luarequire_NavigateResult embedded_to_child(lua_State* L, void* ctx, const char* name)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    req->currentModule = nullptr;\n";
    out << "    if (!name) return NAVIGATE_NOT_FOUND;\n";
    out << "    if (req->currentPath.empty() || req->currentPath.back() == '/')\n";
    out << "        req->currentPath += name;\n";
    out << "    else\n";
    out << "        req->currentPath += std::string(\"/\") + name;\n";
    out << "    return NAVIGATE_SUCCESS;\n";
    out << "}\n\n";

    out << "static bool embedded_is_module_present(lua_State* L, void* ctx)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    if (req->currentModule) return true;\n";
    out << "    const std::string& p = req->currentPath;\n";
    out << "    if (findEmbeddedModule(p.c_str()) ||\n";
    out << "        findEmbeddedModule((p + \".luau\").c_str()) ||\n";
    out << "        findEmbeddedModule((p + \".lua\").c_str()) ||\n";
    out << "        findEmbeddedModule((p + \"/init.luau\").c_str()) ||\n";
    out << "        findEmbeddedModule((p + \"/init.lua\").c_str()) ||\n";
    out << "        findEmbeddedModule((p + \"/index.luau\").c_str()) ||\n";
    out << "        findEmbeddedModule((p + \"/index.lua\").c_str()) ||\n";
    out << "        findEmbeddedAsset(p.c_str()))\n";
    out << "    {\n";
    out << "        return true;\n";
    out << "    }\n";
    out << "    return isFile(p);\n";
    out << "}\n\n";

    out << "static luarequire_WriteResult embedded_get_chunkname(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    std::string name = \"@\" + req->currentPath;\n";
    out << "    size_t sz = name.size() + 1;\n";
    out << "    if (buffer_size < sz) { *size_out = sz; return luarequire_WriteResult::WRITE_BUFFER_TOO_SMALL; }\n";
    out << "    *size_out = sz;\n";
    out << "    memcpy(buffer, name.c_str(), sz);\n";
    out << "    return luarequire_WriteResult::WRITE_SUCCESS;\n";
    out << "}\n\n";

    out << "static luarequire_WriteResult embedded_get_loadname(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    std::string name = req->currentPath;\n";
    out << "    size_t sz = name.size() + 1;\n";
    out << "    if (buffer_size < sz) { *size_out = sz; return luarequire_WriteResult::WRITE_BUFFER_TOO_SMALL; }\n";
    out << "    *size_out = sz;\n";
    out << "    memcpy(buffer, name.c_str(), sz);\n";
    out << "    return luarequire_WriteResult::WRITE_SUCCESS;\n";
    out << "}\n\n";

    out << "static luarequire_WriteResult embedded_get_cache_key(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)\n{\n";
    out << "    return embedded_get_loadname(L, ctx, buffer, buffer_size, size_out);\n";
    out << "}\n\n";

    out << "static luarequire_ConfigStatus embedded_get_config_status(lua_State* L, void* ctx)\n{\n";
    out << "    return CONFIG_ABSENT;\n";
    out << "}\n\n";

    out << "static luarequire_WriteResult embedded_get_config(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)\n{\n";
    out << "    return luarequire_WriteResult::WRITE_FAILURE;\n";
    out << "}\n\n";

    out << "static luarequire_NavigateResult embedded_alias_fallback(lua_State* L, void* ctx, const char* aliasUnprefixed)\n{\n";
    out << "    EmbeddedRequireContext* req = static_cast<EmbeddedRequireContext*>(ctx);\n";
    out << "    if (!aliasUnprefixed) return NAVIGATE_NOT_FOUND;\n";
    out << "    static const char* kPkgDirs[] = {\"klur_modules\", \"luau_packages\", \"packages\", \"node_modules\"};\n";
    out << "    for (const char* dir : kPkgDirs)\n    {\n";
    out << "        std::string cand = std::string(dir) + \"/\" + aliasUnprefixed;\n";
    out << "        if (findEmbeddedModule(cand.c_str()) ||\n";
    out << "            findEmbeddedModule((cand + \".luau\").c_str()) ||\n";
    out << "            findEmbeddedModule((cand + \"/init.luau\").c_str()))\n";
    out << "        {\n";
    out << "            req->currentPath = cand;\n";
    out << "            return NAVIGATE_SUCCESS;\n";
    out << "        }\n";
    out << "    }\n";
    out << "    return NAVIGATE_NOT_FOUND;\n";
    out << "}\n\n";

    out << "static int embedded_load(lua_State* L, void* ctx, const char* path, const char* chunkname, const char* loadname)\n{\n";
    out << "    lua_State* GL = lua_mainthread(L);\n";
    out << "    lua_State* ML = lua_newthread(GL);\n";
    out << "    lua_xmove(GL, L, 1);\n";
    out << "    luaL_sandboxthread(ML);\n\n";
    out << "    const EmbeddedModuleRecord* mod = findEmbeddedModule(loadname);\n";
    out << "    if (!mod) mod = findEmbeddedModule(chunkname);\n";
    out << "    if (!mod) mod = findEmbeddedModule(path);\n";
    out << "    if (!mod && loadname)\n";
    out << "    {\n";
    out << "        std::string s(loadname);\n";
    out << "        mod = findEmbeddedModule((s + \".luau\").c_str());\n";
    out << "        if (!mod) mod = findEmbeddedModule((s + \".lua\").c_str());\n";
    out << "        if (!mod) mod = findEmbeddedModule((s + \"/init.luau\").c_str());\n";
    out << "        if (!mod) mod = findEmbeddedModule((s + \"/init.lua\").c_str());\n";
    out << "        if (!mod) mod = findEmbeddedModule((s + \"/index.luau\").c_str());\n";
    out << "        if (!mod) mod = findEmbeddedModule((s + \"/index.lua\").c_str());\n";
    out << "    }\n\n";
    out << "    int status = LUA_OK;\n";
    out << "    if (mod)\n    {\n";
    out << "        std::string bc = (mod->originalSize > 0) ? decompressData(mod->bytecode, mod->bytecodeSize, mod->originalSize) : std::string(reinterpret_cast<const char*>(mod->bytecode), mod->bytecodeSize);\n";
    out << "        status = luau_load(ML, chunkname, bc.data(), bc.size(), 0);\n";
    out << "    }\n    else\n    {\n";
    out << "        const EmbeddedAssetRecord* asset = findEmbeddedAsset(loadname);\n";
    out << "        if (!asset) asset = findEmbeddedAsset(path);\n";
    out << "        if (asset)\n        {\n";
    out << "            std::string assetData = (asset->originalSize > 0) ? decompressData(asset->data, asset->size, asset->originalSize) : std::string(reinterpret_cast<const char*>(asset->data), asset->size);\n";
    out << "            Luau::CompileOptions copts;\n";
    out << "            copts.optimizationLevel = " << options.optimizationLevel << ";\n";
    out << "            copts.debugLevel = " << options.debugLevel << ";\n";
    out << "            std::string bytecode = Luau::compile(assetData, copts);\n";
    out << "            status = luau_load(ML, chunkname, bytecode.data(), bytecode.size(), 0);\n";
    out << "        }\n        else\n        {\n";
    out << "            std::optional<std::string> contents = readFile(loadname);\n";
    out << "            if (!contents) luaL_error(L, \"could not read module '%s'\", loadname);\n";
    out << "            Luau::CompileOptions copts;\n";
    out << "            copts.optimizationLevel = " << options.optimizationLevel << ";\n";
    out << "            copts.debugLevel = " << options.debugLevel << ";\n";
    out << "            std::string bytecode = Luau::compile(*contents, copts);\n";
    out << "            status = luau_load(ML, chunkname, bytecode.data(), bytecode.size(), 0);\n";
    out << "        }\n    }\n\n";
    out << "    if (status != 0) luaL_error(L, \"failed to load module '%s'\", loadname);\n\n";
    if (options.codegen)
    {
        out << "    Luau::CodeGen::CompilationOptions nativeOptions;\n";
        out << "    Luau::CodeGen::compile(ML, -1, nativeOptions);\n\n";
    }
    out << "    status = lua_resume(ML, L, 0);\n";
    out << "    if (status == 0)\n    {\n";
    out << "        if (lua_gettop(ML) != 1) luaL_error(L, \"module must return a single value\");\n";
    out << "    }\n    else if (status == LUA_YIELD)\n    {\n";
    out << "        luaL_error(L, \"module can not yield\");\n";
    out << "    }\n    else\n    {\n";
    out << "        luaL_error(L, \"error running module: %s\", lua_isstring(ML, -1) ? lua_tostring(ML, -1) : \"unknown\");\n";
    out << "    }\n\n";
    out << "    lua_xmove(ML, L, 1);\n";
    out << "    lua_remove(L, -2);\n";
    out << "    return 1;\n";
    out << "}\n\n";

    out << "static void embeddedRequireConfigInit(luarequire_Configuration* config)\n{\n";
    out << "    config->is_require_allowed = embedded_is_require_allowed;\n";
    out << "    config->reset = embedded_reset;\n";
    out << "    config->jump_to_alias = embedded_jump_to_alias;\n";
    out << "    config->to_parent = embedded_to_parent;\n";
    out << "    config->to_child = embedded_to_child;\n";
    out << "    config->is_module_present = embedded_is_module_present;\n";
    out << "    config->get_config_status = embedded_get_config_status;\n";
    out << "    config->get_chunkname = embedded_get_chunkname;\n";
    out << "    config->get_loadname = embedded_get_loadname;\n";
    out << "    config->get_cache_key = embedded_get_cache_key;\n";
    out << "    config->get_config = embedded_get_config;\n";
    out << "    config->load = embedded_load;\n";
    out << "    config->to_alias_fallback = embedded_alias_fallback;\n";
    out << "}\n\n";

    // Embedded FS Interception Helpers
    out << "static int embedded_fs_readfile(lua_State* L)\n{\n";
    out << "    const char* pathStr = luaL_checkstring(L, 1);\n";
    out << "    const EmbeddedAssetRecord* asset = findEmbeddedAsset(pathStr);\n";
    out << "    if (asset)\n    {\n";
    out << "        std::string d = (asset->originalSize > 0) ? decompressData(asset->data, asset->size, asset->originalSize) : std::string(reinterpret_cast<const char*>(asset->data), asset->size);\n";
    out << "        lua_pushlstring(L, d.data(), d.size());\n";
    out << "        return 1;\n";
    out << "    }\n";
    out << "    std::optional<std::string> fileData = readFile(pathStr);\n";
    out << "    if (!fileData) luaL_error(L, \"fs.readfile: cannot open file: %s\", pathStr);\n";
    out << "    lua_pushlstring(L, fileData->data(), fileData->size());\n";
    out << "    return 1;\n";
    out << "}\n\n";

    out << "static int embedded_fs_exists(lua_State* L)\n{\n";
    out << "    const char* pathStr = luaL_checkstring(L, 1);\n";
    out << "    if (findEmbeddedAsset(pathStr)) { lua_pushboolean(L, 1); return 1; }\n";
    out << "    lua_pushboolean(L, isFile(pathStr) || isDirectory(pathStr));\n";
    out << "    return 1;\n";
    out << "}\n\n";

    out << "static int embedded_fs_isfile(lua_State* L)\n{\n";
    out << "    const char* pathStr = luaL_checkstring(L, 1);\n";
    out << "    if (findEmbeddedAsset(pathStr)) { lua_pushboolean(L, 1); return 1; }\n";
    out << "    lua_pushboolean(L, isFile(pathStr));\n";
    out << "    return 1;\n";
    out << "}\n\n";

    out << "static int embedded_fs_isdir(lua_State* L)\n{\n";
    out << "    const char* pathStr = luaL_checkstring(L, 1);\n";
    out << "    std::string prefix(pathStr);\n";
    out << "    if (prefix.size() >= 2 && prefix[0] == '.' && (prefix[1] == '/' || prefix[1] == '\\\\')) prefix = prefix.substr(2);\n";
    out << "    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\\\') prefix += '/';\n";
    out << "    for (size_t i = 0; i < kNumEmbeddedAssets; ++i)\n    {\n";
    out << "        std::string_view p = kEmbeddedAssets[i].relativePath;\n";
    out << "        if (p.size() >= 2 && p[0] == '.' && (p[1] == '/' || p[1] == '\\\\')) p.remove_prefix(2);\n";
    out << "        if (p.rfind(prefix, 0) == 0) { lua_pushboolean(L, 1); return 1; }\n";
    out << "    }\n";
    out << "    lua_pushboolean(L, isDirectory(pathStr));\n";
    out << "    return 1;\n";
    out << "}\n\n";

    out << "static int embedded_fs_stat(lua_State* L)\n{\n";
    out << "    const char* pathStr = luaL_checkstring(L, 1);\n";
    out << "    const EmbeddedAssetRecord* asset = findEmbeddedAsset(pathStr);\n";
    out << "    if (asset)\n    {\n";
    out << "        size_t actualSize = (asset->originalSize > 0) ? asset->originalSize : asset->size;\n";
    out << "        lua_createtable(L, 0, 4);\n";
    out << "        lua_pushnumber(L, static_cast<double>(actualSize)); lua_setfield(L, -2, \"size\");\n";
    out << "        lua_pushboolean(L, 1); lua_setfield(L, -2, \"is_file\");\n";
    out << "        lua_pushboolean(L, 0); lua_setfield(L, -2, \"is_dir\");\n";
    out << "        lua_pushnumber(L, 0); lua_setfield(L, -2, \"modified\");\n";
    out << "        return 1;\n";
    out << "    }\n";
    out << "    lua_getglobal(L, \"fs\");\n";
    out << "    if (lua_istable(L, -1))\n    {\n";
    out << "        lua_getfield(L, -1, \"stat\");\n";
    out << "        if (lua_iscfunction(L, -1))\n        {\n";
    out << "            lua_pushvalue(L, 1);\n";
    out << "            lua_call(L, 1, 1);\n";
    out << "            lua_remove(L, -2);\n";
    out << "            return 1;\n";
    out << "        }\n";
    out << "    }\n";
    out << "    luaL_error(L, \"fs.stat: cannot stat: %s\", pathStr);\n";
    out << "    return 0;\n";
    out << "}\n\n";

    // Main execution implementation
    out << "int runApplication(int argc, char** argv)\n{\n";
    out << "    lua_State* L = luaL_newstate();\n";
    out << "    if (!L) { fprintf(stderr, \"Failed to initialize Luau VM\\n\"); return 1; }\n\n";
    if (options.codegen)
    {
        out << "    if (Luau::CodeGen::isSupported())\n        Luau::CodeGen::create(L);\n\n";
    }
    out << "    luaL_openlibs(L);\n\n";

    out << "    // Inject embedded filesystem hooks\n";
    out << "    lua_getglobal(L, \"fs\");\n";
    out << "    if (lua_istable(L, -1))\n    {\n";
    out << "        lua_pushcfunction(L, embedded_fs_readfile, \"fs.readfile\");\n";
    out << "        lua_setfield(L, -2, \"readfile\");\n";
    out << "        lua_pushcfunction(L, embedded_fs_readfile, \"fs.readFile\");\n";
    out << "        lua_setfield(L, -2, \"readFile\");\n";
    out << "        lua_pushcfunction(L, embedded_fs_exists, \"fs.exists\");\n";
    out << "        lua_setfield(L, -2, \"exists\");\n";
    out << "        lua_pushcfunction(L, embedded_fs_isfile, \"fs.isfile\");\n";
    out << "        lua_setfield(L, -2, \"isfile\");\n";
    out << "        lua_pushcfunction(L, embedded_fs_isfile, \"fs.isFile\");\n";
    out << "        lua_setfield(L, -2, \"isFile\");\n";
    out << "        lua_pushcfunction(L, embedded_fs_isdir, \"fs.isdir\");\n";
    out << "        lua_setfield(L, -2, \"isdir\");\n";
    out << "        lua_pushcfunction(L, embedded_fs_isdir, \"fs.isDir\");\n";
    out << "        lua_setfield(L, -2, \"isDir\");\n";
    out << "        lua_pushcfunction(L, embedded_fs_stat, \"fs.stat\");\n";
    out << "        lua_setfield(L, -2, \"stat\");\n";
    out << "    }\n";
    out << "    lua_pop(L, 1);\n\n";

    out << "    EmbeddedRequireContext requireCtx;\n";
    out << "    luaopen_require(L, embeddedRequireConfigInit, &requireCtx);\n\n";

    // Setup arg table
    out << "    lua_createtable(L, argc, 0);\n";
    out << "    for (int i = 0; i < argc; ++i)\n    {\n";
    out << "        lua_pushstring(L, argv[i]);\n";
    out << "        lua_rawseti(L, -2, i);\n";
    out << "    }\n";
    out << "    lua_setglobal(L, \"arg\");\n\n";

    // Load entry point module
    out << "    const EmbeddedModuleRecord& entry = kEmbeddedModules[kEntryModuleIndex];\n";
    out << "    std::string entryBytecode = (entry.originalSize > 0) ? decompressData(entry.bytecode, entry.bytecodeSize, entry.originalSize) : std::string(reinterpret_cast<const char*>(entry.bytecode), entry.bytecodeSize);\n";
    out << "    int status = luau_load(L, entry.chunkName, entryBytecode.data(), entryBytecode.size(), 0);\n";
    out << "    if (status != 0)\n    {\n";
    out << "        fprintf(stderr, \"Error loading entry chunk: %s\\n\", lua_tostring(L, -1));\n";
    out << "        lua_close(L);\n";
    out << "        return 1;\n";
    out << "    }\n\n";
    if (options.codegen)
    {
        out << "    if (Luau::CodeGen::isSupported())\n    {\n";
        out << "        Luau::CodeGen::CompilationOptions nativeOpts;\n";
        out << "        Luau::CodeGen::compile(L, -1, nativeOpts);\n";
        out << "    }\n\n";
    }
    out << "    // Run entry module passing argv as varargs\n";
    out << "    for (int i = 1; i < argc; ++i)\n        lua_pushstring(L, argv[i]);\n\n";
    out << "    status = lua_pcall(L, argc > 1 ? argc - 1 : 0, LUA_MULTRET, 0);\n";
    out << "    if (status != 0)\n    {\n";
    out << "        fprintf(stderr, \"Runtime error: %s\\n\", lua_tostring(L, -1));\n";
    out << "        lua_close(L);\n";
    out << "        return 1;\n";
    out << "    }\n\n";
    out << "    lua_close(L);\n";
    out << "    return 0;\n";
    out << "}\n\n";

    // Windows WinMain support for Main Window / GUI applications without console window
    out << "#if defined(_WIN32)\n";
    out << "#ifndef WIN32_LEAN_AND_MEAN\n";
    out << "#define WIN32_LEAN_AND_MEAN\n";
    out << "#endif\n";
    out << "#include <windows.h>\n";
    out << "#include <shellapi.h>\n\n";
    out << "int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)\n{\n";
    out << "    int argc = 0;\n";
    out << "    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);\n";
    out << "    std::vector<std::string> args;\n";
    out << "    std::vector<char*> argv;\n";
    out << "    if (argvW)\n    {\n";
    out << "        args.reserve(argc);\n";
    out << "        argv.reserve(argc + 1);\n";
    out << "        for (int i = 0; i < argc; ++i)\n        {\n";
    out << "            int sz = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, NULL, 0, NULL, NULL);\n";
    out << "            std::string s(sz > 0 ? sz - 1 : 0, 0);\n";
    out << "            if (sz > 0)\n";
    out << "                WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &s[0], sz, NULL, NULL);\n";
    out << "            args.push_back(std::move(s));\n";
    out << "        }\n";
    out << "        LocalFree(argvW);\n";
    out << "    }\n";
    out << "    for (auto& s : args)\n        argv.push_back(&s[0]);\n";
    out << "    argv.push_back(nullptr);\n";
    out << "    return runApplication(static_cast<int>(argv.size()) - 1, argv.data());\n";
    out << "}\n";
    out << "#endif\n\n";

    out << "int main(int argc, char** argv)\n{\n";
    out << "    return runApplication(argc, argv);\n";
    out << "}\n";

    return out.str();
}

static std::string serializePayload(
    const std::vector<DiscoveredModule>& modules,
    size_t entryIndex,
    const std::vector<DiscoveredAsset>& assets,
    const SingleBinaryOptions& options
)
{
    std::string body;
    writeInt32(body, options.optimizationLevel);
    writeInt32(body, options.debugLevel);
    writeUint32(body, static_cast<uint32_t>(entryIndex));

    writeUint32(body, static_cast<uint32_t>(modules.size()));
    for (const auto& mod : modules)
    {
        writeString(body, mod.chunkName);
        writeString(body, mod.loadName);
        writeString(body, mod.absolutePath);
        writeString(body, mod.bytecode);
    }

    writeUint32(body, static_cast<uint32_t>(assets.size()));
    for (const auto& asset : assets)
    {
        writeString(body, asset.path);
        writeString(body, asset.relativePath);
        writeString(body, asset.absolutePath);
        writeString(body, asset.data);
    }

    std::string payload;
    payload.append(kMagicHeader, 8);
    writeUint32(payload, 1); // format version 1

    uint32_t flags = 0;
    if (options.codegen)
        flags |= 1;
    if (options.windowed)
        flags |= 2;
    if (options.verbose)
        flags |= 4;

    if (options.compress)
    {
        flags |= 8;
        writeUint32(payload, flags);
        writeUint64(payload, static_cast<uint64_t>(body.size())); // uncompressed size
        std::string compressedBody = Luau::Vfs::compress(body);
        payload.append(compressedBody);
    }
    else
    {
        writeUint32(payload, flags);
        payload.append(body);
    }

    return payload;
}

static bool compileDirectBundle(
    const SingleBinaryOptions& options,
    const std::vector<DiscoveredModule>& modules,
    size_t entryIndex,
    const std::vector<DiscoveredAsset>& assets
)
{
    std::string stubPath = options.customStubPath;
    if (stubPath.empty())
    {
        if (const char* envStub = getenv("JACI_RUNNER_STUB"))
            stubPath = envStub;
    }
    if (stubPath.empty() || !isFile(stubPath))
    {
        std::string rootDir = "/home/klee/Documentos/jaci";
        if (const char* envRoot = getenv("JACI_ROOT"))
            rootDir = envRoot;
        std::string buildDir = rootDir + "/build";
        if (const char* envBuild = getenv("JACI_BUILD"))
            buildDir = envBuild;

        static const char* kCandidates[] = {
            "/luau", "/luau.exe", "/build/luau", "/build/luau.exe"
        };
        for (const char* cand : kCandidates)
        {
            std::string c1 = buildDir + cand;
            if (isFile(c1)) { stubPath = c1; break; }
            std::string c2 = rootDir + cand;
            if (isFile(c2)) { stubPath = c2; break; }
        }
    }
    if (stubPath.empty() || !isFile(stubPath))
    {
        stubPath = getExecutablePath();
    }

    if (stubPath.empty() || !isFile(stubPath))
    {
        fprintf(stderr, "SingleBinaryCompiler: Could not locate base executable / runner stub for standalone packaging.\n");
        return false;
    }

    std::optional<std::string> baseExeData = readFile(stubPath);
    if (!baseExeData || baseExeData->empty())
    {
        fprintf(stderr, "SingleBinaryCompiler: Failed to read base executable '%s'\n", stubPath.c_str());
        return false;
    }

    // If base executable already has an appended bundle, strip it to start from clean base executable
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

    std::string payload = serializePayload(modules, entryIndex, assets, options);
    std::string outPath = options.outputBinaryPath.empty() ? "a.out" : options.outputBinaryPath;

    if (normalizePath(outPath) == normalizePath(stubPath))
    {
        fprintf(stderr, "SingleBinaryCompiler: Output binary path '%s' cannot overwrite the active executable in-place.\n", outPath.c_str());
        return false;
    }

    FILE* fp = fopen(outPath.c_str(), "wb");
    if (!fp)
    {
        fprintf(stderr, "SingleBinaryCompiler: Cannot open output binary '%s' for writing\n", outPath.c_str());
        return false;
    }

    // 1. Write base executable bytes
    if (fwrite(finalBaseData.data(), 1, baseSize, fp) != baseSize)
    {
        fclose(fp);
        fprintf(stderr, "SingleBinaryCompiler: Failed to write base executable bytes to '%s'\n", outPath.c_str());
        return false;
    }

    // 2. Write payload bytes
    uint64_t payloadOffset = static_cast<uint64_t>(baseSize);
    uint64_t payloadSize = static_cast<uint64_t>(payload.size());

    if (fwrite(payload.data(), 1, payload.size(), fp) != payload.size())
    {
        fclose(fp);
        fprintf(stderr, "SingleBinaryCompiler: Failed to write bundle payload to '%s'\n", outPath.c_str());
        return false;
    }

    // 3. Write trailer
    std::string trailer;
    writeUint64(trailer, payloadOffset);
    writeUint64(trailer, payloadSize);
    trailer.append(kMagicTrailer, 8);

    if (fwrite(trailer.data(), 1, trailer.size(), fp) != trailer.size())
    {
        fclose(fp);
        fprintf(stderr, "SingleBinaryCompiler: Failed to write bundle trailer to '%s'\n", outPath.c_str());
        return false;
    }

    fclose(fp);

#if !defined(_WIN32)
    chmod(outPath.c_str(), 0755);
#endif

    if (options.verbose)
    {
        printf("SingleBinaryCompiler: Packaged standalone binary '%s' (base: %zu bytes, payload: %zu bytes)\n",
               outPath.c_str(), baseSize, payload.size());
    }

    return true;
}

static bool compileNativeRunner(
    const SingleBinaryOptions& options,
    const std::vector<DiscoveredModule>& modules,
    size_t entryIndex,
    const std::vector<DiscoveredAsset>& assets
)
{
    std::string runnerCode = generateRunnerCpp(
        modules,
        entryIndex,
        assets,
        options
    );

    std::string tempPath = getTempRunnerPath();

    FILE* fp = fopen(tempPath.c_str(), "w");
    if (!fp)
    {
        fprintf(stderr, "SingleBinaryCompiler: Could not open temporary runner file '%s'\n", tempPath.c_str());
        return false;
    }
    fwrite(runnerCode.data(), 1, runnerCode.size(), fp);
    fclose(fp);

    std::string rootDir = "/home/klee/Documentos/jaci";
    if (const char* envRoot = getenv("JACI_ROOT"))
        rootDir = envRoot;

    std::string buildDir = rootDir + "/build";
    if (const char* envBuild = getenv("JACI_BUILD"))
        buildDir = envBuild;

    std::string outPath = options.outputBinaryPath.empty() ? "a.out" : options.outputBinaryPath;

    std::string compiler = "c++";
    bool isMSVC = false;
    bool isWindowsTarget = false;
    bool isAppleTarget = false;

    if (!options.compilerCommand.empty())
    {
        compiler = options.compilerCommand;
    }
    else if (!options.targetArchitecture.empty())
    {
        const std::string& tgt = options.targetArchitecture;
        if (tgt == "linux-arm64" || tgt == "aarch64-linux" || tgt == "aarch64-linux-gnu" || tgt == "arm64")
        {
            compiler = "aarch64-linux-gnu-g++";
        }
        else if (tgt == "windows-x64" || tgt == "x86_64-w64-mingw32" || tgt == "win64" || tgt == "windows" || tgt == "x86_64-windows")
        {
#if defined(_WIN32)
            compiler = "cl.exe";
            isMSVC = true;
#else
            compiler = "x86_64-w64-mingw32-g++";
#endif
            isWindowsTarget = true;
        }
        else if (tgt == "windows-msvc" || tgt == "msvc" || tgt == "cl")
        {
            compiler = "cl.exe";
            isMSVC = true;
            isWindowsTarget = true;
        }
        else if (tgt == "windows-gui" || tgt == "windows-windowed" || tgt == "win64-gui")
        {
#if defined(_WIN32)
            compiler = "cl.exe";
            isMSVC = true;
#else
            compiler = "x86_64-w64-mingw32-g++";
#endif
            isWindowsTarget = true;
        }
        else if (tgt == "linux-x64" || tgt == "x86_64-linux" || tgt == "x86_64-linux-gnu" || tgt == "x64")
        {
            compiler = "x86_64-linux-gnu-g++";
        }
        else if (tgt == "macos-arm64" || tgt == "darwin-arm64" || tgt == "macos" || tgt == "darwin")
        {
            compiler = "clang++ -target arm64-apple-macos";
            isAppleTarget = true;
        }
        else if (tgt == "macos-x64" || tgt == "darwin-x64")
        {
            compiler = "clang++ -target x86_64-apple-macos";
            isAppleTarget = true;
        }
        else
        {
            compiler = tgt;
        }
    }
#if defined(_WIN32)
    else
    {
        compiler = "cl.exe";
        isMSVC = true;
        isWindowsTarget = true;
    }
#endif

    // Detect compiler flavor from compiler binary name
    std::string compilerBinary = compiler;
    size_t spacePos = compilerBinary.find(' ');
    if (spacePos != std::string::npos)
        compilerBinary = compilerBinary.substr(0, spacePos);

    std::string compLower = compilerBinary;
    std::transform(compLower.begin(), compLower.end(), compLower.begin(), [](unsigned char c) { return tolower(c); });

    if (compLower == "cl" || compLower == "cl.exe" || compLower.find("clang-cl") != std::string::npos)
    {
        isMSVC = true;
        isWindowsTarget = true;
    }
    else if (compLower.find("mingw") != std::string::npos || compLower.find("windows") != std::string::npos)
    {
        isWindowsTarget = true;
    }
    else if (compLower.find("apple") != std::string::npos || compLower.find("darwin") != std::string::npos)
    {
        isAppleTarget = true;
    }

    // Verify compiler binary availability in PATH
#if defined(_WIN32)
    std::string checkCmd = "where " + compilerBinary + " >nul 2>nul";
#else
    std::string checkCmd = "command -v " + compilerBinary + " >/dev/null 2>&1";
#endif

    if (system(checkCmd.c_str()) != 0)
    {
        if (options.targetArchitecture == "linux-x64" || options.targetArchitecture == "x64" || options.targetArchitecture.empty())
        {
            compiler = "c++";
            isMSVC = false;
        }
        else
        {
            fprintf(stderr, "SingleBinaryCompiler: Toolchain compiler '%s' not found in PATH for target '%s'.\n",
                    compilerBinary.c_str(), options.targetArchitecture.c_str());
            fprintf(stderr, "Please install the required cross-compilation toolchain or use direct bundling mode (--direct).\n");
            remove(tempPath.c_str());
            return false;
        }
    }

    std::ostringstream cmd;
    if (isMSVC)
    {
        std::string optFlag = options.optimizeForSize ? "/O1 /Os" : "/O2";
        cmd << compiler << " /nologo " << optFlag << " /std:c++17 /EHsc /MD /Gy /GR- "
            << "/I\"" << rootDir << "/VM/include\" "
            << "/I\"" << rootDir << "/Common/include\" "
            << "/I\"" << rootDir << "/Ast/include\" "
            << "/I\"" << rootDir << "/Compiler/include\" "
            << "/I\"" << rootDir << "/CodeGen/include\" "
            << "/I\"" << rootDir << "/Inliner/include\" "
            << "/I\"" << rootDir << "/Config/include\" "
            << "/I\"" << rootDir << "/Require/include\" "
            << "/I\"" << rootDir << "/CLI/include\" "
            << "\"" << tempPath << "\" "
            << "\"" << rootDir << "/CLI/src/FileUtils.cpp\" "
            << "/Fe:\"" << outPath << "\" "
            << "/link "
            << "/LIBPATH:\"" << buildDir << "\" "
            << "/OPT:REF /OPT:ICF ";
        if (options.strip)
            cmd << "/INCREMENTAL:NO ";
        cmd << "Luau.Require.lib Luau.CodeGen.lib Luau.Compiler.lib "
            << "Luau.Bytecode.lib Luau.Inliner.lib Luau.VM.lib Luau.Config.lib Luau.Ast.lib Luau.Common.lib "
            << "ws2_32.lib bcrypt.lib user32.lib shell32.lib advapi32.lib ";
        if (options.windowed)
            cmd << "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup";
        else
            cmd << "/SUBSYSTEM:CONSOLE";
    }
    else
    {
        std::string optFlag = options.optimizeForSize ? "-Os" : "-O2";
        cmd << compiler << " -std=c++17 " << optFlag << " -ffunction-sections -fdata-sections -fno-rtti -fvisibility=hidden "
            << "-I\"" << rootDir << "/VM/include\" "
            << "-I\"" << rootDir << "/Common/include\" "
            << "-I\"" << rootDir << "/Ast/include\" "
            << "-I\"" << rootDir << "/Compiler/include\" "
            << "-I\"" << rootDir << "/CodeGen/include\" "
            << "-I\"" << rootDir << "/Inliner/include\" "
            << "-I\"" << rootDir << "/Config/include\" "
            << "-I\"" << rootDir << "/Require/include\" "
            << "-I\"" << rootDir << "/CLI/include\" "
            << "\"" << tempPath << "\" "
            << "\"" << rootDir << "/CLI/src/FileUtils.cpp\" "
            << "-o \"" << outPath << "\" "
            << "-L\"" << buildDir << "\" ";

        if (options.strip)
            cmd << "-s ";

        if (isAppleTarget)
        {
            cmd << "-Wl,-dead_strip "
                << "-lLuau.Require -lLuau.CodeGen -lLuau.Compiler "
                << "-lLuau.Bytecode -lLuau.Inliner -lLuau.VM -lLuau.Config -lLuau.Ast -lLuau.Common "
                << "-framework CoreFoundation -lpthread -lm";
        }
        else if (isWindowsTarget)
        {
            cmd << "-Wl,--gc-sections "
                << "-lLuau.Require -lLuau.CodeGen -lLuau.Compiler "
                << "-lLuau.Bytecode -lLuau.Inliner -lLuau.VM -lLuau.Config -lLuau.Ast -lLuau.Common "
                << "-lws2_32 -lbcrypt -luser32 -lshell32 -ladvapi32 ";
            if (options.windowed)
                cmd << "-mwindows ";
            cmd << "-lpthread -lm";
        }
        else
        {
            cmd << "-Wl,--gc-sections -Wl,--start-group "
                << "-lLuau.Require -lLuau.CodeGen -lLuau.Compiler "
                << "-lLuau.Bytecode -lLuau.Inliner -lLuau.VM -lLuau.Config -lLuau.Ast -lLuau.Common "
                << "-Wl,--end-group "
                << "-ldl -lpthread -lm";
        }
    }

    if (options.verbose)
        printf("SingleBinaryCompiler: Executing: %s\n", cmd.str().c_str());

    int ret = system(cmd.str().c_str());
    remove(tempPath.c_str());

    if (ret != 0)
    {
        fprintf(stderr, "SingleBinaryCompiler: Compilation failed with exit code %d\n", ret);
        return false;
    }

    if (options.verbose)
        printf("SingleBinaryCompiler: Successfully generated standalone binary '%s'\n", outPath.c_str());

    return true;
}

// Runtime Context for in-memory embedded execution
struct EmbeddedRuntimeContext
{
    std::vector<DiscoveredModule> modules;
    std::vector<DiscoveredAsset> assets;
    size_t entryIndex = 0;
    int optimizationLevel = 1;
    int debugLevel = 1;
    bool enableCodegen = true;
    std::string currentPath;
    const DiscoveredModule* currentModule = nullptr;

    const DiscoveredModule* findModule(const char* name) const
    {
        if (!name) return nullptr;
        std::string_view target(name);
        if (!target.empty() && target[0] == '@') target.remove_prefix(1);
        for (const auto& m : modules)
        {
            std::string_view chunk = m.chunkName;
            if (!chunk.empty() && chunk[0] == '@') chunk.remove_prefix(1);
            if (target == chunk || target == m.loadName || target == m.absolutePath)
                return &m;
        }
        for (const auto& m : modules)
        {
            std::string_view absPath(m.absolutePath);
            if (absPath.size() >= target.size() && absPath.substr(absPath.size() - target.size()) == target)
                return &m;
        }
        return nullptr;
    }

    const DiscoveredAsset* findAsset(const char* name) const
    {
        if (!name || !*name || assets.empty()) return nullptr;
        std::string_view target(name);
        if (target.size() >= 2 && target[0] == '.' && (target[1] == '/' || target[1] == '\\'))
            target.remove_prefix(2);
        for (const auto& a : assets)
        {
            std::string_view p = a.path;
            if (p.size() >= 2 && p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p.remove_prefix(2);
            std::string_view rp = a.relativePath;
            if (rp.size() >= 2 && rp[0] == '.' && (rp[1] == '/' || rp[1] == '\\')) rp.remove_prefix(2);
            if (target == p || target == rp || target == a.absolutePath)
                return &a;
        }
        for (const auto& a : assets)
        {
            std::string_view absPath(a.absolutePath);
            if (absPath.size() >= target.size() && absPath.substr(absPath.size() - target.size()) == target)
                return &a;
        }
        return nullptr;
    }
};

static EmbeddedRuntimeContext* g_ActiveRuntimeContext = nullptr;

static bool rt_is_require_allowed(lua_State* L, void* ctx, const char* requirer_chunkname)
{
    return true;
}

static luarequire_NavigateResult rt_reset(lua_State* L, void* ctx, const char* requirer_chunkname)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    if (!requirer_chunkname) return NAVIGATE_NOT_FOUND;
    std::string name = requirer_chunkname;
    if (!name.empty() && name[0] == '@')
        name = name.substr(1);
    req->currentPath = name;
    req->currentModule = req->findModule(name.c_str());
    return NAVIGATE_SUCCESS;
}

static luarequire_NavigateResult rt_jump_to_alias(lua_State* L, void* ctx, const char* path)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    if (!path) return NAVIGATE_NOT_FOUND;
    req->currentPath = path;
    req->currentModule = req->findModule(path);
    return NAVIGATE_SUCCESS;
}

static luarequire_NavigateResult rt_to_parent(lua_State* L, void* ctx)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    req->currentModule = nullptr;
    size_t slash = req->currentPath.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return NAVIGATE_NOT_FOUND;
    req->currentPath = req->currentPath.substr(0, slash);
    return NAVIGATE_SUCCESS;
}

static luarequire_NavigateResult rt_to_child(lua_State* L, void* ctx, const char* name)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    req->currentModule = nullptr;
    if (!name) return NAVIGATE_NOT_FOUND;
    if (req->currentPath.empty() || req->currentPath.back() == '/')
        req->currentPath += name;
    else
        req->currentPath += std::string("/") + name;
    return NAVIGATE_SUCCESS;
}

static bool rt_is_module_present(lua_State* L, void* ctx)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    if (req->currentModule) return true;
    const std::string& p = req->currentPath;
    if (req->findModule(p.c_str()) ||
        req->findModule((p + ".luau").c_str()) ||
        req->findModule((p + ".lua").c_str()) ||
        req->findModule((p + "/init.luau").c_str()) ||
        req->findModule((p + "/init.lua").c_str()) ||
        req->findModule((p + "/index.luau").c_str()) ||
        req->findModule((p + "/index.lua").c_str()) ||
        req->findAsset(p.c_str()))
    {
        return true;
    }
    return isFile(p);
}

static luarequire_WriteResult rt_get_chunkname(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    std::string name = "@" + req->currentPath;
    size_t sz = name.size() + 1;
    if (buffer_size < sz) { *size_out = sz; return luarequire_WriteResult::WRITE_BUFFER_TOO_SMALL; }
    *size_out = sz;
    memcpy(buffer, name.c_str(), sz);
    return luarequire_WriteResult::WRITE_SUCCESS;
}

static luarequire_WriteResult rt_get_loadname(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    const DiscoveredModule* mod = req->findModule(req->currentPath.c_str());
    if (!mod) mod = req->findModule((req->currentPath + ".luau").c_str());
    if (!mod) mod = req->findModule((req->currentPath + "/init.luau").c_str());

    std::string name = mod ? mod->loadName : req->currentPath;
    size_t sz = name.size() + 1;
    if (buffer_size < sz) { *size_out = sz; return luarequire_WriteResult::WRITE_BUFFER_TOO_SMALL; }
    *size_out = sz;
    memcpy(buffer, name.c_str(), sz);
    return luarequire_WriteResult::WRITE_SUCCESS;
}

static luarequire_WriteResult rt_get_cache_key(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    return rt_get_loadname(L, ctx, buffer, buffer_size, size_out);
}

static luarequire_ConfigStatus rt_get_config_status(lua_State* L, void* ctx)
{
    return CONFIG_ABSENT;
}

static luarequire_WriteResult rt_get_config(lua_State* L, void* ctx, char* buffer, size_t buffer_size, size_t* size_out)
{
    return luarequire_WriteResult::WRITE_FAILURE;
}

static luarequire_NavigateResult rt_alias_fallback(lua_State* L, void* ctx, const char* aliasUnprefixed)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    if (!aliasUnprefixed) return NAVIGATE_NOT_FOUND;
    static const char* kPkgDirs[] = {"klur_modules", "luau_packages", "packages", "node_modules"};
    for (const char* dir : kPkgDirs)
    {
        std::string cand1 = std::string(dir) + "/@" + aliasUnprefixed;
        std::string cand2 = std::string(dir) + "/" + aliasUnprefixed;
        for (const std::string& cand : {cand1, cand2})
        {
            if (req->findModule(cand.c_str()) ||
                req->findModule((cand + ".luau").c_str()) ||
                req->findModule((cand + "/init.luau").c_str()))
            {
                req->currentPath = cand;
                return NAVIGATE_SUCCESS;
            }
            for (const auto& m : req->modules)
            {
                if (m.loadName.rfind(cand + "/", 0) == 0 ||
                    m.chunkName.rfind(cand + "/", 0) == 0 ||
                    m.absolutePath.rfind(cand + "/", 0) == 0)
                {
                    req->currentPath = cand;
                    return NAVIGATE_SUCCESS;
                }
            }
        }
    }
    return NAVIGATE_NOT_FOUND;
}

static int rt_load(lua_State* L, void* ctx, const char* path, const char* chunkname, const char* loadname)
{
    EmbeddedRuntimeContext* req = static_cast<EmbeddedRuntimeContext*>(ctx);
    lua_State* GL = lua_mainthread(L);
    lua_State* ML = lua_newthread(GL);
    lua_xmove(GL, L, 1);
    luaL_sandboxthread(ML);

    const DiscoveredModule* mod = req->findModule(loadname);
    if (!mod) mod = req->findModule(chunkname);
    if (!mod) mod = req->findModule(path);
    if (!mod && loadname)
    {
        std::string s(loadname);
        mod = req->findModule((s + ".luau").c_str());
        if (!mod) mod = req->findModule((s + ".lua").c_str());
        if (!mod) mod = req->findModule((s + "/init.luau").c_str());
        if (!mod) mod = req->findModule((s + "/init.lua").c_str());
        if (!mod) mod = req->findModule((s + "/index.luau").c_str());
        if (!mod) mod = req->findModule((s + "/index.lua").c_str());
    }

    int status = LUA_OK;
    if (mod)
    {
        status = luau_load(ML, chunkname, reinterpret_cast<const char*>(mod->bytecode.data()), mod->bytecode.size(), 0);
    }
    else
    {
        const DiscoveredAsset* asset = req->findAsset(loadname);
        if (!asset) asset = req->findAsset(path);
        if (asset)
        {
            Luau::CompileOptions copts;
            copts.optimizationLevel = req->optimizationLevel;
            copts.debugLevel = req->debugLevel;
            std::string bytecode = Luau::compile(asset->data, copts);
            status = luau_load(ML, chunkname, bytecode.data(), bytecode.size(), 0);
        }
        else
        {
            std::optional<std::string> contents = readFile(loadname);
            if (!contents) luaL_error(L, "could not read module '%s'", loadname);
            Luau::CompileOptions copts;
            copts.optimizationLevel = req->optimizationLevel;
            copts.debugLevel = req->debugLevel;
            std::string bytecode = Luau::compile(*contents, copts);
            status = luau_load(ML, chunkname, bytecode.data(), bytecode.size(), 0);
        }
    }

    if (status != 0) luaL_error(L, "failed to load module '%s'", loadname);

    if (req->enableCodegen)
    {
        Luau::CodeGen::CompilationOptions nativeOptions;
        Luau::CodeGen::compile(ML, -1, nativeOptions);
    }

    status = lua_resume(ML, L, 0);
    if (status == 0)
    {
        if (lua_gettop(ML) != 1) luaL_error(L, "module must return a single value");
    }
    else if (status == LUA_YIELD)
    {
        luaL_error(L, "module can not yield");
    }
    else
    {
        luaL_error(L, "error running module: %s", lua_isstring(ML, -1) ? lua_tostring(ML, -1) : "unknown");
    }

    lua_xmove(ML, L, 1);
    lua_remove(L, -2);
    return 1;
}

static void rtRequireConfigInit(luarequire_Configuration* config)
{
    config->is_require_allowed = rt_is_require_allowed;
    config->reset = rt_reset;
    config->jump_to_alias = rt_jump_to_alias;
    config->to_parent = rt_to_parent;
    config->to_child = rt_to_child;
    config->is_module_present = rt_is_module_present;
    config->get_config_status = rt_get_config_status;
    config->get_chunkname = rt_get_chunkname;
    config->get_loadname = rt_get_loadname;
    config->get_cache_key = rt_get_cache_key;
    config->get_config = rt_get_config;
    config->load = rt_load;
    config->to_alias_fallback = rt_alias_fallback;
}

static int rt_fs_readfile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    if (g_ActiveRuntimeContext)
    {
        const DiscoveredAsset* asset = g_ActiveRuntimeContext->findAsset(pathStr);
        if (asset)
        {
            lua_pushlstring(L, asset->data.data(), asset->data.size());
            return 1;
        }
    }
    std::optional<std::string> fileData = readFile(pathStr);
    if (!fileData) luaL_error(L, "fs.readfile: cannot open file: %s", pathStr);
    lua_pushlstring(L, fileData->data(), fileData->size());
    return 1;
}

static int rt_fs_exists(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    if (g_ActiveRuntimeContext && g_ActiveRuntimeContext->findAsset(pathStr))
    {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, isFile(pathStr) || isDirectory(pathStr));
    return 1;
}

static int rt_fs_isfile(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    if (g_ActiveRuntimeContext && g_ActiveRuntimeContext->findAsset(pathStr))
    {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, isFile(pathStr));
    return 1;
}

static int rt_fs_isdir(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    if (g_ActiveRuntimeContext)
    {
        std::string prefix(pathStr);
        if (prefix.size() >= 2 && prefix[0] == '.' && (prefix[1] == '/' || prefix[1] == '\\')) prefix = prefix.substr(2);
        if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\') prefix += '/';
        for (const auto& a : g_ActiveRuntimeContext->assets)
        {
            std::string_view p = a.relativePath;
            if (p.size() >= 2 && p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p.remove_prefix(2);
            if (p.rfind(prefix, 0) == 0) { lua_pushboolean(L, 1); return 1; }
        }
    }
    lua_pushboolean(L, isDirectory(pathStr));
    return 1;
}

static int rt_fs_stat(lua_State* L)
{
    const char* pathStr = luaL_checkstring(L, 1);
    if (g_ActiveRuntimeContext)
    {
        const DiscoveredAsset* asset = g_ActiveRuntimeContext->findAsset(pathStr);
        if (asset)
        {
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, static_cast<double>(asset->data.size())); lua_setfield(L, -2, "size");
            lua_pushboolean(L, 1); lua_setfield(L, -2, "isFile");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "isDirectory");
            lua_pushboolean(L, 1); lua_setfield(L, -2, "exists");
            return 1;
        }
    }
    if (isFile(pathStr))
    {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, 0); lua_setfield(L, -2, "size");
        lua_pushboolean(L, 1); lua_setfield(L, -2, "isFile");
        lua_pushboolean(L, 0); lua_setfield(L, -2, "isDirectory");
        lua_pushboolean(L, 1); lua_setfield(L, -2, "exists");
        return 1;
    }
    if (isDirectory(pathStr))
    {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, 0); lua_setfield(L, -2, "size");
        lua_pushboolean(L, 0); lua_setfield(L, -2, "isFile");
        lua_pushboolean(L, 1); lua_setfield(L, -2, "isDirectory");
        lua_pushboolean(L, 1); lua_setfield(L, -2, "exists");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

} // namespace

std::optional<int> SingleBinaryCompiler::checkAndRunBundledPayload(int argc, char** argv)
{
    std::string exePath = getExecutablePath();
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
    if (version != 1)
        return std::nullopt;

    uint32_t flags = readUint32(ptr, end);
    bool enableCodegen = (flags & 1) != 0;
    bool isCompressed = (flags & 8) != 0;

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

    int optLevel = readInt32(ptr, end);
    int dbgLevel = readInt32(ptr, end);
    uint32_t entryIndex = readUint32(ptr, end);

    uint32_t numModules = readUint32(ptr, end);
    std::vector<DiscoveredModule> modules;
    modules.reserve(numModules);
    for (uint32_t i = 0; i < numModules; ++i)
    {
        DiscoveredModule m;
        m.chunkName = readString(ptr, end);
        m.loadName = readString(ptr, end);
        m.absolutePath = readString(ptr, end);
        m.bytecode = readString(ptr, end);
        modules.push_back(std::move(m));
    }

    uint32_t numAssets = readUint32(ptr, end);
    std::vector<DiscoveredAsset> assets;
    assets.reserve(numAssets);
    for (uint32_t i = 0; i < numAssets; ++i)
    {
        DiscoveredAsset a;
        a.path = readString(ptr, end);
        a.relativePath = readString(ptr, end);
        a.absolutePath = readString(ptr, end);
        a.data = readString(ptr, end);
        assets.push_back(std::move(a));
    }

    if (entryIndex >= modules.size())
    {
        fprintf(stderr, "Jaci Single Binary: Invalid entry module index\n");
        return 1;
    }

    // Initialize Luau state
    lua_State* L = luaL_newstate();
    if (!L)
    {
        fprintf(stderr, "Jaci Single Binary: Failed to initialize Luau VM\n");
        return 1;
    }

    if (enableCodegen && Luau::CodeGen::isSupported())
        Luau::CodeGen::create(L);

    luaL_openlibs(L);

    // Setup active runtime context
    EmbeddedRuntimeContext rtCtx;
    rtCtx.modules = std::move(modules);
    rtCtx.assets = std::move(assets);
    rtCtx.entryIndex = entryIndex;
    rtCtx.enableCodegen = enableCodegen;
    rtCtx.optimizationLevel = optLevel;
    rtCtx.debugLevel = dbgLevel;
    g_ActiveRuntimeContext = &rtCtx;

    // Inject embedded filesystem hooks
    lua_getglobal(L, "fs");
    if (lua_istable(L, -1))
    {
        lua_pushcfunction(L, rt_fs_readfile, "fs.readfile");
        lua_setfield(L, -2, "readfile");
        lua_pushcfunction(L, rt_fs_readfile, "fs.readFile");
        lua_setfield(L, -2, "readFile");
        lua_pushcfunction(L, rt_fs_exists, "fs.exists");
        lua_setfield(L, -2, "exists");
        lua_pushcfunction(L, rt_fs_isfile, "fs.isfile");
        lua_setfield(L, -2, "isfile");
        lua_pushcfunction(L, rt_fs_isfile, "fs.isFile");
        lua_setfield(L, -2, "isFile");
        lua_pushcfunction(L, rt_fs_isdir, "fs.isdir");
        lua_setfield(L, -2, "isdir");
        lua_pushcfunction(L, rt_fs_isdir, "fs.isDir");
        lua_setfield(L, -2, "isDir");
        lua_pushcfunction(L, rt_fs_stat, "fs.stat");
        lua_setfield(L, -2, "stat");
    }
    lua_pop(L, 1);

    luaopen_require(L, rtRequireConfigInit, &rtCtx);

    // Setup arg table
    lua_createtable(L, argc, 0);
    for (int i = 0; i < argc; ++i)
    {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");

    lua_getglobal(L, "process");
    if (lua_istable(L, -1))
    {
        lua_createtable(L, argc, 0);
        for (int i = 0; i < argc; ++i)
        {
            lua_pushstring(L, argv[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "args");
    }
    lua_pop(L, 1);

    // Load entry chunk
    const DiscoveredModule& entry = rtCtx.modules[rtCtx.entryIndex];
    int status = luau_load(L, entry.chunkName.c_str(), entry.bytecode.data(), entry.bytecode.size(), 0);
    if (status != 0)
    {
        fprintf(stderr, "Jaci Single Binary: Error loading entry chunk: %s\n", lua_tostring(L, -1));
        lua_close(L);
        g_ActiveRuntimeContext = nullptr;
        return 1;
    }

    if (enableCodegen && Luau::CodeGen::isSupported())
    {
        Luau::CodeGen::CompilationOptions nativeOpts;
        Luau::CodeGen::compile(L, -1, nativeOpts);
    }

    for (int i = 1; i < argc; ++i)
        lua_pushstring(L, argv[i]);

    status = lua_pcall(L, argc > 1 ? argc - 1 : 0, LUA_MULTRET, 0);
    if (status != 0)
    {
        fprintf(stderr, "Jaci Single Binary: Runtime error: %s\n", lua_tostring(L, -1));
        lua_close(L);
        g_ActiveRuntimeContext = nullptr;
        return 1;
    }

    lua_close(L);
    g_ActiveRuntimeContext = nullptr;
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

    // Traverse Luau module dependencies
    std::vector<DiscoveredModule> modules;
    std::set<std::string> visitedAbsPaths;
    std::queue<std::string> toVisit;

    toVisit.push(entryAbsPath);
    visitedAbsPaths.insert(entryAbsPath);

    Luau::CompileOptions copts;
    copts.optimizationLevel = options.optimizationLevel;
    copts.debugLevel = options.debugLevel;

    size_t entryIndex = 0;

    while (!toVisit.empty())
    {
        std::string currentPath = toVisit.front();
        toVisit.pop();

        std::optional<std::string> source = readFile(currentPath);
        if (!source)
        {
            fprintf(stderr, "SingleBinaryCompiler: Warning: Could not read module '%s'\n", currentPath.c_str());
            continue;
        }

        std::string bytecode = Luau::compile(*source, copts);

        DiscoveredModule mod;
        mod.absolutePath = currentPath;
        mod.loadName = currentPath;
        mod.chunkName = "@" + currentPath;
        mod.source = *source;
        mod.bytecode = bytecode;

        if (currentPath == entryAbsPath)
            entryIndex = modules.size();

        modules.push_back(mod);

        // Parse AST to find require() calls
        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        Luau::ParseOptions popts;
        Luau::ParseResult parseResult = Luau::Parser::parse(source->data(), source->size(), names, allocator, popts);
        if (parseResult.root)
        {
            RequireVisitor visitor;
            parseResult.root->visit(&visitor);

            for (const std::string& reqPath : visitor.requirePaths)
            {
                // Skip built-in virtual standard library modules
                if (reqPath.rfind("@std/", 0) == 0 || reqPath == "@std")
                    continue;

                static const char* kStdLibs[] = {
                    "fs", "io", "ffi", "json", "hash", "crypto", "process", "net", "task",
                    "math", "table", "string", "coroutine", "bit32", "utf8", "os", "debug",
                    "buffer", "vector", "class", "integer", nullptr
                };
                bool isStd = false;
                for (int s = 0; kStdLibs[s]; ++s)
                {
                    if (reqPath == kStdLibs[s])
                    {
                        isStd = true;
                        break;
                    }
                }
                if (isStd)
                    continue;

                // Resolve target path
                std::string target = reqPath;
                std::string resolvedTarget;

                if (target.size() >= 6 && target.substr(0, 6) == "@self/")
                {
                    if (auto parent = getParentPath(currentPath))
                        resolvedTarget = normalizePath(*parent + "/" + target.substr(6));
                }
                else if (target == "@self")
                {
                    if (auto parent = getParentPath(currentPath))
                        resolvedTarget = *parent;
                }
                else if (target.size() >= 2 && target.substr(0, 2) == "./")
                {
                    if (auto parent = getParentPath(currentPath))
                        resolvedTarget = normalizePath(*parent + "/" + target.substr(2));
                }
                else if (target.size() >= 3 && target.substr(0, 3) == "../")
                {
                    if (auto parent = getParentPath(currentPath))
                        resolvedTarget = normalizePath(*parent + "/" + target);
                }
                else if (!target.empty() && target[0] == '@')
                {
                    std::string unprefixed = target.substr(1);
                    static const char* kPkgDirs[] = {"klur_modules", "luau_packages", "packages", "node_modules"};
                    for (const char* pdir : kPkgDirs)
                    {
                        std::string cand1 = std::string(pdir) + "/@" + unprefixed;
                        std::string cand2 = std::string(pdir) + "/" + unprefixed;
                        if (isFile(cand1) || isDirectory(cand1) || isFile(cand1 + ".luau") || isFile(cand1 + "/init.luau"))
                        {
                            resolvedTarget = cand1;
                            break;
                        }
                        if (isFile(cand2) || isDirectory(cand2) || isFile(cand2 + ".luau") || isFile(cand2 + "/init.luau"))
                        {
                            resolvedTarget = cand2;
                            break;
                        }
                    }
                    if (resolvedTarget.empty())
                        resolvedTarget = target;
                }
                else
                {
                    if (auto parent = getParentPath(currentPath))
                        resolvedTarget = normalizePath(*parent + "/" + target);
                }

                // Check potential extensions
                static const char* suffixes[] = {"", ".luau", ".lua", "/init.luau", "/init.lua", "/index.luau", "/index.lua"};
                for (const char* suf : suffixes)
                {
                    std::string cand = resolvedTarget + suf;
                    if (isFile(cand))
                    {
                        if (visitedAbsPaths.find(cand) == visitedAbsPaths.end())
                        {
                            visitedAbsPaths.insert(cand);
                            toVisit.push(cand);
                        }
                        break;
                    }
                }
            }
        }
    }

    // Traverse and collect asset files & directories
    std::vector<DiscoveredAsset> assets;
    std::set<std::string> visitedAssetPaths;

    auto addAssetFile = [&](const std::string& filePath, const std::string& relPath) {
        std::string absPath = normalizePath(filePath);
        if (!isAbsolutePath(absPath))
        {
            if (auto cwd = getCurrentWorkingDirectory())
                absPath = normalizePath(*cwd + "/" + absPath);
        }

        if (visitedAssetPaths.find(absPath) != visitedAssetPaths.end())
            return;
        visitedAssetPaths.insert(absPath);

        std::optional<std::string> content = readFile(absPath);
        if (!content)
        {
            fprintf(stderr, "SingleBinaryCompiler: Warning: Could not read asset '%s'\n", filePath.c_str());
            return;
        }

        DiscoveredAsset asset;
        asset.path = filePath;
        asset.relativePath = relPath;
        asset.absolutePath = absPath;
        asset.data = *content;
        assets.push_back(std::move(asset));
    };

    for (const std::string& assetSpec : options.assetPaths)
    {
        if (assetSpec.empty())
            continue;

        std::string resolved = assetSpec;
        if (!isAbsolutePath(resolved))
        {
            if (auto cwd = getCurrentWorkingDirectory())
                resolved = normalizePath(*cwd + "/" + resolved);
        }

        if (isFile(resolved))
        {
            addAssetFile(resolved, assetSpec);
        }
        else if (isDirectory(resolved))
        {
            traverseDirectory(resolved, [&](const std::string& filePath) {
                std::string rel = filePath;
                if (rel.size() >= resolved.size() && rel.substr(0, resolved.size()) == resolved)
                {
                    std::string sub = rel.substr(resolved.size());
                    if (!sub.empty() && (sub[0] == '/' || sub[0] == '\\'))
                        sub = sub.substr(1);
                    rel = normalizePath(assetSpec + "/" + sub);
                }
                addAssetFile(filePath, rel);
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
            printf("  [module] %s (%zu bytes bytecode)\n", m.absolutePath.c_str(), m.bytecode.size());
        for (const auto& a : assets)
            printf("  [asset]  %s (%zu bytes)\n", a.relativePath.c_str(), a.data.size());
    }

    // Determine packaging mode
    if (options.bundleMode == BundleMode::Direct ||
        options.targetArchitecture == "direct" ||
        options.targetArchitecture == "bundle" ||
        options.targetArchitecture == "stub")
    {
        return compileDirectBundle(options, modules, entryIndex, assets);
    }
    else if (options.bundleMode == BundleMode::Native)
    {
        return compileNativeRunner(options, modules, entryIndex, assets);
    }
    else // BundleMode::Auto
    {
        // If an explicit compiler command or cross-compilation target is specified, prefer native compilation
        if (!options.compilerCommand.empty() ||
            (!options.targetArchitecture.empty() &&
             options.targetArchitecture != "direct" &&
             options.targetArchitecture != "bundle" &&
             options.targetArchitecture != "auto"))
        {
            return compileNativeRunner(options, modules, entryIndex, assets);
        }

        // Otherwise, attempt direct self-contained bundling (fast, zero-toolchain)
        if (compileDirectBundle(options, modules, entryIndex, assets))
            return true;

        // Fallback to native compiler if direct bundling couldn't find a stub
        return compileNativeRunner(options, modules, entryIndex, assets);
    }
}

} // namespace Luau
