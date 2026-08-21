# Jaci ![CI](https://github.com/kleeedolinux/jaci/actions/workflows/build.yml/badge.svg) [![codecov](https://codecov.io/gh/kleeedolinux/jaci/branch/master/graph/badge.svg)](https://codecov.io/gh/kleeedolinux/jaci)

**Jaci** is a fork of [Luau](https://github.com/luau-lang/luau) optimized for **general-purpose programming outside Roblox Studio**.

---

## 🎯 Current Focus

While upstream Luau is primarily constrained by the requirements and sandbox of Roblox Studio, Jaci focuses on adapting Luau for general-purpose use:

- **Reducing Sandbox Limitations**: Relaxing and removing sandbox restrictions designed for Roblox Studio so Luau can be used effectively for standalone scripting and systems tasks.
- **Enhanced FFI & CodeGen**: Advancing native Foreign Function Interface (FFI) capabilities and optimizing native CodeGen for better native interop and performance.
- **Selective Upstream Sync**: Tracking upstream Luau innovations (compiler, constraint solver, VM performance) via automated weekly Pull Requests, allowing maintainers to curate and integrate features while rejecting Roblox-specific constraints.

---

## 🚀 CLI Tools & Usage

Jaci maintains the standard Luau CLI toolchain:

- **`luau`**: Command-line REPL and script runner.
- **`luau-analyze`**: Static type checker and linter.

```sh
# Run a script
luau script.luau

# Typecheck and lint your code
luau-analyze src/
```

---

## 🛠️ Building from Source

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

## 📦 Embedding in C++

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

## 🧪 Testing

Jaci uses Luau's test suites for compiler units and VM conformance:

- **Unit Tests**: `Luau.UnitTest`
- **Conformance Tests**: `Luau.Conformance`

Run all tests via `make`:
```sh
make test
```

---

## 🔄 Upstream Synchronization

A GitHub Actions workflow runs every week to fetch new changes from upstream [`luau-lang/luau`](https://github.com/luau-lang/luau) and opens a Pull Request.

Because not all upstream changes are suitable for general-purpose environments, incoming updates are reviewed and selectively merged.

---

## 📄 License & Attribution

Jaci is distributed under the terms of the **[MIT License](LICENSE.txt)**.

- Copyright (c) 2026 Júlia Klee
- Copyright (c) 2019-2025 Roblox Corporation (Luau)
- Copyright (c) 1994–2019 Lua.org, PUC-Rio (Lua 5.x)
