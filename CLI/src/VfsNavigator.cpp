// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/VfsNavigator.h"

#include "Luau/Common.h"
#include "Luau/Config.h"
#include "Luau/FileUtils.h"
#include "Luau/LuauConfig.h"

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

const std::array<std::string_view, 4> kSuffixes = {".luau", ".lua", ".d.luau", ".d.lua"};
const std::array<std::string_view, 4> kInitSuffixes = {"/init.luau", "/init.lua", "/init.d.luau", "/init.d.lua"};
const std::array<std::string_view, 4> kIndexSuffixes = {"/index.luau", "/index.lua", "/index.d.luau", "/index.d.lua"};

// Native shared library extensions, in priority order.
const std::array<std::string_view, 3> kNativeSuffixes = {".so", ".dylib", ".dll"};

// Package directory names to search for bare module specifiers, in priority order.
const std::array<std::string_view, 4> kPackageDirs = {"klur_modules", "luau_packages", "packages", "node_modules"};

struct ResolvedRealPath
{
    NavigationStatus status;
    std::string realPath;
};

static bool hasSuffix(std::string_view str, std::string_view suffix)
{
    return str.size() >= suffix.size() && str.substr(str.size() - suffix.size()) == suffix;
}

static bool hasNativeSuffix(std::string_view str)
{
    for (std::string_view s : kNativeSuffixes)
    {
        if (hasSuffix(str, s))
            return true;
    }
    return false;
}

// True when realPath is the init/index file inside the directory that
// modulePath denotes (a "directory module"). getModulePath collapses
// dir/init.luau to dir, so after a reset the requirer of such a file sits
// at the directory level; the require state machine's file-to-directory
// step must then be a no-op instead of overshooting to the grandparent.
static bool isDirectoryModule(const std::string& realPath)
{
    for (std::string_view s : kInitSuffixes)
    {
        if (hasSuffix(realPath, s))
            return true;
    }
    for (std::string_view s : kIndexSuffixes)
    {
        if (hasSuffix(realPath, s))
            return true;
    }
    return false;
}

static ResolvedRealPath getRealPath(std::string modulePath)
{
    bool found = false;
    std::string suffix;

    size_t lastSlash = modulePath.find_last_of('/');
    LUAU_ASSERT(lastSlash != std::string::npos);
    std::string_view lastComponent = std::string_view(modulePath).substr(lastSlash + 1);
    bool isInitOrIndex = (lastComponent == "init" || lastComponent == "index");

    std::string testBuf;
    testBuf.reserve(modulePath.size() + 32);

    // Try each suffix in order; returns false on ambiguity (a second hit
    // after one was already found).
    auto trySuffixes = [&](auto&& suffixes) -> bool
    {
        for (std::string_view s : suffixes)
        {
            testBuf = modulePath;
            testBuf.append(s.data(), s.size());
            if (isFile(testBuf))
            {
                if (found)
                    return false;

                suffix = s;
                found = true;
            }
        }
        return true;
    };

    if (isInitOrIndex && isDirectory(modulePath))
    {
        // Directory module: the nested directory's init/index takes
        // precedence over a sibling X/init.luau file (pinned by the
        // upstream nested_inits semantics: require("@self/init") from
        // dir/init.luau resolves to dir/init/init.luau).
        if (!trySuffixes(kInitSuffixes) || (!found && !trySuffixes(kIndexSuffixes)))
            return {NavigationStatus::Ambiguous};

        if (!found)
            found = true; // directory itself counts as presence; load will fail later
    }
    else
    {
        // Plain file resolution (dir/foo.luau). For init/index components
        // without a nested directory this also covers require("cli/init")
        // finding the file cli/init.luau.
        if (!trySuffixes(kSuffixes) || !trySuffixes(kNativeSuffixes))
            return {NavigationStatus::Ambiguous};

        if (!found && isDirectory(modulePath))
        {
            // A directory of the same name is an alternative entry point.
            if (!trySuffixes(kInitSuffixes) || (!found && !trySuffixes(kIndexSuffixes)))
                return {NavigationStatus::Ambiguous};

            if (!found)
                found = true; // directory itself counts as presence; load will fail later
        }
    }

    if (!found)
        return {NavigationStatus::NotFound};

    return {NavigationStatus::Success, modulePath + suffix};
}

static std::string getModulePath(std::string filePath)
{
    for (char& c : filePath)
    {
        if (c == '\\')
            c = '/';
    }

    std::string_view pathView = filePath;

    if (isAbsolutePath(pathView))
    {
        size_t firstSlash = pathView.find_first_of('/');
        LUAU_ASSERT(firstSlash != std::string::npos);
        pathView.remove_prefix(firstSlash);
    }

    for (std::string_view suffix : kInitSuffixes)
    {
        if (hasSuffix(pathView, suffix))
        {
            pathView.remove_suffix(suffix.size());
            return std::string(pathView);
        }
    }
    for (std::string_view suffix : kIndexSuffixes)
    {
        if (hasSuffix(pathView, suffix))
        {
            pathView.remove_suffix(suffix.size());
            return std::string(pathView);
        }
    }
    for (std::string_view suffix : kSuffixes)
    {
        if (hasSuffix(pathView, suffix))
        {
            pathView.remove_suffix(suffix.size());
            return std::string(pathView);
        }
    }
    for (std::string_view suffix : kNativeSuffixes)
    {
        if (hasSuffix(pathView, suffix))
        {
            pathView.remove_suffix(suffix.size());
            return std::string(pathView);
        }
    }

    return std::string(pathView);
}

NavigationStatus VfsNavigator::updateRealPaths()
{
    ResolvedRealPath result = getRealPath(modulePath);
    ResolvedRealPath absoluteResult = getRealPath(absoluteModulePath);
    if (result.status != NavigationStatus::Success || absoluteResult.status != NavigationStatus::Success)
        return result.status;

    realPath = isAbsolutePath(result.realPath) ? absolutePathPrefix + result.realPath : result.realPath;
    absoluteRealPath = absolutePathPrefix + absoluteResult.realPath;

    // Canonicalize absoluteRealPath through symlinks so that alias resolution and
    // findModule/findEmbeddedModule can match against the real (non-symlinked) path.
    // This makes symlink resolution work cross-platform (Windows, macOS, Linux).
    if (auto resolved = resolveSymlink(absoluteRealPath))
        absoluteRealPath = *resolved;

    return NavigationStatus::Success;
}

NavigationStatus VfsNavigator::resetToStdIn()
{
    std::optional<std::string> cwd = getCurrentWorkingDirectory();
    if (!cwd)
        return NavigationStatus::NotFound;

    realPath = "./stdin";
    absoluteRealPath = normalizePath(*cwd + "/stdin");
    modulePath = "./stdin";
    absoluteModulePath = getModulePath(absoluteRealPath);

    size_t firstSlash = absoluteRealPath.find_first_of('/');
    LUAU_ASSERT(firstSlash != std::string::npos);
    absolutePathPrefix = absoluteRealPath.substr(0, firstSlash);

    dirModuleReset = false; // stdin is never a directory module

    return NavigationStatus::Success;
}

NavigationStatus VfsNavigator::resetToPath(const std::string& path)
{
    std::string normalizedPath = normalizePath(path);

    if (isAbsolutePath(normalizedPath))
    {
        modulePath = getModulePath(normalizedPath);
        absoluteModulePath = modulePath;

        size_t firstSlash = normalizedPath.find_first_of('/');
        LUAU_ASSERT(firstSlash != std::string::npos);
        absolutePathPrefix = normalizedPath.substr(0, firstSlash);
    }
    else
    {
        std::optional<std::string> cwd = getCurrentWorkingDirectory();
        if (!cwd)
            return NavigationStatus::NotFound;

        modulePath = getModulePath(normalizedPath);
        std::string joinedPath = normalizePath(*cwd + "/" + normalizedPath);
        absoluteModulePath = getModulePath(joinedPath);

        size_t firstSlash = joinedPath.find_first_of('/');
        LUAU_ASSERT(firstSlash != std::string::npos);
        absolutePathPrefix = joinedPath.substr(0, firstSlash);
    }

    NavigationStatus status = updateRealPaths();
    if (status == NavigationStatus::Success)
    {
        // If the resolved module is a directory module (dir/init.luau or
        // dir/index.luau), modulePath is the directory itself; the require
        // state machine's file-to-directory step must be a no-op next.
        dirModuleReset = isDirectoryModule(realPath);
    }
    else
    {
        dirModuleReset = false;
    }
    return status;
}

NavigationStatus VfsNavigator::toParent()
{
    if (absoluteModulePath == "/")
        return NavigationStatus::NotFound;

    // The require state machine performs one file-to-directory step right
    // after resetting to the requirer. When the requirer is a directory
    // module (dir/init.luau or dir/index.luau), getModulePath already
    // collapsed the path to the directory, so the containing directory is
    // modulePath itself and stepping up would overshoot to the grandparent.
    if (dirModuleReset)
    {
        dirModuleReset = false;
        return NavigationStatus::Success;
    }

    size_t numSlashes = 0;
    for (char c : absoluteModulePath)
    {
        if (c == '/')
            numSlashes++;
    }
    LUAU_ASSERT(numSlashes > 0);

    if (numSlashes == 1)
        return NavigationStatus::NotFound;

    modulePath = normalizePath(modulePath + "/..");
    absoluteModulePath = normalizePath(absoluteModulePath + "/..");

    // There is no ambiguity when navigating up in a tree.
    NavigationStatus status = updateRealPaths();
    return status == NavigationStatus::Ambiguous ? NavigationStatus::Success : status;
}

NavigationStatus VfsNavigator::toChild(const std::string& name)
{
    if (name == ".config")
        return NavigationStatus::NotFound;

    // Navigating to a child ends the reset positioning; a later parent step
    // is an explicit ".." and must actually step up.
    dirModuleReset = false;

    // A child resolves relative to the current module's containing directory.
    // The require state machine always steps to that directory first for
    // relative paths (file-to-directory step), so modulePath is at directory
    // level here and appending is correct.
    modulePath = normalizePath(modulePath + "/" + name);
    absoluteModulePath = normalizePath(absoluteModulePath + "/" + name);
    return updateRealPaths();
}

NavigationStatus VfsNavigator::toBarePackage(const std::string& pkgName)
{
    dirModuleReset = false;

    // Try a single package root: <root>/<pkgDir>/<name> and the scoped
    // layout <root>/<pkgDir>/@<name>. On success the navigator is positioned
    // at the package (its subpath is navigated by the caller).
    auto tryRoot = [&](const std::string& root) -> bool
    {
        for (std::string_view pkgDir : kPackageDirs)
        {
            std::string plain = root + "/" + std::string(pkgDir) + "/" + pkgName;
            std::string scoped = root + "/" + std::string(pkgDir) + "/@" + pkgName;

            for (const std::string& candidateMod : {plain, scoped})
            {
                std::string candidateAbs = absolutePathPrefix + candidateMod;

                bool exists = isDirectory(candidateAbs) || isFile(candidateAbs + ".luau") || isFile(candidateAbs + ".lua") ||
                              isFile(candidateAbs + ".so") || isFile(candidateAbs + ".dylib") || isFile(candidateAbs + ".dll");

                if (exists)
                {
                    absoluteModulePath = candidateMod;
                    modulePath = candidateMod;

                    NavigationStatus status = updateRealPaths();
                    if (status == NavigationStatus::Success)
                        return true;
                }
            }
        }
        return false;
    };

    // 1. Project-local packages: walk from the requirer's directory up to the
    // filesystem root, nearest package directory wins.
    std::string searchDir = absoluteModulePath;

    // Start from the parent directory of the current module.
    size_t lastSlash = searchDir.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash > 0)
        searchDir = searchDir.substr(0, lastSlash);

    while (true)
    {
        if (tryRoot(searchDir))
            return NavigationStatus::Success;

        size_t slash = searchDir.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
            break;
        searchDir = searchDir.substr(0, slash);
    }

    // 2. Toolchain packages: package directories next to the engine binary.
    // Installed once by the toolchain installer and shared by every program
    // in the toolchain (luau, luau-analyze, LSP), so no project-local copy is
    // needed. Absolute by construction: no relative paths involved.
    if (std::optional<std::string> exeDir = getExecutableDirectory())
    {
        if (tryRoot(*exeDir))
            return NavigationStatus::Success;
    }

    return NavigationStatus::NotFound;
}

const std::string& VfsNavigator::getFilePath() const
{
    return realPath;
}

const std::string& VfsNavigator::getAbsoluteFilePath() const
{
    return absoluteRealPath;
}

bool VfsNavigator::isNativeLibrary() const
{
    return hasNativeSuffix(absoluteRealPath);
}

std::string VfsNavigator::getNativeEntryPoint() const
{
    // Derive the entry-point symbol from the file name.
    // Strip directory prefix and known suffixes, then prefix with "luaopen_".
    std::string name = absoluteRealPath;

    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);

    // Strip leading "lib" (common on POSIX: libfoo.so -> foo)
    if (name.size() > 3 && name.substr(0, 3) == "lib")
        name = name.substr(3);

    for (std::string_view suffix : kNativeSuffixes)
    {
        if (hasSuffix(name, suffix))
        {
            name = name.substr(0, name.size() - suffix.size());
            break;
        }
    }

    // Replace non-alphanumeric characters with underscores.
    for (char& c : name)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            c = '_';
    }

    return "luaopen_" + name;
}

std::string VfsNavigator::getConfigPath(const std::string& filename) const
{
    std::string_view directory = realPath;

    for (std::string_view suffix : kInitSuffixes)
    {
        if (hasSuffix(directory, suffix))
        {
            directory.remove_suffix(suffix.size());
            return std::string(directory) + '/' + filename;
        }
    }
    for (std::string_view suffix : kIndexSuffixes)
    {
        if (hasSuffix(directory, suffix))
        {
            directory.remove_suffix(suffix.size());
            return std::string(directory) + '/' + filename;
        }
    }
    for (std::string_view suffix : kSuffixes)
    {
        if (hasSuffix(directory, suffix))
        {
            directory.remove_suffix(suffix.size());
            return std::string(directory) + '/' + filename;
        }
    }

    return std::string(directory) + '/' + filename;
}

VfsNavigator::ConfigStatus VfsNavigator::getConfigStatus() const
{
    bool luaurcExists = isFile(getConfigPath(Luau::kConfigName));
    bool luauConfigExists = isFile(getConfigPath(Luau::kLuauConfigName));

    if (luaurcExists && luauConfigExists)
        return ConfigStatus::Ambiguous;
    else if (luauConfigExists)
        return ConfigStatus::PresentLuau;
    else if (luaurcExists)
        return ConfigStatus::PresentJson;
    else
        return ConfigStatus::Absent;
}

std::optional<std::string> VfsNavigator::getConfig() const
{
    ConfigStatus status = getConfigStatus();
    LUAU_ASSERT(status == ConfigStatus::PresentJson || status == ConfigStatus::PresentLuau);

    if (status == ConfigStatus::PresentJson)
        return readFile(getConfigPath(Luau::kConfigName));
    else if (status == ConfigStatus::PresentLuau)
        return readFile(getConfigPath(Luau::kLuauConfigName));

    LUAU_UNREACHABLE();
}

