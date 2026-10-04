// src/sapi.cpp —— Elem 访问器 + DOM → Json + DOM 序列化
#include "sapi.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <vector>

namespace sb {

// ==========================================================================
// Elem
// ==========================================================================

bool Elem::is_object()  const { return m_ok && m_e.type() == simdjson::dom::element_type::OBJECT; }
bool Elem::is_array()   const { return m_ok && m_e.type() == simdjson::dom::element_type::ARRAY; }
bool Elem::is_string()  const { return m_ok && m_e.type() == simdjson::dom::element_type::STRING; }
bool Elem::is_bool()    const { return m_ok && m_e.type() == simdjson::dom::element_type::BOOL; }
bool Elem::is_number()  const {
    if (!m_ok) return false;
    auto t = m_e.type();
    return t == simdjson::dom::element_type::INT64 ||
           t == simdjson::dom::element_type::UINT64 ||
           t == simdjson::dom::element_type::DOUBLE;
}
bool Elem::is_integer() const {
    if (!m_ok) return false;
    auto t = m_e.type();
    return t == simdjson::dom::element_type::INT64 ||
           t == simdjson::dom::element_type::UINT64;
}
bool Elem::is_float() const { return m_ok && m_e.type() == simdjson::dom::element_type::DOUBLE; }
bool Elem::is_null()  const { return m_ok && m_e.type() == simdjson::dom::element_type::NULL_VALUE; }

Elem Elem::at(std::string_view key) const {
    if (!is_object()) return Elem();
    auto r = m_e.at_key(key);
    if (r.error()) return Elem();
    return Elem(r.value());
}

Elem Elem::op(size_t i) const {
    if (!is_array()) return Elem();
    simdjson::dom::array a;
    if (m_e.get_array().get(a)) return Elem();
    size_t n = 0;
    for (auto e : a) {
        if (n == i) return Elem(e);
        ++n;
    }
    return Elem();
}

bool Elem::contains(std::string_view key) const {
    return is_object() && m_e[key].error() == simdjson::SUCCESS;
}

size_t Elem::size() const {
    if (is_array())  return m_e.get_array().value().size();
    if (is_object()) return m_e.get_object().value().size();
    return 0;
}

std::string_view Elem::sv() const {
    if (!is_string()) return {};
    simdjson::dom::element e = m_e;
    auto r = e.get_string();
    if (r.error()) return {};
    return r.value();
}

std::string Elem::str() const { return std::string(sv()); }

long long Elem::i64() const {
    switch (m_e.type()) {
        case simdjson::dom::element_type::INT64:  return m_e.get_int64().value();
        case simdjson::dom::element_type::UINT64: return (long long)m_e.get_uint64().value();
        case simdjson::dom::element_type::DOUBLE: return (long long)m_e.get_double().value();
        default: return 0;
    }
}

double Elem::f64() const {
    switch (m_e.type()) {
        case simdjson::dom::element_type::INT64:  return (double)m_e.get_int64().value();
        case simdjson::dom::element_type::UINT64: return (double)m_e.get_uint64().value();
        case simdjson::dom::element_type::DOUBLE: return m_e.get_double().value();
        default: return 0;
    }
}

bool Elem::b() const { return is_bool() ? m_e.get_bool().value() : false; }

long long Elem::i64At(std::string_view key, long long def) const {
    Elem v = at(key);
    return v.is_number() ? v.i64() : def;
}
double Elem::f64At(std::string_view key, double def) const {
    Elem v = at(key);
    return v.is_number() ? v.f64() : def;
}
bool Elem::bAt(std::string_view key, bool def) const {
    Elem v = at(key);
    return v.is_bool() ? v.b() : def;
}

// ==========================================================================
// 序列化小工具（与 jdoc.cpp 里 nlohmann 风格的规则保持一致）
// ==========================================================================

// 对象键排序列表（对齐 nlohmann 的 std::map 迭代顺序）
std::vector<std::string> sortedKeysOf(const simdjson::dom::object& o) {
    std::vector<std::string> keys;
    keys.reserve(o.size());
    for (auto f : o) keys.emplace_back(f.key);
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::pair<std::string_view, simdjson::dom::element>>
sortedEntriesOf(const simdjson::dom::object& o) {
    std::vector<std::pair<std::string_view, simdjson::dom::element>> out;
    out.reserve(o.size());
    for (auto f : o) out.emplace_back(f.key, f.value);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void writeJsonString(std::string& out, std::string_view s) {
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
                    out += (char)c;
                }
        }
    }
    out += '"';
}

static std::string doubleStr(double d) {
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

// ==========================================================================
// DOM 序列化（对象键排序，对齐 nlohmann）
// ==========================================================================

namespace {

void writeNumber(std::string& out, const simdjson::dom::element& e) {
    switch (e.type()) {
        case simdjson::dom::element_type::INT64:
            out += std::to_string(e.get_int64().value());
            return;
        case simdjson::dom::element_type::UINT64:
            out += std::to_string(e.get_uint64().value());
            return;
        case simdjson::dom::element_type::DOUBLE:
            out += doubleStr(e.get_double().value());
            return;
        default:
            out += "null";
            return;
    }
}

// 收集对象键（排序后逐个输出）
struct KV { std::string key; simdjson::dom::element value; };

std::vector<KV> sortedPairs(const simdjson::dom::element& e) {
    std::vector<KV> out;
    simdjson::dom::object o;
    if (e.get_object().get(o)) return out;
    for (auto f : o) out.push_back({std::string(f.key), f.value});
    std::sort(out.begin(), out.end(),
              [](const KV& a, const KV& b) { return a.key < b.key; });
    return out;
}

void writeCompact(const simdjson::dom::element& e, std::string& out) {
    using T = simdjson::dom::element_type;
    switch (e.type()) {
        case T::STRING: {
            std::string_view sv = e.get_string().value();
            writeJsonString(out, sv);
            return;
        }
        case T::BOOL:   out += e.get_bool().value() ? "true" : "false"; return;
        case T::NULL_VALUE: out += "null"; return;
        case T::INT64: case T::UINT64: case T::DOUBLE:
            writeNumber(out, e);
            return;
        case T::ARRAY: {
            out += '[';
            bool first = true;
            simdjson::dom::array a = e.get_array().value();
            for (auto x : a) {
                if (!first) out += ',';
                first = false;
                writeCompact(x, out);
            }
            out += ']';
            return;
        }
        case T::OBJECT: {
            out += '{';
            bool first = true;
            for (auto& kv : sortedPairs(e)) {
                if (!first) out += ',';
                first = false;
                writeJsonString(out, kv.key);
                out += ':';
                writeCompact(kv.value, out);
            }
            out += '}';
            return;
        }
        default:
            out += "null";
            return;
    }
}

void writePretty(const simdjson::dom::element& e, int level, int indent,
                 std::string& out) {
    using T = simdjson::dom::element_type;
    switch (e.type()) {
        case T::ARRAY: {
            if (e.get_array().value().size() == 0) { out += "[]"; return; }
            out += "[\n";
            simdjson::dom::array a = e.get_array().value();
            size_t idx = 0, n = a.size();
            for (auto x : a) {
                out.append((size_t)((level + 1) * indent), ' ');
                writePretty(x, level + 1, indent, out);
                if (idx + 1 < n) out += ',';
                out += '\n';
                ++idx;
            }
            out.append((size_t)(level * indent), ' ');
            out += ']';
            return;
        }
        case T::OBJECT: {
            auto pairs = sortedPairs(e);
            if (pairs.empty()) { out += "{}"; return; }
            out += "{\n";
            for (size_t i = 0; i < pairs.size(); ++i) {
                out.append((size_t)((level + 1) * indent), ' ');
                writeJsonString(out, pairs[i].key);
                out += ": ";
                writePretty(pairs[i].value, level + 1, indent, out);
                if (i + 1 < pairs.size()) out += ',';
                out += '\n';
            }
            out.append((size_t)(level * indent), ' ');
            out += '}';
            return;
        }
        default:
            writeCompact(e, out);
            return;
    }
}

} // namespace

std::string compactJson(const simdjson::dom::element& e) {
    std::string out;
    writeCompact(e, out);
    return out;
}

std::string prettyJson(const simdjson::dom::element& e, int indent) {
    std::string out;
    writePretty(e, 0, indent, out);
    return out;
}

// ==========================================================================
// DOM → Json（深拷贝）
// ==========================================================================

namespace {

Json jsonFromElement(const simdjson::dom::element& e) {
    using T = simdjson::dom::element_type;
    switch (e.type()) {
        case T::STRING: {
            std::string_view sv = e.get_string().value();
            return Json(std::string(sv));
        }
        case T::BOOL:   return Json(e.get_bool().value());
        case T::INT64:  return Json(e.get_int64().value());
        case T::UINT64: return Json(e.get_uint64().value());
        case T::DOUBLE: return Json(e.get_double().value());
        case T::NULL_VALUE: return Json(nullptr);
        case T::ARRAY: {
            Json out = Json::array();
            simdjson::dom::array a = e.get_array().value();
            for (auto x : a) out.push_back(jsonFromElement(x));
            return out;
        }
        case T::OBJECT: {
            Json out = Json::object();
            simdjson::dom::object o = e.get_object().value();
            for (auto f : o) out[std::string(f.key)] = jsonFromElement(f.value);
            return out;
        }
        default:
            return Json(nullptr);
    }
}

} // namespace

Json toJson(const Elem& e) {
    if (!e.ok()) return Json(nullptr);
    return jsonFromElement(e.raw());
}

} // namespace sb