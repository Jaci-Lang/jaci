# Jaci ![CI](https://github.com/kleeedolinux/jaci/actions/workflows/build.yml/badge.svg) [![codecov](https://codecov.io/gh/kleeedolinux/jaci/branch/master/graph/badge.svg)](https://codecov.io/gh/kleeedolinux/jaci)

**Jaci** is a high-performance, gradually typed scripting language and runtime forked from [Luau](https://github.com/luau-lang/luau), engineered specifically for **general-purpose programming, standalone applications, and systems development outside of Roblox Studio**.

---

## About Jaci

Luau provides an exceptionally engineered foundation: a completely rewritten interpreter runtime, a sophisticated constraint-based type inference system, and native code generation (AOT/JIT). However, upstream Luau is fundamentally designed around the constraints, safety models, and proprietary APIs of the Roblox platform.

**Jaci** was created to unlock the full potential of this technology as an independent, general-purpose programming language. By removing Roblox-specific sandbox constraints and prioritizing native interoperability, Jaci turns Luau into a standalone language suitable for command-line utilities, high-throughput backend services, standalone game engines, and native system integrations.

---

## Core Focus & Philosophy

### 1. Blazingly Fast Execution
Performance is at the heart of Jaci. We actively optimize execution paths across the entire stack:
- **Optimized VM & Bytecode Execution**: Fast register-based interpreter loop tuned for modern CPU architectures.
- **Aggressive Native CodeGen**: Direct x64 and AArch64 code generation that compiles hot bytecode blocks into optimized machine instructions.
- **Low-Overhead Runtime**: Minimal memory footprint, efficient garbage collection heuristics, and zero unnecessary runtime layers.

### 2. Enhanced FFI and Native Interoperability
General-purpose programming requires direct and fast communication with host operating systems and native C/C++ libraries. Jaci provides a built-in `ffi` module with dynamic library loading (`dlopen`/`LoadLibrary`) and typed call trampolines for near-zero-cost foreign function dispatch.

### 3. Reduced Sandbox Limitations & Rich Native Libraries
Upstream Luau restricts access to system resources, raw memory, and underlying platform facilities. Jaci removes these restrictions and provides standard libraries to bootstrap standalone applications:
- **`ffi`**: Load `.so`/`.dylib`/`.dll` libraries and call C functions directly with typed arguments.
- **`fs`**: Filesystem operations (`readfile`, `writefile`, `mkdir`, `list`, `stat`, `copy`, `move`, `cwd`) with `buffer` and `string` support.
- **`io`**: Stream IO with `io.open`, `io.popen`, `io.stdin`, `io.stdout`, `io.stderr`, buffered reads, and file iteration.
- **`os`**: Extended environment control (`getenv`, `setenv`), command execution (`execute`), and process helpers.
- **`process`**: Subprocess spawning with stdout/stderr capture, stdin piping, and environment management.
- **`net`**: Synchronous TCP socket communication and HTTP/1.1 client (`net.request`, `net.connect`).
- **`json`**: Native JSON encode and decode with cycle detection and formatting options.
- **`hash`**: Built-in CRC32, FNV-1a, MD5, SHA-1, and SHA-256 digests on strings and buffers.
- **`integer`**: 64-bit integer arithmetic and bitwise utilities.

### 4. Selective Upstream Synchronization
Jaci tracks upstream Luau to continuously adopt core improvements in the compiler, type checker, and VM optimizations. Through an automated weekly CI pipeline, upstream changes are imported via Pull Requests and reviewed to ensure they meet Jaci's general-purpose criteria and do not reintroduce Roblox-specific constraints.

---

## CLI Tools and Usage

Jaci maintains the standard Luau CLI toolchain:

- **`luau`**: Command-line REPL and script runner.
- **`luau-analyze`**: High-performance static type checker and linter.

```sh
# Run a script
luau script.luau

# Typecheck and lint your code
luau-analyze src/
```

---

## Building from Source

### Prerequisites
- A C++17 compatible compiler (GCC 7+, Clang 7+, MSVC 2017+)
- [CMake](https://cmake.org/) (3.10+) or `make`

### Building with CMake

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --target Luau.Repl.CLI Luau.Analyze.CLI --config RelWithDebInfo
```

### Building with Make (Linux/macOS)

```sh
make config=release luau luau-analyze
```

---

## Embedding in C++

To embed Jaci in host C++ applications, link against the `Luau.Compiler` and `Luau.VM` targets:

```cpp
#include "lua.h"
#include "lualib.h"
#include "luacode.h"

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    const char* source = "print('Hello from Jaci!')";
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

---

## Testing

Jaci uses Luau's unit and conformance test suites:

- **Unit Tests**: `Luau.UnitTest` (bytecode compiler, AST, and type checker/linter tests)
- **Conformance Tests**: `Luau.Conformance` (VM execution and conformance tests)

Run all tests via `make`:
```sh
make test
```

---

## Upstream Synchronization

A GitHub Actions workflow runs every week to inspect upstream [`luau-lang/luau`](https://github.com/luau-lang/luau) for new commits and opens a Pull Request.

Because not all upstream changes align with general-purpose runtime requirements, incoming updates are reviewed and selectively merged by maintainers.

---

## License & Attribution

Jaci is distributed under the terms of the **[MIT License](LICENSE.txt)**.

- Copyright (c) 2026 Júlia Klee
- Copyright (c) 2019-2025 Roblox Corporation (Luau)
- Copyright (c) 1994–2019 Lua.org, PUC-Rio (Lua 5.x)
