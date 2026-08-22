// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.

#include "lualib.h"
#include "lcommon.h"

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
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
#define LISTENER_MT "Listener*"
#define WEBSOCKET_MT "WebSocket*"

// ============================================================================
// Internal Cryptographic & Base64 Helpers for WebSocket RFC 6455
// ============================================================================

static void get_random_mask_bytes(uint8_t* out, size_t count)
{
#if defined(_WIN32)
    HCRYPTPROV hProvider = 0;
    if (CryptAcquireContextW(&hProvider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
    {
        CryptGenRandom(hProvider, (DWORD)count, out);
        CryptReleaseContext(hProvider, 0);
        return;
    }
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f)
    {
        size_t r = fread(out, 1, count, f);
        fclose(f);
        if (r == count)
            return;
    }
#endif
    for (size_t i = 0; i < count; i++)
        out[i] = (uint8_t)(rand() & 0xFF);
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t b = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) b |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];

        out.push_back(b64_table[(b >> 18) & 0x3F]);
        out.push_back(b64_table[(b >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? b64_table[(b >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? b64_table[b & 0x3F] : '=');
    }
    return out;
}

// SHA-1 Implementation for RFC 6455
#define SHA1_ROL(val, bits) (((val) << (bits)) | ((val) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t block[80];

    for (int i = 0; i < 16; i++)
    {
        block[i] = ((uint32_t)buffer[i * 4] << 24) |
                   ((uint32_t)buffer[i * 4 + 1] << 16) |
                   ((uint32_t)buffer[i * 4 + 2] << 8) |
                   ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++)
        block[i] = SHA1_ROL(block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);

    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }

        uint32_t temp = SHA1_ROL(a, 5) + f + e + k + block[i];
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void compute_sha1(const uint8_t* data, size_t len, uint8_t digest[20])
{
    uint32_t state[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buffer[64];
    size_t offset = 0;

    for (size_t i = 0; i < len; ++i)
    {
        buffer[offset++] = data[i];
        if (offset == 64)
        {
            sha1_transform(state, buffer);
            offset = 0;
        }
    }

    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        sha1_transform(state, buffer);
        offset = 0;
    }
    while (offset < 56) buffer[offset++] = 0;

    uint64_t total_bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (uint8_t)((total_bits >> ((7 - i) * 8)) & 0xFF);

    sha1_transform(state, buffer);

    for (int i = 0; i < 5; ++i)
    {
        digest[i * 4] = (uint8_t)((state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(state[i] & 0xFF);
    }
}

// ============================================================================
// Socket & Listener Structures
// ============================================================================

struct Socket {
    socket_t sock;
};

struct Listener {
    socket_t sock;
    int port;
};

struct WebSocket {
    socket_t sock;
    std::string url;
    bool is_open;
    bool is_server;
};

static Socket* check_socket(lua_State* L, int idx) {
    return (Socket*)luaL_checkudata(L, idx, SOCKET_MT);
}

static Listener* check_listener(lua_State* L, int idx) {
    return (Listener*)luaL_checkudata(L, idx, LISTENER_MT);
}

static WebSocket* check_websocket(lua_State* L, int idx) {
    return (WebSocket*)luaL_checkudata(L, idx, WEBSOCKET_MT);
}

// ============================================================================
// Socket Methods
// ============================================================================

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

static int socket_recvall(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        luaL_error(L, "attempt to use a closed socket");
    }
    int max_bytes = luaL_optinteger(L, 2, 65536);
    if (max_bytes <= 0) max_bytes = 65536;

    std::string result;
    char buffer[4096];
    while ((int)result.size() < max_bytes) {
        int to_read = std::min((int)sizeof(buffer), max_bytes - (int)result.size());
        int received = recv(s->sock, buffer, to_read, 0);
        if (received <= 0) break;
        result.append(buffer, received);
    }

    if (result.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, result.data(), result.size());
    }
    return 1;
}

static int socket_readline(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        luaL_error(L, "attempt to use a closed socket");
    }
    int maxLen = luaL_optinteger(L, 2, 4096);
    if (maxLen <= 0) maxLen = 4096;

    std::string line;
    char c = 0;
    while ((int)line.size() < maxLen) {
        int r = recv(s->sock, &c, 1, 0);
        if (r <= 0) {
            if (line.empty()) {
                lua_pushnil(L);
                return 1;
            }
            break;
        }
        line += c;
        if (c == '\n') {
            break;
        }
    }

    lua_pushlstring(L, line.data(), line.size());
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

static int socket_getsockname(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        lua_pushnil(L);
        return 1;
    }
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(s->sock, (struct sockaddr*)&addr, &len) == 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
        lua_createtable(L, 0, 2);
        lua_pushstring(L, ip);
        lua_setfield(L, -2, "host");
        lua_pushinteger(L, ntohs(addr.sin_port));
        lua_setfield(L, -2, "port");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int socket_getpeername(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        lua_pushnil(L);
        return 1;
    }
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(s->sock, (struct sockaddr*)&addr, &len) == 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
        lua_createtable(L, 0, 2);
        lua_pushstring(L, ip);
        lua_setfield(L, -2, "host");
        lua_pushinteger(L, ntohs(addr.sin_port));
        lua_setfield(L, -2, "port");
        return 1;
    }
    lua_pushnil(L);
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

static int socket_setnonblocking(lua_State* L) {
    Socket* s = check_socket(L, 1);
    if (s->sock == SOCKET_INVALID) {
        luaL_error(L, "attempt to use a closed socket");
    }
    bool nonblocking = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : true;
#if defined(_WIN32)
    u_long mode = nonblocking ? 1 : 0;
    ioctlsocket(s->sock, FIONBIO, &mode);
#else
    int flags = fcntl(s->sock, F_GETFL, 0);
    if (flags != -1) {
        if (nonblocking) flags |= O_NONBLOCK;
        else flags &= ~O_NONBLOCK;
        fcntl(s->sock, F_SETFL, flags);
    }
#endif
    return 0;
}

static const luaL_Reg socket_methods[] = {
    {"send", socket_send},
    {"recv", socket_recv},
    {"recvAll", socket_recvall},
    {"recvall", socket_recvall},
    {"readline", socket_readline},
    {"close", socket_close},
    {"settimeout", socket_settimeout},
    {"setNonBlocking", socket_setnonblocking},
    {"setnonblocking", socket_setnonblocking},
    {"getsockname", socket_getsockname},
    {"sockname", socket_getsockname},
    {"getpeername", socket_getpeername},
    {"peer", socket_getpeername},
    {"peername", socket_getpeername},
    {"__gc", socket_gc},
    {"__tostring", socket_tostring},
    {NULL, NULL}
};

// ============================================================================
// Listener Methods
// ============================================================================

static int listener_accept(lua_State* L) {
    Listener* l = check_listener(L, 1);
    if (l->sock == SOCKET_INVALID) {
        luaL_error(L, "attempt to use a closed listener");
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    socket_t client_sock = accept(l->sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock == SOCKET_INVALID) {
        lua_pushnil(L);
        return 1;
    }

    Socket* s = (Socket*)lua_newuserdata(L, sizeof(Socket));
    s->sock = client_sock;
    luaL_getmetatable(L, SOCKET_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int listener_close(lua_State* L) {
    Listener* l = check_listener(L, 1);
    if (l->sock != SOCKET_INVALID) {
        CLOSESOCKET(l->sock);
        l->sock = SOCKET_INVALID;
    }
    return 0;
}

static int listener_gc(lua_State* L) {
    Listener* l = check_listener(L, 1);
    if (l->sock != SOCKET_INVALID) {
        CLOSESOCKET(l->sock);
        l->sock = SOCKET_INVALID;
    }
    return 0;
}

static int listener_tostring(lua_State* L) {
    Listener* l = check_listener(L, 1);
    if (l->sock != SOCKET_INVALID) {
        lua_pushfstring(L, "listener (port %d)", l->port);
    } else {
        lua_pushstring(L, "listener (closed)");
    }
    return 1;
}

static int listener_port(lua_State* L) {
    Listener* l = check_listener(L, 1);
    lua_pushinteger(L, l->port);
    return 1;
}

static int listener_getsockname(lua_State* L) {
    Listener* l = check_listener(L, 1);
    if (l->sock == SOCKET_INVALID) {
        lua_pushnil(L);
        return 1;
    }
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(l->sock, (struct sockaddr*)&addr, &len) == 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
        lua_createtable(L, 0, 2);
        lua_pushstring(L, ip);
        lua_setfield(L, -2, "host");
        lua_pushinteger(L, ntohs(addr.sin_port));
        lua_setfield(L, -2, "port");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int listener_index(lua_State* L) {
    Listener* l = check_listener(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (strcmp(key, "port") == 0) {
        lua_pushinteger(L, l->port);
        return 1;
    }
    luaL_getmetatable(L, LISTENER_MT);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

static const luaL_Reg listener_methods[] = {
    {"accept", listener_accept},
    {"port", listener_port},
    {"getsockname", listener_getsockname},
    {"sockname", listener_getsockname},
    {"close", listener_close},
    {"__index", listener_index},
    {"__gc", listener_gc},
    {"__tostring", listener_tostring},
    {NULL, NULL}
};

// ============================================================================
// WebSocket RFC 6455 Implementation
// ============================================================================

static bool ws_send_frame(socket_t sock, uint8_t opcode, const uint8_t* payload, size_t len, bool is_client)
{
    std::vector<uint8_t> frame;
    frame.reserve(14 + len);

    // Byte 0: FIN = 1 | opcode
    frame.push_back(0x80 | (opcode & 0x0F));

    // Byte 1: Mask bit | payload length
    uint8_t mask_bit = is_client ? 0x80 : 0x00;
    if (len < 126)
    {
        frame.push_back(mask_bit | (uint8_t)len);
    }
    else if (len <= 0xFFFF)
    {
        frame.push_back(mask_bit | 126);
        frame.push_back((uint8_t)((len >> 8) & 0xFF));
        frame.push_back((uint8_t)(len & 0xFF));
    }
    else
    {
        frame.push_back(mask_bit | 127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }

    if (is_client)
    {
        uint8_t mask[4];
        get_random_mask_bytes(mask, 4);
        frame.insert(frame.end(), mask, mask + 4);

        size_t header_size = frame.size();
        frame.resize(header_size + len);
        for (size_t i = 0; i < len; i++)
            frame[header_size + i] = payload[i] ^ mask[i % 4];
    }
    else
    {
        frame.insert(frame.end(), payload, payload + len);
    }

    size_t total_sent = 0;
    while (total_sent < frame.size())
    {
        int sent = send(sock, (const char*)frame.data() + total_sent, (int)(frame.size() - total_sent), 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

static bool ws_recv_exact(socket_t sock, uint8_t* out, size_t len)
{
    size_t total = 0;
    while (total < len)
    {
        int r = recv(sock, (char*)out + total, (int)(len - total), 0);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

static int ws_send(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    if (!ws->is_open || ws->sock == SOCKET_INVALID)
    {
        luaL_error(L, "attempt to send on a closed WebSocket");
    }

    size_t len = 0;
    const char* data = NULL;
    bool is_binary = false;

    if (lua_isstring(L, 2))
    {
        data = lua_tolstring(L, 2, &len);
        if (lua_isboolean(L, 3))
            is_binary = lua_toboolean(L, 3);
    }
    else if (lua_type(L, 2) == LUA_TBUFFER)
    {
        data = (const char*)lua_tobuffer(L, 2, &len);
        is_binary = true;
    }
    else
    {
        luaL_argerror(L, 2, "string or buffer expected");
        return 0;
    }

    uint8_t opcode = is_binary ? 0x02 : 0x01;
    bool ok = ws_send_frame(ws->sock, opcode, (const uint8_t*)data, len, !ws->is_server);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int ws_receive(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    if (!ws->is_open || ws->sock == SOCKET_INVALID)
    {
        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }

    while (true)
    {
        uint8_t header[2];
        if (!ws_recv_exact(ws->sock, header, 2))
        {
            ws->is_open = false;
            lua_pushnil(L);
            lua_pushliteral(L, "connection closed");
            return 2;
        }

        uint8_t opcode = header[0] & 0x0F;
        bool has_mask = (header[1] & 0x80) != 0;
        uint64_t payload_len = header[1] & 0x7F;

        if (payload_len == 126)
        {
            uint8_t ext[2];
            if (!ws_recv_exact(ws->sock, ext, 2)) return 0;
            payload_len = ((uint64_t)ext[0] << 8) | ext[1];
        }
        else if (payload_len == 127)
        {
            uint8_t ext[8];
            if (!ws_recv_exact(ws->sock, ext, 8)) return 0;
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | ext[i];
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        if (has_mask)
        {
            if (!ws_recv_exact(ws->sock, mask, 4)) return 0;
        }

        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0)
        {
            if (!ws_recv_exact(ws->sock, payload.data(), (size_t)payload_len)) return 0;
            if (has_mask)
            {
                for (size_t i = 0; i < payload.size(); i++)
                    payload[i] ^= mask[i % 4];
            }
        }

        // Control frame handling
        if (opcode == 0x08) // Close
        {
            ws->is_open = false;
            CLOSESOCKET(ws->sock);
            ws->sock = SOCKET_INVALID;
            lua_pushnil(L);
            lua_pushliteral(L, "closed");
            return 2;
        }
        else if (opcode == 0x09) // Ping
        {
            ws_send_frame(ws->sock, 0x0A, payload.data(), payload.size(), !ws->is_server); // Reply with Pong
            continue;
        }
        else if (opcode == 0x0A) // Pong
        {
            continue; // Ignore unsolicited pong
        }
        else if (opcode == 0x01 || opcode == 0x02) // Text or Binary
        {
            lua_pushlstring(L, (const char*)payload.data(), payload.size());
            lua_pushboolean(L, opcode == 0x02 ? 1 : 0);
            return 2;
        }
    }
}

static int ws_ping(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    if (!ws->is_open || ws->sock == SOCKET_INVALID) return 0;

    size_t len = 0;
    const char* data = lua_isstring(L, 2) ? lua_tolstring(L, 2, &len) : "";
    bool ok = ws_send_frame(ws->sock, 0x09, (const uint8_t*)data, len, !ws->is_server);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int ws_pong(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    if (!ws->is_open || ws->sock == SOCKET_INVALID) return 0;

    size_t len = 0;
    const char* data = lua_isstring(L, 2) ? lua_tolstring(L, 2, &len) : "";
    bool ok = ws_send_frame(ws->sock, 0x0A, (const uint8_t*)data, len, !ws->is_server);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int ws_close(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    if (ws->is_open && ws->sock != SOCKET_INVALID)
    {
        uint16_t code = (uint16_t)luaL_optinteger(L, 2, 1000);
        uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
        ws_send_frame(ws->sock, 0x08, payload, 2, !ws->is_server);
        CLOSESOCKET(ws->sock);
        ws->sock = SOCKET_INVALID;
        ws->is_open = false;
    }
    return 0;
}

static int ws_is_open(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    lua_pushboolean(L, (ws->is_open && ws->sock != SOCKET_INVALID) ? 1 : 0);
    return 1;
}

static int ws_url(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    lua_pushlstring(L, ws->url.data(), ws->url.size());
    return 1;
}

static int ws_gc(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    if (ws->sock != SOCKET_INVALID)
    {
        CLOSESOCKET(ws->sock);
        ws->sock = SOCKET_INVALID;
    }
    ws->is_open = false;
    return 0;
}

static int ws_tostring(lua_State* L)
{
    WebSocket* ws = check_websocket(L, 1);
    lua_pushfstring(L, "websocket (%s, %s)", ws->url.c_str(), ws->is_open ? "open" : "closed");
    return 1;
}

static const luaL_Reg websocket_methods[] = {
    {"send", ws_send},
    {"receive", ws_receive},
    {"recv", ws_receive},
    {"ping", ws_ping},
    {"pong", ws_pong},
    {"close", ws_close},
    {"isOpen", ws_is_open},
    {"isopen", ws_is_open},
    {"url", ws_url},
    {"__gc", ws_gc},
    {"__tostring", ws_tostring},
    {NULL, NULL}
};

// ============================================================================
// URL Parsing & Formatting
// ============================================================================

static bool parse_url_full(const std::string& url, std::string& scheme, std::string& host, int& port, std::string& path, std::string& query, std::string& fragment)
{
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;

    scheme = url.substr(0, scheme_end);
    for (char& c : scheme) c = (char)std::tolower((unsigned char)c);

    size_t host_start = scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    size_t query_start = url.find('?', host_start);
    size_t frag_start = url.find('#', host_start);

    size_t host_end = host_start;
    while (host_end < url.size() && url[host_end] != '/' && url[host_end] != '?' && url[host_end] != '#')
        host_end++;

    std::string host_port = url.substr(host_start, host_end - host_start);
    size_t colon = host_port.find(':');
    if (colon != std::string::npos)
    {
        host = host_port.substr(0, colon);
        port = atoi(host_port.substr(colon + 1).c_str());
    }
    else
    {
        host = host_port;
        if (scheme == "https" || scheme == "wss") port = 443;
        else if (scheme == "http" || scheme == "ws") port = 80;
        else port = 80;
    }

    if (path_start != std::string::npos && path_start < query_start && path_start < frag_start)
    {
        size_t p_end = (query_start != std::string::npos) ? query_start : ((frag_start != std::string::npos) ? frag_start : url.size());
        path = url.substr(path_start, p_end - path_start);
    }
    else
    {
        path = "/";
    }

    if (query_start != std::string::npos)
    {
        size_t q_end = (frag_start != std::string::npos) ? frag_start : url.size();
        query = url.substr(query_start + 1, q_end - query_start - 1);
    }
    else
    {
        query = "";
    }

    if (frag_start != std::string::npos)
    {
        fragment = url.substr(frag_start + 1);
    }
    else
    {
        fragment = "";
    }

    return true;
}

static int net_parseurl(lua_State* L) {
    size_t len;
    const char* url_cstr = luaL_checklstring(L, 1, &len);
    std::string url(url_cstr, len);

    std::string scheme, host, path, query, fragment;
    int port = 80;

    if (!parse_url_full(url, scheme, host, port, path, query, fragment)) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 6);
    lua_pushlstring(L, scheme.c_str(), scheme.length());
    lua_setfield(L, -2, "scheme");
    lua_pushlstring(L, host.c_str(), host.length());
    lua_setfield(L, -2, "host");
    lua_pushinteger(L, port);
    lua_setfield(L, -2, "port");
    lua_pushlstring(L, path.c_str(), path.length());
    lua_setfield(L, -2, "path");
    if (!query.empty()) {
        lua_pushlstring(L, query.c_str(), query.length());
        lua_setfield(L, -2, "query");
    }
    if (!fragment.empty()) {
        lua_pushlstring(L, fragment.c_str(), fragment.length());
        lua_setfield(L, -2, "fragment");
    }
    return 1;
}

static int net_urlformat(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "scheme");
    std::string scheme = lua_isstring(L, -1) ? lua_tostring(L, -1) : "http";
    lua_pop(L, 1);

    lua_getfield(L, 1, "host");
    std::string host = lua_isstring(L, -1) ? lua_tostring(L, -1) : "localhost";
    lua_pop(L, 1);

    lua_getfield(L, 1, "port");
    int port = lua_isnumber(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "path");
    std::string path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "/";
    lua_pop(L, 1);

    lua_getfield(L, 1, "query");
    std::string query = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    lua_getfield(L, 1, "fragment");
    std::string fragment = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    std::string formatted = scheme + "://" + host;
    if (port > 0 && !(port == 80 && scheme == "http") && !(port == 443 && scheme == "https") && !(port == 80 && scheme == "ws") && !(port == 443 && scheme == "wss"))
    {
        formatted += ":" + std::to_string(port);
    }
    if (path.empty() || path[0] != '/') formatted += "/";
    formatted += path;
    if (!query.empty()) formatted += "?" + query;
    if (!fragment.empty()) formatted += "#" + fragment;

    lua_pushlstring(L, formatted.data(), formatted.size());
    return 1;
}

static int net_urlencode(lua_State* L) {
    size_t len;
    const char* str = luaL_checklstring(L, 1, &len);
    std::string out;
    out.reserve(len * 2);

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }

    lua_pushlstring(L, out.c_str(), out.length());
    return 1;
}

static int net_urldecode(lua_State* L) {
    size_t len;
    const char* str = luaL_checklstring(L, 1, &len);
    std::string out;
    out.reserve(len);

    for (size_t i = 0; i < len; i++) {
        if (str[i] == '%' && i + 2 < len) {
            char hex[3] = { str[i+1], str[i+2], 0 };
            char* end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                out += (char)val;
                i += 2;
                continue;
            }
        }
        if (str[i] == '+') {
            out += ' ';
        } else {
            out += str[i];
        }
    }

    lua_pushlstring(L, out.c_str(), out.length());
    return 1;
}

// ============================================================================
// TCP Listen & Connect
// ============================================================================

static int net_listen(lua_State* L) {
    const char* host = "0.0.0.0";
    int port = 0;

    if (lua_isnumber(L, 1)) {
        port = luaL_checkinteger(L, 1);
    } else {
        host = luaL_checkstring(L, 1);
        port = luaL_checkinteger(L, 2);
    }

    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == SOCKET_INVALID) {
        luaL_error(L, "failed to create socket");
    }

    int opt = 1;
#if defined(_WIN32)
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSESOCKET(sock);
        luaL_error(L, "failed to bind socket to %s:%d", host, port);
    }

    if (listen(sock, 128) != 0) {
        CLOSESOCKET(sock);
        luaL_error(L, "failed to listen on socket");
    }

    if (port == 0) {
        socklen_t len = sizeof(addr);
        if (getsockname(sock, (struct sockaddr*)&addr, &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }

    Listener* l = (Listener*)lua_newuserdata(L, sizeof(Listener));
    l->sock = sock;
    l->port = port;
    luaL_getmetatable(L, LISTENER_MT);
    lua_setmetatable(L, -2);
    return 1;
}

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
        luaL_error(L, "connection failed to %s:%d", host, port);
    }

    Socket* s = (Socket*)lua_newuserdata(L, sizeof(Socket));
    s->sock = sock;
    luaL_getmetatable(L, SOCKET_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ============================================================================
// WebSocket Connect Client
// ============================================================================

static int net_websocket_connect(lua_State* L)
{
    const char* url_str = luaL_checkstring(L, 1);
    std::string scheme, host, path, query, fragment;
    int port = 80;

    if (!parse_url_full(url_str, scheme, host, port, path, query, fragment))
    {
        luaL_error(L, "invalid websocket url: %s", url_str);
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0)
    {
        luaL_error(L, "failed to resolve host: %s", host.c_str());
    }

    socket_t sock = SOCKET_INVALID;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next)
    {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == SOCKET_INVALID) continue;
        if (connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        CLOSESOCKET(sock);
        sock = SOCKET_INVALID;
    }
    freeaddrinfo(res);

    if (sock == SOCKET_INVALID)
    {
        luaL_error(L, "failed to connect to websocket %s:%d", host.c_str(), port);
    }

    // Handshake
    uint8_t nonce[16];
    get_random_mask_bytes(nonce, 16);
    std::string sec_key = b64_encode(nonce, 16);

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + sec_key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";

    send(sock, req.c_str(), (int)req.length(), 0);

    // Read Handshake Response
    std::string resp;
    char buf[1024];
    while (resp.find("\r\n\r\n") == std::string::npos)
    {
        int r = recv(sock, buf, sizeof(buf), 0);
        if (r <= 0) break;
        resp.append(buf, r);
    }

    if (resp.find("101") == std::string::npos)
    {
        CLOSESOCKET(sock);
        luaL_error(L, "websocket handshake rejected by server");
    }

    WebSocket* ws = (WebSocket*)lua_newuserdata(L, sizeof(WebSocket));
    new (ws) WebSocket();
    ws->sock = sock;
    ws->url = url_str;
    ws->is_open = true;
    ws->is_server = false;

    luaL_getmetatable(L, WEBSOCKET_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ============================================================================
// HTTP Client: net.request & Convenience Methods
// ============================================================================

static int net_request(lua_State* L) {
    std::string url;
    std::string method = "GET";
    std::string body = "";
    std::vector<std::pair<std::string, std::string>> headers;

    if (lua_isstring(L, 1)) {
        url = lua_tostring(L, 1);
    } else if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "url");
        if (lua_isstring(L, -1)) url = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "method");
        if (lua_isstring(L, -1)) method = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "body");
        if (lua_isstring(L, -1)) {
            size_t blen;
            const char* b = lua_tolstring(L, -1, &blen);
            body.assign(b, blen);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "headers");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                    headers.push_back({lua_tostring(L, -2), lua_tostring(L, -1)});
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    } else {
        luaL_argerror(L, 1, "string or table expected");
    }

    std::string scheme, host, path, query, fragment;
    int port = 80;
    if (!parse_url_full(url, scheme, host, port, path, query, fragment)) {
        luaL_error(L, "invalid or unsupported URL: %s", url.c_str());
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        luaL_error(L, "failed to resolve host: %s", host.c_str());
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
        luaL_error(L, "failed to connect to host: %s", host.c_str());
    }

    std::string request_str = method + " " + path + (query.empty() ? "" : ("?" + query)) + " HTTP/1.1\r\n";
    request_str += "Host: " + host + "\r\n";
    request_str += "User-Agent: Jaci/1.0\r\n";
    request_str += "Connection: close\r\n";

    bool has_content_length = false;
    for (const auto& h : headers) {
        request_str += h.first + ": " + h.second + "\r\n";
        std::string lower_k = h.first;
        for (char& c : lower_k) c = (char)tolower((unsigned char)c);
        if (lower_k == "content-length") has_content_length = true;
    }

    if (!body.empty() && !has_content_length) {
        request_str += "Content-Length: " + std::to_string(body.length()) + "\r\n";
    }
    request_str += "\r\n";
    request_str += body;

    send(sock, request_str.c_str(), (int)request_str.length(), 0);

    std::string response;
    char buffer[4096];
    while (true) {
        int bytes = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        response.append(buffer, bytes);
    }
    CLOSESOCKET(sock);

    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        luaL_error(L, "invalid HTTP response");
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

    lua_createtable(L, 0, 4);
    lua_pushboolean(L, (status_code >= 200 && status_code < 400) ? 1 : 0);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, status_code);
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, status_code);
    lua_setfield(L, -2, "statusCode");

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

            for (char& c : key) c = (char)std::tolower((unsigned char)c);

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
    lua_pop(L, 2);

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
            chunk_start = data_start + size + 2;
        }
        lua_pushlstring(L, decoded_body.c_str(), decoded_body.length());
    } else {
        lua_pushlstring(L, response_body.c_str(), response_body.length());
    }
    lua_setfield(L, -2, "body");

    return 1;
}

static int net_get(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");
    lua_pushliteral(L, "GET");
    lua_setfield(L, -2, "method");
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) lua_setfield(L, -3, "headers");
        else lua_pop(L, 1);
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return net_request(L);
}

static int net_post(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    size_t blen = 0;
    const char* body = lua_isstring(L, 2) ? lua_tolstring(L, 2, &blen) : "";
    lua_createtable(L, 0, 4);
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");
    lua_pushliteral(L, "POST");
    lua_setfield(L, -2, "method");
    lua_pushlstring(L, body, blen);
    lua_setfield(L, -2, "body");
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) lua_setfield(L, -3, "headers");
        else lua_pop(L, 1);
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return net_request(L);
}

static int net_put(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    size_t blen = 0;
    const char* body = lua_isstring(L, 2) ? lua_tolstring(L, 2, &blen) : "";
    lua_createtable(L, 0, 4);
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");
    lua_pushliteral(L, "PUT");
    lua_setfield(L, -2, "method");
    lua_pushlstring(L, body, blen);
    lua_setfield(L, -2, "body");
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) lua_setfield(L, -3, "headers");
        else lua_pop(L, 1);
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return net_request(L);
}

static int net_delete(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");
    lua_pushliteral(L, "DELETE");
    lua_setfield(L, -2, "method");
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) lua_setfield(L, -3, "headers");
        else lua_pop(L, 1);
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return net_request(L);
}

static int net_patch(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    size_t blen = 0;
    const char* body = lua_isstring(L, 2) ? lua_tolstring(L, 2, &blen) : "";
    lua_createtable(L, 0, 4);
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");
    lua_pushliteral(L, "PATCH");
    lua_setfield(L, -2, "method");
    lua_pushlstring(L, body, blen);
    lua_setfield(L, -2, "body");
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) lua_setfield(L, -3, "headers");
        else lua_pop(L, 1);
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return net_request(L);
}

static int net_head(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");
    lua_pushliteral(L, "HEAD");
    lua_setfield(L, -2, "method");
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) lua_setfield(L, -3, "headers");
        else lua_pop(L, 1);
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return net_request(L);
}

// ============================================================================
// Registration
// ============================================================================

static const luaL_Reg netlib[] = {
    {"connect", net_connect},
    {"tcpConnect", net_connect},
    {"listen", net_listen},
    {"tcpListen", net_listen},
    {"request", net_request},
    {"get", net_get},
    {"post", net_post},
    {"put", net_put},
    {"delete", net_delete},
    {"patch", net_patch},
    {"head", net_head},
    {"websocket", net_websocket_connect},
    {"websocketConnect", net_websocket_connect},
    {"parseurl", net_parseurl},
    {"urlParse", net_parseurl},
    {"urlformat", net_urlformat},
    {"urlFormat", net_urlformat},
    {"urlencode", net_urlencode},
    {"urlEncode", net_urlencode},
    {"urldecode", net_urldecode},
    {"urlDecode", net_urldecode},
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

    luaL_newmetatable(L, LISTENER_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, listener_methods);
    lua_pop(L, 1);

    luaL_newmetatable(L, WEBSOCKET_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, websocket_methods);
    lua_pop(L, 1);

    luaL_register(L, "net", netlib);
    return 1;
}
