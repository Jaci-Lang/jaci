// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#pragma once

#include "lua.h"

#include <cstdint>

// Suspend the current coroutine until a descriptor becomes readable.
int lua_task_wait_readable(lua_State* L, uintptr_t descriptor, double timeoutSeconds = 0.0);
