# ADR 0013: Secure FFI and Memory Safety Policies

## Context

While direct foreign function interfaces provide near-zero-cost native interop, unrestricted FFI access can introduce security vulnerabilities and segfault crashes from unsafe pointer dereferences or arbitrary dynamic library loading.

## Decision

Implement capability-based security gating and memory safety policies in `lffilib.cpp`:
- **Security Modes (`ffi.mode`)**:
  - `permissive`: Default mode allowing standard FFI calls.
  - `strict`: Enforces pointer safety checks (\(\ge 0x10000\)) preventing null/guard page dereferences, and restricts library loading to explicit allowlists.
  - `disabled`: Completely forbids dynamic library loading and native function invocation.
- **Library Policies**:
  - `ffi.allowLibrary(path)`: Whitelists specific native shared libraries.
  - `ffi.denyLibrary(path)`: Explicitly denies loading specific libraries.
- **Safety Validators**:
  - `ffi.isSafe(ptr)`: Validates that pointers do not reside in protected or null guard pages.
  - `ffi.istype(typename, val)`: Type checker for FFI scalar and composite types.

## Consequences

- Sandboxed or multi-tenant runtimes can restrict FFI capabilities or disable arbitrary shared library loading.
- Crashes from null pointer dereferencing in FFI code are trapped cleanly as Luau runtime errors.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
