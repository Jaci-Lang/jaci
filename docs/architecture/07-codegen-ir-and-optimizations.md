# CodeGen Intermediate Representation & Optimization

## 1. Native Compilation Overview

The Jaci Native CodeGen compiles Luau bytecode into optimized native machine code (x86-64 and AArch64).

Compilation transforms bytecode through three stages:
```
[Luau Bytecode] ---> [SSA-style IR (IrBuilder)] ---> [Optimization Passes] ---> [Machine Lowering & RegAlloc] ---> [Native Executable Memory]
```

---

## 2. Native Proto Metadata (`NativeProtoExecData`)

Every compiled prototype attaches execution metadata via `Proto::execdata`:

```cpp
struct NativeProtoExecDataHeader
{
    NativeModule* nativeModule;          // Owning NativeModule
    const uint8_t* entryOffsetOrAddress; // Pointer to machine code entry point
    uint32_t bytecodeId;                 // Bytecode proto identifier
    uint32_t bytecodeInstructionCount;   // Number of bytecode instructions
    uint32_t extraDataCount;             // Extra metadata entries
    size_t nativeCodeSize;               // Total machine code size in bytes
};
```
- Following the header is a contiguous array of `uint32_t instructionOffsets[bytecodeInstructionCount]`.
- Maps each bytecode PC position to its corresponding native code entry offset.

---

## 3. Intermediate Representation (`IrData.h`)

The Jaci IR uses an explicit Control Flow Graph (CFG) of basic blocks containing linear sequences of three-address instructions.

### 3.1 Basic Blocks (`IrBlock`)
- Sequences of instructions with single-entry, single-exit execution.
- Terminators: unconditional jumps (`JUMP`), conditional branches (`JUMP_EQ_TAG`, `JUMP_CMP_NUM`), returns (`RETURN`), or VM exits.

### 3.2 Instructions (`IrInst`) & Commands (`IrCmd`)
Instructions consist of an `IrCmd` and up to 7 operands (`IrOp`):

```cpp
struct IrInst
{
    IrCmd cmd;           // Operation code (LOAD_TAG, ADD_NUM, CHECK_TAG, etc.)
    IrOp a, b, c, d, e, f, g;
};
```

### 3.3 Operand Kinds (`IrOpKind`)
- `None`: Unused operand.
- `Constant`: Direct compile-time constant (boolean, int, double, tag).
- `VmReg`: Reference to Lua VM stack slot register $R(n)$.
- `VmConst`: Reference to Proto constant table slot $K(n)$.
- `VmUpvalue`: Reference to closure upvalue $UP(n)$.
- `Inst`: Data dependency reference to the result of a previous `IrInst`.
- `Block`: CFG target block reference.
- `VmExit`: Exit handler transition returning control to the VM interpreter.

---

## 4. Guard & Fallback Invariants

To maintain 100% semantic compatibility with the Luau VM while generating fast unboxed machine code, the IR inserts **speculative guards**:

```cpp
// Guard against non-number tag; jumps to fallback on failure
build.inst(IrCmd::CHECK_TAG, tagOp, build.constTag(LUA_TNUMBER), fallbackBlock);
```

### 4.1 Common IR Guards
- `CHECK_TAG`: Verifies operand matches expected `LUA_T*` tag.
- `CHECK_READONLY`: Verifies table is mutable prior to inlined write.
- `CHECK_NO_METATABLE`: Verifies table has no metatable intercepting field lookups.
- `CHECK_ARRAY_SIZE`: Verifies index is within `0 < idx <= sizearray`.
- `CHECK_SLOT_MATCH`: Verifies inline cached hash slot matches key.
- `CHECK_BUFFER_LEN`: Verifies buffer access range $[min, max)$ is in-bounds.

### 4.2 Fallback Stream Scope (`FallbackStreamScope`)
When a guard fails, execution branches to a dedicated fallback block that invokes C helper functions (`luaV_gettable`, `luaV_doarithadd`, etc.) and seamlessly rejoins native execution at the next instruction.

---

## 5. Optimization Pipeline

### 5.1 Constant Propagation & Folding (`OptimizeConstProp.cpp`)
- Evaluates arithmetic and bitwise operations on compile-time constants ($1 + 2 \to 3$).
- Folds identity operations:
  - $x + 0 \to x$, $x \times 1 \to x$, $x - 0 \to x$.
  - $x \ \& \ x \to x$, $x \ | \ 0 \to x$.
- **Tag Specialization**: Tracks known tags across basic blocks. Eliminates redundant `CHECK_TAG` instructions when an operand was already validated in a dominating block.

### 5.2 Dead Store Elimination (`OptimizeDeadStore.cpp`)
- Tracks writes to VM register slots $R(n)$.
- **Load-to-Store Forwarding**: If a value is stored to $R(n)$ and immediately read within the same basic block, replaces the subsequent `LOAD_*` with the stored SSA value.
- **Dead Store Removal**: If register $R(n)$ is overwritten before any intermediate read, GC assist, or VM exit, removes the dead store.

### 5.3 Range & Bounds Analysis
- Tracks integer ranges $[L, U]$ across loop counters and array accesses.
- Eliminates redundant `CHECK_ARRAY_SIZE` and `CHECK_BUFFER_LEN` instructions when indices are provably within bounds.
