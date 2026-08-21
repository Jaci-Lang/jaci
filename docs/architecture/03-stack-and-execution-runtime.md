# Stack Architecture & Execution Runtime

## 1. Lua Thread State (`lua_State`)

Every coroutine and main execution thread is encapsulated by a `lua_State` object (`LUA_TTHREAD`).

```cpp
struct lua_State
{
    CommonHeader;
    uint8_t status;           // Thread execution status (LUA_OK, LUA_YIELD, LUA_BREAK, SCHEDULED_REENTRY)
    uint8_t activememcat;     // Memory category for allocations in this thread
    bool isactive;            // Thread is actively executing (stack mutated without write barriers)
    bool singlestep;          // Single-step debug hook enabled

    StkId top;                // First free slot on the value stack
    StkId base;               // Base register slot (R0) of current CallInfo frame
    global_State* global;     // Pointer to shared VM state
    CallInfo* ci;             // Active CallInfo frame
    StkId stack_last;         // Last usable slot on stack before EXTRA_STACK boundary
    StkId stack;              // Base pointer of the contiguous TValue stack array

    CallInfo* end_ci;         // Last element of CallInfo array
    CallInfo* base_ci;        // Base pointer of the CallInfo array

    int stacksize;            // Total allocated TValue slots in stack
    int size_ci;              // Total allocated CallInfo entries in base_ci

    unsigned short nCcalls;    // Current nested C invocation depth
    unsigned short baseCcalls; // Nested C call depth when resuming coroutine

    int cachedslot;           // Slot index cache for inline table operations
    LuaTable* gt;             // Thread global variables table
    UpVal* openupval;         // Intrusive linked list of open upvalues pointing to this stack
    GCObject* gclist;         // Gray list link for GC traversal
    TString* namecall;        // Method name cache for NAMECALL instruction
    void* userdata;           // User context pointer
};
```

---

## 2. Stack Frame Layout (`CallInfo`)

The Luau stack is a contiguous array of `TValue` slots. Registers $R(0) .. R(N-1)$ referenced by bytecode instructions are direct indices relative to `ci->base`.

### 2.1 Non-Variadic Function Frame Layout
When `Proto::is_vararg == 0`:
```
 Stack Direction: Low Address -----------------------------> High Address
+--------+------------------+-----------------------------+
| (func) | [fixed args ...] | [locals & temporaries ...]  |
+--------+------------------+-----------------------------+
^        ^^
ci->func ci->base (R0)
```
- `ci->func`: Points to the closure `TValue`.
- `ci->base`: Points to $R(0)$, immediately following `ci->func`.
- Argument $i$ is located at `ci->base + i`.

### 2.2 Variadic Function Frame Layout
When `Proto::is_vararg != 0`, Luau replicates fixed arguments so that $0$-based register addressing remains contiguous:
```
+--------+------------------+--------------+------------------+----------------------------+
| (func) | [fixed args ...] | [varargs...] | [fixed args ...] | [locals & temporaries ...] |
+--------+------------------+--------------+------------------+----------------------------+
^                                          ^^
ci->func                                   ci->base (R0)
```
- Number of fixed parameters: `numparams = ci->p->numparams`.
- Number of variadic arguments: `numvararg = (ci->base - ci->func - 1) - numparams`.
- `GETVARARGS` retrieves variadic arguments from the slot region between `ci->func + 1 + numparams` and `ci->base`.

### 2.3 `CallInfo` Structure
```cpp
typedef struct CallInfo
{
    StkId base;                 // Register 0 for this frame
    StkId func;                 // Closure slot for this frame
    StkId top;                  // Frame top boundary (base + maxstacksize)
    Proto* p;                   // Fast Proto pointer (when LuauCIProto enabled)
    union
    {
        const Instruction* savedpc; // Saved PC before nested call
        int errfunc;                // Stack index of error handler (for C functions)
    };
    int nresults;               // Expected results count (-1 = LUA_MULTRET)
    unsigned int flags;         // Call frame execution flags
} CallInfo;
```

### 2.4 Call Frame Flags
- `LUA_CALLINFO_RETURN` (1 << 0): The interpreter loop must terminate and return after unwinding this frame. Set on the root frame.
- `LUA_CALLINFO_HANDLE` (1 << 1): Errors thrown during execution must be intercepted by a C continuation handler on this frame.
- `LUA_CALLINFO_NATIVE` (1 << 2): Frame is executing via native CodeGen callback.
- `LUA_CALLINFO_OPYIELD` (1 << 3): Call frame yielded on a non-call opcode (e.g. `FORGLOOP`) and requires `luaV_finishop` upon resume.

---

## 3. Stack Reallocation & Pointer Relocation Invariants

When stack capacity is exceeded (`stacklimitreached`), the entire `TValue` stack is reallocated via `luaD_reallocstack`.

### 3.1 Relocation Mechanism (`correctstack`)
Because the stack is stored as a single contiguous array, reallocating memory changes the base address of `L->stack`. Every raw pointer into the stack must be updated using pointer arithmetic:
```cpp
static void correctstack(lua_State* L, TValue* oldstack)
{
    L->top = (L->top - oldstack) + L->stack;
    L->base = (L->base - oldstack) + L->stack;

    // Relocate all open upvalue pointers
    for (UpVal* up = L->openupval; up != NULL; up = up->u.open.threadnext)
        up->v = (up->v - oldstack) + L->stack;

    // Relocate active CallInfo pointers
    for (CallInfo* ci = L->base_ci; ci <= L->ci; ci++)
    {
        ci->top = (ci->top - oldstack) + L->stack;
        ci->base = (ci->base - oldstack) + L->stack;
        ci->func = (ci->func - oldstack) + L->stack;
    }
}
```

### 3.2 Fundamental VM Safety Rule: C Stack Pointer Invalidation
> **Critical Invariant**: Any native C++ local variable pointing to a stack slot (e.g., `base`, `ra`, `rb`, `rc`, `StkId`) is **invalidated** whenever a function call, metamethod, or GC assist triggers `luaD_checkstack` or `luaD_reallocstack`.

To prevent use-after-free bugs:
1. Always preserve stack offsets using `savestack(L, ptr)` ($(\text{char*})ptr - (\text{char*})L\text{->stack}$) before invoking nested operations.
2. Restore pointers using `restorestack(L, offset)` after returning.
3. In the bytecode interpreter loop, wrap external invocations in `VM_PROTECT(op)`, which automatically reloads `base = L->base`.

---

## 4. Execution Limits & Recursion Bounds

To prevent stack-overflow crashes from infinite recursion:
- `LUAI_MAXCCALLS` (200): Limit on nested C-to-Lua or C-to-C invocations (`L->nCcalls`). Exceeding this limit throws `"C stack overflow"`.
- `LUAI_MAXCALLS` (20000): Limit on total `CallInfo` depth (`L->size_ci`). Exceeding this limit throws `"stack overflow"`.
- `MAX_STACK_SIZE` (1GB / `sizeof(TValue)`): Hard ceiling on total value stack memory. Exceeding this throws `LUA_ERRMEM`.

---

## 5. Protected Execution & Error Recovery

Jaci supports two error handling backends selected at build time:
1. **Longjmp (`LUA_USE_LONGJMP`)**: Uses POSIX `_setjmp` / `_longjmp` with linked `lua_jmpbuf` frames. Fast on POSIX, but requires manual unwind registration on Windows x64.
2. **C++ Exceptions**: Uses `throw lua_exception(L, errcode)` caught by `try ... catch` blocks inside `luaD_rawrunprotected`.

### 5.1 Error Propagation (`luaD_throw`)
```cpp
l_noret luaD_throw(lua_State* L, int errcode)
```
- Sets status on the top jump buffer `L->global->errorjmp->status = errcode`.
- Unwinds the native stack back to the enclosing `luaD_rawrunprotected` boundary.
- If no protected boundary exists, invokes `L->global->cb.panic` or aborts the process.

---

## 6. Coroutine Yielding & Continuation Model

Jaci coroutines support full yieldability across C boundaries through explicit continuations.

### 6.1 Status Codes
- `LUA_OK` (0): Normal execution.
- `LUA_YIELD` (1): Coroutine suspended via `coroutine.yield()`.
- `LUA_ERRRUN` .. `LUA_ERRMEM`: Uncaught runtime or memory errors.
- `LUA_BREAK` (4): Paused at a debugger breakpoint.
- `SCHEDULED_REENTRY` (0x7f): Internal state indicating an unrolled C call stack ready for bytecode re-entry without increasing C recursion depth.

### 6.2 Continuation Loop (`resume_continue`)
When a coroutine resumes from a yield:
1. The VM iterates backward over `L->ci` frames down to `L->base_ci`.
2. For C closures with continuations (`cl->isC && cl->c.cont`): invokes `cl->c.cont(L, 0)` to complete the C operation.
3. For Lua frames that yielded inside non-call instructions (e.g. `FORGLOOP` yielding during iterator invocation): executes `luau_finishop(L)` to finalize the opcode state.
4. Invokes `luau_execute(L)` to resume bytecode interpretation.
