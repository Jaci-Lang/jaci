# ADR 0016: Advanced Network, HTTP and WebSocket Subsystem

## Context

Native Luau runtimes require high-performance HTTP client/server communication, raw TCP stream manipulation, URL manipulation, and full RFC 6455 WebSocket client/server support.

## Decision

Implement a self-contained, zero-dependency networking library in `lnetlib.cpp`:
- **WebSocket Client & Protocol (RFC 6455)**:
  - `net.websocket(url, options)` and `net.websocketConnect(url)` handling standard HTTP 101 Switching Protocols upgrade handshakes.
  - Automatic frame masking/unmasking, control frame handling (ping/pong/close), text (0x1) and binary (0x2) payload frames.
  - Methods: `ws:send(data, [isBinary])`, `ws:receive([timeout])`, `ws:ping([data])`, `ws:pong([data])`, `ws:close([code], [reason])`, `ws:isOpen()`, `ws:url()`.
- **HTTP Client**:
  - `net.request({ url, method, headers, body, timeout })`, with convenience functions `net.get`, `net.post`, `net.put`, `net.delete`, `net.patch`, `net.head`.
  - HTTP/1.1 chunked transfer encoding and status parsing.
- **TCP Sockets & Listeners**:
  - `net.connect` / `net.tcpConnect`, `net.listen` / `net.tcpListen`.
  - Methods: `sock:send`, `sock:recv`, `sock:recvAll`, `sock:readline`, `sock:setNonBlocking`, `sock:settimeout`, `sock:getsockname`, `sock:getpeername`.
  - `listener:accept`, `listener:port`, `listener:getsockname`.
- **URL Utilities**:
  - `net.urlParse`, `net.urlFormat`, `net.urlEncode`, `net.urlDecode`.

## Consequences

- Direct native network communication without external C library dependencies or curl.
- Built-in support for real-time WebSocket applications and microservices.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
