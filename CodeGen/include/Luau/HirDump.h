// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/HirData.h"

#include <string>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

const char* getCmdName(Cmd cmd);
const char* getTypeName(TypeKind kind);
const char* getArrayStorageName(ArrayStorageKind kind);

std::string toString(const Function& function);
std::string toString(const Block& block, const Function& function);
std::string toString(const Inst& inst, const Function& function);
std::string toString(const Value& value, const Function& function);

void dump(const Function& function);

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
