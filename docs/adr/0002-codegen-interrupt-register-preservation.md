# ADR 0002: CodeGen interrupt register preservation in out-of-line handler

## Context

Jacis native CodeGen lowers an explicit `IrCmd::INTERRUPT` instruction at loop
back-edges and just before every CALL / RETURN. Historically, the lowering of
`IrCmd::INTERRUPT` (in particular on AArch64) called the register allocators
unconditional `spill()` method *before* the inline `cb.interrupt != nullptr`
fast-path check. This forced every live SSA value held in a GPR or SIMD
register to be written back to its stack spill slot and evicted at each such
boundary, preventing any register reuse across loop iterations and adding
stores/reloads proportional to the number of live values around every native
C/FFI call.

The spill was mandated by the out-of-line interrupt handler helper
(`emitInterrupt` on X64 and A64) calling into the C callback
`lua_Callbacks::interrupt(L, -1)` without first preserving call-clobbered
registers. Because the C function follows the platform ABI, it is free to
overwrite every caller-saved register. If the inline fast-path kept values in
those registers, any actual interrupt dispatch would corrupt the native
executions state on resume. On X64 this was already fixed; AArch64 still had
the problem.

Two options were considered:

1. Keep spilling inline before the fast-path check (status quo on A64).
   Keeps the out-of-line helper trivial but imposes the spill cost on 100% of
   loop iterations even though `cb.interrupt` is almost always `nullptr`
   during normal execution.
2. Move the register-preservation cost *into* the out-of-line handler and
   remove the inline spill. The fast path (normal case, no interrupt) becomes
   a single load + compare + conditional branch with zero register pressure
   impact. Only the rare taken path pays to save/restore registers.

## Decision

Adopt option 2 on both AArch64 and X64. The AArch64 backend required:

- Remove `regs.spill(index)` from `IrCmd::INTERRUPT` lowering in
  `IrLoweringA64.cpp`.
- Rewrite `emitInterrupt` in `CodeGenA64.cpp` to allocate a 528-byte
  (16-byte aligned) stack frame on the taken path and save/restore the full
  AAPCS64 call-clobbered set:
  - GPRs: `x0..x17` (18 registers, 144 bytes) via `stp`/`ldp` pairs.
  - SIMD: `q0..q7` and `q16..q31` (24 registers, 384 bytes) via `str`/`ldr`.
  - Luau-pinned registers (`rState=x19`, `rNativeContext=x20`,
    `rGlobalState=x21`, `rConstants=x22`, `rClosure=x23`, `rCode=x24`,
    `rBase=x25`) live in the AAPCS64 callee-save range `x19..x28` and are
    therefore preserved by the C call itself; they are not saved again.
- The race-to-null case (`cb.interrupt` becomes `nullptr` after the inline
  check but before the helper body runs) still skips frame setup, matching
  the X64 layout.
- The error path (`L->status != 0` after the callback) intentionally skips
  the frame restore before calling `emitExit(..., false)`; the native
  execution frame is abandoned as the VM unwinds back to the interpreter, so
  the skipped restore is unobservable and there is no stack leak.

The X64 backend already implemented option 2 (volatile GPR set + 16 xmm saves
around the C call in `EmitCommonX64.cpp::emitInterrupt`). No functional
change was made; clarifying comments were added to make the invariant
symmetric with the AArch64 rewrite.

## Consequences

- **Performance**: Loop bodies that perform C/native calls and carry live
  SSA values across the call now keep those values in registers across
  iterations. Hot loops that previously did O(live-values) stores + loads on
  every iteration now do 0. Targeted speedup is >= 1.05x for pure C-call
  micro-loops and >= 2x on 8+ register-pressure loops on AArch64.
- **Interrupt dispatch cost**: The taken (cold) interrupt path now carries
  an extra ~528-byte save/restore on A64 (~328/360 bytes on X64).
  Interrupts are explicitly used for sandboxing and timeouts (infrequent
  events), so trading fast-path efficiency for rare-path cost is desirable.
- **Handler contract change**: Code that previously reasoned about the
  register state available to the `emitInterrupt` helper (e.g. a future
  assembly-level debugger hook) must now account for the callee-save frame.
  The `interrupt(L, -1)` C callback signature and semantics are unchanged;
  no external API change.
- **Backward compatibility**: Every valid Luau/Jaci program continues to run
  identically; interrupt firing, error propagation, stack unwinding, and
  `L->ci->savedpc` introspection remain unchanged.
- **Code size**: Each compiled function that emits interrupt handlers now
  shares the same out-of-line `helpers.interrupt` thunk; the per-function
  cost is unchanged (one `jmp helpers.interrupt` per handler site). The
  thunk itself is larger on A64 by roughly 110 instructions; amortized
  across all compiled functions this is negligible.

## Copyright

Copyright (c) 2026 Julia Klee, Roblox Corporation, Lua.org/PUC-Rio under
the MIT License (see `LICENSE.txt`).
