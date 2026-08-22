# ADR 0022: High-Performance LSP Autocomplete and Extension Integration

## Context

Editor autocompletion in large codebases requires sub-millisecond response latencies, intelligent candidate ranking, context-sensitive require path suggestions, function call parameter snippet expansion, and lightweight payload serialization. Full typecheck passes over entire dependency graphs on every keystroke introduce unacceptable latency during interactive coding.

## Decision

Implement an optimized autocomplete pipeline across the embedded LSP server and editor extensions (`@extensions/`):

1. **Fragment Autocomplete Fast-Path**:
   - Utilize `Luau::tryFragmentAutocomplete` on dirty documents to perform localized incremental parsing and typechecking around the cursor position without rechecking the entire module graph.
   - Fall back to full `frontend.check` + `Luau::autocomplete` when fragment typechecking fails or when fresh global context is required.

2. **String & Require-by-String Path Resolution**:
   - Provide active `StringCompletionCallback` handling `require("...")` and `require("@...")`.
   - Complete `.luaurc` / `luau.config` aliases (`@alias`), open memory document buffers, and workspace file paths.

3. **Intelligent Sort Ranking (`sortText`)**:
   - Classify suggestions with deterministic sort prefixes:
     - `0000_`: Exact expected type matches (`TypeCorrectKind::Correct`).
     - `0001_`: Return type matches (`TypeCorrectKind::CorrectFunctionResult`).
     - `0002_`: Local bindings.
     - `0003_`: Table and object properties.
     - `0004_`: Modules and require paths.
     - `0005_`: Keywords and statements.
     - `0006_`: Type identifiers.
     - `9999_`: Deprecated symbols.

4. **Parameter Snippets & Smart Parentheses**:
   - Emit LSP snippet expansions (`InsertTextFormat::Snippet`) with named tab stops (`${1:param1}, ${2:param2}`) for function calls when recommended (`ParenthesesRecommendation::CursorInside`).
   - Automatically append `()$0` when `ParenthesesRecommendation::CursorAfter` is signaled.

5. **Lazy Documentation Resolution (`completionItem/resolve`)**:
   - Enable `resolveProvider: true` in LSP server capabilities.
   - Defer full markdown formatting, type signatures, and documentation symbols until completion item selection.

6. **Editor Extension Ecosystem Integration**:
   - **VS Code Extension (`jaci-vscode`)**: Add user configuration controls (`jaci.autocomplete.suggestParens`, `jaci.autocomplete.fillArguments`, `jaci.autocomplete.imports`, `jaci.autocomplete.typeCorrectRanking`), register rich Luau snippets (`snippets/luau.json`), and forward client capabilities.
   - **Core Packages & Crates (`@jaci/core`, `jaci-core`)**: Expose completion protocol data structures, method constants, and serde numeric representations.
   - **Zed Extension**: Configure trigger characters (`.`, `:`, `"`, `'`, `/`, `@`, `<`, `,`).

## Consequences

- **Sub-millisecond Keystroke Latency**: Typing within expressions and function bodies completes near-instantaneously via fragment incremental parsing.
- **Enhanced DX**: Automatic parameter insertion and require-path resolution eliminate manual boilerplate.
- **Preserved Compatibility**: Vanilla Luau behavior and strict typing semantics remain fully intact.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
