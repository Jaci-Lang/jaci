# ADR 0017: Extended Primitive and Sized Numeric Types

## Context

Luau traditionally defines a restricted set of primitive types (`nil`, `boolean`, `number`, `string`, `thread`, `buffer`, `table`, `function`, `any`, `unknown`, `never`, `integer`). General-purpose and systems programming in Jaci (such as FFI bindings, low-level data structures, binary buffer manipulation, and interoperability with native code) requires explicit, descriptive primitive type annotations including `unit`, `void`, sized signed integers (`int8`, `int16`, `int32`, `int64`, `i8`, `i16`, `i32`, `i64`, `int`), sized unsigned integers (`uint8`, `uint16`, `uint32`, `uint64`, `u8`, `u16`, `u32`, `u64`, `uint`, `byte`), and floating-point primitives (`float32`, `float64`, `f32`, `f64`, `float`, `double`).

## Decision

Register extended primitive type bindings in the global type environment and compiler bytecode type resolution:

### 1. Unit & Void Types
- `unit` and `void` are bound globally to `nilType`.
- Functions returning no meaningful value or empty payloads can be annotated as `() -> unit` or `() -> void`.

### 2. Sized Signed Integers
- `int`, `int8`, `int16`, `int32`, `int64`, `i8`, `i16`, `i32`, `i64` are bound to `integerType` (or `numberType` when `LuauIntegerType2` is disabled).
- Compatible with bytecode type generation and type checking constraints.

### 3. Sized Unsigned Integers
- `uint`, `uint8`, `uint16`, `uint32`, `uint64`, `u8`, `u16`, `u32`, `u64`, `byte` are bound to `integerType` (or `numberType` when `LuauIntegerType2` is disabled).

### 4. Floating-Point Numbers
- `float`, `double`, `float32`, `float64`, `f32`, `f64` are bound to `numberType`.

### 5. Bytecode Compiler Mapping
- The bytecode compiler maps extended integer annotations to `LBC_TYPE_INTEGER`, float annotations to `LBC_TYPE_NUMBER`, and unit/void to `LBC_TYPE_NIL`.

## Consequences

- Full backward compatibility is preserved: all valid vanilla Luau programs continue to run unchanged.
- Jaci source code can use explicit primitive type signatures for systems code, binary protocols, and native FFI declarations.
- Autocomplete and type analyzer automatically discover and validate the new primitive types across scopes.
