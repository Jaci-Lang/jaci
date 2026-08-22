# ADR 0018: Near-Zero Cost Low-Latency Garbage Collector

## Context

Luau uses an incremental tri-color mark-and-sweep garbage collector. Under high-throughput allocation workloads, traditional collectors encounter two primary bottlenecks:
1. **Allocator Churn & Kernel Syscall Overhead**: When GC pages are swept and emptied, releasing memory immediately to the operating system via `free` and reallocating via `malloc` on subsequent demands induces high kernel page faulting, lock contention, and cache pollution.
2. **Atomic Synchronization Latency**: Deferring dirty table rescan work to the indivisible Stop-The-World (STW) atomic phase creates noticeable latency pauses under write-heavy workloads.
3. **Traversal Overhead**: Marking deeply nested structures one slot at a time with full function call dispatch stalls processor execution pipelines.

## Decision

Implement a high-throughput, low-latency GC subsystem in Jaci:

### 1. Zero-Syscall Page Pool (Hot Page Cache)
- Maintain size-segregated free page pools (`pagepool_small` and `pagepool_large` in `global_State`) for 16KB and 32KB pages.
- When sweeping empties a page (`busyBlocks == 0`), recycle the page into the page pool in $O(1)$ time instead of releasing it to libc/OS.
- When allocating a new page, pop from the page pool before querying system memory allocators.
- Completely eliminates OS allocation/deallocation overhead during steady-state GC cycles.

### 2. Fast-Path Vectorized Marking & Cache Prefetching
- Optimize `traversetable` with 4-way loop unrolling and hardware cache prefetching (`__builtin_prefetch`) for table arrays and hash node buckets.
- Inline fast-path checks for non-collectable primitive types (integers, floats, booleans, nil) to skip recursive function calls entirely.
- Inline string, vector, and buffer marking paths.

### 3. Multi-Pass Non-Blocking Propagation
- Enhance `GCSpropagateagain` to perform iterative bounded propagation passes while the mutator executes.
- Drain dirty `grayagain` sets in small incremental slices so that when entering the indivisible atomic phase (`GCSatomic`), outstanding work is near zero ($O(1)$), reducing STW pause times to microseconds.

### 4. Branch-Predictor Optimized Write Barriers
- Decorate `luaC_barrier`, `luaC_barriert`, `luaC_barrierfast`, `luaC_objbarrier`, and `luaC_threadbarrier` with branch hints (`LUAU_UNLIKELY`) to minimize mutator bytecode dispatch penalties.

### 5. Streamlined Vectorized Sweeping
- Unroll page sweep loops with cache prefetching and fast bitwise alive checks.

## Consequences

- Near-zero memory allocation overhead and kernel churn in steady-state garbage collection cycles.
- Sub-millisecond Go-like pause latency during atomic synchronization.
- Full backward compatibility with standard Luau and full conformance with all existing language invariants.
