# ADR 0018: Near-Zero Cost Low-Latency Garbage Collector

## Context

Luau uses an incremental tri-color mark-and-sweep collector. Empty-page release and reacquisition adds allocator traffic during repeated churn. Marking leaf objects through the generic object dispatcher also adds avoidable work for string-heavy tables.

## Decision

Keep Luau's collection schedule and write barriers unchanged. Optimize allocation reuse and leaf marking without changing reachability, weak-table, or finalization behavior.

### Reuse empty pages

- Maintain size-segregated pools for 16 KiB and 32 KiB pages.
- Reuse a pooled page before calling the host allocator.
- Cap the pools at 128 small pages and 32 large pages. Limit retained physical memory to 3 MiB.
- Release every pooled page when closing the state.

### Mark leaf objects directly

- Mark strings, buffers, and heap vectors directly in `markvalue`.
- Bypass the generic object switch for objects that cannot contain GC edges.
- Preserve the same white-to-gray-to-black transition.

Reject manual table-scan unrolling, cache prefetching, write-barrier branch hints, and extra `grayagain` passes. Measurements showed throughput regressions, and the extra propagation pass allowed the sampled logical heap to grow from roughly 16 MiB to 95 MiB under churn.

Keep dedicated tests for page reuse, incremental barriers, weak tables, and every table-array scan position. Run the full Luau conformance suite after changes.

## Consequences

- Avoid host allocation calls while a matching pooled page is available.
- Reduce median time for 400 full collections of a 262,144-entry string table from 83.456 ms to 78.061 ms in the release A/B benchmark.
- Retain Luau's GC schedule, write-barrier behavior, table semantics, and public API.
