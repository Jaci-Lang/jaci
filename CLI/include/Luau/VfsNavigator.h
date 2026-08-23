// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#pragma once

#include <optional>
#include <string>

enum class NavigationStatus
{
    Success,
    Ambiguous,
    NotFound
};

class VfsNavigator
{
public:
    NavigationStatus resetToStdIn();
    NavigationStatus resetToPath(const std::string& path);

    NavigationStatus toParent();
    NavigationStatus toChild(const std::string& name);

    // Bare package resolution: search luau_packages/, packages/, node_modules/
    // starting from the current directory and walking up. On success, the
    // navigator is positioned at the found package root.
    NavigationStatus toBarePackage(const std::string& pkgName);

    const std::string& getFilePath() const;
    const std::string& getAbsoluteFilePath() const;

    // Returns true if the resolved path is a native shared library (.so/.dylib/.dll).
    bool isNativeLibrary() const;

    // Returns the native symbol entry-point name for the current resolved path.
    // E.g. for "libmylib.so" returns "luaopen_mylib".
    std::string getNativeEntryPoint() const;

    enum class ConfigStatus
    {
        Absent,
        Ambiguous,
        PresentJson,
        PresentLuau
    };

    ConfigStatus getConfigStatus() const;
    std::optional<std::string> getConfig() const;

private:
    std::string getConfigPath(const std::string& filename) const;

    NavigationStatus updateRealPaths();

    std::string realPath;
    std::string absoluteRealPath;
    std::string absolutePathPrefix;

    std::string modulePath;
    std::string absoluteModulePath;

    // Set when resetToPath lands on a directory module (the requirer file is
    // dir/init.luau or dir/index.luau, so getModulePath collapsed the path to
    // the directory itself). The require state machine's file-to-directory
    // step right after a reset must then be a no-op: the containing directory
    // is already modulePath, and stepping up would overshoot to the
    // grandparent. Cleared by the first toParent or any toChild.
    bool dirModuleReset = false;
};
