-- native_loop_bench.luau
--
-- Benchmark + correctness harness for native C/FFI function calls inside
-- every loop form.  The test driver (native_interop_test.cpp) registers the
-- following globals before running this chunk:
--   c_noop0()             -> nil
--   c_id(x)               -> x
--   c_add(a, b)           -> a + b      (C-side number add)
--   c_sum(arr, n)         -> number sum of arr[1..n]
--   c_transform(buf, n)   -> in-place multiply buf[i] by 1.0 + i * 1e-9
--   c_mulret()            -> three values: 1, 2, 3
-- It also exposes install_interrupt(limit) as a global for the correctness
-- test at the bottom.
--
-- Returns a table with:
--   for_ms, while_ms, repeat_ms, ipairs_ms, empty_ms,
--   checksum, interrupt_ok, interrupt_hits, iters_per_sec

local N = tonumber(arg and arg[1]) or (os.getenv and tonumber(os.getenv("NATIVE_LOOP_N"))) or 5000000
local CHECKSUM_MOD = 1000000007

local function bench_native_for()
    -- Numeric for loop, register-pressured body with several locals.
    local a0, a1, a2, a3, a4, a5, a6, a7 = 1, 2, 3, 4, 5, 6, 7, 8
    local sum = 0
    local start = os.clock()
    for i = 1, N do
        local v = c_add(i, a0)
        v = c_add(v, a1); v = c_add(v, a2); v = c_add(v, a3)
        v = c_add(v, a4); v = c_add(v, a5); v = c_add(v, a6); v = c_add(v, a7)
        sum = sum + v
        -- Rotate the live values to force them through registers across
        -- the interrupt-check back-edge.
        a0, a1, a2, a3, a4, a5, a6, a7 = a1, a2, a3, a4, a5, a6, a7, a0
    end
    local elapsed = os.clock() - start
    return elapsed, sum % CHECKSUM_MOD
end

local function bench_native_while()
    local a0, a1, a2, a3, a4, a5, a6, a7 = 1, 2, 3, 4, 5, 6, 7, 8
    local sum = 0
    local i = 0
    local start = os.clock()
    while i < N do
        i = i + 1
        local v = c_add(i, a0)
        v = c_add(v, a1); v = c_add(v, a2); v = c_add(v, a3)
        v = c_add(v, a4); v = c_add(v, a5); v = c_add(v, a6); v = c_add(v, a7)
        sum = sum + v
        a0, a1, a2, a3, a4, a5, a6, a7 = a1, a2, a3, a4, a5, a6, a7, a0
    end
    local elapsed = os.clock() - start
    return elapsed, sum % CHECKSUM_MOD
end

local function bench_native_repeat()
    local a0, a1, a2, a3, a4, a5, a6, a7 = 1, 2, 3, 4, 5, 6, 7, 8
    local sum = 0
    local i = 0
    local start = os.clock()
    repeat
        i = i + 1
        local v = c_add(i, a0)
        v = c_add(v, a1); v = c_add(v, a2); v = c_add(v, a3)
        v = c_add(v, a4); v = c_add(v, a5); v = c_add(v, a6); v = c_add(v, a7)
        sum = sum + v
        a0, a1, a2, a3, a4, a5, a6, a7 = a1, a2, a3, a4, a5, a6, a7, a0
    until i >= N
    local elapsed = os.clock() - start
    return elapsed, sum % CHECKSUM_MOD
end

local function bench_native_ipairs()
    local arr = {}
    for i = 1, N do arr[i] = i end
    local a0, a1, a2, a3, a4, a5, a6, a7 = 1, 2, 3, 4, 5, 6, 7, 8
    local sum = 0
    local start = os.clock()
    for i, v in ipairs(arr) do
        local x = c_add(v, a0)
        x = c_add(x, a1); x = c_add(x, a2); x = c_add(x, a3)
        x = c_add(x, a4); x = c_add(x, a5); x = c_add(x, a6); x = c_add(x, a7)
        sum = sum + x
        a0, a1, a2, a3, a4, a5, a6, a7 = a1, a2, a3, a4, a5, a6, a7, a0
    end
    local elapsed = os.clock() - start
    return elapsed, sum % CHECKSUM_MOD
end

local function bench_empty()
    -- Baseline: pure-luau numeric loop, no C calls, to compute overhead.
    local a0, a1, a2, a3, a4, a5, a6, a7 = 1, 2, 3, 4, 5, 6, 7, 8
    local sum = 0
    local start = os.clock()
    for i = 1, N do
        local v = i + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7
        sum = sum + v
        a0, a1, a2, a3, a4, a5, a6, a7 = a1, a2, a3, a4, a5, a6, a7, a0
    end
    local elapsed = os.clock() - start
    return elapsed, sum % CHECKSUM_MOD
end

-- Expect the pure-luau formula to match the c_add chain for the same inputs.
local function ref_checksum()
    local a0, a1, a2, a3, a4, a5, a6, a7 = 1, 2, 3, 4, 5, 6, 7, 8
    local sum = 0
    for i = 1, N do
        local v = i + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7
        sum = sum + v
        a0, a1, a2, a3, a4, a5, a6, a7 = a1, a2, a3, a4, a5, a6, a7, a0
    end
    return sum % CHECKSUM_MOD
end

local empty_ms, empty_csum = bench_empty()
local for_ms, for_csum     = bench_native_for()
local while_ms, while_csum = bench_native_while()
local repeat_ms, rep_csum  = bench_native_repeat()
local ipairs_ms, ip_csum   = bench_native_ipairs()

local expected = ref_checksum()
local checks_match = (for_csum == expected) and (while_csum == expected)
                 and (rep_csum == expected) and (ip_csum  == expected)

-- Interrupt-correctness subtest.  Use a deliberately-tiny iteration count so
-- that install_interrupt(limit) will deterministically fire inside native
-- CodeGen.  The driver installs a counter that luaL_error()s on reaching the
-- limit; we catch that error and inspect the counter via
-- install_interrupt(0) which returns the observed count.
local interrupt_ok = false
local interrupt_hits = 0
do
    local function loop_with_interrupt()
        local s = 0
        for i = 1, 10000 do
            s = s + c_add(i, 1)
        end
        return s
    end

    install_interrupt(200) -- driver will error on 200th callback
    local ok, err = pcall(loop_with_interrupt)
    local count_after = install_interrupt(0) -- disarm + return hits
    interrupt_hits = count_after
    -- We expect ok == false because luaL_error was raised, and hits >= 200.
    interrupt_ok = (not ok) and interrupt_hits >= 200
end

local iters_per_sec = math.floor(N / (for_ms + 1e-9))

return {
    for_ms         = for_ms,
    while_ms       = while_ms,
    repeat_ms      = repeat_ms,
    ipairs_ms      = ipairs_ms,
    empty_ms       = empty_ms,
    checksum       = expected,
    checks_match   = checks_match,
    interrupt_ok   = interrupt_ok,
    interrupt_hits = interrupt_hits,
    iters_per_sec  = iters_per_sec,
    n              = N,
}
