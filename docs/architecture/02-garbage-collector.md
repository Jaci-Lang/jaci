# Garbage Collector Architecture & Invariants

## 1. Collector Overview

Jaci implements an incremental, non-generational, non-moving tri-color mark-and-sweep garbage collector.

The garbage collector runs interleaved with mutator execution:
- **GC Steps**: Triggered either via explicit `lua_gc` calls or amortized as **GC assists** during memory allocations (`luaC_checkGC`).
- **Safety**: GC steps execute synchronously on the mutator thread and never execute concurrently with bytecode interpretation or native code.

---

## 2. Tri-Color Model & Bit Invariants

Every collectible object (`GCObject`) contains a `marked` byte in its header (`GCheader::marked`).

### 2.1 Color Bit Definitions
```cpp
#define WHITE0BIT 0 // White generation 0
#define WHITE1BIT 1 // White generation 1
#define BLACKBIT  2 // Reached / scanned
#define FIXEDBIT  3 // Permanent / uncollectable object (e.g. pinned strings)
#define WHITEBITS bit2mask(WHITE0BIT, WHITE1BIT)
```

| State | Bit Representation | Description |
|---|---|---|
| **White (current)** | `marked & WHITEBITS == currentwhite` | Unreached object; candidate for collection if unchanged by atomic phase. |
| **White (other)** | `marked & WHITEBITS == otherwhite` | Dead object from prior cycle; subject to immediate sweep. |
| **Gray** | `!testbits(marked, WHITEBITS \| bitmask(BLACKBIT))` | Marked reachable, but references have not yet been traversed. |
| **Black** | `testbit(marked, BLACKBIT)` | Marked reachable and all direct references have been visited. |
| **Fixed** | `testbit(marked, FIXEDBIT)` | Fixed object; ignored by sweep and never reclaimed. |

### 2.2 Core Invariant: Tri-Color Guard
> **Primary Invariant**: A black object must never point directly to a white object during the `GCSpropagate`, `GCSpropagateagain`, and `GCSatomic` phases.

If a reference to a white object $W$ is written into a black container $B$, a write barrier must execute immediately to prevent $W$ from being collected.

---

## 3. Write Barriers

Luau/Jaci provides two barrier strategies to maintain the tri-color invariant:

### 3.1 Forward Barrier (`luaC_barrier`, `luaC_barrierf`, `luaC_objbarrier`)
Advances GC progress by immediately coloring the referent $W$ gray or black.
```cpp
#define luaC_barrier(L, p, v) \
    if (iscollectable(v) && isblack(obj2gco(p)) && iswhite(gcvalue(v))) \
        luaC_barrierf(L, obj2gco(p), gcvalue(v));
```
- **Use Case**: Used for upvalue modifications (`luaF_close`, `SETUPVAL`) and metatable updates (`setmetatable`).
- **Cost**: Eliminates repeated rescanning of container $B$ at the cost of immediate mark work.

### 3.2 Backward Barrier (`luaC_barrierfast`, `luaC_barrierback`, `luaC_barriertable`)
Re-colors container $B$ from black to gray and links it into `g->grayagain`.
```cpp
#define luaC_barrierfast(L, t) \
    if (isblack(obj2gco(t))) \
        luaC_barrierback(L, obj2gco(t), &t->gclist);
```
- **Use Case**: Used for frequent table mutations (`setobj2t`, `rawset`).
- **Cost**: Near-zero overhead during mutation; defers full rescan of table contents to `GCSpropagateagain` or `GCSatomic`.

### 3.3 Thread Barrier (`luaC_threadbarrier`)
Thread stacks are mutated continuously without individual slot write barriers.
- **Active Threads**: An active executing thread (`th->isactive == true` or `th == mainthread`) is kept permanently gray during propagation and linked to `g->grayagain`.
- **Inactive Threads**: When suspended, threads are painted black. Any external stack modification outside active execution (such as C API pushes into suspended coroutines) invokes `luaC_threadbarrier` to push the thread back to `grayagain`.

---

## 4. GC State Machine & Lifecycle

```
[GCSpause] ---------> [GCSpropagate] ---------> [GCSpropagateagain]
     ^                                                  |
     |                                                  v
[GCSsweep] <--------------------------------------- [GCSatomic]
```

### 4.1 `GCSpause` (Cycle Start)
1. Resets gray lists: `g->gray = NULL`, `g->grayagain = NULL`, `g->weak = NULL`.
2. Marks GC root set (`markroot`):
   - `mainthread` and `mainthread->gt` (globals).
   - Registry table (`g->registry`) and weak registry (`g->weakregistry`).
   - Basic type metatables (`g->mt`) and tagged userdata metatables (`g->udatamt`).
   - Direct userdata access tables (`g->udatadirect`, `g->udatadirectfields`).
3. Sets `gcstate = GCSpropagate`.

### 4.2 `GCSpropagate` (Incremental Mark)
Pulls objects from `g->gray` one by one via `propagatemark(g)`:
- `LUA_TTABLE`: Traverses array and hash nodes. If `__mode` contains `'k'` or `'v'`, appends to `g->weak` and remains gray.
- `LUA_TFUNCTION`: Marks environment and upvalues.
- `LUA_TTHREAD`: Traverses thread stack (`stack` to `top`) and open upvalues (`openupval`). If active, moves to `grayagain`.
- `LUA_TPROTO`: Marks constants `k`, child protos `p`, debug names, and locvars.
- `LUA_TCLASS` / `LUA_TOBJECT`: Traverses class member offsets, superclasses, and instance field slots.
When `g->gray` is empty, transfers `g->grayagain` into `g->gray` and transitions to `GCSpropagateagain`.

### 4.3 `GCSpropagateagain` (Second Phase Mark)
Traverses objects that were dirtied by backward write barriers during initial propagation. Minimizes work needed during the indivisible atomic phase. When complete, transitions to `GCSatomic`.

### 4.4 `GCSatomic` (Indivisible Synchronization Phase)
Must execute in a single indivisible step without preemption:
1. **Remark Upvalues**: Scans global open upvalue list `g->uvhead` via `remarkupvals()`.
2. **Propagate Outstanding Gray**: Empties `g->gray` and `g->grayagain`.
3. **Rescan Weak & Embedder References**: Re-marks weak tables and invokes embedder GC callbacks (`g->embeddergc`).
4. **Weak Table Clearing (`cleartable`)**: Clears keys/values pointing to white (dead) objects (`iscleared(o)`). Shrinks table hash parts if `'s'` mode is set.
5. **Upvalue Cleanup (`clearupvals`)**: Scans `g->uvhead`. Any open upvalue whose `markedopen` flag is 0 (belongs to a dead thread or unreachable) is unlinked and closed via `luaF_closeupval`.
6. **White Generation Flip**: Inverts `g->currentwhite = otherwhite(g)`. All objects currently marked white become officially dead.
7. **Sweep Setup**: Sets `g->sweepgcopage = g->allgcopages` and transitions to `GCSsweep`.

### 4.5 `GCSsweep` (Incremental Page Sweep)
Sweeps heap page-by-page via `sweepgcopage(L, page)`:
- Examines each block in the page.
- If block is `LUA_TNIL`, skip (already free).
- If block matches `otherwhite(g)` (unmarked from previous cycle), invoke destructor (`freeobj`) and return block to `page->freeList`.
- If block is alive, re-color with new `currentwhite` in preparation for the subsequent GC cycle.
- When `g->sweepgcopage == NULL`, sweep is finished; cycle returns to `GCSpause`.

---

## 5. Open Upvalue Invariants (`UpVal`)

An upvalue represents a captured local variable:
```cpp
typedef struct UpVal
{
    CommonHeader;
    uint8_t markedopen; // Set during active thread stack mark in atomic phase
    TValue* v;          // Points to stack (when open) or &u.value (when closed)
    union
    {
        TValue value;   // Storage location when closed
        struct
        {
            struct UpVal* prev; // Global double-linked list (g->uvhead)
            struct UpVal* next;
            struct UpVal* threadnext; // Thread open upvalue list (th->openupval)
        } open;
    } u;
} UpVal;
```

### Invariants:
1. **Never Black When Open**: An open upvalue (`v != &u.value`) must NEVER be colored black. It can only be white or gray. Because stack slots are mutated without write barriers, coloring an open upvalue black would violate the tri-color invariant.
2. **Global Upvalue Chain**: All open upvalues across all threads are linked into `g->uvhead`.
3. **Closure Invariant**: When a thread dies or its stack frame returns (`luaF_close`), `luaF_closeupval` copies the value from `*uv->v` into `uv->u.value`, updates `uv->v = &uv->u.value`, and unlinks the upvalue from `g->uvhead` and `th->openupval`.

---

## 6. Pacing Algorithm & PID Controller

GC pacing ensures reclamation keeps pace with mutator allocation while minimizing CPU overhead.

- **Tunables**:
  - `gcgoal` (default 200%): Target heap size relative to live object footprint at atomic completion.
  - `gcstepmul` (default 200%): Multiplier for GC mark work relative to mutator allocation bytes.
  - `gcstepsize` (default 1KB): Granularity of allocation bytes required to trigger a step.
- **PID Controller (`GCStats`)**:
  - Measures allocation rates between cycles and tracks heap trigger errors over 32 historical terms (`triggerterms`).
  - Integrates errors into `triggerintegral` to adjust `GCthreshold` dynamically, preventing heap oscillation under high allocation pressure.
