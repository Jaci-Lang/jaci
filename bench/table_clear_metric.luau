local tableCount = 256
local keyCount = 4096
local rounds = 8

local keys = table.create(keyCount)
for i = 1, keyCount do
    keys[i] = "table-clear-key-" .. i
end

local tables = table.create(tableCount)
for i = 1, tableCount do
    tables[i] = {}
end

local elapsed = 0
for _ = 1, rounds do
    for tableIndex = 1, tableCount do
        local target = tables[tableIndex]
        for keyIndex = 1, keyCount do
            target[keys[keyIndex]] = keyIndex
        end
    end

    local started = os.clock()
    for tableIndex = 1, tableCount do
        table.clear(tables[tableIndex])
    end
    elapsed += os.clock() - started
end

print(string.format("Cleared %d hash slots in %.3f ms", tableCount * keyCount * rounds, elapsed * 1000))
