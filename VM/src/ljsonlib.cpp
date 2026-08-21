// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"
#include <cmath>
#include <string>
#include <vector>
#include <sstream>
#include <cctype>

#ifndef LUA_JSONLIBNAME
#define LUA_JSONLIBNAME "json"
#endif

// Lightuserdata sentinel for JSON null
static char json_null_marker_dummy = 0;
static const void* json_null_marker = &json_null_marker_dummy;

struct EncodeState {
    lua_State* L;
    std::string out;
    int indent;
    int current_indent;
    std::vector<const void*> visited;

    void indent_line() {
        if (indent > 0) {
            out += '\n';
            for (int i = 0; i < current_indent; ++i) {
                out += ' ';
            }
        }
    }

    void encode_value(int idx) {
        int type = lua_type(L, idx);
        switch (type) {
            case LUA_TNIL:
                out += "null";
                break;
            case LUA_TBOOLEAN:
                out += lua_toboolean(L, idx) ? "true" : "false";
                break;
            case LUA_TNUMBER: {
                double n = lua_tonumber(L, idx);
                if (std::isnan(n) || std::isinf(n)) {
                    luaL_error(L, "JSON encode error: NaN or Infinity cannot be encoded");
                }
                char buf[64];
                if (n == std::floor(n)) {
                    snprintf(buf, sizeof(buf), "%.0f", n);
                } else {
                    snprintf(buf, sizeof(buf), "%.14g", n);
                }
                out += buf;
                break;
            }
            case LUA_TSTRING:
            case LUA_TBUFFER: {
                size_t len;
                const char* s;
                if (type == LUA_TBUFFER) {
                    s = (const char*)luaL_checkbuffer(L, idx, &len);
                } else {
                    s = lua_tolstring(L, idx, &len);
                }
                out += '"';
                for (size_t i = 0; i < len; ++i) {
                    unsigned char c = (unsigned char)s[i];
                    switch (c) {
                        case '"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\b': out += "\\b"; break;
                        case '\f': out += "\\f"; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default:
                            if (c < 0x20) {
                                char buf[8];
                                snprintf(buf, sizeof(buf), "\\u%04x", c);
                                out += buf;
                            } else {
                                out += (char)c;
                            }
                    }
                }
                out += '"';
                break;
            }
            case LUA_TLIGHTUSERDATA: {
                if (lua_touserdata(L, idx) == json_null_marker) {
                    out += "null";
                } else {
                    luaL_error(L, "JSON encode error: unsupported lightuserdata");
                }
                break;
            }
            case LUA_TTABLE: {
                const void* p = lua_topointer(L, idx);
                for (const void* v : visited) {
                    if (v == p) {
                        luaL_error(L, "JSON encode error: table cycle detected");
                    }
                }
                visited.push_back(p);

                bool is_array = false;
                bool has_tag = false;

                if (lua_getmetatable(L, idx)) {
                    lua_getfield(L, -1, "__jsontype");
                    if (lua_isstring(L, -1)) {
                        const char* t = lua_tostring(L, -1);
                        if (strcmp(t, "array") == 0) {
                            is_array = true;
                            has_tag = true;
                        } else if (strcmp(t, "object") == 0) {
                            is_array = false;
                            has_tag = true;
                        }
                    }
                    lua_pop(L, 2);
                }

                if (!has_tag) {
                    size_t len = lua_objlen(L, idx);
                    if (len > 0) {
                        is_array = true;
                        lua_pushnil(L);
                        while (lua_next(L, idx) != 0) {
                            if (lua_type(L, -2) != LUA_TNUMBER) {
                                is_array = false;
                                lua_pop(L, 2);
                                break;
                            }
                            double k = lua_tonumber(L, -2);
                            if (k <= 0 || k > (double)len || k != std::floor(k)) {
                                is_array = false;
                                lua_pop(L, 2);
                                break;
                            }
                            lua_pop(L, 1);
                        }
                    } else {
                        // Empty table default: object if any key, else array if tagged
                        is_array = false;
                    }
                }

                if (is_array) {
                    out += '[';
                    current_indent += indent;
                    size_t len = lua_objlen(L, idx);
                    for (size_t i = 1; i <= len; ++i) {
                        if (i > 1) {
                            out += ',';
                        }
                        indent_line();
                        lua_rawgeti(L, idx, (int)i);
                        encode_value(lua_gettop(L));
                        lua_pop(L, 1);
                    }
                    current_indent -= indent;
                    if (len > 0) indent_line();
                    out += ']';
                } else {
                    out += '{';
                    current_indent += indent;
                    bool first = true;
                    lua_pushnil(L);
                    while (lua_next(L, idx) != 0) {
                        if (!first) {
                            out += ',';
                        }
                        first = false;
                        indent_line();

                        // Key must be converted to string
                        if (lua_type(L, -2) == LUA_TSTRING) {
                            encode_value(lua_gettop(L) - 1);
                        } else {
                            out += '"';
                            luaL_Strbuf sb;
                            luaL_buffinit(L, &sb);
                            luaL_addvalueany(&sb, lua_gettop(L) - 1);
                            luaL_pushresult(&sb);
                            out += lua_tostring(L, -1);
                            lua_pop(L, 1);
                            out += '"';
                        }

                        out += ':';
                        if (indent > 0) out += ' ';

                        encode_value(lua_gettop(L));
                        lua_pop(L, 1);
                    }
                    current_indent -= indent;
                    if (!first) indent_line();
                    out += '}';
                }

                visited.pop_back();
                break;
            }
            default:
                luaL_error(L, "JSON encode error: unsupported type %s", luaL_typename(L, idx));
        }
    }
};

static int json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    int indent = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "indent");
        if (lua_isnumber(L, -1)) {
            indent = (int)lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    } else if (lua_isnumber(L, 2)) {
        indent = (int)lua_tonumber(L, 2);
    }

    EncodeState state;
    state.L = L;
    state.indent = indent > 0 ? indent : 0;
    state.current_indent = 0;

    state.encode_value(1);

    lua_pushlstring(L, state.out.data(), state.out.size());
    return 1;
}

static int json_pretty(lua_State* L) {
    luaL_checkany(L, 1);
    EncodeState state;
    state.L = L;
    state.indent = 2;
    state.current_indent = 0;
    state.encode_value(1);
    lua_pushlstring(L, state.out.data(), state.out.size());
    return 1;
}

struct DecodeState {
    const char* p;
    const char* end;
    lua_State* L;
    std::string error;

    void skip_whitespace() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
    }

    bool parse_value() {
        skip_whitespace();
        if (p >= end) {
            error = "unexpected end of input";
            return false;
        }

        switch (*p) {
            case 'n': return parse_null();
            case 't': return parse_true();
            case 'f': return parse_false();
            case '"': return parse_string();
            case '[': return parse_array();
            case '{': return parse_object();
            default:
                if (*p == '-' || (*p >= '0' && *p <= '9')) {
                    return parse_number();
                }
                error = std::string("unexpected character: '") + *p + "'";
                return false;
        }
    }

    bool parse_null() {
        if (end - p >= 4 && strncmp(p, "null", 4) == 0) {
            p += 4;
            lua_pushlightuserdata(L, (void*)json_null_marker);
            return true;
        }
        error = "invalid null literal";
        return false;
    }

    bool parse_true() {
        if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
            p += 4;
            lua_pushboolean(L, 1);
            return true;
        }
        error = "invalid true literal";
        return false;
    }

    bool parse_false() {
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
            p += 5;
            lua_pushboolean(L, 0);
            return true;
        }
        error = "invalid false literal";
        return false;
    }

    bool parse_number() {
        const char* start = p;
        if (*p == '-') p++;
        if (p >= end || !isdigit((unsigned char)*p)) {
            error = "invalid number format";
            return false;
        }
        while (p < end && isdigit((unsigned char)*p)) p++;
        if (p < end && *p == '.') {
            p++;
            if (p >= end || !isdigit((unsigned char)*p)) {
                error = "invalid fractional number";
                return false;
            }
            while (p < end && isdigit((unsigned char)*p)) p++;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            p++;
            if (p < end && (*p == '+' || *p == '-')) p++;
            if (p >= end || !isdigit((unsigned char)*p)) {
                error = "invalid exponent in number";
                return false;
            }
            while (p < end && isdigit((unsigned char)*p)) p++;
        }

        std::string num_str(start, p - start);
        char* endptr = nullptr;
        double n = strtod(num_str.c_str(), &endptr);
        lua_pushnumber(L, n);
        return true;
    }

    bool parse_string() {
        p++; // skip opening quote
        std::string s;
        while (p < end) {
            unsigned char c = (unsigned char)*p++;
            if (c == '"') {
                lua_pushlstring(L, s.data(), s.size());
                return true;
            }
            if (c == '\\') {
                if (p >= end) {
                    error = "unexpected end of escape sequence";
                    return false;
                }
                char esc = *p++;
                switch (esc) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        if (end - p < 4) {
                            error = "invalid unicode escape";
                            return false;
                        }
                        unsigned int codepoint = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            codepoint <<= 4;
                            if (h >= '0' && h <= '9') codepoint |= (h - '0');
                            else if (h >= 'a' && h <= 'f') codepoint |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') codepoint |= (h - 'A' + 10);
                            else {
                                error = "invalid hex in unicode escape";
                                return false;
                            }
                        }
                        if (codepoint <= 0x7F) {
                            s += (char)codepoint;
                        } else if (codepoint <= 0x7FF) {
                            s += (char)(0xC0 | ((codepoint >> 6) & 0x1F));
                            s += (char)(0x80 | (codepoint & 0x3F));
                        } else {
                            s += (char)(0xE0 | ((codepoint >> 12) & 0x0F));
                            s += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                            s += (char)(0x80 | (codepoint & 0x3F));
                        }
                        break;
                    }
                    default:
                        error = "unknown escape sequence";
                        return false;
                }
            } else {
                s += (char)c;
            }
        }
        error = "unterminated string";
        return false;
    }

    bool parse_array() {
        p++; // skip '['
        lua_newtable(L);
        int index = 1;
        skip_whitespace();
        if (p < end && *p == ']') {
            p++;
            return true;
        }

        while (p < end) {
            if (!parse_value()) return false;
            lua_rawseti(L, -2, index++);
            skip_whitespace();
            if (p >= end) break;
            if (*p == ']') {
                p++;
                return true;
            }
            if (*p == ',') {
                p++;
            } else {
                error = "expected ',' or ']' in array";
                return false;
            }
        }
        error = "unterminated array";
        return false;
    }

    bool parse_object() {
        p++; // skip '{'
        lua_newtable(L);
        skip_whitespace();
        if (p < end && *p == '}') {
            p++;
            return true;
        }

        while (p < end) {
            skip_whitespace();
            if (p >= end || *p != '"') {
                error = "expected string key in object";
                return false;
            }
            if (!parse_string()) return false;
            skip_whitespace();
            if (p >= end || *p != ':') {
                error = "expected ':' after key in object";
                return false;
            }
            p++; // skip ':'
            if (!parse_value()) return false;
            lua_rawset(L, -3);
            skip_whitespace();
            if (p >= end) break;
            if (*p == '}') {
                p++;
                return true;
            }
            if (*p == ',') {
                p++;
            } else {
                error = "expected ',' or '}' in object";
                return false;
            }
        }
        error = "unterminated object";
        return false;
    }
};

static int json_decode(lua_State* L) {
    size_t len;
    const char* str;
    if (lua_type(L, 1) == LUA_TBUFFER) {
        str = (const char*)luaL_checkbuffer(L, 1, &len);
    } else {
        str = luaL_checklstring(L, 1, &len);
    }

    DecodeState state;
    state.p = str;
    state.end = str + len;
    state.L = L;

    if (!state.parse_value()) {
        lua_pushnil(L);
        lua_pushlstring(L, state.error.data(), state.error.size());
        return 2;
    }

    state.skip_whitespace();
    if (state.p != state.end) {
        lua_pop(L, 1);
        lua_pushnil(L);
        std::string err = "trailing garbage after JSON";
        lua_pushlstring(L, err.data(), err.size());
        return 2;
    }

    return 1;
}

static int json_valid(lua_State* L) {
    size_t len;
    const char* str;
    if (lua_type(L, 1) == LUA_TBUFFER) {
        str = (const char*)luaL_checkbuffer(L, 1, &len);
    } else if (lua_isstring(L, 1)) {
        str = lua_tolstring(L, 1, &len);
    } else {
        lua_pushboolean(L, 0);
        return 1;
    }

    DecodeState state;
    state.p = str;
    state.end = str + len;
    state.L = L;

    int top = lua_gettop(L);
    bool ok = state.parse_value();
    if (ok) {
        state.skip_whitespace();
        if (state.p != state.end) ok = false;
    }

    // Clean up any values pushed during parsing
    lua_settop(L, top);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int json_array(lua_State* L) {
    int n = lua_gettop(L);
    lua_createtable(L, n, 0);
    for (int i = 1; i <= n; ++i) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i);
    }
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "array");
    lua_setfield(L, -2, "__jsontype");
    lua_setmetatable(L, -2);
    return 1;
}

static int json_object(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushvalue(L, 1);
    } else {
        lua_newtable(L);
    }
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "object");
    lua_setfield(L, -2, "__jsontype");
    lua_setmetatable(L, -2);
    return 1;
}

static const luaL_Reg jsonlib[] = {
    {"encode", json_encode},
    {"decode", json_decode},
    {"pretty", json_pretty},
    {"valid", json_valid},
    {"array", json_array},
    {"object", json_object},
    {NULL, NULL}
};

int luaopen_json(lua_State* L) {
    luaL_register(L, LUA_JSONLIBNAME, jsonlib);

    lua_pushlightuserdata(L, (void*)json_null_marker);
    lua_setfield(L, -2, "null");

    return 1;
}
