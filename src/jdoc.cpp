// src/jdoc.cpp
// sb::Json 的实现：simdjson 解析（DOM 拷贝在 sapi.cpp）+ 序列化 + 取值。
#include "jdoc.hpp"
#include "sapi.hpp"
#include "simdjson.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace sb {

// ==========================================================================
// 构造
// ==========================================================================

Json::Json(std::initializer_list<Json> v)
    : m_kind(Kind::Array), m_arr(std::make_shared<ArrT>()) {
    m_arr->reserve(v.size());
    for (const Json& x : v) m_arr->push_back(x);
}

Json::Json(const std::vector<std::string>& v)
    : m_kind(Kind::Array), m_arr(std::make_shared<ArrT>()) {
    m_arr->reserve(v.size());
    for (const std::string& s : v) m_arr->emplace_back(s);
}

// ==========================================================================
// 解析：simdjson（DOM → Json 的深拷贝在 sapi.cpp 的 toJson 里）
// ==========================================================================

Json Json::parse(const std::string& text) {
    try {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::error_code err = parser.parse(text).get(root);
        if (err) throw simdjson::simdjson_error(err);
        return toJson(Elem(root));
    } catch (const simdjson::simdjson_error& e) {
        throw parse_error(e.what());
    }
}

// ==========================================================================
// get<T>
// ==========================================================================

template <> std::string Json::get<std::string>() const {
    if (!is_string()) throw type_error("值不是字符串");
    return m_str;
}

template <> bool Json::get<bool>() const {
    if (!is_boolean()) throw type_error("值不是布尔");
    return m_bool;
}

template <> int Json::get<int>() const {
    switch (m_kind) {
        case Kind::Int:    return (int)m_int;
        case Kind::UInt:   return (int)m_uint;
        case Kind::Double: return (int)m_double;
        default: throw type_error("值不是整数");
    }
}

template <> long long Json::get<long long>() const {
    switch (m_kind) {
        case Kind::Int:    return m_int;
        case Kind::UInt:   return (long long)m_uint;
        case Kind::Double: return (long long)m_double;
        default: throw type_error("值不是整数");
    }
}

template <> unsigned long long Json::get<unsigned long long>() const {
    switch (m_kind) {
        case Kind::Int:    return (unsigned long long)m_int;
        case Kind::UInt:   return m_uint;
        case Kind::Double: return (unsigned long long)m_double;
        default: throw type_error("值不是整数");
    }
}

template <> double Json::get<double>() const {
    switch (m_kind) {
        case Kind::Int:    return (double)m_int;
        case Kind::UInt:   return (double)m_uint;
        case Kind::Double: return m_double;
        default: throw type_error("值不是数字");
    }
}

// ==========================================================================
// value(key, 默认值)
// ==========================================================================

std::string Json::value(const std::string& key, const char* def) const {
    if (!is_object()) return def;
    auto it = m_obj->find(key);
    if (it == m_obj->end()) return def;
    const Json& v = it->second;
    return v.is_string() ? v.m_str : std::string(def);
}

std::string Json::value(const std::string& key, const std::string& def) const {
    if (!is_object()) return def;
    auto it = m_obj->find(key);
    if (it == m_obj->end()) return def;
    const Json& v = it->second;
    return v.is_string() ? v.m_str : def;
}

Json Json::value(const std::string& key, const Json& def) const {
    if (!is_object()) return def;
    auto it = m_obj->find(key);
    return it == m_obj->end() ? def : it->second;
}

bool Json::value(const std::string& key, bool def) const {
    if (!is_object()) return def;
    auto it = m_obj->find(key);
    if (it == m_obj->end()) return def;
    const Json& v = it->second;
    return v.is_boolean() ? v.m_bool : def;
}

int Json::value(const std::string& key, int def) const {
    if (!is_object()) return def;
    auto it = m_obj->find(key);
    if (it == m_obj->end()) return def;
    switch (it->second.m_kind) {
        case Kind::Int:    return (int)it->second.m_int;
        case Kind::UInt:   return (int)it->second.m_uint;
        case Kind::Double: return (int)it->second.m_double;
        default: return def;
    }
}

long long Json::value(const std::string& key, long long def) const {
    if (!is_object()) return def;
    auto it = m_obj->find(key);
    if (it == m_obj->end()) return def;
    switch (it->second.m_kind) {
        case Kind::Int:    return it->second.m_int;
        case Kind::UInt:   return (long long)it->second.m_uint;
        case Kind::Double: return (long long)it->second.m_double;
        default: return def;
    }
}

double Json::value(const std::string& key, double def) const {
    if (!is_object()) return def;
    auto it = m_obj->find(key);
    if (it == m_obj->end()) return def;
    switch (it->second.m_kind) {
        case Kind::Int:    return (double)it->second.m_int;
        case Kind::UInt:   return (double)it->second.m_uint;
        case Kind::Double: return it->second.m_double;
        default: return def;
    }
}

// ==========================================================================
// 序列化
// ==========================================================================

namespace {

void writeJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;   // 其余（含中文等非 ASCII）原样 UTF-8
                }
        }
    }
    out += '"';
}

// 最短往返表示的 double（对齐 nlohmann：整数值补 ".0"）
std::string doubleStr(double d) {
    if (std::isnan(d)) return "null";
    if (std::isinf(d)) return d < 0 ? "-1e+999" : "1e+999";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    if (res.ec == std::errc()) {
        std::string s(buf, res.ptr);
        if (s.find('.') == std::string::npos &&
            s.find('e') == std::string::npos &&
            s.find('E') == std::string::npos)
            s += ".0";
        return s;
    }
    std::snprintf(buf, sizeof(buf), "%g", d);
    return buf;
}

void writeCompact(const Json& j, std::string& out) {
    switch (j.kind()) {
        case Json::Kind::Null:   out += "null"; return;
        case Json::Kind::Bool:   out += j.get<bool>() ? "true" : "false"; return;
        case Json::Kind::Int:    out += std::to_string(j.get<long long>()); return;
        case Json::Kind::UInt:   out += std::to_string(j.get<unsigned long long>()); return;
        case Json::Kind::Double: out += doubleStr(j.get<double>()); return;
        case Json::Kind::Str:    writeJsonString(out, j.get<std::string>()); return;
        case Json::Kind::Array: {
            out += '[';
            bool first = true;
            for (const Json& x : j) {
                if (!first) out += ',';
                first = false;
                writeCompact(x, out);
            }
            out += ']';
            return;
        }
        case Json::Kind::Object: {
            out += '{';
            bool first = true;
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (!first) out += ',';
                first = false;
                writeJsonString(out, it.key());
                out += ':';
                writeCompact(it.value(), out);
            }
            out += '}';
            return;
        }
    }
}

// level = 当前元素所在层级（缩进 = level * indent 个空格）
void writePretty(const Json& j, int level, int indent, std::string& out) {
    switch (j.kind()) {
        case Json::Kind::Array: {
            if (j.empty()) { out += "[]"; return; }
            out += "[\n";
            auto e = j.end();
            for (auto it = j.begin(); it != e; ++it) {
                out.append((size_t)((level + 1) * indent), ' ');
                writePretty(*it, level + 1, indent, out);
                auto nx = it;
                ++nx;
                if (nx != e) out += ',';
                out += '\n';
            }
            out.append((size_t)(level * indent), ' ');
            out += ']';
            return;
        }
        case Json::Kind::Object: {
            if (j.empty()) { out += "{}"; return; }
            out += "{\n";
            auto e = j.end();
            for (auto it = j.begin(); it != e; ++it) {
                out.append((size_t)((level + 1) * indent), ' ');
                writeJsonString(out, it.key());
                out += ": ";
                writePretty(it.value(), level + 1, indent, out);
                auto nx = it;
                ++nx;
                if (nx != e) out += ',';
                out += '\n';
            }
            out.append((size_t)(level * indent), ' ');
            out += '}';
            return;
        }
        default:
            writeCompact(j, out);
            return;
    }
}

} // namespace

std::string Json::dump(int indent) const {
    std::string out;
    if (indent > 0) writePretty(*this, 0, indent, out);
    else            writeCompact(*this, out);
    return out;
}

// ==========================================================================
// 比较
// ==========================================================================

namespace {
long double toNumber(const Json& j) {
    switch (j.kind()) {
        case Json::Kind::Int:    return (long double)j.get<long long>();
        case Json::Kind::UInt:   return (long double)j.get<unsigned long long>();
        case Json::Kind::Double: return (long double)j.get<double>();
        default:                 return 0;
    }
}
} // namespace

bool operator==(const Json& a, const Json& b) {
    if (a.m_kind == b.m_kind) {
        switch (a.m_kind) {
            case Json::Kind::Null:   return true;
            case Json::Kind::Bool:   return a.m_bool == b.m_bool;
            case Json::Kind::Int:    return a.m_int == b.m_int;
            case Json::Kind::UInt:   return a.m_uint == b.m_uint;
            case Json::Kind::Double: return a.m_double == b.m_double;
            case Json::Kind::Str:    return a.m_str == b.m_str;
            default:                 return false;  // 数组/对象不逐元素比
        }
    }
    // 跨类型数字比较
    if (a.is_number() && b.is_number()) return toNumber(a) == toNumber(b);
    return false;
}

} // namespace sb