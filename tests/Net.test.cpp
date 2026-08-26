// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/Common.h"
#include "Luau/Repl.h"
#include "lua.h"
#include "lualib.h"
#include "doctest.h"
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#define CLOSESOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define CLOSESOCKET close
#define SOCKET_INVALID (-1)
typedef int socket_t;
#endif

TEST_SUITE_BEGIN("NetTests");

TEST_CASE("UrlParsingAndFormatting")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local parsed = net.urlParse("https://api.example.com:8443/v1/users?search=lua&sort=desc#profile")
        assert(parsed.scheme == "https")
        assert(parsed.host == "api.example.com")
        assert(parsed.port == 8443)
        assert(parsed.path == "/v1/users")
        assert(parsed.query == "search=lua&sort=desc")
        assert(parsed.fragment == "profile")

        local formatted = net.urlFormat({
            scheme = "http",
            host = "localhost",
            port = 3000,
            path = "/health",
            query = "debug=true"
        })
        assert(formatted == "http://localhost:3000/health?debug=true")

        local encoded = net.urlEncode("hello world & foo=bar/baz")
        assert(encoded == "hello%20world%20%26%20foo%3Dbar%2Fbaz")
        assert(net.urlDecode(encoded) == "hello world & foo=bar/baz")
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("TcpClientAndServerLoopback")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local server = net.listen("127.0.0.1", 0)
        assert(server ~= nil)
        local port = server.port
        assert(port > 0)

        local client = net.connect("127.0.0.1", port)
        assert(client ~= nil)

        local accepted = server:accept()
        assert(accepted ~= nil)

        client:send("PING\n")
        local line = accepted:readline()
        assert(line == "PING\n")

        accepted:send("PONG\n")
        local resp = client:readline()
        assert(resp == "PONG\n")

        client:close()
        accepted:close()
        server:close()
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("WebSocketClientServerIntegration")
{
    socket_t server = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(server != SOCKET_INVALID);
    int opt = 1;
    REQUIRE(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(server, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    REQUIRE(listen(server, 1) == 0);

    socklen_t len = sizeof(addr);
    REQUIRE(getsockname(server, (struct sockaddr*)&addr, &len) == 0);
    int boundPort = ntohs(addr.sin_port);
    REQUIRE(boundPort > 0);

    std::thread server_thread([server]() {

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        socket_t cs = accept(server, (struct sockaddr*)&caddr, &clen);
        if (cs != SOCKET_INVALID)
        {
            char buf[1024];
            int r = recv(cs, buf, sizeof(buf), 0);
            if (r > 0)
            {
                const char* resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
                send(cs, resp, (int)strlen(resp), 0);

                uint8_t hdr[2];
                if (recv(cs, (char*)hdr, 2, 0) == 2)
                {
                    uint8_t mask[4];
                    recv(cs, (char*)mask, 4, 0);
                    int plen = hdr[1] & 0x7F;
                    std::vector<char> pbuf(plen);
                    recv(cs, pbuf.data(), plen, 0);

                    std::this_thread::sleep_for(std::chrono::milliseconds(50));

                    std::string s_reply = "pong_from_server";
                    std::string s_frame;
                    s_frame.push_back((char)0x81);
                    s_frame.push_back((char)s_reply.size());
                    s_frame.append(s_reply);
                    send(cs, s_frame.data(), (int)s_frame.size(), 0);
                }
            }
            CLOSESOCKET(cs);
        }
        CLOSESOCKET(server);
    });

    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    std::string script = "local ws = net.websocket('ws://127.0.0.1:" + std::to_string(boundPort) + "/test')\n"
                         "assert(ws ~= nil and ws:isOpen())\n"
                         "ws:send('ping_from_client')\n"
                         "local reply, isBin\n"
                         "local heartbeatRan = false\n"
                         "task.delay(0.01, function() heartbeatRan = true end)\n"
                         "task.spawn(function() reply, isBin = ws:receive() end)\n"
                         "task.run()\n"
                         "assert(heartbeatRan)\n"
                         "assert(reply == 'pong_from_server')\n"
                         "assert(isBin == false)\n"
                         "ws:close()\n";

    std::string err = runCode(L, script);
    server_thread.join();
    INFO(err);
    CHECK(err.empty());
}

TEST_SUITE_END();
