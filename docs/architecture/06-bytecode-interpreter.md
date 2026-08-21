# Bytecode Instruction Set & VM Interpreter

## 1. Bytecode Instruction Encodings

Luau/Jaci bytecode instructions use a 32-bit word-code format. Instructions consist of a primary 32-bit word, optionally followed by an auxiliary 32-bit word (`AUX`).

### 1.1 Instruction Formats
```
ABC Format:
 31             24 23             16 15              8 7              0
+-----------------+-----------------+-----------------+-----------------+
|     C (8b)      |     B (8b)      |     A (8b)      |   Opcode (8b)   |
+-----------------+-----------------+-----------------+-----------------+

AD Format:
 31                                 16 15              8 7              0
+-------------------------------------+-----------------+-----------------+
|               D (signed 16b)        |     A (8b)      |   Opcode (8b)   |
+-------------------------------------+-----------------+-----------------+

E Format:
 31                                                    8 7              0
+-------------------------------------------------------+-----------------+
|                    E (signed 24b)                     |   Opcode (8b)   |
+-------------------------------------------------------+-----------------+
```

### 1.2 Decoding Macros
```cpp
#define LUAU_INSN_OP(insn) ((insn) & 0xff)
#define LUAU_INSN_A(insn)  (((insn) >> 8) & 0xff)
#define LUAU_INSN_B(insn)  (((insn) >> 16) & 0xff)
#define LUAU_INSN_C(insn)  (((insn) >> 24) & 0xff)
#define LUAU_INSN_D(insn)  (int32_t(insn) >> 16)
#define LUAU_INSN_E(insn)  (int32_t(insn) >> 8)
```

---

## 2. Interpreter Dispatch Loop (`lvmexecute.cpp`)

The VM execution loop uses direct-threaded computed `goto` on GCC and Clang (`VM_USE_CGOTO == 1`), eliminating switch-dispatch branch mispredictions:

```cpp
#if VM_USE_CGOTO
#define VM_CASE(op) CASE_##op:
#define VM_NEXT() goto*(SingleStep ? &&dispatch : kDispatchTable[LUAU_INSN_OP(*pc)])
#define VM_CONTINUE(op) goto* kDispatchTable[uint8_t(op)]
#else
#define VM_CASE(op) case op:
#define VM_NEXT() goto dispatch
#define VM_CONTINUE(op) dispatchOp = uint8_t(op); goto dispatchContinue
#endif
```

---

## 3. Interpreter Safety Invariants & Stack Protection

```cpp
// Full protection: saves PC and restores base after possible stack reallocation
#define VM_PROTECT(x) \
    { \
        L->ci->savedpc = pc; \
        { x; }; \
        base = L->base; \
    }

// Amortized GC check: steps GC only if threshold is exceeded
#define VM_CHECK_GC(x) \
    { \
        if (luaC_needsGC(L)) \
        { \
            L->ci->savedpc = pc; \
            luaC_step(L, true); \
            base = L->base; \
        } \
    }

// Minimal protection: saves PC before operations that can throw but never reallocate stack
#define VM_PROTECT_PC() L->ci->savedpc = pc
```

### Fundamental Safety Rules:
1. **`savedpc` Synchronization**: `L->ci->savedpc` must reflect the currently executing bytecode instruction before calling any C/C++ function that can raise an error or invoke GC.
2. **Stack Pointer Invalidation**: Any helper that triggers memory allocation (`luaV_gettable`, `luaV_settable`, `luaD_call`) may cause `luaD_reallocstack` to execute. After such calls, local pointers (`base`, `ra`, `rb`, `rc`) must be reloaded from `L->base`.

---

## 4. Built-in Fastcall Mechanism

Luau accelerates common standard library calls using dedicated bytecode instructions: `LOP_FASTCALL`, `LOP_FASTCALL1`, `LOP_FASTCALL2`, `LOP_FASTCALL3`, and `LOP_FASTCALL2K`.

### 4.1 Bytecode Sequence
A fastcall is compiled as an inline pair consisting of a `FASTCALL` instruction followed by its fallback `CALL`:
```
0001: FASTCALL2 14 R2 R3 +2    ; Fastcall math.max (builtin ID 14) with R2, R3
0002: GETIMPORT R1 [math.max]   ; Setup fallback function object
0003: CALL R1 2 1               ; Standard fallback call
0004: ...                       ; Next instruction
```

### 4.2 Execution Invariant:
1. The VM attempts execution via `luauF_table[builtinId](L, res, arg0, nresults, args, nparams)`.
2. **Success**: The fastcall function computes the result in place and returns the result count ($\ge 0$). The VM advances `pc` past both the fallback setup and the `CALL` instruction.
3. **Failure / Incompatible Types**: The fastcall returns a negative value. The VM advances `pc` to the fallback instructions without modifying registers, transparently executing the normal Lua/C call path.

---

## 5. Loop Instruction Pipelines

### 5.1 Numeric For Loops (`LOP_FORNPREP`, `LOP_FORNLOOP`)
Numeric for loops expect four contiguous registers: `[limit, step, index, variable]`.
- **`FORNPREP`**:
  - Validates that `limit`, `step`, and `index` are numbers.
  - If `step > 0 && index > limit` (or `step < 0 && index < limit`), jumps over the loop body.
  - Copies `index` into `variable` and enters the loop body.
- **`FORNLOOP`**:
  - Adds `step` to `index`.
  - Checks if loop condition holds (`step > 0 ? index <= limit : index >= limit`).
  - If valid: updates `variable = index` and jumps back to loop header.

### 5.2 Generic For Loops (`LOP_FORGLOOP`, `LOP_FORGPREP`)
Generic for loops manage generator tuples: `[generator, state, index, variables...]`.
- **`FORGPREP`**: Initializes generator tuple and jumps unconditionally to the loop test at `FORGLOOP`.
- **`FORGLOOP`**: Invokes `generator(state, index)`:
  - If primary returned value is not `nil`, assigns results to user variables, sets `index = primary_result`, and jumps back.
  - If primary result is `nil`, exits the loop.
