# ADR 0010: Universal Module System and Package Resolution

## Context

Vanilla Luau require-by-string was strictly constrained to explicit relative paths (`./foo`, `../bar`) and alias paths (`@pkg`) backed exclusively by `.luaurc` configurations. Standard programming patterns outside Roblox Studio require:
1. Universal relative module resolution with support for both `.luau` and `.lua` file extensions.
2. Directory entry point resolution (`init.luau`, `init.lua`, `index.luau`, `index.lua`).
3. Bare module resolution (`require("package-name")` and `require("@scope/package-name")`) looking up package directories (`luau_packages/`, `packages/`, `node_modules/`) up the ancestor directory hierarchy.
4. Native C ABI dynamic shared library module loading (`.so`, `.dylib`, `.dll`) calling `luaopen_<modulename>(lua_State* L)`.

## Decision

Extend the core require subsystem (`Luau.Require` and `Luau.CLI.lib`) with universal module resolution semantics:

- **Path Classification**: Updated `Luau::Require::getPathType` in `PathUtilities.cpp` to recognize `BarePackage` identifiers without special `./`, `../`, or `@` prefixes.
- **Directory Entry Points**: Updated `VfsNavigator.cpp` to resolve directory requires against `init.luau`, `init.lua`, `index.luau`, and `index.lua`.
- **Bare Package Search**: Implemented `VfsNavigator::toBarePackage` and `ReplRequirer::alias_fallback` to search `luau_packages/`, `packages/`, and `node_modules/` directories upwards from the requirer's context to root.
- **Native Shared Library Loading**: Implemented `ReplRequirer::loadNativeModule` using `dlopen`/`dlsym` (POSIX) and `LoadLibraryA`/`GetProcAddress` (Windows) to dynamically link C ABI Luau modules exporting `luaopen_<name>(lua_State* L)`.
- **Cyclic Require Protection**: Maintained full compatibility with `luarequire_createplaceholder` and Luau cycle short-circuiting.

## Consequences

- **Ergonomics**: Developers can use standard project layouts (`src/main.luau`, `packages/mylib/init.luau`) and require both Luau source modules and compiled C/C++ native modules seamlessly.
- **Backward Compatibility**: Fully backward compatible with vanilla Luau require rules; all vanilla Luau tests continue to pass without modification.
- **Zero Configuration**: Package directory resolution works automatically without requiring explicit `.luaurc` alias entries for standard packages.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
