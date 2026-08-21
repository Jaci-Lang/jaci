// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <memory>
#include <string>

class HashFixture
{
public:
    HashFixture()
        : state(luaL_newstate(), lua_close)
    {
        L = state.get();
        luaL_openlibs(L);
    }

    std::string run(const std::string& code)
    {
        std::string bytecode = Luau::compile(code);
        if (luau_load(L, "=test", bytecode.data(), bytecode.size(), 0) != 0)
        {
            std::string err = lua_tostring(L, -1);
            lua_pop(L, 1);
            return err;
        }

        int status = lua_pcall(L, 0, 0, 0);
        if (status != 0)
        {
            std::string err = lua_tostring(L, -1);
            lua_pop(L, 1);
            return err;
        }

        return "";
    }

    lua_State* L;

private:
    std::unique_ptr<lua_State, void (*)(lua_State*)> state;
};

TEST_SUITE_BEGIN("HashTests");

TEST_CASE_FIXTURE(HashFixture, "HashVectors")
{
    std::string err = run(R"(
        assert(hash.md5hex("") == "d41d8cd98f00b204e9800998ecf8427e")
        assert(hash.sha1hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709")
        assert(hash.sha256hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")

        assert(hash.crc32("") == 0)
        assert(hash.crc32("123456789") == 0xCBF43926)

        assert(hash.sha256hex("hello") == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824")

        -- Buffer support
        local b = buffer.create(5)
        buffer.writestring(b, 0, "hello")
        assert(hash.sha256hex(b) == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(HashFixture, "HmacAndBase64")
{
    std::string err = run(R"(
        -- HMAC-SHA256 test vector (RFC 4231 Test Case 2: key="Jefe", data="what do ya want for nothing?")
        local hmac256 = hash.hmac_sha256hex("Jefe", "what do ya want for nothing?")
        assert(hmac256 == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

        -- HMAC-MD5 test vector (RFC 2202 Test Case 2: key="Jefe", data="what do ya want for nothing?")
        local hmac_md5 = hash.hmac_md5hex("Jefe", "what do ya want for nothing?")
        assert(hmac_md5 == "750c783e6ab0b503eaa86e310a5db738")

        -- Base64 encode & decode
        local b64 = hash.base64_encode("Hello Jaci Runtime!")
        assert(b64 == "SGVsbG8gSmFjaSBSdW50aW1lIQ==")
        assert(hash.base64_decode(b64) == "Hello Jaci Runtime!")

        -- Hex encode & decode
        local hex = hash.hex_encode("Jaci")
        assert(hex == "4a616369" or hex == "4A616369")
        assert(hash.hex_decode("4a616369") == "Jaci")
    )");
    CHECK(err == "");
}

TEST_SUITE_END();
