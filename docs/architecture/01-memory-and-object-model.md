# Memory Model & Object Representation

## 1. Value Representation (`TValue`)

Luau and Jaci use a discriminated tagged-union value model rather than NaN-boxing. Every value in registers, stacks, and constant tables is represented by a `TValue` struct.

### 1.1 `TValue` Memory Layout

On 64-bit platforms (default vector size 3):
```
+------------------------------------+------------------+---------+
| Value (8 bytes: double/ptr/int64)  | extra (4 bytes)  | tt (4B) |
+------------------------------------+------------------+---------+
Total size: 16 bytes. Alignment: 8 bytes (or 16 bytes for SIMD operations).
```

When 4-wide vectors (`LUA_VECTOR_SIZE == 4`) are enabled:
```
+------------------------------------+------------------+---------+
| Value (12 bytes: float v[3] / etc) | extra (8 bytes)  | tt (4B) |
+------------------------------------+------------------+---------+
Total size: 24 bytes.
```

### 1.2 `Value` Union
```cpp
typedef union
{
    GCObject* gc;   // Collectable heap object pointer (tt >= LUA_TSTRING)
    void* p;        // Lightuserdata raw pointer
    double n;       // IEEE 754 double precision float
    int b;          // Boolean (0 or 1)
    int64_t l;      // 64-bit integer
    float v[2];     // Vector elements v[0], v[1]; v[2] stored in TValue::extra[0]
} Value;
```

### 1.3 Value Types (`tt`)
The tag `tt` defines the runtime type:
- Primitive non-collectable types:
  - `LUA_TNIL` (0)
  - `LUA_TBOOLEAN` (1)
  - `LUA_TLIGHTUSERDATA` (2): Pointer stored in `value.p`; tag stored in `extra[0]`.
  - `LUA_TNUMBER` (3): Float/double value in `value.n`.
  - `LUA_TVECTOR` (4): Float vector stored in `value.v` and `extra` (or heap-allocated `LuauVector` if `LUA_VECTOR_DOUBLE` is enabled).
  - `LUA_TINTEGER` (9): 64-bit signed integer in `value.l`.
- Collectable heap types (`tt >= LUA_TSTRING`):
  - `LUA_TSTRING` (5) -> `TString`
  - `LUA_TTABLE` (6) -> `LuaTable`
  - `LUA_TFUNCTION` (7) -> `Closure`
  - `LUA_TUSERDATA` (8) -> `Udata`
  - `LUA_TTHREAD` (10) -> `lua_State`
  - `LUA_TBUFFER` (11) -> `Buffer`
  - `LUA_TPROTO` (12) -> `Proto`
  - `LUA_TUPVAL` (13) -> `UpVal`
  - `LUA_TCLASS` (14) -> `LuauClass`
  - `LUA_TOBJECT` (15) -> `LuauObject`

### 1.4 Invariant: Value Liveness & Type Match
For any collectable `TValue* o`:
```cpp
LUAU_ASSERT(ttype(o) == o->value.gc->gch.tt);
LUAU_ASSERT(!isdead(L->global, o->value.gc));
```
A `TValue` must never reference a collected or swept `GCObject`.

---

## 2. Collectible Objects Header (`CommonHeader`)

Every garbage-collected object allocated on the heap begins with the `CommonHeader` macro:
```cpp
#define CommonHeader uint8_t tt; uint8_t marked; uint8_t memcat
```
- `tt`: Object type tag matching `LUA_T*`.
- `marked`: GC color bits (`white0`, `white1`, `black`, `fixed`, `markedopen`).
- `memcat`: Memory category tracking identifier (0..`LUA_MEMORY_CATEGORIES - 1`).

All collectible structures are members of the `GCObject` union:
```cpp
union GCObject
{
    GCheader gch;
    struct TString ts;
    struct Udata u;
    struct Closure cl;
    struct LuaTable h;
    struct Proto p;
    struct UpVal uv;
    struct lua_State th;
    struct LuauBuffer buf;
    struct LuauClass lclass;
    struct LuauObject lobject;
    struct LuauVector vec;
};
```

---

## 3. Size-Segregated Page Allocator (`lmem.cpp`)

Memory in Jaci is managed via size-segregated pages (`lua_Page`) for objects up to 1024 bytes (`kMaxSmallSizeUsed`), backed by a general system allocator callback `frealloc(ud, ptr, osize, nsize)`.

### 3.1 Allocator Invariants
1. **Size Separation**: Small allocations (<= 1024 bytes) are served from segregated size-class pages. Allocations > 1024 bytes (or large GCOs > 512 bytes) allocate dedicated pages or bypass the page cache directly via `frealloc`.
2. **Alignment Guarantee**: All size classes are multiples of 8 bytes. For userdata sizes >= 16 bytes, allocations provide 16-byte alignment (`alignas(8)` or 16-byte page payload offsets).
3. **No Direct Isolated GCO Free**: GCO objects contain no back-reference to their hosting `lua_Page`. GCO blocks cannot be freed individually via direct pointers; they are freed exclusively during incremental page sweep via `luaM_freegco`.
4. **Freed GCO Tag Invariant**: A freed GCO slot inside an active page must retain its `GCheader` with `tt = LUA_TNIL` intact. The page sweeper reads `gco->gch.tt` to skip already-freed slots without corrupting freelist links.

### 3.2 `lua_Page` Structure
```cpp
struct lua_Page
{
    lua_Page* prev;     // Linked list of pages with available free blocks
    lua_Page* next;

    lua_Page* listprev; // Intrusive global list of all pages
    lua_Page* listnext;

    int pageSize;       // Total page size in bytes (e.g. 16KB - 24B metadata reduction)
    int blockSize;      // Size class block size in bytes
    void* freeList;     // Head of linked list of recycled freed blocks within page
    int freeNext;       // Offset into page payload for bump-pointer allocation
    int busyBlocks;     // Count of active allocated blocks in this page

    char padding[...];  // Ensures offsetof(lua_Page, data) % 16 == 0
    char data[1];       // Start of memory blocks
};
```

### 3.3 Progressive Size Classes (`SizeClassConfig`)
Size classes balance internal and external fragmentation using four progressive tiers:
- `8 .. 56` bytes: Step by 8 bytes.
- `64 .. 240` bytes: Step by 16 bytes.
- `256 .. 480` bytes: Step by 32 bytes.
- `512 .. 1024` bytes: Step by 64 bytes.

Lookup from size to size class is an $O(1)$ table query (`kSizeClassConfig.classForSize[sz]`).

### 3.4 Allocation Workflow
1. Look up size class $C$ for requested size.
2. If $C \ge 0$, inspect `global_State::freegcopages[C]` (or `freepages[C]` for non-GCOs).
3. If page list is non-empty, allocate from the page:
   - If `page->freeList != nullptr`, pop the head block.
   - Else bump `page->freeNext += blockSize`.
   - Increment `page->busyBlocks`. If page becomes full, unlink from `freegcopages[C]`.
4. If no page is available, invoke `newpage()` via `frealloc`, register page in `allgcopages` and `freegcopages[C]`.
5. If requested size > `kMaxSmallSizeUsed`:
   - For GCO: allocate single-block dedicated page linked into `allgcopages`.
   - For non-GCO: invoke `frealloc` directly with no page header.

### 3.5 Free Workflow
- **Non-GCO Blocks**: Store a pointer to `lua_Page` in block metadata (`metadata(block)`). When freed via `luaM_free_`, retrieve page pointer, push block to `page->freeList`, decrement `page->busyBlocks`. If `busyBlocks == 0`, immediately return the entire page to system memory via `frealloc`.
- **GCO Blocks**: Swept page-by-page. For dead objects (`isdead(g, gco)`), invoke type-specific destructor (`luaH_free`, `luaF_freeclosure`, etc.), then push to `page->freeList` using `freegcolink(block)` (offset `sizeof(GCheader)`).

---

## 4. Memory Categories (`memcat`)

Every allocation is tagged with a memory category (`uint8_t memcat`).
- `L->activememcat`: Inherited by all new allocations performed in the thread context.
- `g->memcatbytes[memcat]`: Tracks running memory totals per category.
- Categories allow fine-grained profiling and quota enforcement without introducing per-object memory overhead.
