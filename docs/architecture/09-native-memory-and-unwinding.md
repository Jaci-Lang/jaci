# Executable Memory Allocation & Stack Unwinding

## 1. Code Allocator Architecture (`CodeAllocator.cpp`, `SharedCodeAllocator.cpp`)

Native machine code generation requires specialized memory management adhering to platform security constraints and processor instruction cache coherence.

---

## 2. W^X Security Invariants & Page Protection

Modern operating systems enforce **W^X** (Write XOR Execute): memory must never be simultaneously writable and executable.

### 2.1 Lifecycle of Native Code Pages
```
[1. Allocation]                [2. Code Emission]           [3. Finalization]
mmap / VirtualAlloc    --->    Assembly generation   --->    mprotect / VirtualProtect
(PROT_READ | PROT_WRITE)       writes instructions           (PROT_READ | PROT_EXEC)
```

### 2.2 Protection Helpers
- `allocatePagesImpl(size)`: Allocates pages with `PROT_READ | PROT_WRITE` (`PAGE_READWRITE`).
- `makePagesExecutable(mem, size)`: Transitions pages to `PROT_READ | PROT_EXEC` (`PAGE_EXECUTE_READ`).
- `makePagesReadOnly(mem, size)`: Transitions constant/data pages to `PROT_READ` (`PAGE_READONLY`).
- `makePagesNotExecutable(mem, size)`: Restores write access if code patching or invalidation is required.

---

## 3. Instruction Cache Synchronization

Because CPUs feature separate instruction and data caches (Harvard architecture / split L1 caches), writing instructions into data cache lines requires flushing data caches and invalidating instruction caches prior to execution:

```cpp
static void flushInstructionCache(uint8_t* mem, size_t size)
{
#if defined(__APPLE__)
    sys_icache_invalidate(mem, size);
#elif defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), mem, size);
#else
    __builtin___clear_cache((char*)mem, (char*)mem + size);
#endif
}
```

---

## 4. Stack Unwinding Implementations

When a native function triggers a C++ exception, longjmp, or system panic, the OS unwind library must walk the native stack across dynamically generated machine code frames.

### 4.1 DWARF2 Call Frame Information (`UnwindBuilderDwarf2.cpp`)
On Linux, BSD, and macOS, Jaci emits DWARF2 `.eh_frame` call frame descriptions:
- **Common Information Entry (CIE)**: Defines data/code alignment factors, return address column, and initial canonical frame rules.
- **Frame Description Entry (FDE)**: Defines PC bounds and call frame instructions:
  - `DW_CFA_def_cfa`: Defines Canonical Frame Address as `[reg + offset]`.
  - `DW_CFA_offset`: Describes saved callee-saved register locations relative to CFA.
  - `DW_CFA_advance_loc`: Advances the virtual instruction pointer between prologue steps.
- **Dynamic Registration**: Emitted unwind tables are registered with the runtime unwinder via `__register_frame` / `__deregister_frame`.

### 4.2 Windows Structured Exception Handling (SEH) (`UnwindBuilderWin.cpp`)
On Windows x64, the operating system kernel requires SEH unwind data to unwind through JIT code:
- **`RUNTIME_FUNCTION`**: Records `BeginAddress`, `EndAddress`, and `UnwindData` offset.
- **`UNWIND_INFO`**: Encodes prologue size, frame register, and an array of `UNWIND_CODE` entries:
  - `UWOP_PUSH_NONVOL`: Non-volatile register pushed to stack.
  - `UWOP_ALLOC_LARGE` / `UWOP_ALLOC_SMALL`: Stack allocation bounds.
  - `UWOP_SAVE_XMM128`: Saved non-volatile 128-bit XMM registers (`xmm6 .. xmm15`).
- **Dynamic Registration**: Tables are registered at runtime via `RtlAddFunctionTable(table, count, baseAddress)`.
