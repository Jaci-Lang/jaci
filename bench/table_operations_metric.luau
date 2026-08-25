local count = 65536
local rounds = 31

local keys = table.create(count)
for i = 1, count do
    keys[i] = "table-operation-key-" .. i
end

local function median(samples)
    table.sort(samples)
    return samples[math.floor(#samples / 2) + 1]
end

local insertSamples = table.create(rounds)
local updateSamples = table.create(rounds)
local lookupSamples = table.create(rounds)
local removeSamples = table.create(rounds)

for round = 1, rounds + 5 do
    collectgarbage()

    local values = {}
    local started = os.clock()
    for i = 1, count do
        values[keys[i]] = i
    end
    local inserted = os.clock()

    for i = 1, count do
        values[keys[i]] = i + 1
    end
    local updated = os.clock()

    local checksum = 0
    for i = 1, count do
        checksum += values[keys[i]]
    end
    local read = os.clock()

    for i = 1, count do
        values[keys[i]] = nil
    end
    local removed = os.clock()

    assert(checksum == count * (count + 3) / 2)
    assert(next(values) == nil)

    if round > 5 then
        local sample = round - 5
        insertSamples[sample] = (inserted - started) * 1000
        updateSamples[sample] = (updated - inserted) * 1000
        lookupSamples[sample] = (read - updated) * 1000
        removeSamples[sample] = (removed - read) * 1000
    end
end

print(string.format("hash insert: %.3f ms median", median(insertSamples)))
print(string.format("hash update: %.3f ms median", median(updateSamples)))
print(string.format("hash lookup: %.3f ms median", median(lookupSamples)))
print(string.format("hash remove: %.3f ms median", median(removeSamples)))
