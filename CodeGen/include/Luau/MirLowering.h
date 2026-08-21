// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/IrBuilder.h"
#include "Luau/MirData.h"

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

class MirLowering
{
public:
    explicit MirLowering(IrBuilder& irBuilder);

    bool lowerFunction(const Function& mirFunction, Proto* proto);

private:
    IrBuilder& ir;
    std::unordered_map<uint32_t, IrOp> mirValueToIrOpMap;
    std::unordered_map<uint32_t, IrOp> mirBlockToIrBlockMap;

    IrOp resolveMirValue(const Function& mirFunction, const Value& val);
};

bool lowerMirToIr(IrBuilder& irBuilder, const Function& mirFunction, Proto* proto);

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
