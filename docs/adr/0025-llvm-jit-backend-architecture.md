# ADR 0025: LLVM JIT Backend Architecture

## Context

ADR 0008 retired the manual `AssemblyBuilderX64`/`AssemblyBuilderA64` pipeline in intent, but the codebase contained only text-emitting stubs (`LlvmBuilder` string streams, a no-op `LlvmEngine::compileFunction` returning a constant pointer). No LLVM library was linked, no LLVM IR was parsed or compiled, and the manual assembly builder remained the sole machine-code producer.

This ADR specifies the real LLVM JIT backend that replaces the entire assembly builder: actual LLVM IR generation from Luau IR, the LLVM optimization pipeline, and native compilation with relocation into the Jaci code allocator.

## Decision

### Pipeline

`Bytecode -> IrBuilder -> OptimizeConstProp + OptimizeDeadStore -> LlvmJitLowering -> LLVM IR module -> LLVM O2 pipeline -> ELF relocatable object -> JitObjectLoader -> CodeAllocator allocation -> bind`.

The existing Luau IR frontend and its optimization passes are retained unchanged. Everything after the regalloc/lowering/machine-emission stage is replaced by LLVM.

### Function model: one LLVM function per proto, indirect-call gate

Each compiled proto becomes a standard C LLVM function:

```
define dso_local i32 @luau_jit_proto_<bytecodeid>(ptr %L, ptr %p, ptr %target, ptr %ctx)
```

- `%L` is `lua_State*`, `%p` is `Proto*`, `%ctx` is `NativeContext*`.
- `%target` is the resume site address, computed by the VM `onEnter` callback as `proto->exectarget + execdata[L->ci->savedpc - proto->code]`.
- Return value is the VM protocol result: `0` = native finished the function (VM exits to C), `1` = continue in the VM interpreter at `L->ci->savedpc`.

The module also contains one gate function per compiled module:

```
define dso_local i32 @luau_jit_gate(ptr %L, ptr %p, ptr %target, ptr %ctx)
  %r = call i32 (ptr, ptr, ptr, ptr) %target(ptr %L, ptr %p, ptr %target, ptr %ctx)
  ret i32 %r
```

The gate indirect-calls `%target` with the full argument list and returns the callee result.

Why the indirection: on the assembly path, per-proto code is a continuation of the gate's stack frame. The VM `onEnter` C-calls the assembly gate, the gate switches to the custom register ABI (`rState`/`rBase`/`sClosure`/`rConstants`/`sCode`) and `jmp`s into the per-proto code, which must later exit through the gate's exit stub (`emitExit`). Code reached that way cannot be invoked directly as a C function by the VM: the VM expects the gate's frame protocol. The LLVM path therefore keeps the VM-facing contract standard C: `onEnter` calls the per-module LLVM gate (a real C function), and the gate performs a normal C `call` to the per-proto function. No custom register ABI is involved on the LLVM path.

The per-proto function's entry block is the dispatcher: it validates state and branches to the per-instruction resume block for the entry index. Each bytecode instruction `i` has a dedicated resume block; the per-instruction `execdata` table stores the offset of that block (relative to the module code region start), so the VM can resume at any instruction after a native fallback.

Implementation notes (validated by the test suites):

- Per-proto functions use `dso_local` linkage, not `internal`: nothing in the module references them from the gate (it calls through a pointer), so `internal` functions are dead-stripped by the optimizer before object emission.
- LLVM 22 opaque pointers: the gate's indirect call states the callee type inline in the `call` instruction; a `bitcast` from `ptr` to a function-pointer type is invalid IR.
- `NativeProtoExecDataHeader` gains `usesGateEntry` + `gateEntry` for LLVM-compiled protos: `gateEntry` stores the gate's offset relative to the code region start, fixed up to an absolute address in the `NativeModule` constructor. `onEnter` selects the LLVM gate over the assembly gate from this flag; a bare null check cannot distinguish "no gate" from a gate at offset 0.
- The return-1 protocol is fallback-first: Phase A stubs unconditionally return 1 (the VM resumes at the unchanged `savedpc`). The Phase B lowering writes the resume instruction index back to `L->ci->savedpc` before returning 1, so the VM resumes exactly at the instruction that fell back.

### Object loading: ELF relocatable object into the Jaci layout

The LLVM module is compiled with a static no-PIC target machine to an ELF relocatable object. `JitObjectLoader` parses the object with the LLVM Object library and produces a `JitObjectLayout` matching `CodeAllocator::allocate(data, code)`:

- Data sections (`.rodata`, `.data`) are packed into the data region (layout offset `0`..`dataSize`).
- Code sections (`.text`) are packed into the code region, which starts at `align(dataSize, 32)` — exactly where `CodeAllocator` places `codeStart`, so `final address = allocationBase + layoutOffset` for every site and symbol.
- Relocations are normalized to a small set (`Abs64`, `Abs32`, `Abs32S`, `Pc32`, `Relative`, AArch64 `Call26`/`AdrPrelPgHi21`/`AddAbsLo12Nc`/`Prel32`/`Prel64`) and applied after allocation through a temporary writable window; W^X protections are restored afterward.
- `.eh_frame` regions are registered via `__register_frame` so C++ exceptions can unwind through JIT frames.

Anything outside the supported relocation set (e.g. GOT relocations, PIC) fails compilation with a descriptive error — the target machine is configured to never produce them.

### Backend selection

`compileInternal` routes to the LLVM backend when the `CodeGen_UseLlvm` flag is set and the build links LLVM (`LUAU_USE_LLVM` cache option: `AUTO`/`ON`/`OFF`, default `AUTO`). Failures surface as `CodeGenCompilationResult::CodeGenLlvmFailure`. The assembly builder path remains the fallback for builds without LLVM.

### Targets

The lowering is target-agnostic (LLVM IR); x86-64 and AArch64 work through the LLVM target machine with the correct platform ABI (SystemV/Windows/AArch64 AAPCS) selected by the host triple.

## Consequences

- Real LLVM optimizations (SROA, GVN, LICM, inlining, auto-vectorization) apply to all JIT-compiled Luau code.
- The manual assembly builder and its lowering stack are removed once the LLVM backend passes the full conformance and unit test suites.
- Correctness is fallback-first: any instruction the lowering cannot handle natively writes the resume index back to `L->ci->savedpc` and returns `1` to the VM, so interpreter semantics remain the reference.
- `LlvmEngine` (test/benchmark) and the JIT path share the same engine and object-loading machinery.
