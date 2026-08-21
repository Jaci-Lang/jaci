// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/JsonRpc.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace Luau
{
namespace Json
{

const Value* Value::get(const std::string& key) const
{
    if (type != Type::Object)
        return nullptr;

    for (const auto& pair : objectValue)
    {
        if (pair.first == key)
            return &pair.second;
    }
    return nullptr;
}

const Value* Value::get(size_t index) const
{
    if (type != Type::Array || index >= arrayValue.size())
        return nullptr;

    return &arrayValue[index];
}

static void escapeString(std::string& out, std::string_view s)
{
    out.push_back('"');
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out.append(buf);
            }
            else
            {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

std::string Value::serialize(bool pretty, int indent) const
{
    std::string out;
    switch (type)
    {
    case Type::Null:
        out = "null";
        break;
    case Type::Boolean:
        out = boolValue ? "true" : "false";
        break;
    case Type::Number:
        if (isInteger)
        {
            out = std::to_string(intValue);
        }
        else
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.17g", numberValue);
            out = buf;
        }
        break;
    case Type::String:
        escapeString(out, stringValue);
        break;
    case Type::Array:
    {
        out = "[";
        for (size_t i = 0; i < arrayValue.size(); ++i)
        {
            if (i > 0)
                out += pretty ? ", " : ",";
            out += arrayValue[i].serialize(pretty, indent + 1);
        }
        out += "]";
        break;
    }
    case Type::Object:
    {
        out = "{";
        for (size_t i = 0; i < objectValue.size(); ++i)
        {
            if (i > 0)
                out += pretty ? ", " : ",";
            escapeString(out, objectValue[i].first);
            out += pretty ? ": " : ":";
            out += objectValue[i].second.serialize(pretty, indent + 1);
        }
        out += "}";
        break;
    }
    }
    return out;
}

namespace
{
struct Parser
{
    std::string_view input;
    size_t pos = 0;
    std::string* error = nullptr;

    void skipWhitespace()
    {
        while (pos < input.size())
        {
            char c = input[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos;
            else
                break;
        }
    }

    char peek()
    {
        skipWhitespace();
        return pos < input.size() ? input[pos] : '\0';
    }

    char get()
    {
        skipWhitespace();
        return pos < input.size() ? input[pos++] : '\0';
    }

    bool match(char expected)
    {
        skipWhitespace();
        if (pos < input.size() && input[pos] == expected)
        {
            ++pos;
            return true;
        }
        return false;
    }

    void setError(const std::string& msg)
    {
        if (error && error->empty())
            *error = msg + " at position " + std::to_string(pos);
    }

    std::optional<std::string> parseString()
    {
        if (!match('"'))
            return std::nullopt;

        std::string result;
        while (pos < input.size())
        {
            char c = input[pos++];
            if (c == '"')
                return result;

            if (c == '\\')
            {
                if (pos >= input.size())
                {
                    setError("Unexpected end of string escape");
                    return std::nullopt;
                }
                char esc = input[pos++];
                switch (esc)
                {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case '/':
                    result.push_back('/');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u':
                {
                    if (pos + 4 > input.size())
                    {
                        setError("Invalid unicode escape");
                        return std::nullopt;
                    }
                    unsigned int codepoint = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        char h = input[pos++];
                        codepoint <<= 4;
                        if (h >= '0' && h <= '9')
                            codepoint |= (h - '0');
                        else if (h >= 'a' && h <= 'f')
                            codepoint |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            codepoint |= (h - 'A' + 10);
                        else
                        {
                            setError("Invalid hex in unicode escape");
                            return std::nullopt;
                        }
                    }
                    if (codepoint < 0x80)
                    {
                        result.push_back(static_cast<char>(codepoint));
                    }
                    else if (codepoint < 0x800)
                    {
                        result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
                        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                    else
                    {
                        result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
                        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                    break;
                }
                default:
                    result.push_back(esc);
                    break;
                }
            }
            else
            {
                result.push_back(c);
            }
        }

        setError("Unterminated string");
        return std::nullopt;
    }

    std::optional<Value> parseNumber()
    {
        skipWhitespace();
        size_t start = pos;
        if (pos < input.size() && input[pos] == '-')
            ++pos;

        bool hasDigits = false;
        while (pos < input.size() && isdigit(static_cast<unsigned char>(input[pos])))
        {
            hasDigits = true;
            ++pos;
        }

        if (!hasDigits)
        {
            setError("Invalid number");
            return std::nullopt;
        }

        bool isInt = true;
        if (pos < input.size() && input[pos] == '.')
        {
            isInt = false;
            ++pos;
            while (pos < input.size() && isdigit(static_cast<unsigned char>(input[pos])))
                ++pos;
        }

        if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E'))
        {
            isInt = false;
            ++pos;
            if (pos < input.size() && (input[pos] == '+' || input[pos] == '-'))
                ++pos;
            while (pos < input.size() && isdigit(static_cast<unsigned char>(input[pos])))
                ++pos;
        }

        std::string numStr(input.substr(start, pos - start));
        char* endPtr = nullptr;
        if (isInt)
        {
            int64_t val = strtoll(numStr.c_str(), &endPtr, 10);
            return Value(val);
        }
        else
        {
            double val = strtod(numStr.c_str(), &endPtr);
            return Value(val);
        }
    }

    std::optional<Value> parseValue()
    {
        skipWhitespace();
        if (pos >= input.size())
        {
            setError("Unexpected EOF");
            return std::nullopt;
        }

        char c = input[pos];
        if (c == 'n')
        {
            if (input.substr(pos, 4) == "null")
            {
                pos += 4;
                return Value(nullptr);
            }
            setError("Unexpected token");
            return std::nullopt;
        }
        if (c == 't')
        {
            if (input.substr(pos, 4) == "true")
            {
                pos += 4;
                return Value(true);
            }
            setError("Unexpected token");
            return std::nullopt;
        }
        if (c == 'f')
        {
            if (input.substr(pos, 5) == "false")
            {
                pos += 5;
                return Value(false);
            }
            setError("Unexpected token");
            return std::nullopt;
        }
        if (c == '"')
        {
            auto str = parseString();
            if (!str)
                return std::nullopt;
            return Value(std::move(*str));
        }
        if (c == '-' || isdigit(static_cast<unsigned char>(c)))
        {
            return parseNumber();
        }
        if (c == '[')
        {
            ++pos;
            Array arr;
            skipWhitespace();
            if (match(']'))
                return Value(std::move(arr));

            while (true)
            {
                auto val = parseValue();
                if (!val)
                    return std::nullopt;
                arr.push_back(std::move(*val));

                skipWhitespace();
                if (match(']'))
                    break;
                if (!match(','))
                {
                    setError("Expected ',' or ']' in array");
                    return std::nullopt;
                }
            }
            return Value(std::move(arr));
        }
        if (c == '{')
        {
            ++pos;
            Object obj;
            skipWhitespace();
            if (match('}'))
                return Value(std::move(obj));

            while (true)
            {
                skipWhitespace();
                auto key = parseString();
                if (!key)
                {
                    setError("Expected string key in object");
                    return std::nullopt;
                }

                skipWhitespace();
                if (!match(':'))
                {
                    setError("Expected ':' after key in object");
                    return std::nullopt;
                }

                auto val = parseValue();
                if (!val)
                    return std::nullopt;

                obj.emplace_back(std::move(*key), std::move(*val));

                skipWhitespace();
                if (match('}'))
                    break;
                if (!match(','))
                {
                    setError("Expected ',' or '}' in object");
                    return std::nullopt;
                }
            }
            return Value(std::move(obj));
        }

        setError("Unexpected character");
        return std::nullopt;
    }
};
} // namespace

std::optional<Value> parse(std::string_view json, std::string* error)
{
    Parser p;
    p.input = json;
    p.error = error;
    auto result = p.parseValue();
    if (result)
    {
        p.skipWhitespace();
        if (p.pos < p.input.size())
        {
            p.setError("Trailing characters after JSON value");
            return std::nullopt;
        }
    }
    return result;
}

namespace
{
static void encodeMsgPack(std::string& out, const Value& val)
{
    switch (val.type)
    {
    case Type::Null:
        out.push_back(static_cast<char>(0xc0));
        break;
    case Type::Boolean:
        out.push_back(val.boolValue ? static_cast<char>(0xc3) : static_cast<char>(0xc2));
        break;
    case Type::Number:
        if (val.isInteger)
        {
            int64_t n = val.intValue;
            if (n >= 0 && n <= 127)
            {
                out.push_back(static_cast<char>(n));
            }
            else if (n >= -32 && n < 0)
            {
                out.push_back(static_cast<char>(n));
            }
            else if (n >= 0 && n <= 0xFF)
            {
                out.push_back(static_cast<char>(0xcc));
                out.push_back(static_cast<char>(n));
            }
            else if (n >= -128 && n <= 127)
            {
                out.push_back(static_cast<char>(0xd0));
                out.push_back(static_cast<char>(n));
            }
            else if (n >= 0 && n <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xcd));
                out.push_back(static_cast<char>((n >> 8) & 0xFF));
                out.push_back(static_cast<char>(n & 0xFF));
            }
            else if (n >= -32768 && n <= 32767)
            {
                out.push_back(static_cast<char>(0xd1));
                out.push_back(static_cast<char>((n >> 8) & 0xFF));
                out.push_back(static_cast<char>(n & 0xFF));
            }
            else if (n >= 0 && n <= 0xFFFFFFFFLL)
            {
                out.push_back(static_cast<char>(0xce));
                out.push_back(static_cast<char>((n >> 24) & 0xFF));
                out.push_back(static_cast<char>((n >> 16) & 0xFF));
                out.push_back(static_cast<char>((n >> 8) & 0xFF));
                out.push_back(static_cast<char>(n & 0xFF));
            }
            else if (n >= -2147483648LL && n <= 2147483647LL)
            {
                out.push_back(static_cast<char>(0xd2));
                out.push_back(static_cast<char>((n >> 24) & 0xFF));
                out.push_back(static_cast<char>((n >> 16) & 0xFF));
                out.push_back(static_cast<char>((n >> 8) & 0xFF));
                out.push_back(static_cast<char>(n & 0xFF));
            }
            else
            {
                out.push_back(static_cast<char>(0xd3));
                for (int i = 7; i >= 0; --i)
                    out.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
            }
        }
        else
        {
            out.push_back(static_cast<char>(0xcb));
            double d = val.numberValue;
            uint64_t bits;
            memcpy(&bits, &d, sizeof(bits));
            for (int i = 7; i >= 0; --i)
                out.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
        }
        break;
    case Type::String:
    {
        size_t len = val.stringValue.size();
        if (len < 32)
        {
            out.push_back(static_cast<char>(0xa0 | len));
        }
        else if (len <= 0xFF)
        {
            out.push_back(static_cast<char>(0xd9));
            out.push_back(static_cast<char>(len));
        }
        else if (len <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xda));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(len & 0xFF));
        }
        else
        {
            out.push_back(static_cast<char>(0xdb));
            out.push_back(static_cast<char>((len >> 24) & 0xFF));
            out.push_back(static_cast<char>((len >> 16) & 0xFF));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(len & 0xFF));
        }
        out.append(val.stringValue);
        break;
    }
    case Type::Array:
    {
        size_t len = val.arrayValue.size();
        if (len < 16)
        {
            out.push_back(static_cast<char>(0x90 | len));
        }
        else if (len <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xdc));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(len & 0xFF));
        }
        else
        {
            out.push_back(static_cast<char>(0xdd));
            out.push_back(static_cast<char>((len >> 24) & 0xFF));
            out.push_back(static_cast<char>((len >> 16) & 0xFF));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(len & 0xFF));
        }
        for (const auto& item : val.arrayValue)
            encodeMsgPack(out, item);
        break;
    }
    case Type::Object:
    {
        size_t len = val.objectValue.size();
        if (len < 16)
        {
            out.push_back(static_cast<char>(0x80 | len));
        }
        else if (len <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xde));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(len & 0xFF));
        }
        else
        {
            out.push_back(static_cast<char>(0xdf));
            out.push_back(static_cast<char>((len >> 24) & 0xFF));
            out.push_back(static_cast<char>((len >> 16) & 0xFF));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(len & 0xFF));
        }
        for (const auto& pair : val.objectValue)
        {
            Value keyVal(pair.first);
            encodeMsgPack(out, keyVal);
            encodeMsgPack(out, pair.second);
        }
        break;
    }
    }
}

struct MsgPackDecoder
{
    std::string_view input;
    size_t pos = 0;
    std::string* error = nullptr;

    std::optional<Value> decode()
    {
        if (pos >= input.size())
            return std::nullopt;

        uint8_t b = static_cast<uint8_t>(input[pos++]);

        // positive fixint
        if (b <= 0x7f)
            return Value(static_cast<int64_t>(b));
        // fixmap
        if ((b & 0xf0) == 0x80)
        {
            size_t count = b & 0x0f;
            Object obj;
            for (size_t i = 0; i < count; ++i)
            {
                auto k = decode();
                if (!k || !k->isString()) return std::nullopt;
                auto v = decode();
                if (!v) return std::nullopt;
                obj.emplace_back(k->getString(), std::move(*v));
            }
            return Value(std::move(obj));
        }
        // fixarray
        if ((b & 0xf0) == 0x90)
        {
            size_t count = b & 0x0f;
            Array arr;
            for (size_t i = 0; i < count; ++i)
            {
                auto v = decode();
                if (!v) return std::nullopt;
                arr.push_back(std::move(*v));
            }
            return Value(std::move(arr));
        }
        // fixstr
        if ((b & 0xe0) == 0xa0)
        {
            size_t len = b & 0x1f;
            if (pos + len > input.size()) return std::nullopt;
            std::string s(input.substr(pos, len));
            pos += len;
            return Value(std::move(s));
        }
        // negative fixint
        if (b >= 0xe0)
        {
            int8_t sb = static_cast<int8_t>(b);
            return Value(static_cast<int64_t>(sb));
        }

        switch (b)
        {
        case 0xc0: return Value(nullptr);
        case 0xc2: return Value(false);
        case 0xc3: return Value(true);
        case 0xca: // float 32
        {
            if (pos + 4 > input.size()) return std::nullopt;
            uint32_t bits = 0;
            for (int i = 0; i < 4; ++i) bits = (bits << 8) | static_cast<uint8_t>(input[pos++]);
            float f;
            memcpy(&f, &bits, 4);
            return Value(static_cast<double>(f));
        }
        case 0xcb: // float 64
        {
            if (pos + 8 > input.size()) return std::nullopt;
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i) bits = (bits << 8) | static_cast<uint8_t>(input[pos++]);
            double d;
            memcpy(&d, &bits, 8);
            return Value(d);
        }
        case 0xcc: // uint 8
            if (pos >= input.size()) return std::nullopt;
            return Value(static_cast<int64_t>(static_cast<uint8_t>(input[pos++])));
        case 0xcd: // uint 16
        {
            if (pos + 2 > input.size()) return std::nullopt;
            uint16_t val = (static_cast<uint8_t>(input[pos]) << 8) | static_cast<uint8_t>(input[pos + 1]);
            pos += 2;
            return Value(static_cast<int64_t>(val));
        }
        case 0xce: // uint 32
        {
            if (pos + 4 > input.size()) return std::nullopt;
            uint32_t val = 0;
            for (int i = 0; i < 4; ++i) val = (val << 8) | static_cast<uint8_t>(input[pos++]);
            return Value(static_cast<int64_t>(val));
        }
        case 0xcf: // uint 64
        {
            if (pos + 8 > input.size()) return std::nullopt;
            uint64_t val = 0;
            for (int i = 0; i < 8; ++i) val = (val << 8) | static_cast<uint8_t>(input[pos++]);
            return Value(val);
        }
        case 0xd0: // int 8
            if (pos >= input.size()) return std::nullopt;
            return Value(static_cast<int64_t>(static_cast<int8_t>(input[pos++])));
        case 0xd1: // int 16
        {
            if (pos + 2 > input.size()) return std::nullopt;
            int16_t val = static_cast<int16_t>((static_cast<uint8_t>(input[pos]) << 8) | static_cast<uint8_t>(input[pos + 1]));
            pos += 2;
            return Value(static_cast<int64_t>(val));
        }
        case 0xd2: // int 32
        {
            if (pos + 4 > input.size()) return std::nullopt;
            int32_t val = 0;
            for (int i = 0; i < 4; ++i) val = (val << 8) | static_cast<uint8_t>(input[pos++]);
            return Value(static_cast<int64_t>(val));
        }
        case 0xd3: // int 64
        {
            if (pos + 8 > input.size()) return std::nullopt;
            int64_t val = 0;
            for (int i = 0; i < 8; ++i) val = (val << 8) | static_cast<uint8_t>(input[pos++]);
            return Value(val);
        }
        case 0xd9: // str 8
        {
            if (pos >= input.size()) return std::nullopt;
            size_t len = static_cast<uint8_t>(input[pos++]);
            if (pos + len > input.size()) return std::nullopt;
            std::string s(input.substr(pos, len));
            pos += len;
            return Value(std::move(s));
        }
        case 0xda: // str 16
        {
            if (pos + 2 > input.size()) return std::nullopt;
            size_t len = (static_cast<uint8_t>(input[pos]) << 8) | static_cast<uint8_t>(input[pos + 1]);
            pos += 2;
            if (pos + len > input.size()) return std::nullopt;
            std::string s(input.substr(pos, len));
            pos += len;
            return Value(std::move(s));
        }
        case 0xdb: // str 32
        {
            if (pos + 4 > input.size()) return std::nullopt;
            size_t len = 0;
            for (int i = 0; i < 4; ++i) len = (len << 8) | static_cast<uint8_t>(input[pos++]);
            if (pos + len > input.size()) return std::nullopt;
            std::string s(input.substr(pos, len));
            pos += len;
            return Value(std::move(s));
        }
        case 0xdc: // array 16
        {
            if (pos + 2 > input.size()) return std::nullopt;
            size_t count = (static_cast<uint8_t>(input[pos]) << 8) | static_cast<uint8_t>(input[pos + 1]);
            pos += 2;
            Array arr;
            for (size_t i = 0; i < count; ++i)
            {
                auto v = decode();
                if (!v) return std::nullopt;
                arr.push_back(std::move(*v));
            }
            return Value(std::move(arr));
        }
        case 0xdd: // array 32
        {
            if (pos + 4 > input.size()) return std::nullopt;
            size_t count = 0;
            for (int i = 0; i < 4; ++i) count = (count << 8) | static_cast<uint8_t>(input[pos++]);
            Array arr;
            for (size_t i = 0; i < count; ++i)
            {
                auto v = decode();
                if (!v) return std::nullopt;
                arr.push_back(std::move(*v));
            }
            return Value(std::move(arr));
        }
        case 0xde: // map 16
        {
            if (pos + 2 > input.size()) return std::nullopt;
            size_t count = (static_cast<uint8_t>(input[pos]) << 8) | static_cast<uint8_t>(input[pos + 1]);
            pos += 2;
            Object obj;
            for (size_t i = 0; i < count; ++i)
            {
                auto k = decode();
                if (!k || !k->isString()) return std::nullopt;
                auto v = decode();
                if (!v) return std::nullopt;
                obj.emplace_back(k->getString(), std::move(*v));
            }
            return Value(std::move(obj));
        }
        case 0xdf: // map 32
        {
            if (pos + 4 > input.size()) return std::nullopt;
            size_t count = 0;
            for (int i = 0; i < 4; ++i) count = (count << 8) | static_cast<uint8_t>(input[pos++]);
            Object obj;
            for (size_t i = 0; i < count; ++i)
            {
                auto k = decode();
                if (!k || !k->isString()) return std::nullopt;
                auto v = decode();
                if (!v) return std::nullopt;
                obj.emplace_back(k->getString(), std::move(*v));
            }
            return Value(std::move(obj));
        }
        default:
            return std::nullopt;
        }
    }
};
} // namespace

std::string Value::toBinaryMsgPack() const
{
    std::string out;
    encodeMsgPack(out, *this);
    return out;
}

std::optional<Value> parseBinaryMsgPack(std::string_view bytes, std::string* error)
{
    MsgPackDecoder dec;
    dec.input = bytes;
    dec.error = error;
    auto res = dec.decode();
    if (!res && error && error->empty())
        *error = "Failed to decode MessagePack binary at offset " + std::to_string(dec.pos);
    return res;
}

} // namespace Json

namespace JsonRpc
{

Json::Value Response::toJson() const
{
    Json::Object obj;
    obj.emplace_back("jsonrpc", Json::Value("2.0"));
    obj.emplace_back("id", id.toJson());
    if (error)
        obj.emplace_back("error", *error);
    else if (result)
        obj.emplace_back("result", *result);
    else
        obj.emplace_back("result", Json::Value(nullptr));

    return Json::Value(std::move(obj));
}

std::string Response::serialize() const
{
    return toJson().serialize();
}

std::string Response::toBinaryMsgPack() const
{
    return toJson().toBinaryMsgPack();
}

std::optional<std::string> MessageReader::readMessage(std::istream& in)
{
    auto framed = readFramedMessage(in);
    if (!framed)
        return std::nullopt;
    return framed->payload;
}

std::optional<FramedMessage> MessageReader::readFramedMessage(std::istream& in)
{
    std::string headerLine;
    int contentLength = -1;
    TransportFormat format = TransportFormat::TextJson;

    while (std::getline(in, headerLine))
    {
        // Trim \r
        if (!headerLine.empty() && headerLine.back() == '\r')
            headerLine.pop_back();

        if (headerLine.empty())
        {
            // Empty line marks end of headers
            if (contentLength < 0)
                return std::nullopt;
            break;
        }

        constexpr std::string_view prefix = "Content-Length: ";
        if (headerLine.compare(0, prefix.size(), prefix) == 0)
        {
            contentLength = atoi(headerLine.c_str() + prefix.size());
        }

        constexpr std::string_view ctPrefix = "Content-Type: ";
        if (headerLine.compare(0, ctPrefix.size(), ctPrefix) == 0)
        {
            std::string ct = headerLine.substr(ctPrefix.size());
            if (ct.find("msgpack") != std::string::npos || ct.find("binary-json") != std::string::npos)
            {
                format = TransportFormat::BinaryMsgPack;
            }
        }
    }

    if (contentLength <= 0)
        return std::nullopt;

    std::string content(contentLength, '\0');
    in.read(&content[0], contentLength);
    if (in.gcount() != contentLength)
        return std::nullopt;

    FramedMessage result;
    result.format = format;
    result.payload = std::move(content);
    return result;
}

void MessageWriter::writeMessage(std::ostream& out, const std::string& content, TransportFormat format)
{
    if (format == TransportFormat::BinaryMsgPack)
    {
        out << "Content-Length: " << content.size() << "\r\nContent-Type: application/msgpack\r\n\r\n";
    }
    else
    {
        out << "Content-Length: " << content.size() << "\r\n\r\n";
    }
    out.write(content.data(), content.size());
    out.flush();
}

void MessageWriter::writeResponse(std::ostream& out, const Response& response, TransportFormat format)
{
    if (format == TransportFormat::BinaryMsgPack)
    {
        writeMessage(out, response.toBinaryMsgPack(), format);
    }
    else
    {
        writeMessage(out, response.serialize(), format);
    }
}

void MessageWriter::writeNotification(std::ostream& out, const std::string& method, const Json::Value& params, TransportFormat format)
{
    Json::Object obj;
    obj.emplace_back("jsonrpc", Json::Value("2.0"));
    obj.emplace_back("method", Json::Value(method));
    obj.emplace_back("params", params);

    Json::Value val(std::move(obj));
    if (format == TransportFormat::BinaryMsgPack)
    {
        writeMessage(out, val.toBinaryMsgPack(), format);
    }
    else
    {
        writeMessage(out, val.serialize(), format);
    }
}

} // namespace JsonRpc
} // namespace Luau
