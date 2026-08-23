// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/VfsLayer.h"

#include "Luau/FileUtils.h"

#include <algorithm>
#include <unordered_map>

namespace Luau
{
namespace VfsLayer
{

namespace
{

std::unordered_map<std::string, std::string> g_files;
std::vector<std::string> g_sortedPaths;

// Normalize a path for embedding/lookup so that the navigator's candidate
// paths (absolute, normalized) match the stored keys exactly.
std::string normalizeKey(const std::string& path)
{
    return normalizePath(path);
}

} // namespace

void setEmbeddedFiles(std::vector<std::pair<std::string, std::string>> files)
{
    g_files.clear();
    g_sortedPaths.clear();
    g_sortedPaths.reserve(files.size());
    for (auto& [path, data] : files)
    {
        std::string key = normalizeKey(path);
        g_files.emplace(std::move(key), std::move(data));
    }
    for (const auto& [path, _] : g_files)
        g_sortedPaths.push_back(path);
    std::sort(g_sortedPaths.begin(), g_sortedPaths.end());
}

bool hasEmbeddedFiles()
{
    return !g_files.empty();
}

bool isEmbeddedFile(const std::string& path)
{
    if (g_files.empty())
        return false;
    return g_files.find(normalizeKey(path)) != g_files.end();
}

bool isEmbeddedDirectory(const std::string& path)
{
    if (g_files.empty())
        return false;
    std::string key = normalizeKey(path);
    if (key.empty() || key.back() == '/')
        key.pop_back();
    std::string prefix = key + "/";
    // A directory exists if any embedded path lies strictly under it.
    auto it = std::lower_bound(g_sortedPaths.begin(), g_sortedPaths.end(), prefix);
    if (it == g_sortedPaths.end())
        return false;
    return it->compare(0, prefix.size(), prefix) == 0;
}

std::optional<std::string> readEmbeddedFile(const std::string& path)
{
    if (g_files.empty())
        return std::nullopt;
    auto it = g_files.find(normalizeKey(path));
    if (it == g_files.end())
        return std::nullopt;
    return it->second;
}

} // namespace VfsLayer
} // namespace Luau
