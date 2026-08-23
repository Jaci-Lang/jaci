# ADR 0024: Single Binary as Engine + Embedded VFS, One Navigator

## Context

The single-binary bundler (`luau --build`, `klur build`) carried a second, parallel
require-navigation implementation: in-process `rt_*` navigation callbacks plus a
static BFS that re-implemented specifier resolution by hand (per-prefix string
branches for `@self/`, `./`, `../`, `@alias`), a five-strategy string-match module
lookup (`findModule` reconciling absolute/relative/`./`-prefixed name forms), and a
generated-C++ runner stub that re-implemented the same logic again.

Observed failures of that design:

- Package requires (`@scope/name`) failed in built binaries while working in the
  REPL, because the embedded fallback matched module names by string prefix against
  inconsistent stored forms.
- The same file could be embedded twice under two name forms (absolute via package
  directory seeding, relative via the BFS), because dedup used normalized strings of
  different kinds.
- Build-time `@alias` resolution was cwd-relative (`isFile("klur_modules/x")`), so
  building from the wrong directory silently produced a broken binary.
- The embedded runtime reported `CONFIG_ABSENT`, so `.luaurc` aliases that work in
  the REPL were dead in the binary.
- The generated-C++ path diverged from the in-process path (broken prefix checks,
  extra CodeGen pass) and was eventually dead code for all bundle modes.

Root cause: two sources of truth for module resolution. The engine's real navigator
(`RequireNavigator` + `VfsNavigator`, shared by the REPL, `luau-analyze`, and the
LSP) is the verified implementation; the bundler and the binary runtime each carried
their own approximations of it.

## Decision

1. **One navigator.** The single binary runs the entry through the same
   `ReplRequirer` + `VfsNavigator` + `RequireNavigator` stack the REPL uses. The
   `rt_*` navigation maze, `findModule` string matching, and the generated-C++ runner
   are deleted. There is no second navigation implementation.

2. **Embedded files are a VFS layer, not a module table.** The payload stores files
   by canonical absolute path. At startup the files are registered with a process
   file layer (`VfsLayer`) that the navigator's filesystem checks (`isFile`,
   `isDirectory`, `readFile`) consult: embedded files first, disk second. The
   navigator, alias/config lookup, package resolution, `@self`, `../`, and
   `klur_modules`/toolchain semantics all work unmodified, in the binary and on disk,
   because they are the same code.

3. **Discovery resolves with the real navigator.** The build's BFS keeps static
   AST require extraction (no program execution at build time), but each require
   string is resolved exactly like `luau-analyze` does: a fresh `FileNavigationContext`
   reset to the requirer, `Navigator::navigate(path)`, take the resolved identifier.
   Whatever the type checker resolves, the bundler embeds. Resolution is
   cwd-independent (absolute requirer paths) and honors `.luaurc`.

4. **Neutral engine, solver in klur.** The engine's `luau --build` embeds the static
   require graph plus explicit `--embed <file|dir>` entries. It does not know about
   package managers. `klur build` (the solver) decides what to embed: the project's
   `klur_modules/` and the toolchain package directories next to the engine, so that
   dynamically loaded modules (test files via `loadstring`) resolve in the binary.

5. **Payload container unchanged.** Base executable + serialized payload + trailer
   (magic `JACIPKG\0`), format version 2. Records carry canonical absolute path,
   bytecode, and source. Dedup is by canonical absolute path; a file appears once.

## Consequences

- A binary resolves modules exactly as the REPL and the LSP do, from the same code
  path; resolution bugs cannot diverge between the three.
- `.luaurc` aliases, scoped packages, `klur_modules`, and the toolchain package root
  work in built binaries without any special-casing.
- Building is cwd-independent; duplicate embedding is impossible by construction.
- The `fs.*` Lua hooks in the binary read through the same file layer.
- Modules only reachable through dynamic code must be passed via `--embed`
  (automated by `klur build`); the engine stays a neutral bundler.
- `generateRunnerCpp`, the `rt_*` callbacks, and `EmbeddedRuntimeContext` are removed
  (~1500 lines of parallel logic).

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT
License (see `LICENSE.txt`).
