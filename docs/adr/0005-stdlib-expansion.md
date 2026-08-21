# ADR 0005: Standard Library Expansion (json, hash, process, net)

## Context

Jaci relaxed the Roblox sandbox (ADR 0003) and added `fs`, `io`, and extended `os`. However, a truly self-bootstrapping runtime needs more: JSON parsing for configuration and API communication, cryptographic hashing for integrity checks, structured process spawning for build tools and scripting, and basic networking for fetching resources.

These are table-stakes features in every modern scripting runtime (Python, Node.js, Deno, Bun). Without them, Jaci users must either shell out via `os.execute` (losing structured output) or write C++ bindings manually.

## Decision

Add four new VM-level libraries, all registered by default in `luaL_openlibs`:

### `json`
- `json.encode(value, opts?)` -> string. Recursive serializer with cycle detection.
- `json.decode(str)` -> value, or nil+errmsg on failure. Strict JSON parser.
- `json.null` -> sentinel for roundtripping JSON null.
- Self-contained recursive descent parser in C++. No external dependencies.

### `hash`
- `hash.crc32(data)`, `hash.fnv1a(data)` -> number.
- `hash.md5(data)`, `hash.sha1(data)`, `hash.sha256(data)` -> raw digest string.
- `hash.md5hex`, `hash.sha1hex`, `hash.sha256hex` -> hex-encoded string.
- All accept string or buffer inputs.
- All algorithms are self-contained C++ implementations. No OpenSSL or external crypto dependency.

### `process`
- `process.spawn(cmd, args?, opts?)` -> `{stdout, stderr, exitcode}`. Synchronous.
- `process.env` -> proxy table for getenv/setenv.
- `process.exit(code?)` -> terminate.
- Uses fork+execvp (POSIX) / CreateProcess (Win32) with pipe capture.

### `net`
- `net.request(opts)` -> `{ok, status, headers, body}`. Synchronous HTTP/1.1.
- `net.connect(host, port)` -> Socket userdata (blocking TCP).
- Socket methods: send, recv, close, settimeout.
- No HTTPS (no OpenSSL dependency). TLS can be layered via `ffi` in a follow-up.
- Uses POSIX sockets / Winsock2.

## Consequences

- Jaci becomes a viable standalone scripting runtime without external dependencies for common tasks.
- The VM binary size increases modestly (hash algorithms are compact; JSON parser is ~500 lines).
- `net` introduces a link dependency on `ws2_32` (Windows only). POSIX sockets are part of libc.
- All new libraries follow existing conventions: luaL_Reg arrays, luaopen_* entry points, registered in linit.cpp.
- Type definitions for `luau-analyze` should be added to EmbeddedBuiltinDefinitions.cpp in a follow-up.
