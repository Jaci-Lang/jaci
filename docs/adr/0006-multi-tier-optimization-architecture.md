# ADR 0006: Multi-Tier Optimization Architecture (IR -> HIR -> MIR -> Backend)

## Context

Jaci requires high-performance execution for general-purpose applications, native embedding, and computational workloads. The previous single-tier IR translation lowered bytecode directly into low-level IR where VM register loads/stores and dynamic language operations remained tightly coupled to physical bytecode layout. This limited opportunities for high-level semantic specializations (such as table shape optimization, array layout specialization, virtual allocation scalar replacement, call devirtualization, and multivalue optimization) as well as concrete SSA/CFG memory optimizations (such as global value numbering, loop-invariant code motion, redundant guard elimination, and memory alias disambiguation).

## Decision

Implement a multi-tier optimization pipeline preserving the existing IR as the stable input representation:

`existing IR → HIR → MIR → backend`

1. **HIR (High-Level Intermediate Representation)**:
   - Serves as the semantic optimization layer.
   - Lifts VM register state into SSA values via abstract execution.
   - Performs path-sensitive fact propagation (ranges, constants, truthiness, semantic types, table shapes, array representations, alias classes, escape states).
   - Optimizes tables as first-class hybrid objects with independent property shapes and array storage.
   - Performs scalar replacement for non-escaping virtual tables and closures.
   - Manages snapshots for safe deoptimization recovery.

2. **MIR (Medium-Level Intermediate Representation)**:
   - Serves as the concrete SSA machine optimization layer.
   - Uses representation-level types (`Int32`, `Int64`, `Float64`, `Bool`, `Tag`, `RawPtr`, `GcPtr`, `ManagedRef`, `TValueBoxed`).
   - Owns explicit memory operations, explicit shape/tag/bounds guards, allocations, and write barriers.
   - Implements precise memory location classes and alias modeling (`TableHeader`, `TableProperties`, `TableArray`, `UpvalueSlot`, `BufferBytes`, `GlobalEnv`).
   - Executes conventional SSA/CFG optimization passes: Redundant Guard Elimination, Bounds Check Elimination, GVN/CSE, LICM, Load/Store Elimination, GC Barrier Elimination, and Dead Code Elimination.

3. **Backend Lowering**:
   - Lowers optimized MIR back to executable IR targeting native code generation (x64 / AArch64).

## Consequences

- Direct, near-zero-cost field accesses via shape guards and direct slot addressing.
- Typed array operations avoid repeated tag checks and table dispatch overhead.
- Non-escaping temporary allocations disappear completely via virtual table scalar replacement.
- Redundant guards and loop-invariant operations are hoisted and eliminated across basic blocks.
- Fully backward compatible with vanilla Luau while providing an expressive superset foundation for future native features.
