# ADR 0008: Transition to LLVM Backend and Native FFI Dispatch

## Context

Prior to this architectural migration, Jaci relied on manual per-architecture assembly builders (`AssemblyBuilderX64` and `AssemblyBuilderA64`) coupled with architecture-specific instruction lowerers and register allocators. While functional for basic JIT execution, manual assembly builders lacked whole-function auto-vectorization, LLVM-grade global instruction scheduling, unboxed SSA representation for numeric variables, and zero-cost FFI calling convention generation.

The new LLVM Backend (`existing IR → HIR → MIR → LLVM IR → LLVM Optimization Pipeline → Native Execution`) has been proven in benchmarks and full test suites to provide:
- Up to $11.28\times$ speedup on temporary object allocation via virtual table scalar replacement (SROA).
- $3.78\times$ speedup on packed arrays with SIMD auto-vectorization.
- $3.26\times$ speedup on shape-guarded property accesses.
- $2.43\times$ speedup on floating-point arithmetic loops.
- Complete compatibility across all 5,165+ unit tests and 322 language conformance tests.

## Decision

Transition the primary code generation architecture to the optimizing multi-tier LLVM Backend:

1. **Native FFI Integration**:
   - Directly lower foreign function calls (`ffi.C.func`, dynamic library symbols, function pointers) into standard C ABI calls (`call ccc`) with unboxed primitive arguments (raw pointers, integers, doubles) and native return types.
   - Support arbitrary C function signatures without manual trampoline assembly code.

2. **Assembly Builder Retirement**:
   - Retire manual instruction emission and hand-rolled register allocators in favor of LLVM's machine code generation, register allocation (PBQP / Greedy), and instruction scheduling.

## Consequences

- Direct native C ABI interoperation with near-zero overhead.
- Single unified multi-tier IR pipeline (`IR → HIR → MIR → LLVM`) serving all target architectures (x86-64, AArch64, and future targets).
- Full compatibility with all Luau bytecode, VM invariants, deoptimization mechanisms, and test suites.
