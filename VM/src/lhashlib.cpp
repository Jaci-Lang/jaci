// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <vector>

#define LUA_HASHLIBNAME "hash"

static const char* get_hash_data(lua_State* L, int idx, size_t* len)
{
    if (lua_type(L, idx) == LUA_TSTRING)
    {
        return lua_tolstring(L, idx, len);
    }
    else if (lua_isbuffer(L, idx))
    {
        return (const char*)luaL_checkbuffer(L, idx, len);
    }
    else
    {
        luaL_typeerror(L, idx, "string or buffer");
        return NULL;
    }
}

static void bytes_to_hex(const uint8_t* bytes, size_t len, char* hex)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i)
    {
        hex[i * 2] = hex_chars[bytes[i] >> 4];
        hex[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
}

static int hex_char_to_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// CRC-32 (ISO-HDLC)
static int hash_crc32(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= (uint8_t)data[i];
        for (int j = 0; j < 8; ++j)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    crc ^= 0xFFFFFFFF;

    lua_pushnumber(L, (double)crc);
    return 1;
}

// FNV-1a (64-bit)
static int hash_fnv1a(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);

    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i)
    {
        hash ^= (uint8_t)data[i];
        hash *= 0x100000001b3ULL;
    }

    lua_pushnumber(L, (double)hash);
    return 1;
}

// MD5
#define F(x,y,z) ((x & y) | (~x & z))
#define G(x,y,z) ((x & z) | (y & ~z))
#define H(x,y,z) (x ^ y ^ z)
#define I(x,y,z) (y ^ (x | ~z))
#define ROTL32(x,n) (((x) << (n)) | ((x) >> (32 - (n))))

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    for (int i = 0; i < 16; ++i)
        x[i] = ((uint32_t)block[i*4]) | (((uint32_t)block[i*4+1]) << 8) | (((uint32_t)block[i*4+2]) << 16) | (((uint32_t)block[i*4+3]) << 24);

#define FF(a,b,c,d,m,s,t) a = b + ROTL32(a + F(b,c,d) + x[m] + t, s)
#define GG(a,b,c,d,m,s,t) a = b + ROTL32(a + G(b,c,d) + x[m] + t, s)
#define HH(a,b,c,d,m,s,t) a = b + ROTL32(a + H(b,c,d) + x[m] + t, s)
#define II(a,b,c,d,m,s,t) a = b + ROTL32(a + I(b,c,d) + x[m] + t, s)

    FF(a, b, c, d,  0,  7, 0xd76aa478);
    FF(d, a, b, c,  1, 12, 0xe8c7b756);
    FF(c, d, a, b,  2, 17, 0x242070db);
    FF(b, c, d, a,  3, 22, 0xc1bdceee);
    FF(a, b, c, d,  4,  7, 0xf57c0faf);
    FF(d, a, b, c,  5, 12, 0x4787c62a);
    FF(c, d, a, b,  6, 17, 0xa8304613);
    FF(b, c, d, a,  7, 22, 0xfd469501);
    FF(a, b, c, d,  8,  7, 0x698098d8);
    FF(d, a, b, c,  9, 12, 0x8b44f7af);
    FF(c, d, a, b, 10, 17, 0xffff5bb1);
    FF(b, c, d, a, 11, 22, 0x895cd7be);
    FF(a, b, c, d, 12,  7, 0x6b901122);
    FF(d, a, b, c, 13, 12, 0xfd987193);
    FF(c, d, a, b, 14, 17, 0xa679438e);
    FF(b, c, d, a, 15, 22, 0x49b40821);

    GG(a, b, c, d,  1,  5, 0xf61e2562);
    GG(d, a, b, c,  6,  9, 0xc040b340);
    GG(c, d, a, b, 11, 14, 0x265e5a51);
    GG(b, c, d, a,  0, 20, 0xe9b6c7aa);
    GG(a, b, c, d,  5,  5, 0xd62f105d);
    GG(d, a, b, c, 10,  9, 0x02441453);
    GG(c, d, a, b, 15, 14, 0xd8a1e681);
    GG(b, c, d, a,  4, 20, 0xe7d3fbc8);
    GG(a, b, c, d,  9,  5, 0x21e1cde6);
    GG(d, a, b, c, 14,  9, 0xc33707d6);
    GG(c, d, a, b,  3, 14, 0xf4d50d87);
    GG(b, c, d, a,  8, 20, 0x455a14ed);
    GG(a, b, c, d, 13,  5, 0xa9e3e905);
    GG(d, a, b, c,  2,  9, 0xfcefa3f8);
    GG(c, d, a, b,  7, 14, 0x676f02d9);
    GG(b, c, d, a, 12, 20, 0x8d2a4c8a);

    HH(a, b, c, d,  5,  4, 0xfffa3942);
    HH(d, a, b, c,  8, 11, 0x8771f681);
    HH(c, d, a, b, 11, 16, 0x6d9d6122);
    HH(b, c, d, a, 14, 23, 0xfde5380c);
    HH(a, b, c, d,  1,  4, 0xa4beea44);
    HH(d, a, b, c,  4, 11, 0x4bdecfa9);
    HH(c, d, a, b,  7, 16, 0xf6bb4b60);
    HH(b, c, d, a, 10, 23, 0xbebfbc70);
    HH(a, b, c, d, 13,  4, 0x289b7ec6);
    HH(d, a, b, c,  0, 11, 0xeaa127fa);
    HH(c, d, a, b,  3, 16, 0xd4ef3085);
    HH(b, c, d, a,  6, 23, 0x04881d05);
    HH(a, b, c, d,  9,  4, 0xd9d4d039);
    HH(d, a, b, c, 12, 11, 0xe6db99e5);
    HH(c, d, a, b, 15, 16, 0x1fa27cf8);
    HH(b, c, d, a,  2, 23, 0xc4ac5665);

    II(a, b, c, d,  0,  6, 0xf4292244);
    II(d, a, b, c,  7, 10, 0x432aff97);
    II(c, d, a, b, 14, 15, 0xab9423a7);
    II(b, c, d, a,  5, 21, 0xfc93a039);
    II(a, b, c, d, 12,  6, 0x655b59c3);
    II(d, a, b, c,  3, 10, 0x8f0ccc92);
    II(c, d, a, b, 10, 15, 0xffeff47d);
    II(b, c, d, a,  1, 21, 0x85845dd1);
    II(a, b, c, d,  8,  6, 0x6fa87e4f);
    II(d, a, b, c, 15, 10, 0xfe2ce6e0);
    II(c, d, a, b,  6, 15, 0xa3014314);
    II(b, c, d, a, 13, 21, 0x4e0811a1);
    II(a, b, c, d,  4,  6, 0xf7537e82);
    II(d, a, b, c, 11, 10, 0xbd3af235);
    II(c, d, a, b,  2, 15, 0x2ad7d2bb);
    II(b, c, d, a,  9, 21, 0xeb86d391);

#undef FF
#undef GG
#undef HH
#undef II

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void compute_md5(const char* data, size_t len, uint8_t out[16])
{
    uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    uint8_t buffer[64];
    size_t offset = 0;

    for (size_t i = 0; i < len; ++i)
    {
        buffer[offset++] = data[i];
        if (offset == 64)
        {
            md5_transform(state, buffer);
            offset = 0;
        }
    }

    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        md5_transform(state, buffer);
        offset = 0;
    }

    while (offset < 56) buffer[offset++] = 0;

    uint64_t bits = len * 8;
    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (bits >> (i * 8)) & 0xFF;

    md5_transform(state, buffer);

    for (int i = 0; i < 4; ++i)
    {
        out[i*4]   = state[i] & 0xFF;
        out[i*4+1] = (state[i] >> 8) & 0xFF;
        out[i*4+2] = (state[i] >> 16) & 0xFF;
        out[i*4+3] = (state[i] >> 24) & 0xFF;
    }
}

static int hash_md5(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[16];
    compute_md5(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 16);
    return 1;
}

static int hash_md5hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[16];
    compute_md5(data, len, digest);
    char hex[32];
    bytes_to_hex(digest, 16, hex);
    lua_pushlstring(L, hex, 32);
    return 1;
}

// SHA-1
static void sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t w[80], a, b, c, d, e, t;

    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)buffer[i*4] << 24) | ((uint32_t)buffer[i*4+1] << 16) | ((uint32_t)buffer[i*4+2] << 8) | ((uint32_t)buffer[i*4+3]);

    for (int i = 16; i < 80; ++i)
        w[i] = ROTL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (int i = 0; i < 80; ++i)
    {
        if (i < 20)      t = ((b & c) | (~b & d)) + 0x5A827999;
        else if (i < 40) t = (b ^ c ^ d) + 0x6ED9EBA1;
        else if (i < 60) t = ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDC;
        else             t = (b ^ c ^ d) + 0xCA62C1D6;

        t += ROTL32(a, 5) + e + w[i];
        e = d; d = c; c = ROTL32(b, 30); b = a; a = t;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void compute_sha1(const char* data, size_t len, uint8_t out[20])
{
    uint32_t state[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buffer[64];
    size_t offset = 0;

    for (size_t i = 0; i < len; ++i)
    {
        buffer[offset++] = data[i];
        if (offset == 64)
        {
            sha1_transform(state, buffer);
            offset = 0;
        }
    }

    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        sha1_transform(state, buffer);
        offset = 0;
    }

    while (offset < 56) buffer[offset++] = 0;

    uint64_t bits = len * 8;
    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (bits >> ((7 - i) * 8)) & 0xFF;

    sha1_transform(state, buffer);

    for (int i = 0; i < 5; ++i)
    {
        out[i*4]   = (state[i] >> 24) & 0xFF;
        out[i*4+1] = (state[i] >> 16) & 0xFF;
        out[i*4+2] = (state[i] >> 8) & 0xFF;
        out[i*4+3] = state[i] & 0xFF;
    }
}

static int hash_sha1(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[20];
    compute_sha1(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 20);
    return 1;
}

static int hash_sha1hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[20];
    compute_sha1(data, len, digest);
    char hex[40];
    bytes_to_hex(digest, 20, hex);
    lua_pushlstring(L, hex, 40);
    return 1;
}

// SHA-256
#define ROTR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR32(x,2) ^ ROTR32(x,13) ^ ROTR32(x,22))
#define EP1(x) (ROTR32(x,6) ^ ROTR32(x,11) ^ ROTR32(x,25))
#define SIG0(x) (ROTR32(x,7) ^ ROTR32(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR32(x,17) ^ ROTR32(x,19) ^ ((x) >> 10))

static const uint32_t k_sha256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(uint32_t state[8], const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, i, t1, t2, m[64];

    for (i = 0; i < 16; ++i)
        m[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4+1] << 16) | ((uint32_t)data[i*4+2] << 8) | ((uint32_t)data[i*4+3]);

    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; ++i)
    {
        t1 = h + EP1(e) + CH(e, f, g) + k_sha256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void compute_sha256(const char* data, size_t len, uint8_t out[32])
{
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t buffer[64];
    size_t offset = 0;

    for (size_t i = 0; i < len; ++i)
    {
        buffer[offset++] = data[i];
        if (offset == 64)
        {
            sha256_transform(state, buffer);
            offset = 0;
        }
    }

    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        sha256_transform(state, buffer);
        offset = 0;
    }

    while (offset < 56) buffer[offset++] = 0;

    uint64_t bits = len * 8;
    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (bits >> ((7 - i) * 8)) & 0xFF;

    sha256_transform(state, buffer);

    for (int i = 0; i < 8; ++i)
    {
        out[i*4]   = (state[i] >> 24) & 0xFF;
        out[i*4+1] = (state[i] >> 16) & 0xFF;
        out[i*4+2] = (state[i] >> 8) & 0xFF;
        out[i*4+3] = state[i] & 0xFF;
    }
}

static int hash_sha256(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[32];
    compute_sha256(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 32);
    return 1;
}

static int hash_sha256hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[32];
    compute_sha256(data, len, digest);
    char hex[64];
    bytes_to_hex(digest, 32, hex);
    lua_pushlstring(L, hex, 64);
    return 1;
}

// Generic HMAC helper (block size = 64 bytes for MD5, SHA-1, SHA-256)
static void compute_hmac(
    const char* key, size_t key_len,
    const char* data, size_t data_len,
    void (*hash_fn)(const char*, size_t, uint8_t*),
    size_t hash_len,
    uint8_t* out
)
{
    uint8_t k_pad[64];
    memset(k_pad, 0, sizeof(k_pad));

    if (key_len > 64)
    {
        hash_fn(key, key_len, k_pad);
    }
    else
    {
        memcpy(k_pad, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; ++i)
    {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5C;
    }

    std::vector<char> inner;
    inner.reserve(64 + data_len);
    inner.insert(inner.end(), ipad, ipad + 64);
    inner.insert(inner.end(), data, data + data_len);

    std::vector<uint8_t> inner_hash(hash_len);
    hash_fn(inner.data(), inner.size(), inner_hash.data());

    std::vector<char> outer;
    outer.reserve(64 + hash_len);
    outer.insert(outer.end(), opad, opad + 64);
    outer.insert(outer.end(), (const char*)inner_hash.data(), (const char*)inner_hash.data() + hash_len);

    hash_fn(outer.data(), outer.size(), out);
}

static int hash_hmac_sha256(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[32];
    compute_hmac(key, key_len, data, data_len, compute_sha256, 32, out);
    lua_pushlstring(L, (const char*)out, 32);
    return 1;
}

static int hash_hmac_sha256hex(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[32];
    compute_hmac(key, key_len, data, data_len, compute_sha256, 32, out);
    char hex[64];
    bytes_to_hex(out, 32, hex);
    lua_pushlstring(L, hex, 64);
    return 1;
}

static int hash_hmac_sha1(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[20];
    compute_hmac(key, key_len, data, data_len, compute_sha1, 20, out);
    lua_pushlstring(L, (const char*)out, 20);
    return 1;
}

static int hash_hmac_sha1hex(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[20];
    compute_hmac(key, key_len, data, data_len, compute_sha1, 20, out);
    char hex[40];
    bytes_to_hex(out, 20, hex);
    lua_pushlstring(L, hex, 40);
    return 1;
}

static int hash_hmac_md5(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[16];
    compute_hmac(key, key_len, data, data_len, compute_md5, 16, out);
    lua_pushlstring(L, (const char*)out, 16);
    return 1;
}

static int hash_hmac_md5hex(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[16];
    compute_hmac(key, key_len, data, data_len, compute_md5, 16, out);
    char hex[32];
    bytes_to_hex(out, 16, hex);
    lua_pushlstring(L, hex, 32);
    return 1;
}

// Base64 encoding & decoding
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int hash_base64_encode(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t octet_a = i < len ? (unsigned char)data[i] : 0;
        uint32_t octet_b = (i + 1) < len ? (unsigned char)data[i + 1] : 0;
        uint32_t octet_c = (i + 2) < len ? (unsigned char)data[i + 2] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(b64_table[(triple >> 18) & 0x3F]);
        out.push_back(b64_table[(triple >> 12) & 0x3F]);
        out.push_back((i + 1) < len ? b64_table[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2) < len ? b64_table[triple & 0x3F] : '=');
    }

    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int hash_base64_decode(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);

    static int8_t dtable[256];
    static bool dtable_inited = false;
    if (!dtable_inited)
    {
        memset(dtable, -1, sizeof(dtable));
        for (int i = 0; i < 64; ++i)
            dtable[(unsigned char)b64_table[i]] = (int8_t)i;
        dtable_inited = true;
    }

    std::string out;
    out.reserve((len / 4) * 3);

    uint32_t buffer = 0;
    int bits_collected = 0;

    for (size_t i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)data[i];
        if (c == '=') break;
        if (dtable[c] < 0) continue;

        buffer = (buffer << 6) | dtable[c];
        bits_collected += 6;

        if (bits_collected >= 8)
        {
            bits_collected -= 8;
            out.push_back((char)((buffer >> bits_collected) & 0xFF));
        }
    }

    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int hash_hex_encode(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    std::string out;
    out.resize(len * 2);
    bytes_to_hex((const uint8_t*)data, len, &out[0]);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int hash_hex_decode(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    if (len % 2 != 0)
    {
        luaL_error(L, "invalid hex string length");
    }

    std::string out;
    out.resize(len / 2);
    for (size_t i = 0; i < len; i += 2)
    {
        int hi = hex_char_to_val(data[i]);
        int lo = hex_char_to_val(data[i + 1]);
        if (hi < 0 || lo < 0)
        {
            luaL_error(L, "invalid hex character in string");
        }
        out[i / 2] = (char)((hi << 4) | lo);
    }

    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// SHA-224
static void compute_sha224(const char* data, size_t len, uint8_t out[28])
{
    uint32_t state[8] = {
        0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
        0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4
    };
    uint8_t buffer[64];
    size_t offset = 0;

    for (size_t i = 0; i < len; ++i)
    {
        buffer[offset++] = (uint8_t)data[i];
        if (offset == 64)
        {
            sha256_transform(state, buffer);
            offset = 0;
        }
    }

    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        sha256_transform(state, buffer);
        offset = 0;
    }

    while (offset < 56) buffer[offset++] = 0;

    uint64_t bits = len * 8;
    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (bits >> ((7 - i) * 8)) & 0xFF;

    sha256_transform(state, buffer);

    for (int i = 0; i < 7; ++i)
    {
        out[i*4]   = (state[i] >> 24) & 0xFF;
        out[i*4+1] = (state[i] >> 16) & 0xFF;
        out[i*4+2] = (state[i] >> 8) & 0xFF;
        out[i*4+3] = state[i] & 0xFF;
    }
}

static int hash_sha224(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[28];
    compute_sha224(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 28);
    return 1;
}

static int hash_sha224hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[28];
    compute_sha224(data, len, digest);
    char hex[56];
    bytes_to_hex(digest, 28, hex);
    lua_pushlstring(L, hex, 56);
    return 1;
}

// SHA-512 & SHA-384
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH64(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ64(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0_64(x) (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define EP1_64(x) (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define SIG0_64(x) (ROTR64(x, 1) ^ ROTR64(x, 8) ^ ((x) >> 7))
#define SIG1_64(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ ((x) >> 6))

static const uint64_t k_sha512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_transform(uint64_t state[8], const uint8_t data[128])
{
    uint64_t a, b, c, d, e, f, g, h, t1, t2, m[80];

    for (int i = 0; i < 16; ++i)
    {
        m[i] = ((uint64_t)data[i*8] << 56) |
               ((uint64_t)data[i*8+1] << 48) |
               ((uint64_t)data[i*8+2] << 40) |
               ((uint64_t)data[i*8+3] << 32) |
               ((uint64_t)data[i*8+4] << 24) |
               ((uint64_t)data[i*8+5] << 16) |
               ((uint64_t)data[i*8+6] << 8) |
               ((uint64_t)data[i*8+7]);
    }

    for (int i = 16; i < 80; ++i)
        m[i] = SIG1_64(m[i - 2]) + m[i - 7] + SIG0_64(m[i - 15]) + m[i - 16];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (int i = 0; i < 80; ++i)
    {
        t1 = h + EP1_64(e) + CH64(e, f, g) + k_sha512[i] + m[i];
        t2 = EP0_64(a) + MAJ64(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void compute_sha512(const char* data, size_t len, uint8_t out[64])
{
    uint64_t state[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };
    uint8_t buffer[128];
    size_t offset = 0;

    for (size_t i = 0; i < len; ++i)
    {
        buffer[offset++] = (uint8_t)data[i];
        if (offset == 128)
        {
            sha512_transform(state, buffer);
            offset = 0;
        }
    }

    buffer[offset++] = 0x80;
    if (offset > 112)
    {
        while (offset < 128) buffer[offset++] = 0;
        sha512_transform(state, buffer);
        offset = 0;
    }

    while (offset < 112) buffer[offset++] = 0;

    uint64_t bits_low = (uint64_t)len * 8;
    uint64_t bits_high = (uint64_t)(len >> 61);

    for (int i = 0; i < 8; ++i)
        buffer[112 + i] = (bits_high >> ((7 - i) * 8)) & 0xFF;
    for (int i = 0; i < 8; ++i)
        buffer[120 + i] = (bits_low >> ((7 - i) * 8)) & 0xFF;

    sha512_transform(state, buffer);

    for (int i = 0; i < 8; ++i)
    {
        for (int j = 0; j < 8; ++j)
            out[i*8 + j] = (state[i] >> ((7 - j) * 8)) & 0xFF;
    }
}

static void compute_sha384(const char* data, size_t len, uint8_t out[48])
{
    uint64_t state[8] = {
        0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
        0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
        0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL
    };
    uint8_t buffer[128];
    size_t offset = 0;

    for (size_t i = 0; i < len; ++i)
    {
        buffer[offset++] = (uint8_t)data[i];
        if (offset == 128)
        {
            sha512_transform(state, buffer);
            offset = 0;
        }
    }

    buffer[offset++] = 0x80;
    if (offset > 112)
    {
        while (offset < 128) buffer[offset++] = 0;
        sha512_transform(state, buffer);
        offset = 0;
    }

    while (offset < 112) buffer[offset++] = 0;

    uint64_t bits_low = (uint64_t)len * 8;
    uint64_t bits_high = (uint64_t)(len >> 61);

    for (int i = 0; i < 8; ++i)
        buffer[112 + i] = (bits_high >> ((7 - i) * 8)) & 0xFF;
    for (int i = 0; i < 8; ++i)
        buffer[120 + i] = (bits_low >> ((7 - i) * 8)) & 0xFF;

    sha512_transform(state, buffer);

    for (int i = 0; i < 6; ++i)
    {
        for (int j = 0; j < 8; ++j)
            out[i*8 + j] = (state[i] >> ((7 - j) * 8)) & 0xFF;
    }
}

static int hash_sha512(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[64];
    compute_sha512(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 64);
    return 1;
}

static int hash_sha512hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[64];
    compute_sha512(data, len, digest);
    char hex[128];
    bytes_to_hex(digest, 64, hex);
    lua_pushlstring(L, hex, 128);
    return 1;
}

static int hash_sha384(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[48];
    compute_sha384(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 48);
    return 1;
}

static int hash_sha384hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, 1, &len);
    uint8_t digest[48];
    compute_sha384(data, len, digest);
    char hex[96];
    bytes_to_hex(digest, 48, hex);
    lua_pushlstring(L, hex, 96);
    return 1;
}

// HMAC-SHA512 (block size = 128 bytes)
static void compute_hmac_sha512(const char* key, size_t key_len, const char* data, size_t data_len, uint8_t* out)
{
    uint8_t k_pad[128];
    memset(k_pad, 0, sizeof(k_pad));

    if (key_len > 128)
        compute_sha512(key, key_len, k_pad);
    else
        memcpy(k_pad, key, key_len);

    uint8_t ipad[128], opad[128];
    for (int i = 0; i < 128; ++i)
    {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5C;
    }

    std::vector<char> inner;
    inner.reserve(128 + data_len);
    inner.insert(inner.end(), ipad, ipad + 128);
    inner.insert(inner.end(), data, data + data_len);

    uint8_t inner_hash[64];
    compute_sha512(inner.data(), inner.size(), inner_hash);

    std::vector<char> outer;
    outer.reserve(128 + 64);
    outer.insert(outer.end(), opad, opad + 128);
    outer.insert(outer.end(), (const char*)inner_hash, (const char*)inner_hash + 64);

    compute_sha512(outer.data(), outer.size(), out);
}

static int hash_hmac_sha512(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[64];
    compute_hmac_sha512(key, key_len, data, data_len, out);
    lua_pushlstring(L, (const char*)out, 64);
    return 1;
}

static int hash_hmac_sha512hex(lua_State* L)
{
    size_t key_len = 0, data_len = 0;
    const char* key = get_hash_data(L, 1, &key_len);
    const char* data = get_hash_data(L, 2, &data_len);

    uint8_t out[64];
    compute_hmac_sha512(key, key_len, data, data_len, out);
    char hex[128];
    bytes_to_hex(out, 64, hex);
    lua_pushlstring(L, hex, 128);
    return 1;
}

// CSPRNG Random Bytes
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

static int hash_random_bytes(lua_State* L)
{
    int count = luaL_checkinteger(L, 1);
    luaL_argcheck(L, count > 0 && count <= 1048576, 1, "size must be between 1 and 1048576 bytes");

    std::string buf;
    buf.resize(count);

#if !defined(_WIN32)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0)
    {
        size_t readBytes = 0;
        while (readBytes < (size_t)count)
        {
            ssize_t res = read(fd, &buf[readBytes], count - readBytes);
            if (res <= 0) break;
            readBytes += res;
        }
        close(fd);
    }
#else
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
    {
        CryptGenRandom(hProv, (DWORD)count, (BYTE*)buf.data());
        CryptReleaseContext(hProv, 0);
    }
#endif

    lua_pushlstring(L, buf.data(), buf.size());
    return 1;
}

// Timing-Safe Equality Comparison (prevents side-channel timing attacks)
static int hash_timing_safe_equal(lua_State* L)
{
    size_t lenA = 0, lenB = 0;
    const char* a = get_hash_data(L, 1, &lenA);
    const char* b = get_hash_data(L, 2, &lenB);

    if (lenA != lenB)
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    uint8_t diff = 0;
    for (size_t i = 0; i < lenA; ++i)
        diff |= ((uint8_t)a[i] ^ (uint8_t)b[i]);

    lua_pushboolean(L, diff == 0);
    return 1;
}

// ChaCha20 Stream Cipher (RFC 7539)
#define CHACHA20_QUARTERROUND(a, b, c, d) \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7);

static void chacha20_block(const uint32_t key[8], const uint32_t nonce[3], uint32_t counter, uint8_t out[64])
{
    uint32_t state[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, // "expand 32-byte k"
        key[0], key[1], key[2], key[3],
        key[4], key[5], key[6], key[7],
        counter, nonce[0], nonce[1], nonce[2]
    };
    uint32_t working[16];
    memcpy(working, state, sizeof(state));

    for (int i = 0; i < 10; ++i)
    {
        // Column rounds
        CHACHA20_QUARTERROUND(working[0], working[4], working[8], working[12]);
        CHACHA20_QUARTERROUND(working[1], working[5], working[9], working[13]);
        CHACHA20_QUARTERROUND(working[2], working[6], working[10], working[14]);
        CHACHA20_QUARTERROUND(working[3], working[7], working[11], working[15]);
        // Diagonal rounds
        CHACHA20_QUARTERROUND(working[0], working[5], working[10], working[15]);
        CHACHA20_QUARTERROUND(working[1], working[6], working[11], working[12]);
        CHACHA20_QUARTERROUND(working[2], working[7], working[8], working[13]);
        CHACHA20_QUARTERROUND(working[3], working[4], working[9], working[14]);
    }

    for (int i = 0; i < 16; ++i)
    {
        uint32_t val = working[i] + state[i];
        out[i*4]   = val & 0xFF;
        out[i*4+1] = (val >> 8) & 0xFF;
        out[i*4+2] = (val >> 16) & 0xFF;
        out[i*4+3] = (val >> 24) & 0xFF;
    }
}

static int crypto_chacha20(lua_State* L)
{
    size_t keyLen = 0, nonceLen = 0, dataLen = 0;
    const char* keyData = get_hash_data(L, 1, &keyLen);
    const char* nonceData = get_hash_data(L, 2, &nonceLen);
    const char* plainData = get_hash_data(L, 3, &dataLen);
    uint32_t counter = (uint32_t)luaL_optinteger(L, 4, 1);

    luaL_argcheck(L, keyLen == 32, 1, "key must be exactly 32 bytes (256-bit)");
    luaL_argcheck(L, nonceLen == 12, 2, "nonce must be exactly 12 bytes (96-bit)");

    uint32_t key[8], nonce[3];
    for (int i = 0; i < 8; ++i)
        key[i] = ((uint32_t)(uint8_t)keyData[i*4]) |
                 ((uint32_t)(uint8_t)keyData[i*4+1] << 8) |
                 ((uint32_t)(uint8_t)keyData[i*4+2] << 16) |
                 ((uint32_t)(uint8_t)keyData[i*4+3] << 24);

    for (int i = 0; i < 3; ++i)
        nonce[i] = ((uint32_t)(uint8_t)nonceData[i*4]) |
                   ((uint32_t)(uint8_t)nonceData[i*4+1] << 8) |
                   ((uint32_t)(uint8_t)nonceData[i*4+2] << 16) |
                   ((uint32_t)(uint8_t)nonceData[i*4+3] << 24);

    std::string result;
    result.resize(dataLen);

    uint8_t block[64];
    size_t offset = 0;
    while (offset < dataLen)
    {
        chacha20_block(key, nonce, counter++, block);
        size_t chunk = (dataLen - offset < 64) ? (dataLen - offset) : 64;
        for (size_t i = 0; i < chunk; ++i)
            result[offset + i] = (char)((uint8_t)plainData[offset + i] ^ block[i]);
        offset += chunk;
    }

    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static const luaL_Reg hashlib[] = {
    {"crc32", hash_crc32},
    {"fnv1a", hash_fnv1a},
    {"md5", hash_md5},
    {"md5hex", hash_md5hex},
    {"sha1", hash_sha1},
    {"sha1hex", hash_sha1hex},
    {"sha224", hash_sha224},
    {"sha224hex", hash_sha224hex},
    {"sha256", hash_sha256},
    {"sha256hex", hash_sha256hex},
    {"sha384", hash_sha384},
    {"sha384hex", hash_sha384hex},
    {"sha512", hash_sha512},
    {"sha512hex", hash_sha512hex},
    {"hmac_sha256", hash_hmac_sha256},
    {"hmac_sha256hex", hash_hmac_sha256hex},
    {"hmac_sha512", hash_hmac_sha512},
    {"hmac_sha512hex", hash_hmac_sha512hex},
    {"hmac_sha1", hash_hmac_sha1},
    {"hmac_sha1hex", hash_hmac_sha1hex},
    {"hmac_md5", hash_hmac_md5},
    {"hmac_md5hex", hash_hmac_md5hex},
    {"base64_encode", hash_base64_encode},
    {"base64_decode", hash_base64_decode},
    {"hex_encode", hash_hex_encode},
    {"hex_decode", hash_hex_decode},
    {"random_bytes", hash_random_bytes},
    {"randomBytes", hash_random_bytes},
    {"timing_safe_equal", hash_timing_safe_equal},
    {"timingSafeEqual", hash_timing_safe_equal},
    {"chacha20", crypto_chacha20},
    // Aliases
    {"base64", hash_base64_encode},
    {"unbase64", hash_base64_decode},
    {"hex", hash_hex_encode},
    {"unhex", hash_hex_decode},
    {NULL, NULL},
};

static const luaL_Reg cryptolib[] = {
    {"sha256", hash_sha256},
    {"sha256hex", hash_sha256hex},
    {"sha512", hash_sha512},
    {"sha512hex", hash_sha512hex},
    {"sha384", hash_sha384},
    {"sha384hex", hash_sha384hex},
    {"sha224", hash_sha224},
    {"sha224hex", hash_sha224hex},
    {"sha1", hash_sha1},
    {"sha1hex", hash_sha1hex},
    {"md5", hash_md5},
    {"md5hex", hash_md5hex},
    {"hmac_sha256", hash_hmac_sha256},
    {"hmac_sha256hex", hash_hmac_sha256hex},
    {"hmac_sha512", hash_hmac_sha512},
    {"hmac_sha512hex", hash_hmac_sha512hex},
    {"hmac_sha1", hash_hmac_sha1},
    {"hmac_sha1hex", hash_hmac_sha1hex},
    {"hmac_md5", hash_hmac_md5},
    {"hmac_md5hex", hash_hmac_md5hex},
    {"random_bytes", hash_random_bytes},
    {"randomBytes", hash_random_bytes},
    {"timing_safe_equal", hash_timing_safe_equal},
    {"timingSafeEqual", hash_timing_safe_equal},
    {"chacha20", crypto_chacha20},
    {"base64_encode", hash_base64_encode},
    {"base64_decode", hash_base64_decode},
    {"hex_encode", hash_hex_encode},
    {"hex_decode", hash_hex_decode},
    {NULL, NULL},
};

int luaopen_hash(lua_State* L)
{
    luaL_register(L, LUA_HASHLIBNAME, hashlib);
    return 1;
}

int luaopen_crypto(lua_State* L)
{
    luaL_register(L, "crypto", cryptolib);
    return 1;
}

