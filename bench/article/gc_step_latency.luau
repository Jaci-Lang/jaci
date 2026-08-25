local bench = require("../bench_support")

local count = 524288
local root = {}
for i = 1, count do
    root["gc-step-key-" .. i] = { value = i }
end

bench.runCode(function()
    local maximum = 0
    local complete = false
    repeat
        local started = os.clock()
        complete = collectgarbage("step", 1)
        maximum = math.max(maximum, os.clock() - started)
    until complete
    assert(root["gc-step-key-524288"].value == count)
    return maximum
end, "524K strong table: maximum 1 KiB GC step")
