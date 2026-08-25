// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Luau::Cli
{

enum class ColorMode
{
    Auto,
    Always,
    Never,
};

inline ColorMode& colorMode()
{
    static ColorMode mode = ColorMode::Auto;
    return mode;
}

inline bool setColorMode(const char* value)
{
    if (strcmp(value, "auto") == 0)
        colorMode() = ColorMode::Auto;
    else if (strcmp(value, "always") == 0)
        colorMode() = ColorMode::Always;
    else if (strcmp(value, "never") == 0)
        colorMode() = ColorMode::Never;
    else
        return false;

    return true;
}

inline bool colorEnabled(FILE* stream)
{
    if (colorMode() == ColorMode::Always)
        return true;
    if (colorMode() == ColorMode::Never)
        return false;

    if (const char* noColor = getenv("NO_COLOR"); noColor && *noColor)
        return false;
    if (const char* forceColor = getenv("FORCE_COLOR"); forceColor && *forceColor && strcmp(forceColor, "0") != 0)
        return true;

#if defined(_WIN32)
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

inline const char* style(FILE* stream, const char* ansi)
{
    return colorEnabled(stream) ? ansi : "";
}

inline const char* reset(FILE* stream)
{
    return style(stream, "\033[0m");
}

inline const char* executableName(const char* argv0)
{
    const char* slash = strrchr(argv0, '/');
    const char* backslash = strrchr(argv0, '\\');
    const char* separator = slash;
    if (backslash && (!separator || backslash > separator))
        separator = backslash;
    return separator ? separator + 1 : argv0;
}

inline void title(FILE* stream, const char* name, const char* description)
{
    fprintf(stream, "%s%s%s %s- %s%s\n", style(stream, "\033[1;36m"), name, reset(stream), style(stream, "\033[2m"), description, reset(stream));
}

inline void heading(FILE* stream, const char* name)
{
    fprintf(stream, "\n%s%s%s\n", style(stream, "\033[1;36m"), name, reset(stream));
}

inline void usage(FILE* stream, const char* invocation)
{
    fprintf(stream, "  %s%s%s\n", style(stream, "\033[1;32m"), invocation, reset(stream));
}

inline void option(FILE* stream, const char* flags, const char* description)
{
    fprintf(stream, "  %s%-28s%s %s\n", style(stream, "\033[1;33m"), flags, reset(stream), description);
}

inline void status(FILE* stream, const char* label, const char* detail)
{
    fprintf(stream, "%s%s%s %s\n", style(stream, "\033[1;36m"), label, reset(stream), detail);
}

inline void success(FILE* stream, const char* detail)
{
    fprintf(stream, "%sDone%s %s\n", style(stream, "\033[1;32m"), reset(stream), detail);
}

inline void error(FILE* stream, const char* message)
{
    fprintf(stream, "%serror:%s %s\n", style(stream, "\033[1;31m"), reset(stream), message);
}

inline void hint(FILE* stream, const char* message)
{
    fprintf(stream, "%shint:%s %s\n", style(stream, "\033[1;36m"), reset(stream), message);
}

} // namespace Luau::Cli
