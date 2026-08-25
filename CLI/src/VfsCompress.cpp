// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/VfsCompress.h"

#include <algorithm>
#include <cstring>

namespace Luau
{
namespace Vfs
{

namespace
{

constexpr size_t kMinMatch = 4;
constexpr size_t kHashLog = 13;
constexpr size_t kHashSize = 1 << kHashLog;

inline uint32_t hash4(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - kHashLog);
}

} // namespace

std::string compress(std::string_view input)
{
    if (input.empty())
        return "";

    const uint8_t* src = reinterpret_cast<const uint8_t*>(input.data());
    const size_t srcLen = input.size();

    if (srcLen < kMinMatch + 4)
    {
        // For tiny buffers, prefix with flag 0 and raw bytes
        std::string out;
        out.reserve(srcLen + 1);
        out.push_back('\0'); // uncompressed literal marker
        out.append(input);
        return out;
    }

    std::string out;
    out.reserve(srcLen / 2 + 16);
    out.push_back('\x01'); // compressed marker

    uint32_t hashTable[kHashSize];
    memset(hashTable, 0, sizeof(hashTable));

    size_t ip = 0;
    size_t anchor = 0;

    while (ip + kMinMatch + 4 <= srcLen)
    {
        uint32_t h = hash4(&src[ip]);
        size_t ref = hashTable[h];
        hashTable[h] = static_cast<uint32_t>(ip);

        if (ref < ip && (ip - ref) < 65535 && memcmp(&src[ip], &src[ref], 4) == 0)
        {
            // Found a match
            size_t matchLen = 4;
            while (ip + matchLen < srcLen && src[ip + matchLen] == src[ref + matchLen])
                ++matchLen;

            // Emit literals before match
            size_t litLen = ip - anchor;
            while (litLen >= 255)
            {
                out.push_back(static_cast<char>(255));
                out.append(reinterpret_cast<const char*>(&src[anchor]), 255);
                anchor += 255;
                litLen -= 255;
            }
            out.push_back(static_cast<char>(litLen));
            if (litLen > 0)
            {
                out.append(reinterpret_cast<const char*>(&src[anchor]), litLen);
            }

            // Emit match: 2 bytes offset (LE), 1 byte length (or extended)
            uint16_t offset = static_cast<uint16_t>(ip - ref);
            out.push_back(static_cast<char>(offset & 0xFF));
            out.push_back(static_cast<char>((offset >> 8) & 0xFF));

            size_t encLen = matchLen - kMinMatch;
            if (encLen < 255)
            {
                out.push_back(static_cast<char>(encLen));
            }
            else
            {
                out.push_back(static_cast<char>(255));
                encLen -= 255;
                while (encLen >= 255)
                {
                    out.push_back(static_cast<char>(255));
                    encLen -= 255;
                }
                out.push_back(static_cast<char>(encLen));
            }

            ip += matchLen;
            anchor = ip;
        }
        else
        {
            ++ip;
        }
    }

    // Emit trailing literals
    size_t litLen = srcLen - anchor;
    while (litLen >= 255)
    {
        out.push_back(static_cast<char>(255));
        out.append(reinterpret_cast<const char*>(&src[anchor]), 255);
        anchor += 255;
        litLen -= 255;
    }
    out.push_back(static_cast<char>(litLen));
    if (litLen > 0)
    {
        out.append(reinterpret_cast<const char*>(&src[anchor]), litLen);
    }

    // Match marker offset 0 indicates end
    out.push_back('\0');
    out.push_back('\0');

    out.shrink_to_fit();
    return out;
}

std::string decompress(std::string_view compressed, size_t originalSize)
{
    if (compressed.empty())
        return "";

    if (compressed[0] == '\0')
    {
        return std::string(compressed.substr(1));
    }

    const uint8_t* src = reinterpret_cast<const uint8_t*>(compressed.data()) + 1;
    const size_t srcLen = compressed.size() - 1;

    std::string out;
    out.resize(originalSize);
    uint8_t* dst = reinterpret_cast<uint8_t*>(&out[0]);

    size_t ip = 0;
    size_t op = 0;

    while (ip < srcLen && op < originalSize)
    {
        // Read literal length
        size_t litLen = src[ip++];
        if (litLen == 255)
        {
            while (ip < srcLen && src[ip] == 255)
            {
                litLen += 255;
                ++ip;
            }
            if (ip < srcLen)
                litLen += src[ip++];
        }

        if (litLen > 0)
        {
            if (ip + litLen > srcLen || op + litLen > originalSize)
                break;
            memcpy(&dst[op], &src[ip], litLen);
            ip += litLen;
            op += litLen;
        }

        if (ip + 2 > srcLen)
            break;

        uint16_t offset = static_cast<uint16_t>(src[ip] | (src[ip + 1] << 8));
        ip += 2;

        if (offset == 0)
            break; // End of compressed stream

        if (ip >= srcLen)
            break;

        size_t matchLen = kMinMatch + src[ip++];
        if (matchLen == kMinMatch + 255)
        {
            while (ip < srcLen && src[ip] == 255)
            {
                matchLen += 255;
                ++ip;
            }
            if (ip < srcLen)
                matchLen += src[ip++];
        }

        if (offset > op || op + matchLen > originalSize)
            break;

        size_t ref = op - offset;
        for (size_t i = 0; i < matchLen; ++i)
        {
            dst[op + i] = dst[ref + i];
        }
        op += matchLen;
    }

    out.resize(op);
    return out;
}

CompactLineOffsets::CompactLineOffsets(std::string_view text)
{
    build(text);
}

void CompactLineOffsets::build(std::string_view text)
{
    offsets.clear();
    offsets.push_back(0);
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
            offsets.push_back(static_cast<uint32_t>(i + 1));
    }
    offsets.shrink_to_fit();
}

size_t CompactLineOffsets::getOffset(int line, int character, size_t totalTextSize) const
{
    if (line < 0 || offsets.empty())
        return 0;
    if (static_cast<size_t>(line) >= offsets.size())
        return totalTextSize;

    size_t lineStart = offsets[line];
    size_t lineEnd = (static_cast<size_t>(line) + 1 < offsets.size()) ? offsets[line + 1] : totalTextSize;
    size_t charOffset = lineStart + std::max(0, character);
    return std::min(charOffset, lineEnd);
}

void CompactLineOffsets::getPosition(size_t offset, int& outLine, int& outChar, size_t totalTextSize) const
{
    if (offset > totalTextSize)
        offset = totalTextSize;

    if (offsets.empty())
    {
        outLine = 0;
        outChar = 0;
        return;
    }

    auto it = std::upper_bound(offsets.begin(), offsets.end(), static_cast<uint32_t>(offset));
    int line = static_cast<int>(std::distance(offsets.begin(), it) - 1);
    if (line < 0)
        line = 0;

    outLine = line;
    outChar = static_cast<int>(offset - offsets[line]);
}

CompressedFileBuffer::CompressedFileBuffer(std::string text, bool autoCompress)
{
    setText(std::move(text), autoCompress);
}

void CompressedFileBuffer::setText(std::string text, bool autoCompress)
{
    origSize = text.size();
    if (autoCompress && origSize > 512)
    {
        data = Luau::Vfs::compress(text);
        isComp = true;
    }
    else
    {
        data = std::move(text);
        isComp = false;
    }
    data.shrink_to_fit();
}

std::string CompressedFileBuffer::getText() const
{
    if (isComp)
    {
        return Luau::Vfs::decompress(data, origSize);
    }
    return data;
}

std::string_view CompressedFileBuffer::getRawTextIfUncompressed() const
{
    if (!isComp)
        return data;
    return {};
}

void CompressedFileBuffer::compress()
{
    if (!isComp && origSize > 128)
    {
        data = Luau::Vfs::compress(data);
        isComp = true;
        data.shrink_to_fit();
    }
}

void CompressedFileBuffer::decompress()
{
    if (isComp)
    {
        data = Luau::Vfs::decompress(data, origSize);
        isComp = false;
        data.shrink_to_fit();
    }
}

} // namespace Vfs
} // namespace Luau
