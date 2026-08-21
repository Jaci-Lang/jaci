// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/LlvmBuilder.h"
#include "Luau/LlvmTable.h"
#include "Luau/MirData.h"

struct Proto;

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

class LlvmLowering
{
public:
    explicit LlvmLowering(LlvmBuilder& builder, TableSpecializer& tableSpecializer);

    bool lowerFunction(const Mir::Function& mirFunction, Proto* proto, const std::string& functionName = "jaci_entry");

private:
    LlvmBuilder& builder;
    TableSpecializer& tableSpecializer;

    std::unordered_map<uint32_t, LlvmValue> mirValueToLlvmMap;
    std::unordered_map<uint32_t, std::string> mirBlockToBlockNameMap;

    LlvmValue resolveMirValue(const Mir::Function& mirFunction, const Mir::Value& val);
    Type mapMirTypeToLlvmType(const Mir::Type& mtype);
};

bool lowerMirToLlvm(LlvmBuilder& builder, TableSpecializer& tableSpecializer, const Mir::Function& mirFunction, Proto* proto);

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
