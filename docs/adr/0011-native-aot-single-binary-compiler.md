# ADR 0011: Native AOT Single Binary Compiler

## Context

Deploying standalone CLI applications, services, and native utilities written in Luau previously required shipping separate Luau runtime binaries, shared libraries, and filesystem source trees. Users requested a single-command build workflow to compile Luau projects and all transitive module dependencies into a standalone, portable single executable binary embedding the Luau VM, native runtime, standard libraries, and precompiled bytecode chunks.

## Decision

Implement the Single Binary Compiler subsystem (`SingleBinaryCompiler.h`, `SingleBinaryCompiler.cpp`), integrated into both `luau --build` and `luau-compile --native-binary`:

- **Dependency Traversal & AST Bundling**:
  - Traverses the AST of the entry point script using `AstVisitor` to identify all static `require(...)` invocations.
  - Recursively resolves and compiles all discovered module dependencies into optimized Luau bytecode chunks (`Luau::compile`).
- **Embedded In-Memory Virtual Runtime**:
  - Generates a standalone C++ compilation unit embedding all bytecode chunks in static memory tables.
  - Configures an embedded in-memory `luarequire_Configuration` providing virtual module navigation and fallback without physical filesystem dependencies.
  - Initializes the Luau VM (`luaL_newstate`), JIT CodeGen (`Luau::CodeGen::create`), and the complete Jaci standard library (`fs`, `io`, `ffi`, `net`, `process`, `json`, `hash`, `task`, `class`).
  - Sets up command line arguments (`arg` table) and executes the entry module with varargs.
- **Linker Pipeline**:
  - Links the generated runner against the static Luau runtime archives (`libLuau.VM.a`, `libLuau.Require.a`, `libLuau.CodeGen.a`, `libLuau.Compiler.a`, `libLuau.Bytecode.a`, `libLuau.Common.a`, `libLuau.Config.a`, `libLuau.Analysis.a`, `libLuau.Ast.a`, `libLuau.Inliner.a`, `libLuau.CLI.lib.a`, `libisocline.a`) and system libraries (`dl`, `pthread`, `m`).
  - Emits a standalone, portable executable binary directly to the specified `-o <output>` path.

## Consequences

- **Portability**: Compiled binaries are self-contained single executables that run anywhere on the target system without external Luau dependencies or source files.
- **CLI Workflow**:
  - `luau --build -o myapp src/main.luau`
  - `luau-compile --native-binary -o myapp src/main.luau`
- **Native Performance**: Leverages native CodeGen AOT compilation and precompiled bytecode for instant startup and execution.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
