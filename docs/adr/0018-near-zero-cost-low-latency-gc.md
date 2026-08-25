# ADR 0018: Near-Zero Cost Low-Latency Garbage Collector

## Context

Luau uses an incremental tri-color mark-and-sweep collector. Empty-page release and reacquisition adds allocator traffic during repeated churn. Marking leaf objects through the generic object dispatcher also adds avoidable work for string-heavy tables. Marking one table was indivisible, so a 524,288-entry live hash table could turn a 1 KiB GC step into a 16.745 ms pause.

## Decision

Keep Luau's reachability, weak-table, and finalization behavior. Optimize allocation reuse and leaf marking. Traverse large tables through a single collector-owned continuation so one table cannot consume an unbounded incremental step.

### Reuse empty pages

- Maintain size-segregated pools for 16 KiB and 32 KiB pages.
- Reuse a pooled page before calling the host allocator.
- Cap the pools at 128 small pages and 32 large pages. Limit retained physical memory to 3 MiB.
- Release every pooled page when closing the state.

### Mark leaf objects directly

- Mark strings, buffers, and heap vectors directly in `markvalue`.
- Bypass the generic object switch for objects that cannot contain GC edges.
- Preserve the same white-to-gray-to-black transition.

### Bound table propagation

- Keep one table continuation in `global_State`; add no fields to individual tables.
- Charge array and hash slots against the existing byte work budget.
- Keep the table gray until every strong edge is marked.
- Restart the cursor after a table mutation or resize so writes to an already scanned slot cannot hide a white object.
- Re-read the metatable and `__mode` between chunks. Preserve weak-key, weak-value, and fully weak traversal rules when the mode changes.
- Remove an empty weak table from the atomic clear list only after its complete incremental scan proves that it has no entries.
- Specialize atomic weak clearing by mode so strong keys or values are not checked again.

Reject manual table-scan unrolling, cache prefetching, write-barrier branch hints, and extra `grayagain` passes. Measurements showed throughput regressions, and the extra propagation pass allowed the sampled logical heap to grow from roughly 16 MiB to 95 MiB under churn.

Keep dedicated tests for page reuse, incremental barriers, weak tables, every table-array scan position, continuation mutation, resize, and metatable replacement. Run the full Luau conformance suite after changes.

## Consequences

- Avoid host allocation calls while a matching pooled page is available.
- Reduce median time for 400 full collections of a 262,144-entry string table from 83.456 ms to 78.061 ms in the release A/B benchmark.
- Reduce the measured 1 KiB-step maximum for a 524,288-entry strong hash graph from 16.745 ms to 0.026-0.358 ms across three final runs without a metatable; p99 stayed at 0.005-0.010 ms. An ordinary-metatable probe measured 20.800 ms before and 0.028 ms after.
- Reduce the measured steady-state maximum for an emptied 524,288-slot weak-value cache from 3.023 ms to 1.905 ms in the explicit-step probe; individual propagation and sweep work stayed below 0.1 ms, while the public explicit-step call combined multiple sweep pages.
- Retain Luau table semantics and the public API. Add one predictable pointer comparison to table write barriers only while checking for the global continuation.
- Leave a 524,288-entry live weak-value table as the proven architectural limit: its indivisible atomic closure measured 10.992-16.047 ms across four final runs. Splitting that phase safely requires allocation coloring, dirty weak-table tracking, and resumable atomic clearing; do not approximate it with unsafe cursor yielding.
