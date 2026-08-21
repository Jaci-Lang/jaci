# Table Structure & Hash Lookup Mechanics

## 1. Table Architecture (`LuaTable`)

The Jaci table is a hybrid associative data structure consisting of:
1. **Array Part**: A $0$-indexed contiguous array of `TValue` for sequential integer keys $1 .. \text{sizearray}$.
2. **Hash Part**: A power-of-two sized open-addressed array of `LuaNode` with collision chaining.

```cpp
typedef struct LuaTable
{
    CommonHeader;
    uint8_t tmcache;    // Bitmask: (1 << TM_*) is set if metamethod is absent
    uint8_t readonly;   // Read-only guard for sandboxed tables
    uint8_t safeenv;    // Fast global environment flag
    uint8_t lsizenode;  // Base-2 logarithm of hash node count
    uint8_t nodemask8;  // Fast mask ((1 << lsizenode) - 1) truncated to 8 bits

    int sizearray;      // Total size of array part
    union
    {
        int lastfree;   // Scan pointer for free node allocation
        int aboundary;  // Cached negated boundary for length calculation
    };

    struct LuaTable* metatable; // Associated metatable
    TValue* array;              // Array part buffer
    LuaNode* node;              // Hash part node buffer (or luaH_dummynode)
    GCObject* gclist;           // Intrusive gray list pointer for GC
} LuaTable;
```

---

## 2. Node Layout & Hash Key Encoding (`LuaNode`, `TKey`)

```cpp
typedef struct TKey
{
    ::Value value;             // 8-byte value payload
    int extra[LUA_EXTRA_SIZE]; // 4-byte extra data
    unsigned tt : 4;           // 4-bit type tag (LUA_T*)
    int next : 28;             // 28-bit signed offset to next chained collision node
} TKey;

typedef struct LuaNode
{
    TValue val;  // Value slot
    TKey key;    // Key slot with embedded next chain offset
} LuaNode;
```

### 2.1 Node Chain Invariants
- `next == 0`: Indicates the end of the collision chain.
- `next != 0`: Indicates `node + next` is the subsequent node in the collision chain.
- Empty Sentinel: Empty hash tables point `t->node` to the shared immutable `luaH_dummynode`.

---

## 3. Hash Lookup & Collision Resolution

The hash part uses an optimized form of Brent's method with **Main Position Invariants**.

### 3.1 Main Position Calculation (`mainposition`)
Every key maps to an ideal "main position" node index:
- **Strings**: `ts->hash & (sizenode - 1)`
- **Integers**: Integer hash mix mapped modulo node size.
- **Numbers / Floats**: Bitwise double-precision float mixing.
- **Pointers / Objects**: Object address hash mix.

### 3.2 Key Lookup Workflow (`luaH_get`)
```cpp
const TValue* luaH_get(LuaTable* t, const TValue* key)
```
1. Compute `mainposition(t, key)`.
2. Inspect node at main position.
3. If node matches key type and value: return `&node->val`.
4. If node does not match and `node->key.next != 0`:
   - Follow `node = node + node->key.next`.
   - Repeat comparison until match found or chain ends (`next == 0`).
5. Return `luaO_nilobject` if key is not found.

### 3.3 Key Insertion & Node Eviction Invariant (`luaH_newkey`)
When inserting a key $K$ that maps to main position $M$:
1. If node $M$ is empty: assign $K$ directly into $M$.
2. If node $M$ is occupied:
   - Compute the main position of the occupant key currently residing at $M$, denoted $M_{\text{occ}}$.
   - **Case A ($M_{\text{occ}} == M$)**: The occupant is in its own main position. Find a free slot $F$ using `t->lastfree`, insert $K$ into $F$, and chain $F$ onto the tail of $M$'s collision list.
   - **Case B ($M_{\text{occ}} \ne M$)**: The occupant was placed in $M$ due to an earlier collision and is **not** in its own main position.
     - **Eviction**: Evict the colliding occupant to a free slot $F$.
     - Update the previous node in the occupant's chain to point to $F$.
     - Relocate the occupant's value and key into $F$.
     - Occupy $M$ with the new key $K$, making it the head of its own collision chain.

> **Table Invariant**: The head of every collision chain must reside at its true hash main position. Colliding keys never displace a key from its own main position.

---

## 4. Metamethod Fast Access Cache (`tmcache`)

To eliminate repeated hash lookups for non-existent metamethods, `t->tmcache` stores a bitmask of absent metamethods:
- Bit $i$ (`1 << i`): Set if tag method $i$ (`TMS` enum) is known **not** to exist in `t->metatable`.

### 4.1 Fast Access Macros
```cpp
#define fasttm(l, et, e) \
    ((et) == NULL ? NULL : ((et)->tmcache & (1u << (e))) ? NULL : luaT_gettm(et, e, (l)->global->tmname[e]))

#define fastnotm(et, e) ((et) == NULL || ((et)->tmcache & (1u << (e))))
```

### 4.2 Cache Invalidation Invariant
```cpp
#define invalidateTMcache(t) (t)->tmcache = 0
```
Whenever a field is inserted into a table, updated, or its metatable pointer changes, `invalidateTMcache(t)` must be called immediately to clear stale negative cache entries.

---

## 5. Array Boundary Optimization (`aboundary`)

Computing table length (`#t`) requires finding the boundary index $i$ such that $t[i] \ne \text{nil}$ and $t[i+1] == \text{nil}$.
- For dense arrays: `t->aboundary` stores a negative integer `-(boundary)`.
- If `t->aboundary < 0`, length calculation executes in $O(1)$ by returning `-t->aboundary`.
- If table mutations invalidate density, `t->aboundary` is reset to 0, falling back to binary search (`luaH_getn`).

---

## 6. Table Sandboxing & Safety Invariants

- **`readonly` Flag**: If `t->readonly != 0`, all write operations (`luaH_set`, `SETTABLE`, `SETTABLEKS`) throw a runtime protection error. Used to freeze library tables and shared module exports.
- **`safeenv` Flag**: Set on script global tables when the environment does not share globals with untrusted scripts, permitting aggressive CodeGen optimizations (e.g. constant folding of globals).
