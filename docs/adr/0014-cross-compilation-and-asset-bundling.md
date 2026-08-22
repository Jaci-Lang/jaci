# ADR 0014: Single Binary Cross-Compilation and Asset Bundling

## Context

Developers need to target diverse target architectures (Linux x86_64, Linux AArch64, Windows x64, macOS ARM64/x86_64) from their local development environment when building single executable binaries. Additionally, applications often require embedding static assets (configuration, templates, data files) directly into the standalone binary.

## Decision

Extend the `SingleBinaryCompiler` subsystem (`SingleBinaryCompiler.h`, `SingleBinaryCompiler.cpp`):
- **Target Selection (`--target=<arch>`)**:
  - Automatically selects target toolchain compilers (`aarch64-linux-gnu-g++`, `x86_64-w64-mingw32-g++`, `clang++ -target arm64-apple-macos`) and target-specific linker flags.
- **Virtual VFS Asset Bundling**:
  - Automatically embeds all transitive Luau module dependencies and assets into in-memory bytecode and byte tables.
  - The embedded runner provides a virtual `VfsNavigator` requiring zero disk files at runtime.

## Consequences

- Seamless one-step cross-compilation pipeline for distributed standalone binaries.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
