// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#define getcwd _getcwd
#define chdir _chdir
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#endif

static auto g_procStartTime = std::chrono::steady_clock::now();

static int proc_exit(lua_State* L)
{
    int code = luaL_optinteger(L, 1, 0);
    exit(code);
    return 0;
}

static int proc_getpid(lua_State* L)
{
#if defined(_WIN32)
    lua_pushinteger(L, (int)GetCurrentProcessId());
#else
    lua_pushinteger(L, (int)getpid());
#endif
    return 1;
}

static int proc_cwd(lua_State* L)
{
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) != NULL)
    {
        lua_pushstring(L, buf);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int proc_chdir(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    int res = chdir(path);
    lua_pushboolean(L, res == 0);
    return 1;
}

static int proc_kill(lua_State* L)
{
    int pid = luaL_checkinteger(L, 1);
    int sig = luaL_optinteger(L, 2, 15); // default SIGTERM

#if defined(_WIN32)
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!hProcess)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    BOOL success = TerminateProcess(hProcess, (UINT)sig);
    CloseHandle(hProcess);
    lua_pushboolean(L, success ? 1 : 0);
    return 1;
#else
    int res = kill((pid_t)pid, sig);
    lua_pushboolean(L, res == 0);
    return 1;
#endif
}

static int proc_uptime(lua_State* L)
{
    auto now = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(now - g_procStartTime).count();
    lua_pushnumber(L, sec);
    return 1;
}

static int proc_env_index(lua_State* L)
{
    const char* key = luaL_checkstring(L, 2);
    const char* val = getenv(key);
    if (val)
        lua_pushstring(L, val);
    else
        lua_pushnil(L);
    return 1;
}

static int proc_env_newindex(lua_State* L)
{
    const char* key = luaL_checkstring(L, 2);
    if (lua_isnoneornil(L, 3))
    {
#if defined(_WIN32)
        _putenv_s(key, "");
#else
        unsetenv(key);
#endif
    }
    else
    {
        size_t len = 0;
        const char* val = lua_type(L, 3) == LUA_TBUFFER ? lua_tostring(L, 3) : luaL_checklstring(L, 3, &len);
#if defined(_WIN32)
        _putenv_s(key, val);
#else
        setenv(key, val, 1);
#endif
    }
    return 0;
}

#if defined(_WIN32)
static std::string readPipe(HANDLE hFile)
{
    std::string result;
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0)
    {
        result.append(buffer, bytesRead);
    }
    return result;
}

static std::string buildCommandLine(const char* cmd, const std::vector<std::string>& args)
{
    std::string result = "\"";
    result += cmd;
    result += "\"";
    for (const auto& arg : args)
    {
        result += " \"";
        for (char c : arg)
        {
            if (c == '"') result += "\\\"";
            else if (c == '\\') result += "\\\\";
            else result += c;
        }
        result += "\"";
    }
    return result;
}
#else
static std::string readFd(int fd)
{
    std::string result;
    char buffer[4096];
    ssize_t bytesRead;
    while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0)
    {
        result.append(buffer, bytesRead);
    }
    return result;
}
#endif

static int proc_spawn(lua_State* L)
{
    const char* cmd = luaL_checkstring(L, 1);
    std::vector<std::string> args;
    if (lua_istable(L, 2))
    {
        int len = (int)lua_objlen(L, 2);
        for (int i = 1; i <= len; ++i)
        {
            lua_rawgeti(L, 2, i);
            if (lua_isstring(L, -1))
            {
                args.push_back(lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
    }

    std::string stdin_data;
    std::string cwd_dir;
    std::vector<std::string> env_vars;

    if (lua_istable(L, 3))
    {
        lua_getfield(L, 3, "stdin");
        if (lua_isstring(L, -1))
        {
            size_t l;
            const char* d = lua_tolstring(L, -1, &l);
            stdin_data.assign(d, l);
        }
        else if (lua_isbuffer(L, -1))
        {
            size_t l;
            const char* d = (const char*)lua_tobuffer(L, -1, &l);
            stdin_data.assign(d, l);
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "cwd");
        if (lua_isstring(L, -1))
        {
            cwd_dir = lua_tostring(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "env");
        if (lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                if (lua_isstring(L, -2) && lua_isstring(L, -1))
                {
                    std::string var = lua_tostring(L, -2);
                    var += "=";
                    var += lua_tostring(L, -1);
                    env_vars.push_back(var);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    std::string stdout_str;
    std::string stderr_str;
    int exitcode = -1;

#if defined(_WIN32)
    HANDLE hStdInRead = NULL, hStdInWrite = NULL;
    HANDLE hStdOutRead = NULL, hStdOutWrite = NULL;
    HANDLE hStdErrRead = NULL, hStdErrWrite = NULL;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0);
    CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0);
    CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0);

    SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0);

    if (!stdin_data.empty())
    {
        DWORD bytesWritten;
        WriteFile(hStdInWrite, stdin_data.data(), (DWORD)stdin_data.size(), &bytesWritten, NULL);
    }
    CloseHandle(hStdInWrite);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput = hStdInRead;
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdErrWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdline = buildCommandLine(cmd, args);

    std::vector<char> env_block;
    if (!env_vars.empty())
    {
        for (const auto& var : env_vars)
        {
            env_block.insert(env_block.end(), var.begin(), var.end());
            env_block.push_back('\0');
        }
        env_block.push_back('\0');
    }

    const char* pCwd = cwd_dir.empty() ? NULL : cwd_dir.c_str();
    LPVOID pEnv = env_block.empty() ? NULL : env_block.data();

    if (CreateProcessA(NULL, &cmdline[0], NULL, NULL, TRUE, 0, pEnv, pCwd, &si, &pi))
    {
        CloseHandle(hStdInRead);
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdErrWrite);

        stdout_str = readPipe(hStdOutRead);
        stderr_str = readPipe(hStdErrRead);

        CloseHandle(hStdOutRead);
        CloseHandle(hStdErrRead);

        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD dwExitCode = 0;
        GetExitCodeProcess(pi.hProcess, &dwExitCode);
        exitcode = (int)dwExitCode;

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else
    {
        CloseHandle(hStdInRead);
        CloseHandle(hStdOutRead);
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdErrRead);
        CloseHandle(hStdErrWrite);
        exitcode = -1;
    }
#else
    int pipe_stdin[2];
    int pipe_stdout[2];
    int pipe_stderr[2];

    if (pipe(pipe_stdin) < 0 || pipe(pipe_stdout) < 0 || pipe(pipe_stderr) < 0)
    {
        luaL_error(L, "failed to create pipes for process.spawn");
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process
        close(pipe_stdin[1]);
        dup2(pipe_stdin[0], STDIN_FILENO);
        close(pipe_stdin[0]);

        close(pipe_stdout[0]);
        dup2(pipe_stdout[1], STDOUT_FILENO);
        close(pipe_stdout[1]);

        close(pipe_stderr[0]);
        dup2(pipe_stderr[1], STDERR_FILENO);
        close(pipe_stderr[1]);

        if (!cwd_dir.empty())
        {
            if (chdir(cwd_dir.c_str()) != 0)
            {
                _exit(127);
            }
        }

        for (const auto& ev : env_vars)
        {
            putenv(strdup(ev.c_str()));
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(cmd));
        for (const auto& a : args)
        {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(NULL);

        execvp(cmd, argv.data());
        _exit(127);
    }
    else if (pid > 0)
    {
        // Parent process
        close(pipe_stdin[0]);
        if (!stdin_data.empty())
        {
            ssize_t written = write(pipe_stdin[1], stdin_data.data(), stdin_data.size());
            (void)written;
        }
        close(pipe_stdin[1]);

        close(pipe_stdout[1]);
        close(pipe_stderr[1]);

        stdout_str = readFd(pipe_stdout[0]);
        stderr_str = readFd(pipe_stderr[0]);

        close(pipe_stdout[0]);
        close(pipe_stderr[0]);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
        {
            exitcode = WEXITSTATUS(status);
        }
        else if (WIFSIGNALED(status))
        {
            exitcode = -WTERMSIG(status);
        }
    }
#endif

    lua_createtable(L, 0, 3);
    lua_pushlstring(L, stdout_str.data(), stdout_str.size());
    lua_setfield(L, -2, "stdout");
    lua_pushlstring(L, stderr_str.data(), stderr_str.size());
    lua_setfield(L, -2, "stderr");
    lua_pushinteger(L, exitcode);
    lua_setfield(L, -2, "exitcode");

    return 1;
}

static const luaL_Reg proclib[] = {
    {"exit", proc_exit},
    {"spawn", proc_spawn},
    {"getpid", proc_getpid},
    {"pid", proc_getpid},
    {"cwd", proc_cwd},
    {"chdir", proc_chdir},
    {"kill", proc_kill},
    {"uptime", proc_uptime},
    {NULL, NULL}
};

int luaopen_process(lua_State* L)
{
    luaL_register(L, "process", proclib);

    // process.pid field
#if defined(_WIN32)
    lua_pushinteger(L, (int)GetCurrentProcessId());
#else
    lua_pushinteger(L, (int)getpid());
#endif
    lua_setfield(L, -2, "pid");

    // process.os & process.arch
#if defined(_WIN32)
    lua_pushstring(L, "windows");
#elif defined(__APPLE__)
    lua_pushstring(L, "macos");
#elif defined(__linux__)
    lua_pushstring(L, "linux");
#else
    lua_pushstring(L, "unknown");
#endif
    lua_setfield(L, -2, "os");

#if defined(__x86_64__) || defined(_M_X64)
    lua_pushstring(L, "x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    lua_pushstring(L, "arm64");
#elif defined(__i386__) || defined(_M_IX86)
    lua_pushstring(L, "x86");
#else
    lua_pushstring(L, "unknown");
#endif
    lua_setfield(L, -2, "arch");

    lua_newtable(L);
    lua_setfield(L, -2, "args");

    // process.env proxy table
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, proc_env_index, "proc_env_index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, proc_env_newindex, "proc_env_newindex");
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "env");

    return 1;
}
