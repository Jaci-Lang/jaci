# ADR 0003: Sandbox Relaxation and Filesystem/IO Runtime

## Context

Luau was designed with a Roblox-specific sandboxed execution model: the `os` library exposed only time/date functions, no filesystem or IO libraries existed, `loadfile`/`dofile` were absent, and the CLI called `luaL_sandbox(L)` unconditionally on every state before executing user scripts.

Jaci is a standalone fork of Luau targeting general-purpose and systems programming outside Roblox Studio. This context demands full POSIX-class runtime APIs.

## Decision

Remove the unconditional `luaL_sandbox(L)` call from the standalone CLI (`setupState`). Host applications that require sandboxing must call `luaL_sandbox(L)` explicitly after `luaL_openlibs(L)`. This is the correct separation of concerns for an embeddable runtime.

Implement three new runtime library subsystems:

### `fs` (Filesystem)
- Backed by `std::filesystem` (C++17).
- Functions: `readfile`, `writefile`, `appendfile`, `removefile`, `removedir`, `mkdir`, `list`, `isfile`, `isdir`, `exists`, `stat`, `copy`, `move`, `cwd`.
- CamelCase aliases: `readFile`, `writeFile`, `appendFile`, `removeFile`, `removeDir`, `makeDir`, `readDir`, `isFile`, `isDir`.
- Accepts both `string` and `buffer` for write/append operations.
- `stat` returns `FileStat` table: `exists`, `isFile`, `isDirectory`, `size`, `modified`.
- Opened by default in `luaL_openlibs`.

### `io` (Standard IO Streams)
- Backed by POSIX `FILE*` wrapped in userdata with metatable `FILE*`.
- Functions: `open`, `popen`, `tmpfile`, `input`, `output`, `read`, `write`, `flush`, `close`, `lines`, `type`.
- Standard file handles `io.stdin`, `io.stdout`, `io.stderr` registered as non-closeable handles.
- File handle methods: `read`, `write`, `seek`, `flush`, `lines`, `close`.
- Format specifiers: `"*l"` / `"*line"` (line), `"*n"` / `"*n"` (number), `"*a"` (all), numeric (N bytes).
- Opened by default in `luaL_openlibs`.

### `os` extensions
- Added: `getenv`, `setenv`, `execute`, `remove`, `rename`, `exit`, `tmpname`.
- `tmpname` uses `mkstemp` on POSIX / `GetTempFileNameA` on Windows (replaces insecure `tmpnam`).
- `execute` returns `(exitcode, "exit"|"signal", code)` on POSIX; bare exit code on Windows.
- `setenv(key, nil)` calls `unsetenv` / `_putenv_s(key, "")` to unset.

### `loadfile` / `dofile` (base library extensions)
- `loadfile(filename[, chunkname])`: compiles file to a function, returns `(fn, errmsg?)`.
- `dofile([filename])`: compiles and executes file (or stdin if nil), returns all results.
- Both respect current `codegen` state and honor `safeenv` correctly.

## Consequences

- Every valid Luau program continues to run on Jaci (backward compatibility maintained).
- `luaL_sandbox(L)` still exists and is callable by host embedders who need it.
- `luaL_sandboxthread(L)` continues to be used for REPL thread isolation.
- Static type checker (`luau-analyze`) receives full type declarations for `fs`, `io`, extended `os`, `loadfile`, and `dofile` via `EmbeddedBuiltinDefinitions.cpp`.
- `io.stdin` / `io.stdout` / `io.stderr` values are runtime `FILE*` userdata; the type conformance test ignores them (they do not have static type counterparts by design).
- `loadstring` / `loadfile` / `dofile` are similarly ignored in `types.luau` conformance since they are CLI-injected and not part of the analysis type system definitions.
