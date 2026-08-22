-- Live Table & Builtin Benchmark
local N = 100000

print("=== RUNNING LIVE BENCHMARK ON REAL LUAU RUNTIME ===")

-- 1. Table creation and field lookups
local t0 = os.clock()
local total = 0
for i = 1, N do
    local t = { x = i, y = i * 2, z = i * 3 }
    total += t.x + t.y + t.z
end
local t1 = os.clock()
print(string.format("1. Table Creation & Lookups (100,000 objects): %.3f ms (checksum=%d)", (t1 - t0) * 1000, total))

-- 2. Pre-sized Table Allocation (table.create)
local t2 = os.clock()
local arr = table.create(N, 1.25)
local t3 = os.clock()
print(string.format("2. table.create (100,000 elements): %.3f ms", (t3 - t2) * 1000))

-- 3. Table Freezing & Status Query (table.freeze / table.isfrozen)
local t4 = os.clock()
local frozen = table.freeze(arr)
local isf = table.isfrozen(frozen)
local t5 = os.clock()
print(string.format("3. table.freeze & isfrozen (100,000 elements): %.4f ms (isFrozen=%s)", (t5 - t4) * 1000, tostring(isf)))

-- 4. Table Native Cloning (table.clone)
local t6 = os.clock()
local cloned = table.clone(frozen)
local t7 = os.clock()
print(string.format("4. table.clone (100,000 elements): %.3f ms", (t7 - t6) * 1000))

-- 5. Table Vectorized Clear (table.clear)
local t8 = os.clock()
table.clear(cloned)
local t9 = os.clock()
print(string.format("5. table.clear (100,000 elements): %.3f ms", (t9 - t8) * 1000))

-- 6. Table Linear Search (table.find)
local searchTarget = 1.25
local t10 = os.clock()
local foundIdx = table.find(arr, searchTarget)
local t11 = os.clock()
print(string.format("6. table.find in array (100,000 elements): %.4f ms (found at index=%s)", (t11 - t10) * 1000, tostring(foundIdx)))

print("=== ALL LIVE BENCHMARKS EXECUTED SUCCESSFULLY ===")
