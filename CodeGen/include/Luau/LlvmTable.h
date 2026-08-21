// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/LlvmData.h"

#include <memory>
#include <string>
#include <vector>

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

class TableSpecializer
{
public:
    TableSpecializer();

    // Register and look up shapes
    uint32_t registerShape(const std::vector<std::string>& propertyNames, ArraySpecialization arraySpec = ArraySpecialization::Generic);
    const TableLayoutDescriptor* getLayout(uint32_t shapeId) const;

    // Polymorphic Inline Cache (PIC) management
    PicSite& getOrCreatePicSite(uint32_t siteId, const std::string& propertyName);

    // Checks whether direct fast path can bypass metatables when a key is proven to exist
    bool canBypassMetatable(uint32_t shapeId, const std::string& propertyName) const;

    // Determine array element stride and representation
    uint32_t getArrayElementSize(ArraySpecialization spec) const;
    Type getArrayElementType(ArraySpecialization spec) const;

private:
    std::vector<TableLayoutDescriptor> layouts;
    std::unordered_map<uint32_t, PicSite> picSites;
    std::unordered_map<uint32_t, std::vector<std::string>> shapeProperties;
};

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
