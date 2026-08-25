local bench = require("../bench_support")

local count = 65536
local keys = table.create(count)
for i = 1, count do
    keys[i] = "table-operation-key-" .. i
end

local function populated()
    local values = {}
    for i = 1, count do
        values[keys[i]] = i
    end
    return values
end

bench.runCode(function()
    local values = {}
    for i = 1, count do
        values[keys[i]] = i
    end
    assert(values[keys[count]] == count)
end, "Hash insert: 65,536 keys")

bench.runCode(function()
    local values = populated()
    local started = os.clock()
    for i = 1, count do
        values[keys[i]] = i + 1
    end
    assert(values[keys[count]] == count + 1)
    return os.clock() - started
end, "Hash update: 65,536 keys")

bench.runCode(function()
    local values = populated()
    local started = os.clock()
    table.clear(values)
    assert(next(values) == nil)
    return os.clock() - started
end, "table.clear: 65,536 hash keys")
