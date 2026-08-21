// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Julia Klee

#include "lualib.h"
#include "lcommon.h"
#include <cmath>
#include <string>
#include <vector>
#include <sstream>

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

                // Check if the table is an array (1-based contiguous integer keys)
                bool is_array = true;
                int max_idx = 0;
                int count = 0;

                int t_idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;

                lua_pushnil(L);
                while (lua_next(L, t_idx) != 0) {
                    if (lua_type(L, -2) == LUA_TNUMBER) {
                        double k = lua_tonumber(L, -2);
                        if (k == std::floor(k) && k >= 1) {
                            if (k > max_idx) max_idx = (int)k;
                        } else {
                            is_array = false;
                            lua_pop(L, 2);
                            break;
                        }
                    } else {
                        is_array = false;
                        lua_pop(L, 2);
                        break;
                    }
                    count++;
                    lua_pop(L, 1);
                }

                if (is_array && count != max_idx) {
                    is_array = false;
                }

                if (is_array) {
                    out += "[";
                    if (max_idx > 0 && indent > 0) out += "\n";
                    current_indent += indent;
                    for (int i = 1; i <= max_idx; ++i) {
                        if (indent > 0) out.append(current_indent, ' ');
                        lua_rawgeti(L, t_idx, i);
                        encode_value(lua_gettop(L));
                        lua_pop(L, 1);
                        if (i < max_idx) {
                            out += ",";
                            if (indent > 0) out += "\n";
                        } else {
                            if (indent > 0) out += "\n";
                        }
                    }
                    current_indent -= indent;
                    if (max_idx > 0 && indent > 0) out.append(current_indent, ' ');
                    out += "]";
                } else {
                    out += "{";
                    bool first = true;
                    lua_pushnil(L);

                    current_indent += indent;
                    while (lua_next(L, t_idx) != 0) {
                        if (!first) {
                            out += ",";
                        }
                        if (indent > 0) out += "\n";
                        if (indent > 0) out.append(current_indent, ' ');
                        first = false;

                        // Encode key
                        if (lua_type(L, -2) == LUA_TSTRING || lua_type(L, -2) == LUA_TBUFFER) {
                            lua_pushvalue(L, -2);
                            encode_value(lua_gettop(L));
                            lua_pop(L, 1);
                        } else if (lua_type(L, -2) == LUA_TNUMBER) {
                            double k = lua_tonumber(L, -2);
                            char buf[64];
                            if (k == std::floor(k)) {
                                snprintf(buf, sizeof(buf), "\"%.0f\"", k);
                            } else {
                                snprintf(buf, sizeof(buf), "\"%.14g\"", k);
                            }
                            out += buf;
                        } else {
                            luaL_error(L, "JSON encode error: table key must be a string or number");
                        }

                        out += (indent > 0) ? ": " : ":";
                        encode_value(lua_gettop(L));
                        lua_pop(L, 1);
                    }
                    current_indent -= indent;
                    if (!first && indent > 0) {
                        out += "\n";
                        out.append(current_indent, ' ');
                    }
                    out += "}";
                }
                visited.pop_back();
                break;
            }
            default:
                luaL_error(L, "JSON encode error: unsupported type %s", lua_typename(L, type));
        }
    }
};

static int json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    int indent = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "indent");
        if (lua_type(L, -1) == LUA_TNUMBER) {
            indent = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    EncodeState state;
    state.L = L;
    state.indent = indent;
    state.current_indent = 0;
    state.encode_value(1);

    lua_pushlstring(L, state.out.c_str(), state.out.length());
    return 1;
}

struct DecodeState {
    lua_State* L;
    const char* str;
    size_t len;
    size_t pos;
    std::string err;

    void skip_whitespace() {
        while (pos < len && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\n' || str[pos] == '\r')) {
            pos++;
        }
    }

    bool decode_value() {
        skip_whitespace();
        if (pos >= len) {
            err = "unexpected end of input";
            return false;
        }

        char c = str[pos];
        if (c == 'n') {
            if (pos + 3 < len && str[pos+1] == 'u' && str[pos+2] == 'l' && str[pos+3] == 'l') {
                lua_pushlightuserdata(L, (void*)json_null_marker);
                pos += 4;
                return true;
            }
            err = "expected 'null'";
            return false;
        } else if (c == 't') {
            if (pos + 3 < len && str[pos+1] == 'r' && str[pos+2] == 'u' && str[pos+3] == 'e') {
                lua_pushboolean(L, 1);
                pos += 4;
                return true;
            }
            err = "expected 'true'";
            return false;
        } else if (c == 'f') {
            if (pos + 4 < len && str[pos+1] == 'a' && str[pos+2] == 'l' && str[pos+3] == 's' && str[pos+4] == 'e') {
                lua_pushboolean(L, 0);
                pos += 5;
                return true;
            }
            err = "expected 'false'";
            return false;
        } else if (c == '"') {
            return decode_string();
        } else if (c == '[') {
            return decode_array();
        } else if (c == '{') {
            return decode_object();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            return decode_number();
        }

        err = "unexpected character";
        return false;
    }

    bool decode_string() {
        pos++; // skip "
        std::string s;
        while (pos < len) {
            char c = str[pos++];
            if (c == '"') {
                lua_pushlstring(L, s.c_str(), s.length());
                return true;
            } else if (c == '\\') {
                if (pos >= len) {
                    err = "unexpected end of input in string escape";
                    return false;
                }
                char esc = str[pos++];
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
                        if (pos + 4 > len) {
                            err = "unexpected end of input in \\u escape";
                            return false;
                        }
                        unsigned int cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char hc = str[pos++];
                            cp <<= 4;
                            if (hc >= '0' && hc <= '9') cp |= (hc - '0');
                            else if (hc >= 'a' && hc <= 'f') cp |= (hc - 'a' + 10);
                            else if (hc >= 'A' && hc <= 'F') cp |= (hc - 'A' + 10);
                            else {
                                err = "invalid \\u escape";
                                return false;
                            }
                        }
                        
                        // Basic utf-8 encode. Assumes valid scalar value.
                        if (cp < 0x80) {
                            s += (char)cp;
                        } else if (cp < 0x800) {
                            s += (char)(0xC0 | (cp >> 6));
                            s += (char)(0x80 | (cp & 0x3F));
                        } else {
                            s += (char)(0xE0 | (cp >> 12));
                            s += (char)(0x80 | ((cp >> 6) & 0x3F));
                            s += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        err = "invalid escape character";
                        return false;
                }
            } else {
                s += c;
            }
        }
        err = "unexpected end of input in string";
        return false;
    }

    bool decode_number() {
        size_t start = pos;
        if (pos < len && str[pos] == '-') pos++;
        while (pos < len && str[pos] >= '0' && str[pos] <= '9') pos++;
        if (pos < len && str[pos] == '.') {
            pos++;
            while (pos < len && str[pos] >= '0' && str[pos] <= '9') pos++;
        }
        if (pos < len && (str[pos] == 'e' || str[pos] == 'E')) {
            pos++;
            if (pos < len && (str[pos] == '+' || str[pos] == '-')) pos++;
            while (pos < len && str[pos] >= '0' && str[pos] <= '9') pos++;
        }
        
        std::string num_str(str + start, pos - start);
        try {
            double num = std::stod(num_str);
            lua_pushnumber(L, num);
            return true;
        } catch (...) {
            err = "invalid number format";
            return false;
        }
    }

    bool decode_array() {
        pos++; // skip [
        lua_newtable(L);
        skip_whitespace();
        if (pos < len && str[pos] == ']') {
            pos++;
            return true;
        }
        int i = 1;
        while (true) {
            if (!decode_value()) {
                return false;
            }
            lua_rawseti(L, -2, i++);
            skip_whitespace();
            if (pos >= len) {
                err = "unexpected end of input in array";
                return false;
            }
            if (str[pos] == ']') {
                pos++;
                break;
            }
            if (str[pos] != ',') {
                err = "expected ',' or ']'";
                return false;
            }
            pos++; // skip ,
        }
        return true;
    }

    bool decode_object() {
        pos++; // skip {
        lua_newtable(L);
        skip_whitespace();
        if (pos < len && str[pos] == '}') {
            pos++;
            return true;
        }
        while (true) {
            skip_whitespace();
            if (pos >= len || str[pos] != '"') {
                err = "expected string key in object";
                return false;
            }
            if (!decode_string()) return false;
            skip_whitespace();
            if (pos >= len || str[pos] != ':') {
                err = "expected ':'";
                return false;
            }
            pos++; // skip :
            if (!decode_value()) return false;
            lua_settable(L, -3); // set key-value pair
            skip_whitespace();
            if (pos >= len) {
                err = "unexpected end of input in object";
                return false;
            }
            if (str[pos] == '}') {
                pos++;
                break;
            }
            if (str[pos] != ',') {
                err = "expected ',' or '}'";
                return false;
            }
            pos++; // skip ,
        }
        return true;
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
    state.L = L;
    state.str = str;
    state.len = len;
    state.pos = 0;

    if (state.decode_value()) {
        state.skip_whitespace();
        if (state.pos < state.len) {
            lua_pushnil(L);
            lua_pushstring(L, "trailing characters found after JSON value");
            return 2;
        }
        return 1; // return the decoded value
    } else {
        lua_pushnil(L);
        lua_pushstring(L, state.err.c_str());
        return 2; // return nil, error_message
    }
}

static const luaL_Reg jsonlib[] = {
    {"encode", json_encode},
    {"decode", json_decode},
    {NULL, NULL}
};

int luaopen_json(lua_State* L) {
    luaL_register(L, LUA_JSONLIBNAME, jsonlib);

    lua_pushlightuserdata(L, (void*)json_null_marker);
    lua_setfield(L, -2, "null");

    return 1;
}
