// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cctype>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSESOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define CLOSESOCKET close
#endif

#define SOCKET_MT "Socket*"

struct Socket {
    socket_t sock;
};

static Socket* check_socket(lua_State* L, int idx) {
    return (Socket*)luaL_checkudata(L, idx, SOCKET_MT);
}

static int socket_send(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        luaL_error(L, "attempt to use a closed socket");
    }
    size_t len;
    const char* data;
    if (lua_isstring(L, 2)) {
        data = lua_tolstring(L, 2, &len);
    } else if (lua_type(L, 2) == LUA_TBUFFER) {
        data = (const char*)lua_tobuffer(L, 2, &len);
    } else {
        luaL_argerror(L, 2, "string or buffer expected");
        return 0;
    }
    
    int sent = send(s->sock, data, (int)len, 0);
    if (sent < 0) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, sent);
    }
    return 1;
}

static int socket_recv(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        luaL_error(L, "attempt to use a closed socket");
    }
    int n = luaL_checkinteger(L, 2);
    if (n <= 0) {
        luaL_error(L, "invalid receive size");
    }
    
    std::vector<char> buf(n);
    int received = recv(s->sock, buf.data(), n, 0);
    if (received <= 0) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, buf.data(), received);
    }
    return 1;
}

static int socket_close(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock != SOCKET_INVALID) {
        CLOSESOCKET(s->sock);
        s->sock = SOCKET_INVALID;
    }
    return 0;
}

static int socket_gc(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock != SOCKET_INVALID) {
        CLOSESOCKET(s->sock);
        s->sock = SOCKET_INVALID;
    }
    return 0;
}

static int socket_tostring(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock != SOCKET_INVALID) {
        lua_pushstring(L, "socket (connected)");
    } else {
        lua_pushstring(L, "socket (closed)");
    }
    return 1;
}

static int socket_settimeout(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        luaL_error(L, "attempt to use a closed socket");
    }
    double seconds = luaL_checknumber(L, 2);
#if defined(_WIN32)
    DWORD timeout = (DWORD)(seconds * 1000.0);
    setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s->sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = (time_t)seconds;
    tv.tv_usec = (suseconds_t)((seconds - tv.tv_sec) * 1000000.0);
    setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    return 0;
}

static const luaL_Reg socket_methods[] = {
    {"send", socket_send},
    {"recv", socket_recv},
    {"close", socket_close},
    {"settimeout", socket_settimeout},
    {"__gc", socket_gc},
    {"__tostring", socket_tostring},
    {NULL, NULL}
};

static int net_connect(lua_State* L) {
    size_t host_len;
    const char* host = luaL_checklstring(L, 1, &host_len);
    int port = luaL_checkinteger(L, 2);
    
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        luaL_error(L, "getaddrinfo failed");
    }
    
    socket_t sock = SOCKET_INVALID;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == SOCKET_INVALID) continue;
        if (connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) {
            break;
        }
        CLOSESOCKET(sock);
        sock = SOCKET_INVALID;
    }
    freeaddrinfo(res);
    
    if (sock == SOCKET_INVALID) {
        luaL_error(L, "connection failed");
    }
    
    Socket* s = (Socket*)lua_newuserdata(L, sizeof(Socket));
    s->sock = sock;
    luaL_getmetatable(L, SOCKET_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static bool parse_url(const std::string& url, std::string& host, int& port, std::string& path) {
    if (url.compare(0, 7, "http://") != 0) return false;
    size_t host_start = 7;
    size_t path_start = url.find('/', host_start);
    size_t port_start = url.find(':', host_start);

    if (port_start != std::string::npos && (path_start == std::string::npos || port_start < path_start)) {
        host = url.substr(host_start, port_start - host_start);
        std::string port_str;
        if (path_start == std::string::npos) {
            port_str = url.substr(port_start + 1);
            path = "/";
        } else {
            port_str = url.substr(port_start + 1, path_start - port_start - 1);
            path = url.substr(path_start);
        }
        port = atoi(port_str.c_str());
    } else {
        if (path_start == std::string::npos) {
            host = url.substr(host_start);
            path = "/";
        } else {
            host = url.substr(host_start, path_start - host_start);
            path = url.substr(path_start);
        }
        port = 80;
    }
    return true;
}

static int net_request(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    
    lua_getfield(L, 1, "url");
    if (!lua_isstring(L, -1)) luaL_error(L, "url is required");
    std::string url = lua_tostring(L, -1);
    lua_pop(L, 1);
    
    std::string method = "GET";
    lua_getfield(L, 1, "method");
    if (lua_isstring(L, -1)) method = lua_tostring(L, -1);
    lua_pop(L, 1);
    
    std::string body = "";
    lua_getfield(L, 1, "body");
    if (lua_isstring(L, -1)) {
        size_t blen;
        const char* b = lua_tolstring(L, -1, &blen);
        body.assign(b, blen);
    }
    lua_pop(L, 1);
    
    std::string host, path;
    int port;
    if (!parse_url(url, host, port, path)) {
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        return 1;
    }
    
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        return 1;
    }
    
    socket_t sock = SOCKET_INVALID;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == SOCKET_INVALID) continue;
        if (connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) {
            break;
        }
        CLOSESOCKET(sock);
        sock = SOCKET_INVALID;
    }
    freeaddrinfo(res);
    
    if (sock == SOCKET_INVALID) {
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        return 1;
    }
    
    std::string request_str = method + " " + path + " HTTP/1.1\r\n";
    request_str += "Host: " + host + "\r\n";
    request_str += "Connection: close\r\n";
    
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                request_str += lua_tostring(L, -2);
                request_str += ": ";
                request_str += lua_tostring(L, -1);
                request_str += "\r\n";
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    
    if (!body.empty()) {
        char clen[32];
        snprintf(clen, sizeof(clen), "%zu", body.length());
        request_str += "Content-Length: ";
        request_str += clen;
        request_str += "\r\n";
    }
    request_str += "\r\n";
    request_str += body;
    
    send(sock, request_str.c_str(), (int)request_str.length(), 0);
    
    std::string response;
    char buf[4096];
    while (true) {
        int r = recv(sock, buf, sizeof(buf), 0);
        if (r <= 0) break;
        response.append(buf, r);
    }
    CLOSESOCKET(sock);
    
    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        return 1;
    }
    
    std::string headers_str = response.substr(0, header_end);
    std::string response_body = response.substr(header_end + 4);
    
    size_t first_line_end = headers_str.find("\r\n");
    std::string first_line = headers_str.substr(0, first_line_end);
    
    int status_code = 0;
    size_t space1 = first_line.find(' ');
    if (space1 != std::string::npos) {
        size_t space2 = first_line.find(' ', space1 + 1);
        if (space2 != std::string::npos) {
            status_code = atoi(first_line.substr(space1 + 1, space2 - space1 - 1).c_str());
        }
    }
    
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, status_code);
    lua_setfield(L, -2, "status");
    
    lua_newtable(L);
    size_t pos = first_line_end;
    if (pos != std::string::npos) pos += 2;
    while (pos != std::string::npos && pos < headers_str.length()) {
        size_t line_end = headers_str.find("\r\n", pos);
        std::string line;
        if (line_end == std::string::npos) {
            line = headers_str.substr(pos);
            pos = std::string::npos;
        } else {
            line = headers_str.substr(pos, line_end - pos);
            pos = line_end + 2;
        }
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            size_t val_start = colon + 1;
            while (val_start < line.length() && (line[val_start] == ' ' || line[val_start] == '\t')) val_start++;
            std::string val = line.substr(val_start);
            
            for (char& c : key) c = std::tolower((unsigned char)c);
            
            lua_pushlstring(L, val.c_str(), val.length());
            lua_setfield(L, -2, key.c_str());
        }
    }
    lua_setfield(L, -2, "headers");
    
    // Chunked parsing if Transfer-Encoding: chunked is present
    lua_getfield(L, -1, "headers");
    lua_getfield(L, -1, "transfer-encoding");
    bool is_chunked = false;
    if (lua_isstring(L, -1)) {
        std::string te = lua_tostring(L, -1);
        if (te == "chunked") is_chunked = true;
    }
    lua_pop(L, 2); // pop transfer-encoding value and headers table
    
    if (is_chunked) {
        std::string decoded_body;
        size_t chunk_start = 0;
        while (chunk_start < response_body.length()) {
            size_t rn = response_body.find("\r\n", chunk_start);
            if (rn == std::string::npos) break;
            std::string hex = response_body.substr(chunk_start, rn - chunk_start);
            size_t size = strtoul(hex.c_str(), NULL, 16);
            if (size == 0) break;
            size_t data_start = rn + 2;
            if (data_start + size > response_body.length()) break;
            decoded_body.append(response_body.substr(data_start, size));
            chunk_start = data_start + size + 2; // +2 for \r\n
        }
        lua_pushlstring(L, decoded_body.c_str(), decoded_body.length());
    } else {
        lua_pushlstring(L, response_body.c_str(), response_body.length());
    }
    lua_setfield(L, -2, "body");
    
    return 1;
}

static const luaL_Reg netlib[] = {
    {"connect", net_connect},
    {"request", net_request},
    {NULL, NULL}
};

int luaopen_net(lua_State* L) {
#if defined(_WIN32)
    static bool wsa_initialized = false;
    if (!wsa_initialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        wsa_initialized = true;
    }
#endif

    luaL_newmetatable(L, SOCKET_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, socket_methods);
    lua_pop(L, 1);

    luaL_register(L, "net", netlib);
    return 1;
}
