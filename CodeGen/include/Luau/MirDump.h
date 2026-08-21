// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/MirData.h"

#include <string>

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

const char* getCmdName(Cmd cmd);
const char* getTypeName(TypeKind kind);
const char* getLocationClassName(LocationClass lc);

std::string toString(const Function& function);
std::string toString(const Block& block, const Function& function);
std::string toString(const Inst& inst, const Function& function);
std::string toString(const Value& value, const Function& function);

void dump(const Function& function);

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
