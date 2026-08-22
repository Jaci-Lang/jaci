// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "PathUtilities.h"

#include <string_view>

namespace Luau::Require
{

PathType getPathType(std::string_view path)
{
    if (path.size() >= 2 && path.substr(0, 2) == "./")
        return PathType::RelativeToCurrent;
    if (path.size() >= 3 && path.substr(0, 3) == "../")
        return PathType::RelativeToParent;
    if (path.size() >= 1 && path[0] == '@')
        return PathType::Aliased;

    // A bare name (no prefix, no path separator as first char, non-empty)
    // is treated as a package specifier. The VFS navigator resolves it
    // against luau_packages/, packages/, and node_modules/ directories.
    if (!path.empty() && path[0] != '/' && path[0] != '\\')
        return PathType::BarePackage;

    return PathType::Unsupported;
}

std::pair<std::string_view, std::string_view> splitPath(std::string_view path)
{
    size_t pos = path.find_first_of('/');
    if (pos == std::string_view::npos)
        return {path, {}};

    return {path.substr(0, pos), path.substr(pos + 1)};
}

} // namespace Luau::Require
