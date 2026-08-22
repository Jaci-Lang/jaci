// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/Common.h"
#include "Luau/Repl.h"
#include "lua.h"
#include "lualib.h"
#include "doctest.h"
#include <memory>
#include <string>

TEST_SUITE_BEGIN("CryptoTests");

TEST_CASE("Sha224AndSha384AndSha512")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    // Test SHA-224 of "abc" -> 23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7
    const char* script224 = "assert(crypto.sha224hex('abc') == '23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7')";
    std::string err = runCode(L, script224);
    CHECK(err.empty());

    // Test SHA-384 of "abc" -> cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7
    const char* script384 = "assert(crypto.sha384hex('abc') == 'cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7')";
    err = runCode(L, script384);
    CHECK(err.empty());

    // Test SHA-512 of "abc" -> ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f
    const char* script512 = "assert(crypto.sha512hex('abc') == 'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f')";
    err = runCode(L, script512);
    CHECK(err.empty());
}

TEST_CASE("HmacSha512")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = "local hmac = crypto.hmac_sha512hex('secret_key', 'hello world message'); assert(#hmac == 128)";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("CsprngRandomBytesAndTimingSafeEqual")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local r1 = crypto.randomBytes(32)
        local r2 = crypto.randomBytes(32)
        assert(#r1 == 32 and #r2 == 32)
        assert(not crypto.timingSafeEqual(r1, r2))
        assert(crypto.timingSafeEqual(r1, r1))
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_CASE("ChaCha20Cipher")
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();
    setupState(L);

    const char* script = R"(
        local key = string.rep("K", 32)
        local nonce = string.rep("N", 12)
        local plaintext = "TopSecretMessage1234567890!@#$%^"
        local encrypted = crypto.chacha20(key, nonce, plaintext, 1)
        local decrypted = crypto.chacha20(key, nonce, encrypted, 1)
        assert(encrypted ~= plaintext)
        assert(decrypted == plaintext)
    )";
    std::string err = runCode(L, script);
    CHECK(err.empty());
}

TEST_SUITE_END();
