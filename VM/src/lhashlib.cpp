// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"
#include <string.h>
#include <stdint.h>

#define LUA_HASHLIBNAME "hash"

static const char* get_hash_data(lua_State* L, size_t* len)
{
    if (lua_type(L, 1) == LUA_TSTRING)
    {
        return lua_tolstring(L, 1, len);
    }
    else if (lua_isbuffer(L, 1))
    {
        return (const char*)luaL_checkbuffer(L, 1, len);
    }
    else
    {
        luaL_typeerror(L, 1, "string or buffer");
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

// CRC-32 (ISO-HDLC)
static int hash_crc32(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, &len);

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
    const char* data = get_hash_data(L, &len);

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

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
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
    
    uint64_t bitlen = (uint64_t)len * 8;
    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        md5_transform(state, buffer);
        offset = 0;
    }
    while (offset < 56) buffer[offset++] = 0;
    
    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (uint8_t)(bitlen >> (i * 8));
        
    md5_transform(state, buffer);
    
    for (int i = 0; i < 4; ++i)
    {
        out[i*4]   = (uint8_t)(state[i]);
        out[i*4+1] = (uint8_t)(state[i] >> 8);
        out[i*4+2] = (uint8_t)(state[i] >> 16);
        out[i*4+3] = (uint8_t)(state[i] >> 24);
    }
}

static int hash_md5(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, &len);
    uint8_t out[16];
    compute_md5(data, len, out);
    lua_pushlstring(L, (const char*)out, 16);
    return 1;
}

static int hash_md5hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, &len);
    uint8_t out[16];
    compute_md5(data, len, out);
    char hex[32];
    bytes_to_hex(out, 16, hex);
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

    uint64_t bitlen = (uint64_t)len * 8;
    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        sha1_transform(state, buffer);
        offset = 0;
    }
    while (offset < 56) buffer[offset++] = 0;

    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (uint8_t)(bitlen >> (56 - i * 8));

    sha1_transform(state, buffer);

    for (int i = 0; i < 5; ++i)
    {
        out[i*4]   = (uint8_t)(state[i] >> 24);
        out[i*4+1] = (uint8_t)(state[i] >> 16);
        out[i*4+2] = (uint8_t)(state[i] >> 8);
        out[i*4+3] = (uint8_t)(state[i]);
    }
}

static int hash_sha1(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, &len);
    uint8_t out[20];
    compute_sha1(data, len, out);
    lua_pushlstring(L, (const char*)out, 20);
    return 1;
}

static int hash_sha1hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, &len);
    uint8_t out[20];
    compute_sha1(data, len, out);
    char hex[40];
    bytes_to_hex(out, 20, hex);
    lua_pushlstring(L, hex, 40);
    return 1;
}

// SHA-256
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(uint32_t state[8], const uint8_t buffer[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)buffer[i*4] << 24) | ((uint32_t)buffer[i*4+1] << 16) | ((uint32_t)buffer[i*4+2] << 8) | ((uint32_t)buffer[i*4+3]);

    for (int i = 16; i < 64; ++i)
    {
        uint32_t s0 = ROTR32(w[i-15], 7) ^ ROTR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROTR32(w[i-2], 17) ^ ROTR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (int i = 0; i < 64; ++i)
    {
        uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + sha256_k[i] + w[i];
        
        uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e; state[5] += f; state[6] += g; state[7] += h;
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

    uint64_t bitlen = (uint64_t)len * 8;
    buffer[offset++] = 0x80;
    if (offset > 56)
    {
        while (offset < 64) buffer[offset++] = 0;
        sha256_transform(state, buffer);
        offset = 0;
    }
    while (offset < 56) buffer[offset++] = 0;

    for (int i = 0; i < 8; ++i)
        buffer[56 + i] = (uint8_t)(bitlen >> (56 - i * 8));

    sha256_transform(state, buffer);

    for (int i = 0; i < 8; ++i)
    {
        out[i*4]   = (uint8_t)(state[i] >> 24);
        out[i*4+1] = (uint8_t)(state[i] >> 16);
        out[i*4+2] = (uint8_t)(state[i] >> 8);
        out[i*4+3] = (uint8_t)(state[i]);
    }
}

static int hash_sha256(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, &len);
    uint8_t out[32];
    compute_sha256(data, len, out);
    lua_pushlstring(L, (const char*)out, 32);
    return 1;
}

static int hash_sha256hex(lua_State* L)
{
    size_t len = 0;
    const char* data = get_hash_data(L, &len);
    uint8_t out[32];
    compute_sha256(data, len, out);
    char hex[64];
    bytes_to_hex(out, 32, hex);
    lua_pushlstring(L, hex, 64);
    return 1;
}

static const luaL_Reg hashlib[] = {
    {"crc32", hash_crc32},
    {"fnv1a", hash_fnv1a},
    {"md5", hash_md5},
    {"md5hex", hash_md5hex},
    {"sha1", hash_sha1},
    {"sha1hex", hash_sha1hex},
    {"sha256", hash_sha256},
    {"sha256hex", hash_sha256hex},
    {NULL, NULL}
};

int luaopen_hash(lua_State* L)
{
    luaL_register(L, LUA_HASHLIBNAME, hashlib);
    return 1;
}
