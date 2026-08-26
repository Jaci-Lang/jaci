# Native 64-bit bitwise operators

## Context

`bit32` truncates values to 32 bits. General-purpose programs need exact permission masks, identifiers, and protocol fields above bit 31.

## Decision

Parse `&`, `|`, `~`, `<<`, and `>>` as expression operators. Use unary `~` for NOT and binary `~` for XOR. Convert integral `number` operands to signed 64-bit integers, preserve `integer` operands, and return `integer`. Define right shift as logical, reverse the shift direction for negative counts, and return zero when the absolute shift count reaches 64.

Compile the syntax through the existing integer fastcalls so interpreter and native CodeGen share one implementation. Preserve Luau generic instantiation syntax such as `f<<T>>()` by recognizing adjacent shift tokens only after primary-expression parsing.

## Consequences

Use `1 << 40` directly without `bit32` truncation or exponentiation. Reject fractional, non-finite, and out-of-range double operands. Keep vanilla Luau programs valid while making the operators a Jaci-only extension.
