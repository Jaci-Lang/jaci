// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#endif

static int proc_exit(lua_State* L)
{
    int code = luaL_optinteger(L, 1, 0);
    exit(code);
    return 0;
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
        int len = lua_objlen(L, 2);
        for (int i = 1; i <= len; i++)
        {
            lua_rawgeti(L, 2, i);
            args.push_back(luaL_checkstring(L, -1));
            lua_pop(L, 1);
        }
    }
    
    std::string stdin_data;
    std::string cwd;
    bool has_env = false;
    std::vector<std::string> env;
    
    if (lua_istable(L, 3))
    {
        lua_getfield(L, 3, "stdin");
        if (!lua_isnoneornil(L, -1)) stdin_data = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        
        lua_getfield(L, 3, "cwd");
        if (!lua_isnoneornil(L, -1)) cwd = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        
        lua_getfield(L, 3, "env");
        if (lua_istable(L, -1))
        {
            has_env = true;
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                std::string k = luaL_checkstring(L, -2);
                std::string v = luaL_checkstring(L, -1);
                env.push_back(k + "=" + v);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

#if defined(_WIN32)
    HANDLE hChildStd_IN_Rd = NULL;
    HANDLE hChildStd_IN_Wr = NULL;
    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;
    HANDLE hChildStd_ERR_Rd = NULL;
    HANDLE hChildStd_ERR_Wr = NULL;
    
    SECURITY_ATTRIBUTES saAttr; 
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 
    
    CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0);
    SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);
    
    CreatePipe(&hChildStd_ERR_Rd, &hChildStd_ERR_Wr, &saAttr, 0);
    SetHandleInformation(hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0);
    
    CreatePipe(&hChildStd_IN_Rd, &hChildStd_IN_Wr, &saAttr, 0);
    SetHandleInformation(hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0);
    
    STARTUPINFOA siStartInfo;
    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
    siStartInfo.cb = sizeof(STARTUPINFOA); 
    siStartInfo.hStdError = hChildStd_ERR_Wr;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.hStdInput = hChildStd_IN_Rd;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;
    
    std::string cmdLine = buildCommandLine(cmd, args);
    std::vector<char> cmdLineMod(cmdLine.begin(), cmdLine.end());
    cmdLineMod.push_back('\0');
    
    std::vector<char> envBlock;
    if (has_env)
    {
        for (const auto& e : env)
        {
            envBlock.insert(envBlock.end(), e.begin(), e.end());
            envBlock.push_back('\0');
        }
        envBlock.push_back('\0');
    }
    
    BOOL bSuccess = CreateProcessA(
        NULL,
        cmdLineMod.data(),
        NULL,
        NULL,
        TRUE,
        0,
        has_env ? envBlock.data() : NULL,
        cwd.empty() ? NULL : cwd.c_str(),
        &siStartInfo,
        &piProcInfo
    );
    
    if (!bSuccess)
        luaL_error(L, "Failed to create process");
        
    if (!stdin_data.empty())
    {
        DWORD dwWritten;
        WriteFile(hChildStd_IN_Wr, stdin_data.c_str(), stdin_data.size(), &dwWritten, NULL);
    }
    
    CloseHandle(hChildStd_OUT_Wr);
    CloseHandle(hChildStd_ERR_Wr);
    CloseHandle(hChildStd_IN_Wr);
    CloseHandle(hChildStd_IN_Rd);
    
    std::string stdout_str = readPipe(hChildStd_OUT_Rd);
    std::string stderr_str = readPipe(hChildStd_ERR_Rd);
    
    CloseHandle(hChildStd_OUT_Rd);
    CloseHandle(hChildStd_ERR_Rd);
    
    WaitForSingleObject(piProcInfo.hProcess, INFINITE);
    DWORD exitCode;
    GetExitCodeProcess(piProcInfo.hProcess, &exitCode);
    
    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);
    
    lua_newtable(L);
    lua_pushlstring(L, stdout_str.c_str(), stdout_str.size());
    lua_setfield(L, -2, "stdout");
    lua_pushlstring(L, stderr_str.c_str(), stderr_str.size());
    lua_setfield(L, -2, "stderr");
    lua_pushinteger(L, exitCode);
    lua_setfield(L, -2, "exitcode");
    
    return 1;
#else
    int pipe_out[2], pipe_err[2], pipe_in[2];
    if (pipe(pipe_out) == -1 || pipe(pipe_err) == -1 || pipe(pipe_in) == -1)
        luaL_error(L, "Failed to create pipes");
        
    pid_t pid = fork();
    if (pid == -1)
    {
        luaL_error(L, "Failed to fork");
    }
    else if (pid == 0)
    {
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);
        dup2(pipe_in[0], STDIN_FILENO);
        
        close(pipe_out[0]); close(pipe_out[1]);
        close(pipe_err[0]); close(pipe_err[1]);
        close(pipe_in[0]); close(pipe_in[1]);
        
        if (!cwd.empty())
        {
            if (chdir(cwd.c_str()) != 0)
                exit(1);
        }
        
        if (has_env)
        {
            for (const auto& e : env)
            {
                size_t eq = e.find('=');
                if (eq != std::string::npos)
                {
                    std::string k = e.substr(0, eq);
                    std::string v = e.substr(eq + 1);
                    setenv(k.c_str(), v.c_str(), 1);
                }
            }
        }
        
        std::vector<char*> c_args;
        c_args.push_back(const_cast<char*>(cmd));
        for (const auto& a : args)
            c_args.push_back(const_cast<char*>(a.c_str()));
        c_args.push_back(nullptr);
        
        execvp(cmd, c_args.data());
        exit(127);
    }
    else
    {
        close(pipe_out[1]);
        close(pipe_err[1]);
        close(pipe_in[0]);
        
        if (!stdin_data.empty())
        {
            ssize_t w = write(pipe_in[1], stdin_data.c_str(), stdin_data.size());
            (void)w;
        }
        close(pipe_in[1]);
        
        std::string stdout_str = readFd(pipe_out[0]);
        std::string stderr_str = readFd(pipe_err[0]);
        
        close(pipe_out[0]);
        close(pipe_err[0]);
        
        int status;
        waitpid(pid, &status, 0);
        
        int exitCode = -1;
        if (WIFEXITED(status))
            exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            exitCode = 128 + WTERMSIG(status);
            
        lua_newtable(L);
        lua_pushlstring(L, stdout_str.c_str(), stdout_str.size());
        lua_setfield(L, -2, "stdout");
        lua_pushlstring(L, stderr_str.c_str(), stderr_str.size());
        lua_setfield(L, -2, "stderr");
        lua_pushinteger(L, exitCode);
        lua_setfield(L, -2, "exitcode");
        
        return 1;
    }
#endif
}

static const luaL_Reg proclib[] = {
    {"exit", proc_exit},
    {"spawn", proc_spawn},
    {NULL, NULL}
};

int luaopen_process(lua_State* L)
{
    luaL_register(L, "process", proclib);
    
    lua_newtable(L);
    lua_setfield(L, -2, "args");
    
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
