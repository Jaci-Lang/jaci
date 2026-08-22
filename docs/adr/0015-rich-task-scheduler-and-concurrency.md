# ADR 0015: Rich Task Scheduler and Async Concurrency Model

## Context

Standalone Luau applications and native network services require structured concurrency, microtask queues, non-blocking timers, CSP channels, and Promise combinators beyond basic thread resuming.

## Decision

Provide a comprehensive `task` library (`ltasklib.cpp`, `lreactor.cpp`, `lreactor.h`) featuring:
- **Thread & Microtask Scheduling**: `task.spawn`, `task.defer`, `task.delay`, `task.wait`, `task.cancel`, `task.yield`, `task.status`.
- **Promise & Await Combinators**: `task.all`, `task.race`, `task.any`, `task.allSettled`, `task.resolve`, `task.reject`, `task.async`, `task.await`.
- **Communicating Sequential Processes (CSP)**: `task.channel(capacity)` with buffered/unbuffered rendezvous channels, `send`, `recv`, `try_send`, `try_receive`, `close`.
- **Timers**: `task.timer`, `task.every(interval, fn)`.
- **Parallel Safety**: `task.desynchronize`, `task.synchronize`.

## Consequences

- Full asynchronous concurrency without external event loop dependencies.
- Zero-cost coroutine yield/resume integration with VM scheduler.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
