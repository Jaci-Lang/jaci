// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "lua.h"

#include <functional>
#include <cstddef>
#include <string>
#include <vector>

using AddCompletionCallback = std::function<void(const std::string& completion, const std::string& display)>;

enum class ReplHighlightKind
{
    Keyword,
    String,
    Number,
    Comment,
    Function,
    Builtin,
    Type,
    Operator,
    Attribute,
    Error,
};

struct ReplHighlightSpan
{
    size_t start;
    size_t length;
    ReplHighlightKind kind;
};

// Note: These are internal functions which are being exposed in a header
// so they can be included by unit tests.
void* createCliRequireContext(lua_State* L);
void setupState(lua_State* L);
std::string runCode(lua_State* L, const std::string& source);
void getCompletions(lua_State* L, const std::string& editBuffer, const AddCompletionCallback& addCompletionCallback);
std::vector<ReplHighlightSpan> getReplHighlightSpans(const std::string& source);
void setReplDebugLevel(int level);
int getReplDebugLevel();

int replMain(int argc, char** argv);
