# Jaci Development Guidelines & Invariants

## Project Overview
Jaci is an independent Luau fork optimized for general-purpose programming, standalone applications, and native embedding outside Roblox Studio.

## Core Technical Focus
- **Blazing Fast Performance**: Aggressive native CodeGen (x64 / AArch64), low runtime overhead, and optimized VM bytecode loop.
- **Enhanced FFI & Native Interop**: Direct, near-zero-cost foreign function calls and seamless C/C++ integration.
- **Reduced Sandbox Limitations**: Relaxing Roblox-specific sandbox constraints to provide standalone filesystem, process, and system access.
- **Selective Upstream Sync**: Tracking upstream `luau-lang/luau` via weekly automated PRs for selective curation of language/compiler advances.

## Tooling & CLI Constraints
- Standard CLI tools and CMake/Make targets must remain named `luau` and `luau-analyze`. Do not rename CLI binaries to `jaci`.

## Documentation Style
- Do not use emojis in documentation, Markdown files, or commit messages. Maintain a clean, professional, and technical tone.

## Licensing
- When referencing or updating license attribution, include `Copyright (c) 2026 Júlia Klee` alongside Roblox Corporation and Lua.org/PUC-Rio under the MIT License.
