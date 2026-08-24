local function measure_gc_profile(name, n_objects, churn_loops)
    print("==================================================")
    print("Benchmark: " .. name)
    print(string.format("Config: %d active objects, %d mutation cycles", n_objects, churn_loops))
    
    local root = {}
    local gc_start_mem = gcinfo()
    
    -- Warmup & initial allocation
    for i = 1, n_objects do
        root[i] = { id = i, val = i * 1.5, tag = "item_" .. (i % 100) }
    end
    
    local max_batch_us = 0
    local total_batch_us = 0
    local samples = 0
    local t0 = os.clock()
    
    for cycle = 1, churn_loops do
        local p0 = os.clock()
        -- Mutate and generate garbage
        for i = 1, 1000 do
            local idx = (i * 31) % n_objects + 1
            root[idx] = { id = idx, val = (cycle * 1000 + i) * 1.5, tag = "item_" .. (i % 100) }
        end
        local p1 = os.clock()
        local batch_us = (p1 - p0) * 1e6
        if batch_us > max_batch_us then max_batch_us = batch_us end
        total_batch_us = total_batch_us + batch_us
        samples = samples + 1
    end
    
    local total_time_ms = (os.clock() - t0) * 1000
    local avg_batch_us = total_batch_us / samples
    local total_allocs = churn_loops * 1000
    local throughput_m_allocs_sec = (total_allocs / (total_time_ms / 1000)) / 1e6

    print(string.format("Throughput:            %.2f Million allocs/sec", throughput_m_allocs_sec))
    print(string.format("Total Runtime:         %.2f ms", total_time_ms))
    print(string.format("Average Batch Time:    %.2f µs (per 1K allocations)", avg_batch_us))
    print(string.format("Worst Batch Time:      %.2f µs", max_batch_us))
    print(string.format("Memory Baseline:       %d KB -> End: %d KB", gc_start_mem, gcinfo()))
end

measure_gc_profile("Heavy High-Churn Mutation & Sweep", 50000, 500)
measure_gc_profile("Dense Tree & Map Traversals", 100000, 200)
