# ADR 0012: Cryptographic Library Expansion

## Context

Luau developer tooling and server applications require standard cryptographic primitives for hashing, authentication, random data generation, and cipher operations without relying on external dependencies or C bindings.

## Decision

Expand the built-in `hash` and `crypto` library modules (`lhashlib.cpp`) with full cryptographic primitives:
- **Hashing**: SHA-224, SHA-256, SHA-384, SHA-512, MD5, SHA-1, CRC32, FNV-1a.
- **Message Authentication**: HMAC-SHA256, HMAC-SHA512, HMAC-SHA1, HMAC-MD5.
- **CSPRNG**: `crypto.randomBytes(n)` backed by `/dev/urandom` on POSIX and `CryptGenRandom` on Windows.
- **Timing Attacks Defense**: `crypto.timingSafeEqual(a, b)` constant-time buffer comparison.
- **Stream Ciphers**: RFC 7539 ChaCha20 256-bit encryption and decryption.
- **Encoding**: Standard RFC 4648 Base64 and Hex encoder/decoder.

## Consequences

- Applications can perform standard authentication tokens (e.g. JWT HMAC signing), password hashing, secure random ID generation, and encryption purely in native Jaci.
- 100% pure C/C++ zero-dependency implementation.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
