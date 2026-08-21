# ADR 0004: FFI Native Dispatch

## Context

Jaci targets general-purpose standalone programming outside Roblox Studio. Real-world applications need to call into existing C libraries (e.g., SQLite, OpenSSL, system APIs) without writing custom C++ bindings for each one. Luau provides no FFI mechanism; the only interop path is writing C closures manually and rebuilding the host.

## Decision

Add an `ffi` library to the VM that exposes `dlopen`/`dlsym` (POSIX) and `LoadLibrary`/`GetProcAddress` (Win32) to Luau scripts. The API provides:

- `ffi.open(path)` -> Library userdata (wraps a shared library handle).
- `ffi.sym(lib, name, rettype, argtypes...)` -> Symbol userdata (wraps a resolved function pointer with a type signature).
- Calling a Symbol marshals Luau values to C types, dispatches the call, and pushes the return value.
- Supported types: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64`, `ptr`, `str`, `buf`, `void`.
- Helper functions: `ffi.cast`, `ffi.ptr`, `ffi.sizeof`, `ffi.nullptr`.

### Trampoline Strategy

Instead of depending on libffi, use typed function pointer casts:
- For all-integer/pointer arguments: cast to `intptr_t (*)(intptr_t, ...)` variants.
- For double-returning math-style functions: cast to `double (*)(double, ...)` variants.
- Switch on argument count (max 16) to select the correct typedef.
- This covers x86-64 System V and Win64 ABIs for the common case.

**Known limitation:** Mixed integer+float positional arguments may not be dispatched correctly on all ABIs because variadic calls place floats differently than positional calls. This affects only exotic C signatures; common library APIs (libc, libm, SQLite, etc.) work correctly.

AArch64 float argument dispatch is deferred to a follow-up using platform-specific inline assembly or a future libffi integration.

### Security Posture

The `ffi` library is inherently unsafe. It calls arbitrary native code with no bounds checking at the C boundary. Host embedders that need isolation must selectively open libraries instead of calling `luaL_openlibs()`. The library is registered by default to match Jaci's "reduced sandbox" philosophy for standalone use.

## Consequences

- Luau scripts can call into any C shared library without rebuilding the host binary.
- The VM gains a link dependency on `libdl` (Linux/macOS). Windows uses kernel32 APIs already available.
- Misuse of `ffi` can crash the process or corrupt memory. This is acceptable for a standalone runtime targeting system programmers.
- Struct layout support is deferred to a follow-up ADR; structs are currently passed as raw `buffer` slices.
