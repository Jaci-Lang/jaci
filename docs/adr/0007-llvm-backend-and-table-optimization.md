# ADR 0007: LLVM Backend Architecture and Table Specialization

## Context

Jaci requires extreme execution performance for general-purpose computing, dense numeric loops, native FFI dispatch, and standalone applications. Manual per-architecture assembly builders (`AssemblyBuilderX64` and `AssemblyBuilderA64`) incur high maintenance overhead, lack whole-function auto-vectorization (SIMD), and cannot perform global instruction scheduling or advanced register allocation across complex control flow.

To transition smoothly and verify measurable performance gains before removing the manual assembly builders, Jaci requires an optimizing LLVM Backend that integrates directly into the `IR → HIR → MIR → LLVM IR` pipeline alongside comparative benchmarking infrastructure.

## Decision

Implement an optimizing LLVM Backend for Jaci:

`existing IR → HIR → MIR → LLVM IR → LLVM Optimization Pipeline → Native Execution`

1. **High-Level Table & Array Operations**:
   - Table operations remain in semantic IR before lowering (`TableGetConstStr`, `TableGetInt`, `TableSet`, `NewTable`, `GuardShape`).
   - Multiple internal representations: dense array storage, shape-based property storage, and generic hash fallback.
   - Constant string accesses (`obj.x`) specialize to shape-guarded fixed slot accesses (`check shape -> load fixed slot`).
   - Polymorphic Inline Caches (PIC): Multi-shape caching at call/access sites with fallback to generic lookup.
   - Specialized Array Storage: `PACKED_I64`, `PACKED_F64`, `PACKED_REF`, and generic boxed values.
   - Vectorization: Typed contiguous numeric arrays emit LLVM loop vectorization hints (`llvm.loop.vectorize.enable = true`).
   - Metatable fast paths: Proven existing properties bypass metamethod dispatch.

2. **SSA Representation & Memory Alias Modeling**:
   - Unboxed SSA: Raw `i64` for integers, `double` for numbers, `i1` for booleans, `ptr` for GC references; boxed `TValue` containers reserved strictly for generic dynamic values.
   - Precise Alias Scopes / TBAA: Disambiguate `TableHeader`, `TableProperties`, `TableArray`, `UpvalueSlot`, `BufferBytes`, and `GlobalEnv`.
   - Write Barrier Elimination: Omit GC write barriers for non-reference (i64, double, bool) assignments and newly allocated nursery tables.

3. **Comparative Execution & Benchmarking**:
   - Provide `LlvmEngine::comparePerformance` and `BackendMode` switching to run benchmarks comparing the manual assembly builder vs LLVM backend side-by-side.

## Consequences

- Full LLVM optimization power (SROA, GVN, LICM, auto-vectorization, instruction scheduling).
- Retains manual assembly builders in parallel during evaluation, enabling direct speedup comparison and verification before final deprecation.
- Preserves 100% backward compatibility with vanilla Luau while unlocking C-level execution speed for typed arrays, shapes, and numeric algorithms.
