# Target Machine Backends: x64 & AArch64

## 1. Machine Lowering Overview

The CodeGen backend lowers optimized IR instructions into target machine assembly using specialized assembly builders (`AssemblyBuilderX64` and `AssemblyBuilderA64`).

---

## 2. Dedicated Register Reservations

To eliminate memory traffic for frequently accessed VM pointers, both architectures pin critical execution context in callee-saved registers throughout native execution.

### 2.1 x86-64 Register Assignments
```cpp
inline constexpr RegisterX64 rState         = r15; // lua_State* L
inline constexpr RegisterX64 rBase          = r14; // StkId base (R0)
inline constexpr RegisterX64 rNativeContext = r13; // NativeContext* context
inline constexpr RegisterX64 rConstants     = r12; // TValue* k
```

### 2.2 AArch64 Register Assignments
```cpp
inline constexpr RegisterA64 rState         = x19; // lua_State* L
inline constexpr RegisterA64 rNativeContext = x20; // NativeContext* context
inline constexpr RegisterA64 rGlobalState   = x21; // global_State* L->global
inline constexpr RegisterA64 rConstants     = x22; // TValue* k
inline constexpr RegisterA64 rClosure       = x23; // Closure* cl
inline constexpr RegisterA64 rCode          = x24; // Instruction* code
inline constexpr RegisterA64 rBase          = x25; // StkId base (R0)
```

---

## 3. Register Allocator (`IrRegAllocX64`, `IrRegAllocA64`)

Jaci employs a linear-scan register allocator designed for fast single-pass compilation.

### 3.1 Spill Slot Organization
Native code operates stacklessly with a fixed frame layout:
- **x64 Spill Frame**:
  - `kExtraLocals = 3` (8-byte slots for `sClosure`, `sCode`, `sTemporarySlot`).
  - `kSpillSlots = 23` (8-byte slots for temporary register spills).
  - Preserves 16-byte stack alignment.
- **A64 Spill Frame**:
  - `kStashSlots = 9` (stashed non-volatile registers).
  - `kTempSlots = 1` (temporary slot).
  - `kSpillSlots = 22` (spill area).

### 3.2 ABI Compatibility Invariants
- **System V AMD64 (Linux / macOS x64)**:
  - Integer arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`.
  - Floating-point: `xmm0` .. `xmm7`.
  - All XMM registers are volatile.
- **Windows x64**:
  - Integer arguments: `rcx`, `rdx`, `r8`, `r9` (with 32-byte shadow space).
  - Floating-point: `xmm0` .. `xmm3`.
  - `xmm6` .. `xmm15` are non-volatile and must be preserved across native calls.
- **AAPCS64 (AArch64)**:
  - Arguments: `x0` .. `x7`, `v0` .. `v7`.
  - `x19` .. `x28` and `v8` .. `v15` (lower 64-bits) are non-volatile.

---

## 4. Entry & Exit Gateways (`gateEntry`, `gateExit`)

Transitions between the C++ runtime and native execution pass through runtime-compiled gateway routines.

```
[C++ Runtime] ---> (gateEntry) ---> [Native CodeGen Function]
                          |                      |
                     (gateExit) <----------------+
                          v
                   [C++ Runtime]
```

### 4.1 `gateEntry` Responsibilities
1. Pushes host platform callee-saved registers to native stack.
2. Allocates fixed spill and local storage.
3. Loads dedicated registers (`rState`, `rBase`, `rNativeContext`, `rConstants`).
4. Jumps to the target native function entry offset.

### 4.2 `gateExit` Responsibilities
1. Synchronizes `L->base` and updates `L->top`.
2. Restores host platform callee-saved registers from stack.
3. Returns status code (0 for normal exit, error code, or yield status) to the VM interpreter.

---

## 5. Deoptimization & Breakpoint Fallbacks (`onDisable`)

When a debugger attaches or sets a breakpoint on a natively compiled proto:
1. `proto->codeentry` is reset to `proto->code` (reverting dispatch to bytecode).
2. `proto->exectarget` is set to 0.
3. `onDisable()` traverses all live thread stacks (`L->base_ci .. L->ci`) and clears the `LUA_CALLINFO_NATIVE` flag on matching frames, ensuring safe seamless deoptimization back to the interpreter.
