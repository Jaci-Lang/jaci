--!optimize 2
-- Jaci Task & Async Reactor Performance Benchmark Suite
-- Measures raw throughput of native task runtime, CSP channels, promises, and native await

local ITERATIONS = 500_000

local function bench(name: string, fn: () -> ())
    local t0 = os.clock()
    fn()
    local elapsed = os.clock() - t0
    local rate = ITERATIONS / elapsed
    print(string.format("  [%-30s] Elapsed: %6.2f ms | Rate: %10.0f ops/sec", name, elapsed * 1000.0, rate))
end

print("================================================================================")
print("             Jaci Native Task & Async Reactor Benchmarks (O2)")
print("================================================================================")

-- 1. task.spawn immediate coroutine throughput
bench("task.spawn (immediate)", function()
    local count = 0
    for i = 1, ITERATIONS do
        task.spawn(function()
            count += 1
        end)
    end
    assert(count == ITERATIONS, "spawn count mismatch")
end)

-- 2. task.defer microtask queue throughput
bench("task.defer (microtask queue)", function()
    local count = 0
    for i = 1, ITERATIONS do
        task.defer(function()
            count += 1
        end)
    end
    task.run()
    assert(count == ITERATIONS, "defer count mismatch")
end)

-- 3. Promise creation and synchronous resolution
bench("task.resolve (Promise)", function()
    local sum = 0
    for i = 1, ITERATIONS do
        local p = task.resolve(i)
        sum += p:value()
    end
    assert(sum > 0, "sum mismatch")
end)

-- 4. Native await on resolved Promise
bench("await <Promise>", function()
    local sum = 0
    for i = 1, ITERATIONS do
        local p = task.resolve(i)
        local v = await p
        sum += v
    end
    assert(sum > 0, "sum mismatch")
end)

-- 5. Buffered CSP Channel throughput (send + recv)
bench("Channel:send / Channel:recv (buffered)", function()
    local ch = task.channel(1000)
    local BATCH = 1000
    local batches = ITERATIONS / BATCH
    for b = 1, batches do
        for i = 1, BATCH do
            ch:send(i)
        end
        for i = 1, BATCH do
            local val, ok = ch:recv()
        end
    end
end)

-- 6. Buffered CSP Channel with native await
bench("await Channel (buffered)", function()
    local ch = task.channel(1000)
    local BATCH = 1000
    local batches = ITERATIONS / BATCH
    for b = 1, batches do
        for i = 1, BATCH do
            ch:send(i)
        end
        for i = 1, BATCH do
            local val, ok = await ch
        end
    end
end)

-- 7. Unbuffered Rendezvous Channel Ping-Pong
bench("Channel Rendezvous Ping-Pong", function()
    local COUNT = 100_000
    local ch = task.channel()
    local received = 0

    task.spawn(function()
        for i = 1, COUNT do
            ch:send(i)
        end
        ch:close()
    end)

    task.spawn(function()
        while true do
            local val, ok = await ch
            if not ok then break end
            received += 1
        end
    end)

    task.run()
    assert(received == COUNT, "rendezvous count mismatch")
end)

print("================================================================================")
print("Benchmark suite completed successfully.")
