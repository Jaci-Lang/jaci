# Object-Oriented Subsystem & Class Model

## 1. Class System Architecture

Jaci provides first-class object-oriented primitives implemented at the native runtime level, bypassing metatable table-indirection overhead for instance property access.

The system is composed of two primary GC types:
1. `LuauClass` (`LUA_TCLASS`): The class descriptor object, containing schema metadata, static members, and method tables.
2. `LuauObject` (`LUA_TOBJECT`): An allocated class instance containing fixed-offset member slots.

---

## 2. Class Descriptor (`LuauClass`)

```cpp
typedef struct LuauClass
{
    CommonHeader;
    GCObject* gclist;                 // GC gray list linkage
    TString* name;                    // Class identifier name
    struct LuauClass* super;          // Superclass descriptor (NULL if root)

    TValue* staticmembers;            // Array of static member values (e.g. methods)
    LuaTable* memberstooffset;        // Hash table: member name (TString*) -> slot offset (int)
    TString** offsettomember;         // Array: slot offset -> member name (TString*)

    LuaTable* metatable;              // Metatable for class object itself (e.g. __call constructor)
    LuaTable* instancemetatable;      // Metatable for instance objects

    uint32_t numberofinstancemembers; // Number of fields allocated per instance
    uint32_t numberofallmembers;      // Total count of instance + static members

    bool isopen;                      // True if class allows subclassing/extension
    bool hasuserinitinchain;          // True if this class or any ancestor defines __init
} LuauClass;
```

---

## 3. Instance Object (`LuauObject`)

```cpp
typedef struct LuauObject
{
    CommonHeader;
    GCObject* gclist;           // GC gray list linkage
    LuauClass* lclass;          // Class descriptor pointer
    uint32_t numberofmembers;   // Member count allocated in this instance
    TValue* members;            // Contiguous array of instance field values
} LuauObject;
```

### 3.1 Memory Self-Containment Invariant
> **GC Invariant**: `LuauObject` stores `numberofmembers` directly inside the instance struct.

If a `LuauObject` and its `LuauClass` descriptor are reclaimed in the same garbage collection cycle, the class descriptor may be swept before the instance. Storing `numberofmembers` inside `LuauObject` ensures the memory allocator (`luaM_freearray`) can deallocate `members` without dereferencing a potentially freed `lclass` pointer.

---

## 4. Member Slot Layout & Indexing Mechanics

Member offsets are split into two contiguous ranges:

```
Offset Range:  0 ..................... numberofinstancemembers-1 | numberofinstancemembers ......... numberofallmembers-1
Content:      [         Instance Fields (LuauObject)            ] | [           Static Methods (LuauClass)             ]
```

### 4.1 Access Invariants
1. **Instance Property Access**: If `offset < class->numberofinstancemembers`:
   - Slot resides in `instance->members[offset]`.
2. **Static Method Access**: If `offset >= class->numberofinstancemembers`:
   - Slot index in static storage is `offset - class->numberofinstancemembers`.
   - Value resides in `class->staticmembers[offset - class->numberofinstancemembers]`.

---

## 5. Constructor & Inheritance Invariants

### 5.1 Constructor Constraint (`hasuserinitinchain`)
- If an ancestor class defines an `__init` constructor, all derived subclasses must explicitly define their own `__init` constructor.
- Because static analysis cannot guarantee constructor presence across dynamic script boundaries, the runtime enforces this via the `hasuserinitinchain` flag.
- Default class constructors check `hasuserinitinchain`; if true, instantiation without an explicit subclass constructor throws a runtime error.

### 5.2 Single Inheritance Chain
- `super` points strictly to single parent `LuauClass` descriptors.
- Subclasses inherit all member offset mappings from `super`, appending new instance fields to the end of the instance offset range to preserve base class slot offsets.
