-- This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
-- Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.

-- Literal word hyphens remain literal without escaping.
assert(string.match("a-b", "a-b") == "a-b")
assert(string.match("ab-c", "ab-c") == "ab-c")
assert(string.match("Sec-WebSocket-Accept", "Sec-WebSocket-Accept") == "Sec-WebSocket-Accept")
assert(string.match("Sec-WebSocket-Accept", "Sec%-WebSocket%-Accept") == "Sec-WebSocket-Accept")

-- Preserve minimum repetition when '-' is not between literal word characters.
assert(string.match("aaa", "a-") == "")

return "OK"
