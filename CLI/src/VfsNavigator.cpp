// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/VfsNavigator.h"

#include "Luau/Common.h"
#include "Luau/Config.h"
#include "Luau/FileUtils.h"
#include "Luau/LuauConfig.h"

#include <array>
#include <string>
#include <string_view>

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

static ResolvedRealPath getRealPath(std::string modulePath)
{
    bool found = false;
    std::string suffix;

    size_t lastSlash = modulePath.find_last_of('/');
    LUAU_ASSERT(lastSlash != std::string::npos);
    std::string_view lastComponent = std::string_view(modulePath).substr(lastSlash + 1);

    std::string testBuf;
    testBuf.reserve(modulePath.size() + 32);

    if (lastComponent != "init" && lastComponent != "index")
    {
        for (std::string_view potentialSuffix : kSuffixes)
        {
            testBuf = modulePath;
            testBuf.append(potentialSuffix.data(), potentialSuffix.size());
            if (isFile(testBuf))
            {
                if (found)
                    return {NavigationStatus::Ambiguous};

                suffix = potentialSuffix;
                found = true;
            }
        }

        // Check for native shared library.
        for (std::string_view potentialSuffix : kNativeSuffixes)
        {
            testBuf = modulePath;
            testBuf.append(potentialSuffix.data(), potentialSuffix.size());
            if (isFile(testBuf))
            {
                if (found)
                    return {NavigationStatus::Ambiguous};

                suffix = potentialSuffix;
                found = true;
            }
        }
    }

    if (isDirectory(modulePath))
    {
        if (found)
            return {NavigationStatus::Ambiguous};

        // Try init.luau / init.lua first.
        for (std::string_view potentialSuffix : kInitSuffixes)
        {
            testBuf = modulePath;
            testBuf.append(potentialSuffix.data(), potentialSuffix.size());
            if (isFile(testBuf))
            {
                if (found)
                    return {NavigationStatus::Ambiguous};

                suffix = potentialSuffix;
                found = true;
            }
        }

        // Try index.luau / index.lua as an alternative entry point.
        if (!found)
        {
            for (std::string_view potentialSuffix : kIndexSuffixes)
            {
                testBuf = modulePath;
                testBuf.append(potentialSuffix.data(), potentialSuffix.size());
                if (isFile(testBuf))
                {
                    if (found)
                        return {NavigationStatus::Ambiguous};

                    suffix = potentialSuffix;
                    found = true;
                }
            }
        }

        if (!found)
            found = true; // directory itself counts as presence; load will fail later
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

    return updateRealPaths();
}

NavigationStatus VfsNavigator::toParent()
{
    if (absoluteModulePath == "/")
        return NavigationStatus::NotFound;

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

    modulePath = normalizePath(modulePath + "/" + name);
    absoluteModulePath = normalizePath(absoluteModulePath + "/" + name);

    return updateRealPaths();
}

NavigationStatus VfsNavigator::toBarePackage(const std::string& pkgName)
{
    // Walk from the current directory up to the filesystem root, searching
    // for kPackageDirs/<pkgName> at each level.
    std::string searchDir = absoluteModulePath;

    // Start from the parent directory of the current module.
    size_t lastSlash = searchDir.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash > 0)
        searchDir = searchDir.substr(0, lastSlash);

    while (true)
    {
        for (std::string_view pkgDir : kPackageDirs)
        {
            std::string candidateMods[2] = {
                searchDir + "/" + std::string(pkgDir) + "/" + pkgName,
                searchDir + "/" + std::string(pkgDir) + "/@" + pkgName
            };

            for (const auto& candidateMod : candidateMods)
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
                        return NavigationStatus::Success;
                }
            }
        }

        // Go up one level.
        size_t slash = searchDir.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
            break;
        searchDir = searchDir.substr(0, slash);
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

