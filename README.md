# Jaci ![CI](https://github.com/kleeedolinux/jaci/actions/workflows/build.yml/badge.svg) [![codecov](https://codecov.io/gh/kleeedolinux/jaci/branch/master/graph/badge.svg)](https://codecov.io/gh/kleeedolinux/jaci)

**Jaci** is a fast, lightweight, gradually typed embeddable scripting language and runtime. Forked from [Luau](https://github.com/luau-lang/luau), Jaci is engineered and optimized for **general-purpose programming, standalone scripting, and embedding outside of the Roblox Studio ecosystem**.

---

## 🌟 Why Jaci?

Upstream Luau is primarily tailored to the constraints, security models, and workflows of Roblox Studio. While this produces a highly optimized and sandboxed engine, standalone and general-purpose systems development often require greater flexibility, modern standard tooling, and standalone execution capabilities.

**Jaci bridges this gap:**
- **General-Purpose & Standalone**: Tailored for building command-line utilities, native applications, backend services, and game engine integrations outside Roblox Studio.
- **Selective Upstream Sync**: Jaci actively tracks upstream Luau innovations—such as state-of-the-art constraint-based type solving, JIT/AOT code generation, and VM performance improvements—through an automated weekly CI pipeline. Each upstream change is manually reviewed so that Roblox-specific limitations or breaking changes do not compromise general-purpose usability.
- **Gradual Type System**: Advanced type inference, linting, and gradual type checking built on Luau's type engine.
- **High Performance**: Features a rewritten register-based interpreter, Native CodeGen (AOT/JIT for x64 and AArch64), and fast bytecode execution.
- **Embeddable C/C++ API**: Clean, minimal C and C++ APIs designed for seamless embedding into host applications.

---

## 🚀 CLI Tools & Usage

Jaci provides command-line tools for running code and analyzing projects:

- **`luau` / `jaci`**: Standalone command-line REPL and script runner.
- **`luau-analyze`**: High-performance static type checker and linter. It inspects your codebase and produces diagnostics based on type annotations, `--!` mode comments, or [`.luaurc`](https://rfcs.luau.org/config-luaurc) configuration files.

```sh
# Run a script
luau script.luau

# Typecheck and lint your project
luau-analyze src/
```

---

## 🛠️ Building from Source

### Prerequisites
- A C++17 compatible compiler (GCC 7+, Clang 7+, MSVC 2017+)
- [CMake](https://cmake.org/) (3.10+) or `make`

### Building with CMake (Recommended)

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

## 📦 Embedding Jaci in C++

To embed Jaci in your C++ applications, link against the `Luau.Compiler` and `Luau.VM` targets:

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

Jaci includes a comprehensive test suite covering compiler units, type inference, and VM conformance:

- **Unit Tests**: `Luau.UnitTest` (bytecode compiler, AST, and type checker/linter tests)
- **Conformance Tests**: `Luau.Conformance` (VM tests)

Run all tests via `make`:
```sh
make test
```

---

## 🔄 Upstream Synchronization

Jaci uses an automated GitHub Actions workflow that runs weekly to inspect upstream [`luau-lang/luau`](https://github.com/luau-lang/luau) for new commits and opens a Pull Request.

Because Jaci is customized for general-purpose usage outside Roblox Studio, upstream changes are curated, tested, and selectively merged by maintainers to maintain stability and general-purpose optimizations.

---

## 📄 License & Attribution

Jaci is distributed under the terms of the **[MIT License](LICENSE.txt)**.

- Copyright (c) 2026 Júlia Klee
- Copyright (c) 2019-2025 Roblox Corporation (Luau)
- Copyright (c) 1994–2019 Lua.org, PUC-Rio (Lua 5.x)
