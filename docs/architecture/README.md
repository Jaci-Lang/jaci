# Jaci Architecture & Subsystem Invariants

## Overview
Jaci is an independent Luau fork optimized for standalone runtime performance, high-throughput execution, native interoperation, and aggressive compilation. This directory documents the internal architecture, memory layout, execution model, and invariants governing the Virtual Machine (`VM/`) and Native Code Generator (`CodeGen/`).

## Subsystem Navigation

1. [Memory Model & Object Representation](file:///home/klee/Documentos/jaci/docs/architecture/01-memory-and-object-model.md)
   - Value tagging (`TValue`, `Value`, `extra`, `tt`).
   - Collectible headers (`CommonHeader`, `GCheader`).
   - Size-segregated page allocator (`lua_Page`, size classes, bump allocation, free lists).
   - GCO vs non-GCO memory isolation and memory category accounting (`memcat`).

2. [Garbage Collection Invariants](file:///home/klee/Documentos/jaci/docs/architecture/02-garbage-collector.md)
   - Incremental non-moving 3-color mark-and-sweep algorithm (`white0`, `white1`, `gray`, `black`, `fixed`).
   - Tri-color invariant maintenance and write barriers (forward vs backward).
   - GC lifecycle phases (`GCSpause`, `GCSpropagate`, `GCSpropagateagain`, `GCSatomic`, `GCSsweep`).
   - Atomic phase operations, weak table clearing, stack rescanning, and upvalue finalization.
   - Pacing feedback loop and PID heap controller.

3. [Stack Architecture & Execution Runtime](file:///home/klee/Documentos/jaci/docs/architecture/03-stack-and-execution-runtime.md)
   - Contiguous Lua stack frames (`lua_State`, `CallInfo`, `base`, `top`, `stack_last`).
   - Fixed vs variadic parameter layout and stack adjustment invariants.
   - Stack reallocation and pointer relocation (`luaD_reallocstack`, `correctstack`).
   - C stack depth bounds (`nCcalls`, `baseCcalls`) and recursion guards.
   - Protected execution boundaries (`luaD_rawrunprotected`), coroutine yields, re-entry (`SCHEDULED_REENTRY`), and continuation mechanics.

4. [Table Structure & Hash Lookup Mechanics](file:///home/klee/Documentos/jaci/docs/architecture/04-tables-and-lookup-mechanics.md)
   - Hybrid array and open-addressed hash map structure (`LuaTable`, `LuaNode`).
   - Main position calculation, hash collisions, Robin Hood displacement, and chained offsets.
   - Fast metamethod cache (`tmcache`), slot prediction, and cache invalidation.
   - Array boundary caching (`aboundary`), readonly enforcement, and safe environment flags.

5. [Object-Oriented Subsystem & Class Model](file:///home/klee/Documentos/jaci/docs/architecture/05-class-system-runtime.md)
   - Class descriptor (`LuauClass`) and instance layout (`LuauObject`).
   - Instance vs static member offset translation tables.
   - Class inheritance hierarchies, constructor validation (`hasuserinitinchain`), and metatables.

6. [Bytecode Instruction Set & VM Interpreter](file:///home/klee/Documentos/jaci/docs/architecture/06-bytecode-interpreter.md)
   - Word-code instruction encodings (ABC, AD, E, AUX) and instruction formats.
   - Computed `goto` table dispatch loop (`VM_USE_CGOTO`) and switch dispatch fallback.
   - Execution safety macros (`VM_PROTECT`, `VM_CHECK_GC`, `VM_PROTECT_PC`).
   - Built-in fastcall dispatch (`LOP_FASTCALL*`) and operand extraction.
   - Numerical and generic loop execution pipelines.

7. [CodeGen Intermediate Representation & Optimization](file:///home/klee/Documentos/jaci/docs/architecture/07-codegen-ir-and-optimizations.md)
   - Native proto execution metadata (`NativeProtoExecDataHeader`) and binding lifecycle.
   - Static single-assignment IR structures (`IrBlock`, `IrCmd`, `IrOp`).
   - Bytecode-to-IR translation (`IrTranslation`, `IrTranslateBuiltins`).
   - Optimization passes: constant propagation (`OptimizeConstProp`), dead store elimination (`OptimizeDeadStore`), range analysis, and tag specialization.

8. [Target Machine Backends: x64 & AArch64](file:///home/klee/Documentos/jaci/docs/architecture/08-codegen-backends-x64-a64.md)
   - Linear-scan register allocation (`IrRegAllocX64`, `IrRegAllocA64`) and spill area management.
   - Dedicated register reservations (`rState`, `rBase`, `rNativeContext`, `rConstants`).
   - ABI compliance (System V AMD64, Windows x64, AAPCS64) and entry/exit gateways (`gateEntry`, `gateExit`).
   - Fallback execution, deoptimization, and VM context synchronization.

9. [Executable Memory Allocation & Stack Unwinding](file:///home/klee/Documentos/jaci/docs/architecture/09-native-memory-and-unwinding.md)
   - Executable memory allocators (`CodeAllocator`, `SharedCodeAllocator`).
   - W^X security invariants, page protection transitions (`mprotect`, `VirtualProtect`), and instruction cache synchronization.
   - Stack unwinding implementations: DWARF2 Call Frame Information (`.eh_frame`, CIE/FDE) on Linux/macOS and SEH tables on Windows.

## Invariant Summary

| Subsystem | Primary Invariant | Violation Consequence |
|---|---|---|
| **Memory** | GCO blocks must never be freed in isolation outside page sweep. | Heap corruption / dangling freelist pointers. |
| **GC** | A black object must never point directly to a white object during mark. | Premature deallocation of reachable objects. |
| **Stack** | C pointers to stack slots (`base`, `ra`, `rb`, `rc`) are invalidated across any call that may reallocate stack. | Use-after-free / silent stack corruption. |
| **Table** | Nodes in the hash part must reside at their hash main position or follow a chain from that position. | Key lookup misses and corrupted hash chains. |
| **Interpreter** | `L->ci->savedpc` must be synchronized prior to any operation that can invoke GC or throw an error. | Inaccurate debug backtraces and unhandled exceptions. |
| **CodeGen Gateway** | Native frames must restore VM registers (`rState`, `rBase`) and synchronize stack top upon VM return or fallback. | Stack desynchronization and register stomping. |
| **Code Allocator** | Native memory pages must never be simultaneously writable and executable (W^X enforcement). | Security violation / CPU execution fault. |
