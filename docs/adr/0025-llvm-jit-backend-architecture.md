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

### Numeric SSA regions and direct Lua calls

Lower eligible numeric `FORNPREP`/`FORNLOOP` bodies twice: keep the normal per-instruction blocks for arbitrary VM resume, and add one guarded SSA loop region for the normal entry path.

- Accept numeric loads, moves, unary negation, arithmetic operations with numeric constants, forward branches, cached numeric string-field accesses, and numeric reads from plain arrays.
- Reject backedges inside the body, calls, allocation, callbacks, and writes to loop control registers before selecting the region.
- Run forward definite-assignment analysis across the acyclic body. Materialize private scalar slots and let LLVM `mem2reg` reconstruct loop and branch phi nodes.
- Check every loop-carried input tag once at region entry.
- Keep intermediate and loop-carried numbers in LLVM SSA values across iterations.
- Resolve cached string fields with `luaH_getstr` once at region entry. Require an existing numeric node and a writable table for stores; a call-free, allocation-free body cannot invalidate that node before the next safepoint.
- Guard dynamic array keys for positive exact integers, bounds, and numeric elements. Synchronize values already defined in the current iteration before resuming the precise `GETTABLE` bytecode on a miss.
- Synchronize modified `TValue` registers only at loop exit or immediately before a VM safepoint fallback.
- Check the interrupt callback and optional heap threshold at every backedge. Resume the generic `FORNLOOP` block after synchronizing state when either check requires the VM.
- Keep the generic instruction blocks authoritative for type failures, errors, debugger resume, and unsupported bodies.

Emit numeric floor division and modulo through `llvm.floor`. This lets the host target select hardware rounding instructions such as x86-64 `vroundsd` instead of emitting an indirect `floor()` call inside the loop. Embed immutable numeric bytecode constants directly in LLVM IR and specialize a statically loaded numeric loop step.

Collapse straight numeric leaf functions into guarded scalar SSA regions and return directly into the caller's result slots when the requested result count matches. Build a module-wide prototype switch for eligible numeric leaves; guarded call sites evaluate a matching leaf directly in caller SSA without constructing a `CallInfo` frame. Preserve the generic call for debugger/interrupt activity, type-guard failures, varargs, and unsupported result shapes. Direct other fixed-argument calls to already compiled LLVM children through the child's standard C entry, resume the parent immediately when the child unwinds its frame, and propagate VM fallback or yield status when the child frame remains active. Fresh calls branch directly to bytecode block zero without computing a resume index or executing the dispatcher switch.

Resolve a missed `GETGLOBAL`, `GETTABLEKS`, `SETGLOBAL`, `SETTABLEKS`, or `NAMECALL` prediction with `luaH_getstr` inside native code. LLVM embeds the original C prediction byte and cannot observe the VM's later bytecode patch, so immediate fallback would repeat on every invocation. Continue natively only for an existing value and preserve the VM path for nil/metatable behavior, readonly writes, and collectable-value barriers.

Lower guarded scalar math fastcalls (`abs`, `floor`, `ceil`, `sqrt`, `round`, `deg`, `rad`, `sign`, `min`, and `max`) directly to LLVM scalar operations and intrinsics. Recognize their `FASTCALL1`, `FASTCALL2`, and `FASTCALL2K` slow-path bytecode sequences inside numeric SSA loops, validate the safe environment once at region entry, and omit the skipped import/call blocks from the hot CFG. Preserve ordered `min`/`max` argument selection for NaN and signed-zero behavior. Preserve the complete slow sequence for debugger activity, unsafe environments, non-numeric arguments, and unsupported builtins.

Implementation notes (validated by the test suites):

- Per-proto functions use `dso_local` linkage, not `internal`: nothing in the module references them from the gate (it calls through a pointer), so `internal` functions are dead-stripped by the optimizer before object emission.
- LLVM 22 opaque pointers: the gate's indirect call states the callee type inline in the `call` instruction; a `bitcast` from `ptr` to a function-pointer type is invalid IR.
- `NativeProtoExecDataHeader` gains `usesGateEntry` + `gateEntry` for LLVM-compiled protos: `gateEntry` stores the gate's offset relative to the code region start, fixed up to an absolute address in the `NativeModule` constructor. `onEnter` selects the LLVM gate over the assembly gate from this flag; a bare null check cannot distinguish "no gate" from a gate at offset 0.
- The return-1 protocol is fallback-first: Phase A stubs unconditionally return 1 (the VM resumes at the unchanged `savedpc`). The Phase B lowering writes the resume instruction index back to `L->ci->savedpc` before returning 1, so the VM resumes exactly at the instruction that fell back.
- Record the active bytecode index on entry to every instruction block. Runtime guards can branch to the shared fallback from any fresh native invocation; defaulting the index to zero would replay already completed side effects before the failing instruction.
- `CallInfo::func` is a `StkId`, not an inline `TValue`. Load the stack pointer from the `CallInfo` field before decoding the current closure in every helper path; treating the field address itself as a tagged value corrupts closure-dependent fast paths.

### Object loading: ELF relocatable object into the Jaci layout

The LLVM module is compiled with a static no-PIC target machine to an ELF relocatable object. `JitObjectLoader` parses the object with the LLVM Object library and produces a `JitObjectLayout` matching `CodeAllocator::allocate(data, code)`:

- Data sections (`.rodata`, `.data`) are packed into the data region (layout offset `0`..`dataSize`).
- Code sections (`.text`) are packed into the code region. Pad the final data size to `CodeAllocator::kCodeAlignment` because the allocator places data immediately before its aligned code pointer; section-local alignment alone does not guarantee an aligned data base.
- Relocations are normalized to a small set (`Abs64`, `Abs32`, `Abs32S`, `Pc32`, `Relative`, AArch64 `Call26`/`AdrPrelPgHi21`/`AddAbsLo12Nc`/`Prel32`/`Prel64`) and applied after allocation through a temporary writable window; W^X protections are restored afterward.
- Resolve the bounded runtime symbols LLVM may synthesize while optimizing scalar stores (`memset`, `memcpy`, `memmove`, and the `floor` fallback) to absolute host-process addresses. Reject every other undefined symbol.
- `.eh_frame` regions are registered via `__register_frame` so C++ exceptions can unwind through JIT frames.

Anything outside the supported relocation set (e.g. GOT relocations, PIC) fails compilation with a descriptive error — the target machine is configured to never produce them.

### Backend selection

`compileInternal` routes to the LLVM backend when the `CodeGen_UseLlvm` flag is set and the build links LLVM (`LUAU_USE_LLVM` cache option: `AUTO`/`ON`/`OFF`, default `AUTO`). Failures surface as `CodeGenCompilationResult::CodeGenLlvmFailure`. The assembly builder path remains the fallback for builds without LLVM.

### Targets

The lowering is target-agnostic (LLVM IR); x86-64 and AArch64 work through the LLVM target machine with the correct platform ABI (SystemV/Windows/AArch64 AAPCS) selected by the host triple.

## Consequences

- Real LLVM optimizations (SROA, GVN, LICM, inlining, auto-vectorization) apply to all JIT-compiled Luau code.
- Numeric CFG loops and straight numeric leaves avoid per-op tag checks and VM-register traffic on hot paths while preserving exact bytecode resume points on cold paths.
- The manual assembly builder and its lowering stack are removed once the LLVM backend passes the full conformance and unit test suites.
- Correctness is fallback-first: any instruction the lowering cannot handle natively writes the resume index back to `L->ci->savedpc` and returns `1` to the VM, so interpreter semantics remain the reference.
- `LlvmEngine` (test/benchmark) and the JIT path share the same engine and object-loading machinery.
