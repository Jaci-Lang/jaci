// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/Repl.h"
#include "Luau/Flags.h"
#include "Luau/SingleBinaryCompiler.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    setLuauFlagsDefault();

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> argStrings;
    std::vector<char*> argv;
    if (argvW)
    {
        argStrings.reserve(argc);
        argv.reserve(argc + 1);
        for (int i = 0; i < argc; ++i)
        {
            int sz = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, NULL, 0, NULL, NULL);
            std::string s(sz > 0 ? sz - 1 : 0, 0);
            if (sz > 0)
                WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &s[0], sz, NULL, NULL);
            argStrings.push_back(std::move(s));
        }
        LocalFree(argvW);
    }
    for (auto& s : argStrings)
        argv.push_back(&s[0]);
    argv.push_back(nullptr);

    if (auto exitCode = Luau::SingleBinaryCompiler::checkAndRunBundledPayload(static_cast<int>(argv.size()) - 1, argv.data()))
        return *exitCode;

    return replMain(static_cast<int>(argv.size()) - 1, argv.data());
}
#endif

int main(int argc, char** argv)
{
    setLuauFlagsDefault();

    if (auto exitCode = Luau::SingleBinaryCompiler::checkAndRunBundledPayload(argc, argv))
        return *exitCode;

    return replMain(argc, argv);
}

