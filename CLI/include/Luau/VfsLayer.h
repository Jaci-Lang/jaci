// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Luau
{

// Process-wide overlay of embedded files, consulted by the CLI filesystem
// utilities (FileUtils isFile/isDirectory/readFile) before the real disk.
//
// A single binary registers its payload files here at startup; the require
// navigator (shared with the REPL, luau-analyze and the LSP) then resolves
// modules through the ordinary filesystem checks and finds the embedded files,
// so the binary runs the exact same navigation stack as every other consumer.
// With no registered files the layer is inert (pure disk behavior).
namespace VfsLayer
{

// Register the embedded file set (canonical absolute path -> data).
// Replaces any previously registered set; an empty vector clears it.
void setEmbeddedFiles(std::vector<std::pair<std::string, std::string>> files);

bool hasEmbeddedFiles();

// True when the exact path is an embedded file.
bool isEmbeddedFile(const std::string& path);

// True when the path is a directory containing embedded files.
bool isEmbeddedDirectory(const std::string& path);

// Embedded file contents for the exact path.
std::optional<std::string> readEmbeddedFile(const std::string& path);

} // namespace VfsLayer

} // namespace Luau
