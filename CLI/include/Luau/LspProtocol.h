// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/JsonRpc.h"
#include "Luau/Location.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Luau
{
namespace Lsp
{

struct Position
{
    int line = 0;      // 0-indexed
    int character = 0; // 0-indexed UTF-16 code units or UTF-8 offset

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("line", Json::Value(line));
        o.emplace_back("character", Json::Value(character));
        return Json::Value(std::move(o));
    }

    static Position fromJson(const Json::Value& v)
    {
        Position p;
        if (const auto* l = v.get("line"))
            p.line = static_cast<int>(l->getInt(0));
        if (const auto* c = v.get("character"))
            p.character = static_cast<int>(c->getInt(0));
        return p;
    }

    Luau::Position toLuau() const
    {
        return Luau::Position{static_cast<unsigned int>(line), static_cast<unsigned int>(character)};
    }

    static Position fromLuau(const Luau::Position& p)
    {
        return Position{static_cast<int>(p.line), static_cast<int>(p.column)};
    }
};

struct Range
{
    Position start;
    Position end;

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("start", start.toJson());
        o.emplace_back("end", end.toJson());
        return Json::Value(std::move(o));
    }

    static Range fromJson(const Json::Value& v)
    {
        Range r;
        if (const auto* s = v.get("start"))
            r.start = Position::fromJson(*s);
        if (const auto* e = v.get("end"))
            r.end = Position::fromJson(*e);
        return r;
    }

    static Range fromLuau(const Luau::Location& loc)
    {
        Range r;
        r.start = Position::fromLuau(loc.begin);
        r.end = Position::fromLuau(loc.end);
        return r;
    }
};

struct Location
{
    std::string uri;
    Range range;

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("uri", Json::Value(uri));
        o.emplace_back("range", range.toJson());
        return Json::Value(std::move(o));
    }
};

enum class DiagnosticSeverity
{
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

struct Diagnostic
{
    Range range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string source = "luau";
    std::string message;

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("range", range.toJson());
        o.emplace_back("severity", Json::Value(static_cast<int>(severity)));
        if (!code.empty())
            o.emplace_back("code", Json::Value(code));
        o.emplace_back("source", Json::Value(source));
        o.emplace_back("message", Json::Value(message));
        return Json::Value(std::move(o));
    }
};

enum class CompletionItemKind
{
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Property = 10,
    Unit = 11,
    Value = 12,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    Color = 16,
    File = 17,
    Reference = 18,
    Folder = 19,
    EnumMember = 20,
    Constant = 21,
    Struct = 22,
    Event = 23,
    Operator = 24,
    TypeParameter = 25,
};

enum class InsertTextFormat
{
    PlainText = 1,
    Snippet = 2,
};

struct CompletionItem
{
    std::string label;
    CompletionItemKind kind = CompletionItemKind::Text;
    std::string detail;
    std::string documentation;
    std::string insertText;
    InsertTextFormat insertTextFormat = InsertTextFormat::PlainText;
    std::string sortText;
    std::string filterText;
    bool deprecated = false;
    std::optional<Json::Value> data = std::nullopt;
    std::vector<std::string> commitCharacters;

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("label", Json::Value(label));
        o.emplace_back("kind", Json::Value(static_cast<int>(kind)));
        if (!detail.empty())
            o.emplace_back("detail", Json::Value(detail));
        if (!documentation.empty())
        {
            Json::Object doc;
            doc.emplace_back("kind", Json::Value("markdown"));
            doc.emplace_back("value", Json::Value(documentation));
            o.emplace_back("documentation", Json::Value(std::move(doc)));
        }
        if (!insertText.empty())
            o.emplace_back("insertText", Json::Value(insertText));
        if (insertTextFormat != InsertTextFormat::PlainText)
            o.emplace_back("insertTextFormat", Json::Value(static_cast<int>(insertTextFormat)));
        if (!sortText.empty())
            o.emplace_back("sortText", Json::Value(sortText));
        if (!filterText.empty())
            o.emplace_back("filterText", Json::Value(filterText));
        if (deprecated)
            o.emplace_back("deprecated", Json::Value(true));
        if (data)
            o.emplace_back("data", *data);
        if (!commitCharacters.empty())
        {
            Json::Array cc;
            for (const auto& c : commitCharacters)
                cc.push_back(Json::Value(c));
            o.emplace_back("commitCharacters", Json::Value(std::move(cc)));
        }
        return Json::Value(std::move(o));
    }
};

enum class SymbolKind
{
    File = 1,
    Module = 2,
    Namespace = 3,
    Package = 4,
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    String = 15,
    Number = 16,
    Boolean = 17,
    Array = 18,
    Object = 19,
    Key = 20,
    Null = 21,
    EnumMember = 22,
    Struct = 23,
    Event = 24,
    Operator = 25,
    TypeParameter = 26,
};

struct DocumentSymbol
{
    std::string name;
    std::string detail;
    SymbolKind kind = SymbolKind::Variable;
    Range range;
    Range selectionRange;
    std::vector<DocumentSymbol> children;

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("name", Json::Value(name));
        if (!detail.empty())
            o.emplace_back("detail", Json::Value(detail));
        o.emplace_back("kind", Json::Value(static_cast<int>(kind)));
        o.emplace_back("range", range.toJson());
        o.emplace_back("selectionRange", selectionRange.toJson());
        if (!children.empty())
        {
            Json::Array ch;
            for (const auto& c : children)
                ch.push_back(c.toJson());
            o.emplace_back("children", Json::Value(std::move(ch)));
        }
        return Json::Value(std::move(o));
    }
};

struct TextEdit
{
    Range range;
    std::string newText;

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("range", range.toJson());
        o.emplace_back("newText", Json::Value(newText));
        return Json::Value(std::move(o));
    }
};

struct WorkspaceEdit
{
    std::vector<std::pair<std::string, std::vector<TextEdit>>> changes;

    Json::Value toJson() const
    {
        Json::Object o;
        Json::Object changesObj;
        for (const auto& pair : changes)
        {
            Json::Array editsArr;
            for (const auto& e : pair.second)
                editsArr.push_back(e.toJson());
            changesObj.emplace_back(pair.first, Json::Value(std::move(editsArr)));
        }
        o.emplace_back("changes", Json::Value(std::move(changesObj)));
        return Json::Value(std::move(o));
    }
};

struct InlayHint
{
    Position position;
    std::string label;
    int kind = 1; // 1 = Type, 2 = Parameter
    bool paddingLeft = false;
    bool paddingRight = false;

    Json::Value toJson() const
    {
        Json::Object o;
        o.emplace_back("position", position.toJson());
        o.emplace_back("label", Json::Value(label));
        o.emplace_back("kind", Json::Value(kind));
        if (paddingLeft)
            o.emplace_back("paddingLeft", Json::Value(true));
        if (paddingRight)
            o.emplace_back("paddingRight", Json::Value(true));
        return Json::Value(std::move(o));
    }
};

// URI conversion utilities
inline std::string uriToPath(std::string_view uri)
{
    if (uri.rfind("file://", 0) == 0)
    {
        std::string_view path = uri.substr(7);
#ifdef _WIN32
        if (path.size() >= 3 && path[0] == '/' && isalpha(path[1]) && path[2] == ':')
            path = path.substr(1);
#endif
        // Decode percent-encoded characters
        std::string decoded;
        decoded.reserve(path.size());
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (path[i] == '%' && i + 2 < path.size())
            {
                auto hexVal = [](char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int h1 = hexVal(path[i + 1]);
                int h2 = hexVal(path[i + 2]);
                if (h1 != -1 && h2 != -1)
                {
                    decoded.push_back(static_cast<char>((h1 << 4) | h2));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(path[i]);
        }
        return decoded;
    }
    return std::string(uri);
}

inline std::string pathToUri(std::string_view path)
{
    if (path.rfind("file://", 0) == 0)
        return std::string(path);

    std::string uri = "file://";
#ifdef _WIN32
    if (!path.empty() && path[0] != '/')
        uri.push_back('/');
#endif
    for (char c : path)
    {
        if (c == '\\')
        {
            uri.push_back('/');
        }
        else if (isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' || c == '-' || c == '_' || c == '~' || c == ':')
        {
            uri.push_back(c);
        }
        else
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            uri.append(buf);
        }
    }
    return uri;
}

} // namespace Lsp
} // namespace Luau
