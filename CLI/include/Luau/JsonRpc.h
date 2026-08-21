// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Luau
{
namespace Json
{

enum class Type
{
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value
{
    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    int64_t intValue = 0;
    bool isInteger = false;
    std::string stringValue;
    Array arrayValue;
    Object objectValue;

    Value() : type(Type::Null) {}
    Value(std::nullptr_t) : type(Type::Null) {}
    Value(bool b) : type(Type::Boolean), boolValue(b) {}
    Value(int n) : type(Type::Number), numberValue(n), intValue(n), isInteger(true) {}
    Value(unsigned int n) : type(Type::Number), numberValue(n), intValue(n), isInteger(true) {}
    Value(int64_t n) : type(Type::Number), numberValue(static_cast<double>(n)), intValue(n), isInteger(true) {}
    Value(uint64_t n) : type(Type::Number), numberValue(static_cast<double>(n)), intValue(static_cast<int64_t>(n)), isInteger(true) {}
    Value(double n) : type(Type::Number), numberValue(n), intValue(static_cast<int64_t>(n)), isInteger(false) {}
    Value(const char* s) : type(Type::String), stringValue(s ? s : "") {}
    Value(const std::string& s) : type(Type::String), stringValue(s) {}
    Value(std::string&& s) : type(Type::String), stringValue(std::move(s)) {}
    Value(std::string_view s) : type(Type::String), stringValue(s) {}
    Value(const Array& a) : type(Type::Array), arrayValue(a) {}
    Value(Array&& a) : type(Type::Array), arrayValue(std::move(a)) {}
    Value(const Object& o) : type(Type::Object), objectValue(o) {}
    Value(Object&& o) : type(Type::Object), objectValue(std::move(o)) {}

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Boolean; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    bool getBool(bool defaultValue = false) const { return isBool() ? boolValue : defaultValue; }
    double getDouble(double defaultValue = 0.0) const { return isNumber() ? numberValue : defaultValue; }
    int64_t getInt(int64_t defaultValue = 0) const { return isNumber() ? intValue : defaultValue; }
    const std::string& getString() const { return stringValue; }
    const Array& getArray() const { return arrayValue; }
    const Object& getObject() const { return objectValue; }

    const Value* get(const std::string& key) const;
    const Value* get(size_t index) const;

    bool has(const std::string& key) const { return get(key) != nullptr; }

    std::string serialize(bool pretty = false, int indent = 0) const;
    std::string toBinaryMsgPack() const;
};

std::optional<Value> parse(std::string_view json, std::string* error = nullptr);
std::optional<Value> parseBinaryMsgPack(std::string_view bytes, std::string* error = nullptr);

} // namespace Json

namespace JsonRpc
{

enum class TransportFormat
{
    TextJson,
    BinaryMsgPack,
};

struct FramedMessage
{
    TransportFormat format = TransportFormat::TextJson;
    std::string payload;
};

struct Id
{
    enum class Kind { None, Number, String } kind = Kind::None;
    int64_t numberId = 0;
    std::string stringId;

    Id() : kind(Kind::None) {}
    Id(int64_t n) : kind(Kind::Number), numberId(n) {}
    Id(const std::string& s) : kind(Kind::String), stringId(s) {}

    bool isNone() const { return kind == Kind::None; }
    Json::Value toJson() const
    {
        if (kind == Kind::Number)
            return Json::Value(numberId);
        if (kind == Kind::String)
            return Json::Value(stringId);
        return Json::Value(nullptr);
    }
};

struct Request
{
    Id id;
    std::string method;
    Json::Value params;
    bool isNotification = false;
    TransportFormat format = TransportFormat::TextJson;
};

struct Response
{
    Id id;
    std::optional<Json::Value> result;
    std::optional<Json::Value> error;

    static Response ok(const Id& id, Json::Value result)
    {
        Response r;
        r.id = id;
        r.result = std::move(result);
        return r;
    }

    static Response err(const Id& id, int code, const std::string& message, std::optional<Json::Value> data = std::nullopt)
    {
        Response r;
        r.id = id;
        Json::Object errObj;
        errObj.emplace_back("code", Json::Value(code));
        errObj.emplace_back("message", Json::Value(message));
        if (data)
            errObj.emplace_back("data", std::move(*data));
        r.error = Json::Value(std::move(errObj));
        return r;
    }

    std::string serialize() const;
    std::string toBinaryMsgPack() const;
    Json::Value toJson() const;
};

struct MessageReader
{
    static std::optional<std::string> readMessage(std::istream& in);
    static std::optional<FramedMessage> readFramedMessage(std::istream& in);
};

struct MessageWriter
{
    static void writeMessage(std::ostream& out, const std::string& content, TransportFormat format = TransportFormat::TextJson);
    static void writeResponse(std::ostream& out, const Response& response, TransportFormat format = TransportFormat::TextJson);
    static void writeNotification(std::ostream& out, const std::string& method, const Json::Value& params, TransportFormat format = TransportFormat::TextJson);
};

} // namespace JsonRpc
} // namespace Luau
