// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/LlvmTable.h"

#include <algorithm>

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

TableSpecializer::TableSpecializer()
{
}

uint32_t TableSpecializer::registerShape(const std::vector<std::string>& propertyNames, ArraySpecialization arraySpec)
{
    uint32_t id = uint32_t(layouts.size());
    TableLayoutDescriptor desc;
    desc.shapeId = id;
    desc.arraySpec = arraySpec;
    desc.propertyCapacity = uint32_t(propertyNames.size());
    desc.arrayCapacity = (arraySpec != ArraySpecialization::Generic) ? 16 : 0;
    desc.hasMetatable = false;

    layouts.push_back(desc);
    shapeProperties[id] = propertyNames;
    return id;
}

const TableLayoutDescriptor* TableSpecializer::getLayout(uint32_t shapeId) const
{
    if (shapeId < layouts.size())
        return &layouts[shapeId];
    return nullptr;
}

PicSite& TableSpecializer::getOrCreatePicSite(uint32_t siteId, const std::string& propertyName)
{
    auto it = picSites.find(siteId);
    if (it != picSites.end())
        return it->second;

    PicSite site;
    site.siteId = siteId;
    site.propertyName = propertyName;
    picSites[siteId] = std::move(site);
    return picSites[siteId];
}

bool TableSpecializer::canBypassMetatable(uint32_t shapeId, const std::string& propertyName) const
{
    auto it = shapeProperties.find(shapeId);
    if (it != shapeProperties.end())
    {
        const auto& props = it->second;
        return std::find(props.begin(), props.end(), propertyName) != props.end();
    }
    return false;
}

uint32_t TableSpecializer::getArrayElementSize(ArraySpecialization spec) const
{
    switch (spec)
    {
    case ArraySpecialization::PackedInt64:
        return 8;
    case ArraySpecialization::PackedDouble:
        return 8;
    case ArraySpecialization::PackedRef:
        return 8;
    case ArraySpecialization::Generic:
    default:
        return 16; // 16 bytes per TValue
    }
}

Type TableSpecializer::getArrayElementType(ArraySpecialization spec) const
{
    switch (spec)
    {
    case ArraySpecialization::PackedInt64:
        return Type(TypeKind::Int64);
    case ArraySpecialization::PackedDouble:
        return Type(TypeKind::Double);
    case ArraySpecialization::PackedRef:
        return Type(TypeKind::Pointer);
    case ArraySpecialization::Generic:
    default:
        return Type(TypeKind::TValue);
    }
}

uint32_t TableSpecializer::registerStaticArray(const std::vector<double>& values, bool isFrozen)
{
    uint32_t id = uint32_t(staticTables.size());
    StaticTableDescriptor desc;
    desc.tableId = id;
    desc.globalSymbol = "@jaci_static_array_" + std::to_string(id);
    desc.isArray = true;
    desc.isFrozen = isFrozen;
    desc.packedDoubles = values;

    for (size_t i = 0; i < values.size(); ++i)
    {
        StaticTableEntry entry;
        entry.intKey = int64_t(i + 1);
        entry.type = Type(TypeKind::Double);
        entry.val.f64 = values[i];
        desc.entries.push_back(std::move(entry));
    }

    staticTables.push_back(std::move(desc));
    return id;
}

uint32_t TableSpecializer::registerStaticIntArray(const std::vector<int64_t>& values, bool isFrozen)
{
    uint32_t id = uint32_t(staticTables.size());
    StaticTableDescriptor desc;
    desc.tableId = id;
    desc.globalSymbol = "@jaci_static_iarray_" + std::to_string(id);
    desc.isArray = true;
    desc.isFrozen = isFrozen;
    desc.packedInt64s = values;

    for (size_t i = 0; i < values.size(); ++i)
    {
        StaticTableEntry entry;
        entry.intKey = int64_t(i + 1);
        entry.type = Type(TypeKind::Int64);
        entry.val.i64 = values[i];
        desc.entries.push_back(std::move(entry));
    }

    staticTables.push_back(std::move(desc));
    return id;
}

uint32_t TableSpecializer::registerStaticDictionary(const std::vector<std::pair<std::string, double>>& properties, bool isFrozen)
{
    uint32_t id = uint32_t(staticTables.size());
    StaticTableDescriptor desc;
    desc.tableId = id;
    desc.globalSymbol = "@jaci_static_dict_" + std::to_string(id);
    desc.isArray = false;
    desc.isFrozen = isFrozen;

    for (size_t i = 0; i < properties.size(); ++i)
    {
        StaticTableEntry entry;
        entry.key = properties[i].first;
        entry.type = Type(TypeKind::Double);
        entry.val.f64 = properties[i].second;
        desc.keyToEntryMap[entry.key] = uint32_t(desc.entries.size());
        desc.entries.push_back(std::move(entry));
    }

    staticTables.push_back(std::move(desc));
    return id;
}

const StaticTableDescriptor* TableSpecializer::getStaticTable(uint32_t tableId) const
{
    if (tableId < staticTables.size())
        return &staticTables[tableId];
    return nullptr;
}

bool TableSpecializer::isStaticTable(uint32_t tableId) const
{
    return tableId < staticTables.size();
}

std::optional<double> TableSpecializer::lookupStaticDouble(uint32_t tableId, const std::string& key) const
{
    const StaticTableDescriptor* desc = getStaticTable(tableId);
    if (!desc || desc->isArray)
        return std::nullopt;

    auto it = desc->keyToEntryMap.find(key);
    if (it != desc->keyToEntryMap.end() && it->second < desc->entries.size())
    {
        return desc->entries[it->second].val.f64;
    }
    return std::nullopt;
}

std::optional<double> TableSpecializer::lookupStaticArrayElement(uint32_t tableId, size_t index) const
{
    const StaticTableDescriptor* desc = getStaticTable(tableId);
    if (!desc || !desc->isArray)
        return std::nullopt;

    if (index < desc->packedDoubles.size())
        return desc->packedDoubles[index];

    return std::nullopt;
}

const std::vector<StaticTableDescriptor>& TableSpecializer::getAllStaticTables() const
{
    return staticTables;
}

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
