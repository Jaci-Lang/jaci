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

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
