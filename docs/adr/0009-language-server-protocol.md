# ADR 0009: Language Server Protocol Mode and Incremental Analysis

## Context

Luau and Jaci developer tooling previously lacked a unified, embedded Language Server Protocol (LSP) implementation within the core CLI binaries (`luau` and `luau-analyze`). Editor integrations and IDE extensions previously had to rely on separate third-party language server forks or external inference wrappers, which suffered from out-of-sync typechecker semantics, redundant parsing and disk reads, and high process overhead.

A state-of-the-art LSP mode was required directly in the CLI toolchain to provide:
1. Standard JSON-RPC 2.0 transport over stdio.
2. In-memory virtual filesystem (VFS) document synchronization with incremental edits.
3. Fast incremental typechecking and immediate diagnostic publication via `Luau::Frontend`.
4. High-fidelity editor services: hover, autocomplete, definition, type definition, document symbols, references, highlight, rename, semantic tokens, signature help, and inlay hints.
5. Direct AST, bytecode, and partial execution introspection endpoints (`luau/ast`, `luau/bytecode`, `luau/eval`).

## Decision

Implement an integrated LSP subsystem within `Luau.CLI.lib`, exposed via the `--lsp` flag in both `luau` and `luau-analyze`:

- **Zero-Dependency JSON-RPC 2.0 & Binary JSON Engine**: Built lightweight, high-performance JSON tokenizer/parser, MessagePack binary JSON encoder/decoder, and response serializer (`JsonRpc.h`, `JsonRpc.cpp`) supporting standard header framing (`Content-Length: <n>\r\nContent-Type: application/msgpack\r\n\r\n`), text and binary requests, responses, notifications, and error codes.
- **In-Memory VFS Synchronization**: `DocumentState` tracks open document buffers and line offsets, supporting both full text replacement and incremental range edits (`textDocument/didOpen`, `textDocument/didChange`, `textDocument/didClose`, `textDocument/didSave`).
- **LSP File and Config Resolvers**: `LspFileResolver` prioritizes in-memory buffers while falling back to disk and integrating `RequireNavigator` for module imports. `LspConfigResolver` cascades `.luaurc` and `.luau.toml` configurations.
- **Analysis Integration**:
  - `publishDiagnostics`: Translates `TypeError`, `SyntaxError`, and `LintWarning` into LSP `Diagnostic` ranges and severities.
  - `textDocument/hover`: Resolves AST ancestry, type signatures, and required module exports with markdown formatting.
  - `textDocument/completion` & `completionItem/resolve`: Integrates `Luau::autocomplete` to suggest properties, bindings, keywords, modules, functions with full parameter signatures, and snippets with lazy documentation resolution.
  - `textDocument/definition` and `textDocument/typeDefinition`: Resolves symbol binding locations and module require targets.
  - `textDocument/documentSymbol`: Traverses the AST to produce structured symbol outlines.
  - `textDocument/documentHighlight` and `textDocument/references`: Finds local variable definitions and all references.
  - `textDocument/rename` and `textDocument/prepareRename`: Emits atomic workspace edits across symbol occurrences.
  - `textDocument/semanticTokens/full`: Emits delta-encoded semantic tokens covering types, functions, properties, variables, strings, numbers, and operators.
  - `textDocument/signatureHelp`: Identifies enclosing call expressions, active argument index, parameter labels, and parameter documentation.
  - `textDocument/inlayHint`: Provides inferred parameter names and variable type annotations.
  - `luau/*` Custom Endpoints: AST dumping (`luau/ast`), all inferred types (`luau/types`), require graph (`luau/requireGraph`), bytecode inspection (`luau/bytecode`), and sandboxed execution (`luau/eval`).

## Consequences

- **Performance**: Document updates are processed incrementally in memory without spawning subprocesses or performing redundant disk I/O.
- **Tooling Compatibility**: Both `luau --lsp` and `luau-analyze --lsp` serve standard LSP 3.17 requests directly from VS Code, Neovim, Helix, Emacs, and other editors.
- **Superset & Backward Compatibility**: Vanilla Luau scripts and Jaci runtime extensions are fully supported. No breaking changes to existing CLI flags or targets.
- **Maintainability**: The LSP engine is self-contained in `Luau.CLI.lib`, covered by dedicated unit and integration tests in `Luau.UnitTest`.

## Copyright

Copyright (c) 2026 Julia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
