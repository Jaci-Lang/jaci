# ADR 0023: Global Warning System and Enhanced Debug Subsystem

## Context

Developing, testing, and debugging complex Luau applications requires robust diagnostic tooling. Developers need:
1. A standard global `warn(...)` facility consistent with modern Luau / Lua runtimes.
2. Complete runtime reflection and introspection across active stack frames, local variables, upvalues, closure prototypes, constants, and registry tables without artificial sandbox roadblocks.
3. High-performance, deep value inspection with circular reference detection and configurable formatting.
4. Dynamic strict global detection and warning hooks to rapidly pinpoint typos and undeclared global accesses.

## Decision

Implement a unified warning and enhanced debugging subsystem in the VM runtime, standard library, and type solver:

1. **Global `warn(...)` and Warning Handler Hooks**:
   - Add global `warn(...)` built-in function to `VM/src/lbaselib.cpp`.
   - Direct output to `stderr` by default with tab delimiter formatting and immediate flush.
   - Expose `debug.setwarnhandler(fn)` and `debug.getwarnhandler()` allowing test frameworks and embeds to intercept warnings programmatically.
   - Register static type signatures `warn<T...>(...: T...)` in `BuiltinDefinitions.cpp`, `EmbeddedBuiltinDefinitions.cpp`, and `TypeFunctionRuntime.cpp`.

2. **Full-Spectrum Stack & Closure Introspection (`debug.*`)**:
   - `debug.setlocal([thread], level, index, value)`: Modify local register variables dynamically.
   - `debug.getupvalue(func, indexOrName)` & `debug.setupvalue(func, indexOrName, value)`: Inspect and modify closure upvalues by numeric index or string name.
   - `debug.getmetatable(obj)` & `debug.setmetatable(obj, mt)`: Directly access and modify raw metatables, bypassing `__metatable` protection.
   - `debug.getregistry()`: Expose the VM registry table (`LUA_REGISTRYINDEX`).
   - `debug.getconstants(func)` & `debug.getconstant(func, index)`: Inspect prototype constants tables.
   - `debug.getprotos(func)` & `debug.getproto(func, index)`: Inspect child function prototypes.
   - `debug.getstack([thread], level, [index])`: Inspect call frame registers and evaluation stack slots.
   - `debug.getfenv(target)` & `debug.setfenv(target, env)`: Inspect and update environments.

3. **High-Performance Deep Inspector (`debug.inspect` & `debug.dump`)**:
   - Fast, cycle-safe table and value inspection.
   - Support indentation, depth limiting, compact mode, metatable printing, and sorted key ordering.
   - Provide `debug.dump(val, [options])` for immediate formatted stdout printing.

4. **Runtime Undeclared Global Warnings (`debug.setglobalwarning` / `debug.strictglobals`)**:
   - Dynamically attach strict metatables to global environments to intercept and warn on undefined global variable reads and undeclared writes.

## Consequences

- **Superior Debuggability**: Developers can inspect, modify, and monitor runtime state with precision.
- **Fast Diagnostic Feedback**: Accidental typos in variable names and unexpected global mutations trigger immediate warnings.
- **Full Backward Compatibility**: All standard Luau and Lua 5.1/Roblox semantics are preserved.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
