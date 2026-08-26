# Jaci/Luau

[![CI](https://github.com/kleeedolinux/jaci/actions/workflows/build.yml/badge.svg)](https://github.com/kleeedolinux/jaci/actions/workflows/build.yml)
[![codecov](https://codecov.io/gh/kleeedolinux/jaci/branch/master/graph/badge.svg)](https://codecov.io/gh/kleeedolinux/jaci)

**Jaci/Luau** (the official name) is an independent, high-performance, gradually typed scripting language forked from Luau. It is engineered specifically for general-purpose programming, standalone applications, and systems development outside the constraints of Roblox Studio.

## Architecture & Philosophy

Jaci/Luau avoids the bloated, heavy standardization of runtimes like Node.js or Deno. Instead, it provides a blazingly fast language engine paired with a rich, highly capable native standard library. This design empowers you to write everything you need directly in Luau—from high-throughput backend services to native system integrations—without relying on external dependencies or a massive host runtime.

By aggressively optimizing execution paths, unchaining Luau from sandbox restrictions, and implementing a powerful native API surface, Jaci/Luau turns the Luau language into a formidable standalone tool.

## Core Features

### 1. Blazing Fast Execution
Execution paths are actively optimized across the stack:
- **Optimized VM**: Fast register-based interpreter loop tuned for modern CPU architectures.
- **Native CodeGen**: Direct x64 and AArch64 code generation that compiles hot bytecode blocks into highly optimized machine instructions.
- **Low-Overhead Runtime**: Minimal memory footprint, efficient garbage collection heuristics, and zero unnecessary abstraction layers.

### 2. Enhanced FFI
Direct, fast communication with host operating systems and native C/C++ libraries is a core priority. Jaci/Luau provides a built-in `ffi` module with dynamic library loading (`dlopen`/`LoadLibrary`) and typed call trampolines for near-zero-cost foreign function dispatch.

### 3. Rich Native Standard Library
Upstream Luau restricts access to system resources. Jaci/Luau removes these restrictions and provides an extensive built-in standard library to bootstrap standalone applications:
- **`ffi`**: Load `.so`/`.dylib`/`.dll` libraries and call C functions directly with typed arguments.
- **`fs`**: Comprehensive filesystem operations (`readfile`, `writefile`, `mkdir`, `list`, `stat`, `copy`, `move`, `cwd`) supporting buffers and strings.
- **`io`**: Stream IO with `io.open`, `io.popen`, `io.stdin`, `io.stdout`, `io.stderr`, buffered reads, and file iteration.
- **`os`**: Extended environment control (`getenv`, `setenv`) and process execution (`execute`).
- **`process`**: Subprocess spawning with stdout/stderr capture, stdin piping, and robust environment management.
- **`net`**: Synchronous TCP socket communication and HTTP/1.1 client capabilities (`net.request`, `net.connect`).
- **`json`**: Native JSON encode and decode with cycle detection and custom formatting options.
- **`hash`**: High-performance built-in CRC32, FNV-1a, MD5, SHA-1, and SHA-256 digests on strings and buffers.
- **`integer`**: 64-bit integer arithmetic and bitwise utilities.
- **Native bitwise syntax**: Use `&`, `|`, binary `~` (XOR), unary `~` (NOT), `<<`, and `>>` with exact 64-bit integer results.

### 4. Selective Upstream Synchronization
Jaci/Luau tracks upstream `luau-lang/luau` to adopt improvements in the compiler, type checker, and VM optimizations. A weekly automated CI pipeline imports upstream changes via Pull Requests. These are selectively curated to ensure they meet general-purpose criteria without reintroducing sandbox constraints.

## CLI Tools and Usage

Jaci/Luau maintains the standard Luau CLI toolchain:

- **`luau`**: Command-line REPL and script runner.
- **`luau-analyze`**: High-performance static type checker and linter.

```sh
# Run a script
luau script.luau

# Typecheck and lint your code
luau-analyze src/
```

## Building from Source

### Prerequisites
- C++17 compatible compiler (GCC 7+, Clang 7+, MSVC 2017+)
- CMake (3.10+) or `make`

### Build (CMake)
```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --target Luau.Repl.CLI Luau.Analyze.CLI --config RelWithDebInfo
```

### Build (Make - Linux/macOS)
```sh
make config=release luau luau-analyze
```

## Embedding in C++

Link against the `Luau.Compiler` and `Luau.VM` targets to embed Jaci/Luau in host C++ applications:

```cpp
#include "lua.h"
#include "lualib.h"
#include "luacode.h"

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    const char* source = "print('Hello from Jaci/Luau!')";
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source, strlen(source), NULL, &bytecodeSize);

    if (luau_load(L, "=main", bytecode, bytecodeSize, 0) == 0) {
        lua_pcall(L, 0, 0, 0);
    }

    free(bytecode);
    lua_close(L);
    return 0;
}
```

## Testing

Execute unit tests (bytecode compiler, AST, type checker) and conformance tests (VM execution) via `make`:

```sh
make test
```

## License & Attribution

Jaci/Luau is distributed under the terms of the MIT License.

- Copyright (c) 2026 Júlia Klee
- Copyright (c) 2019-2025 Roblox Corporation (Luau)
- Copyright (c) 1994–2019 Lua.org, PUC-Rio (Lua 5.x)
