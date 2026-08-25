local function median(samples)
    table.sort(samples)
    return samples[math.floor(#samples / 2) + 1]
end

local function measure(name, root, rounds)
    collectgarbage()

    for _ = 1, 10 do
        collectgarbage()
    end

    local samples = table.create(rounds)
    for round = 1, rounds do
        local started = os.clock()
        collectgarbage()
        samples[round] = (os.clock() - started) * 1000
    end

    print(string.format("%s: %.3f ms median", name, median(samples)))
    assert(root)
end

local function measureStrong()
    local root = {}
    for i = 1, 262144 do
        root["strong-key-" .. i] = i
    end
    measure("strong hash table", root, 101)
end

local function measureStrongWithMetatable()
    local root = setmetatable({}, {})
    for i = 1, 262144 do
        root["metatable-key-" .. i] = i
    end
    measure("strong hash table with metatable", root, 101)
end

local function measureWeakValues()
    local weakValues = setmetatable({}, { __mode = "v" })
    local roots = table.create(262144)
    for i = 1, 262144 do
        local value = { i }
        roots[i] = value
        weakValues["weak-key-" .. i] = value
    end
    measure("weak-value hash table", { weakValues, roots }, 101)
end

measureStrong()
collectgarbage()
measureStrongWithMetatable()
collectgarbage()
measureWeakValues()
