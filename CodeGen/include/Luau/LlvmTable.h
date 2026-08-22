// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/LlvmData.h"

#include <memory>
#include <optional>
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

    // Static Data Promotion in Assembly / JIT / AOT (Direct zero-overhead static data)
    uint32_t registerStaticArray(const std::vector<double>& values, bool isFrozen = true);
    uint32_t registerStaticIntArray(const std::vector<int64_t>& values, bool isFrozen = true);
    uint32_t registerStaticDictionary(const std::vector<std::pair<std::string, double>>& properties, bool isFrozen = true);
    const StaticTableDescriptor* getStaticTable(uint32_t tableId) const;
    bool isStaticTable(uint32_t tableId) const;
    std::optional<double> lookupStaticDouble(uint32_t tableId, const std::string& key) const;
    std::optional<double> lookupStaticArrayElement(uint32_t tableId, size_t index) const;
    const std::vector<StaticTableDescriptor>& getAllStaticTables() const;

private:
    std::vector<TableLayoutDescriptor> layouts;
    std::unordered_map<uint32_t, PicSite> picSites;
    std::unordered_map<uint32_t, std::vector<std::string>> shapeProperties;
    std::vector<StaticTableDescriptor> staticTables;
};

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
