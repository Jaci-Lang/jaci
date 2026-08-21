# Jaci Development Guidelines & Invariants

## Project Overview
Jaci is an independent Luau fork optimized for general-purpose programming, standalone applications, and native embedding outside Roblox Studio.

## Core Technical Focus
- **Blazing Fast Performance**: Aggressive native CodeGen (x64 / AArch64), low runtime overhead, and optimized VM bytecode loop.
- **Enhanced FFI & Native Interop**: Direct, near-zero-cost foreign function calls and seamless C/C++ integration.
- **Reduced Sandbox Limitations**: Relax Roblox-specific sandbox constraints to provide standalone filesystem, process, and system access.
- **Selective Upstream Sync**: Track upstream `luau-lang/luau` via weekly automated PRs for selective curation of language/compiler advances.

## Communication & Documentation Style
- **Imperative English**: Write documentation, commit messages, code comments, and design notes in direct, imperative English (e.g., "Add feature", "Fix buffer overflow", "Require argument").
- **Concise & Low-Ceremony**: Keep text brief, dense, and technically precise. Avoid fluff, boilerplate, and unnecessary ceremony.
- **No Emojis**: Do not use emojis in documentation, Markdown files, PR descriptions, or commit messages. Maintain a clean, professional, and technical tone.

## Architectural Decision Records (ADRs)
- **Document Architectural Decisions**: Record non-trivial architecture, runtime, or subsystem designs as lightweight ADRs (e.g., in `docs/adr/`).
- **Low Ceremony Structure**: Limit ADRs to context, decision, and consequences. Avoid bureaucratic process and bloated templates.
- **Precision Over Length**: State technical rationale, invariants, and trade-offs directly and precisely.

## Compatibility & Superset Invariant
- **Luau Backward Compatibility**: Maintain backward compatibility with vanilla Luau. Every valid Luau program must run on Jaci.
- **Asymmetric Superset**: Jaci is an expressive superset of Luau. Jaci features and APIs do not require backward compatibility with vanilla Luau (every Luau program runs on Jaci; not every Jaci program runs on Luau).

## Tooling & CLI Constraints
- Standard CLI tools and CMake/Make targets must remain named `luau` and `luau-analyze`. Do not rename CLI binaries to `jaci`.

## Testing & Verification Invariant
- **Test Before and After Changes**: Always execute and verify tests before and after implementing changes or introducing new features.
- **Comprehensive Coverage**: Ensure every new feature, runtime optimization, or bug fix is accompanied by dedicated unit tests and does not regress existing VM/CodeGen test suites.

## Licensing
- When referencing or updating license attribution, include `Copyright (c) 2026 Júlia Klee` alongside Roblox Corporation and Lua.org/PUC-Rio under the MIT License.
