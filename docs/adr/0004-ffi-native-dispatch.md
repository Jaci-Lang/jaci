# ADR 0004: FFI Native Dispatch & Memory Model

## Context

Jaci targets general-purpose standalone programming outside Roblox Studio. Real-world applications need direct, zero-friction interoperation with existing C libraries, operating system APIs, and raw memory structures without requiring custom C++ bindings for each dependency.

## Decision

Implement a full Foreign Function Interface (`ffi`) directly inside the Jaci VM with the following capabilities:

### 1. Dynamic Library Management & Symbol Lookup
- `ffi.open(path?)`: Dynamically load shared libraries (`.so`/`.dylib`/`.dll`) or access the default process image.
- `ffi.C`: Standard C / process global namespace enabling direct access to standard library and exported functions.
- `ffi.sym(lib, name, rettype, ...argtypes)`: Explicit symbol resolution with runtime type signatures.

### 2. C Declaration Parser (`ffi.cdef`)
- Parses standard C function declarations, typedefs, and struct prototypes directly from string literals.
- Automatically maps C types (`int`, `size_t`, `const char*`, `double`, `void*`, `int64_t`, etc.) and binds symbols to `ffi.C` or library instances for natural invocation:
  ```lua
  ffi.cdef[[
      double cos(double x);
      size_t strlen(const char* s);
  ]]
  print(ffi.C.cos(0.0))
  print(ffi.C.strlen("hello"))
  ```

### 3. Raw Memory & Struct Operations
- `ffi.new(type, count?)`: Allocates a formatted Luau `buffer` sized to the type or array.
- `ffi.ptr(buffer | string)`: Obtains a raw memory lightuserdata pointer.
- `ffi.string(ptr, len?)`: Reads a C string or raw byte slice into a Luau string.
- `ffi.copy(dst, src, len)` / `ffi.fill(dst, len, val)`: Direct memory transfer and setting.
- `ffi.read(ptr, offset, type)` / `ffi.write(ptr, offset, type, val)`: Typed memory reads and writes.
- `ffi.struct({ {name, type}, ... })`: Struct layout engine calculating natural field offsets and struct alignment.
- `ffi.sizeof(type)` / `ffi.alignof(type)`: Type introspection.
- `ffi.errno(val?)`: Access and set platform error status.

### 4. Calling Convention Dispatch
- On x86-64 System V AMD64: Direct register-mapped trampoline utilizing GPRs (`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`) for integer/pointers and SSE registers (`xmm0`..`xmm7`) for floating-point values.
- Portable typed fallback for other architectures.

## Consequences

- Full, idiomatic C interop is available in pure Luau.
- Eliminates the need for manual C wrapper boilerplate.
- Memory access is direct and unsafe, matching Jaci's systems programming focus.
