// src/jdoc.hpp
// sb::Json —— 本工具自用的 JSON 值类型。
//
//   · 解析统一用 simdjson 完成（third_party/simdjson.cpp / simdjson.h，见 jdoc.cpp），
//     不再依赖 nlohmann json.hpp。
//   · simdjson 的 DOM 是只读、且借用解析缓冲区；而这个工具还需要
//     构造输出 JSON、以及 sb2→sb3 的"转换构造"，因此解析完成后
//     拷贝进自有表示（对象用 std::map 保持键序与 nlohmann 的
//     std::map 一致，数组用 std::vector）。
//   · API 覆盖原 Json 在本项目里用到的子集，行为尽量对齐：
//     contains / find / value / is_* / get<> / dump() / dump(2) /
//     统一的 begin()/end() 迭代器（key() / value()）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sb {

class Json {
private:
    using ArrT = std::vector<Json>;
    using ObjT = std::map<std::string, Json>;

public:
    enum class Kind : uint8_t {
        Null, Bool, Int, UInt, Double, Str, Array, Object
    };

    class parse_error : public std::runtime_error {
    public:
        explicit parse_error(const std::string& msg) : std::runtime_error(msg) {}
    };
    class type_error : public std::runtime_error {
    public:
        explicit type_error(const std::string& msg) : std::runtime_error(msg) {}
    };

    // ---- 构造（隐式转换，与 nlohmann 的用法一致）----
    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v)                   : m_kind(Kind::Bool),   m_bool(v) {}
    Json(int v)                    : m_kind(Kind::Int),    m_int(v) {}
    Json(long v)                   : m_kind(Kind::Int),    m_int(v) {}
    Json(long long v)              : m_kind(Kind::Int),    m_int(v) {}
    Json(unsigned v)               : m_kind(Kind::UInt),   m_uint(v) {}
    Json(unsigned long v)          : m_kind(Kind::UInt),   m_uint(v) {}
    Json(unsigned long long v)     : m_kind(Kind::UInt),   m_uint(v) {}
    Json(float v)                  : m_kind(Kind::Double), m_double(v) {}
    Json(double v)                 : m_kind(Kind::Double), m_double(v) {}
    Json(const std::string& v)     : m_kind(Kind::Str),    m_str(v) {}
    Json(std::string&& v)          : m_kind(Kind::Str),    m_str(std::move(v)) {}
    Json(const char* v)            : m_kind(Kind::Str),    m_str(v ? v : "") {}
    // 数组
    Json(std::initializer_list<Json> v);
    Json(const std::vector<std::string>& v);
    // 对象（键转字符串；用于 std::map 直接构造/赋值 Json）
    template <class K, class V>
    Json(const std::map<K, V>& m) : m_kind(Kind::Object), m_obj(std::make_shared<ObjT>()) {
        for (const auto& kv : m) (*m_obj)[std::string(kv.first)] = Json(kv.second);
    }

    Json(const Json& o) { copyFrom(o); }
    Json(Json&&) = default;
    Json& operator=(const Json& o) {
        if (this != &o) copyFrom(o);
        return *this;
    }
    Json& operator=(Json&&) = default;

    static Json object() {
        Json j;
        j.m_kind = Kind::Object;
        j.m_obj = std::make_shared<ObjT>();
        return j;
    }
    static Json array() {
        Json j;
        j.m_kind = Kind::Array;
        j.m_arr = std::make_shared<ArrT>();
        return j;
    }
    static Json array(std::initializer_list<Json> v) { return Json(v); }

    // 解析（内部用 simdjson；失败抛 parse_error）
    static Json parse(const std::string& text);

    // ---- 类型查询 ----
    Kind kind() const { return m_kind; }
    bool is_null() const              { return m_kind == Kind::Null; }
    bool is_boolean() const           { return m_kind == Kind::Bool; }
    bool is_number() const            { return m_kind == Kind::Int || m_kind == Kind::UInt || m_kind == Kind::Double; }
    bool is_number_integer() const    { return m_kind == Kind::Int || m_kind == Kind::UInt; }
    bool is_number_float() const      { return m_kind == Kind::Double; }
    bool is_string() const            { return m_kind == Kind::Str; }
    bool is_array() const             { return m_kind == Kind::Array; }
    bool is_object() const            { return m_kind == Kind::Object; }
    bool is_primitive() const         { return !is_array() && !is_object(); }

    bool empty() const { return size() == 0; }
    size_t size() const {
        switch (m_kind) {
            case Kind::Array:  return m_arr->size();
            case Kind::Object: return m_obj->size();
            case Kind::Str:    return m_str.size();
            default:           return 0;
        }
    }

    bool contains(const std::string& key) const {
        return is_object() && m_obj->find(key) != m_obj->end();
    }

    // ---- 迭代器（对象/数组统一；数组模式支持 std::sort 等随机访问）----
    class iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = Json;
        using difference_type = std::ptrdiff_t;
        using pointer = Json*;
        using reference = Json&;

        iterator() = default;
        bool operator==(const iterator& o) const {
            if (m_mode != o.m_mode) return false;
            if (m_mode == Mode::Arr) return m_a == o.m_a;
            if (m_mode == Mode::Obj) return m_o == o.m_o;
            return true;
        }
        bool operator!=(const iterator& o) const { return !(*this == o); }
        iterator& operator++() {
            if (m_mode == Mode::Arr) ++m_a;
            else if (m_mode == Mode::Obj) ++m_o;
            return *this;
        }
        iterator operator++(int) { iterator t = *this; ++(*this); return t; }
        iterator& operator--() {
            if (m_mode == Mode::Arr) --m_a;
            else if (m_mode == Mode::Obj) --m_o;
            return *this;
        }
        iterator operator--(int) { iterator t = *this; --(*this); return t; }
        iterator& operator+=(difference_type n) { if (m_mode == Mode::Arr) m_a += n; return *this; }
        iterator& operator-=(difference_type n) { if (m_mode == Mode::Arr) m_a -= n; return *this; }
        iterator operator+(difference_type n) const { iterator t = *this; t += n; return t; }
        iterator operator-(difference_type n) const { iterator t = *this; t -= n; return t; }
        friend iterator operator+(difference_type n, const iterator& it) { return it + n; }
        difference_type operator-(const iterator& o) const {
            return (m_mode == Mode::Arr) ? (m_a - o.m_a) : difference_type(0);
        }
        bool operator<(const iterator& o) const {
            return (m_mode == Mode::Arr) ? (m_a < o.m_a) : false;
        }
        bool operator>(const iterator& o) const { return o < *this; }
        bool operator<=(const iterator& o) const { return !(o < *this); }
        bool operator>=(const iterator& o) const { return !(*this < o); }
        Json& operator[](difference_type n) const { return *(*this + n); }
        const std::string& key() const { return m_o->first; }
        Json& value() const { return (m_mode == Mode::Arr) ? *m_a : m_o->second; }
        Json& operator*() const { return value(); }
        Json* operator->() const { return &value(); }
    private:
        friend class Json;
        friend class const_iterator;
        enum class Mode { None, Arr, Obj };
        Mode m_mode = Mode::None;
        ArrT::iterator m_a{};
        ObjT::iterator m_o{};
        iterator(Mode mode, ArrT::iterator a, ObjT::iterator o)
            : m_mode(mode), m_a(a), m_o(o) {}
    };
    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = Json;
        using difference_type = std::ptrdiff_t;
        using pointer = const Json*;
        using reference = const Json&;

        const_iterator() = default;
        bool operator==(const const_iterator& o) const {
            if (m_mode != o.m_mode) return false;
            if (m_mode == Mode::Arr) return m_a == o.m_a;
            if (m_mode == Mode::Obj) return m_o == o.m_o;
            return true;
        }
        bool operator!=(const const_iterator& o) const { return !(*this == o); }
        const_iterator& operator++() {
            if (m_mode == Mode::Arr) ++m_a;
            else if (m_mode == Mode::Obj) ++m_o;
            return *this;
        }
        const_iterator operator++(int) { const_iterator t = *this; ++(*this); return t; }
        const_iterator& operator--() {
            if (m_mode == Mode::Arr) --m_a;
            else if (m_mode == Mode::Obj) --m_o;
            return *this;
        }
        const_iterator operator--(int) { const_iterator t = *this; --(*this); return t; }
        const_iterator& operator+=(difference_type n) { if (m_mode == Mode::Arr) m_a += n; return *this; }
        const_iterator& operator-=(difference_type n) { if (m_mode == Mode::Arr) m_a -= n; return *this; }
        const_iterator operator+(difference_type n) const { const_iterator t = *this; t += n; return t; }
        const_iterator operator-(difference_type n) const { const_iterator t = *this; t -= n; return t; }
        friend const_iterator operator+(difference_type n, const const_iterator& it) { return it + n; }
        difference_type operator-(const const_iterator& o) const {
            return (m_mode == Mode::Arr) ? (m_a - o.m_a) : difference_type(0);
        }
        bool operator<(const const_iterator& o) const {
            return (m_mode == Mode::Arr) ? (m_a < o.m_a) : false;
        }
        bool operator>(const const_iterator& o) const { return o < *this; }
        bool operator<=(const const_iterator& o) const { return !(o < *this); }
        bool operator>=(const const_iterator& o) const { return !(*this < o); }
        const Json& operator[](difference_type n) const { return *(*this + n); }
        const std::string& key() const { return m_o->first; }
        const Json& value() const { return (m_mode == Mode::Arr) ? *m_a : m_o->second; }
        const Json& operator*() const { return value(); }
        const Json* operator->() const { return &value(); }
    private:
        friend class Json;
        enum class Mode { None, Arr, Obj };
        Mode m_mode = Mode::None;
        ArrT::const_iterator m_a{};
        ObjT::const_iterator m_o{};
        const_iterator(Mode mode, ArrT::const_iterator a, ObjT::const_iterator o)
            : m_mode(mode), m_a(a), m_o(o) {}
    };

    iterator find(const std::string& key) {
        if (!is_object()) return iterator();
        return iterator(iterator::Mode::Obj, ArrT::iterator{}, m_obj->find(key));
    }
    const_iterator find(const std::string& key) const {
        if (!is_object()) return const_iterator();
        return const_iterator(const_iterator::Mode::Obj, ArrT::const_iterator{}, m_obj->find(key));
    }

    iterator begin() {
        if (m_kind == Kind::Array)
            return iterator(iterator::Mode::Arr, m_arr->begin(), ObjT::iterator{});
        if (m_kind == Kind::Object)
            return iterator(iterator::Mode::Obj, ArrT::iterator{}, m_obj->begin());
        return iterator();
    }
    iterator end() {
        if (m_kind == Kind::Array)
            return iterator(iterator::Mode::Arr, m_arr->end(), ObjT::iterator{});
        if (m_kind == Kind::Object)
            return iterator(iterator::Mode::Obj, ArrT::iterator{}, m_obj->end());
        return iterator();
    }
    const_iterator begin() const {
        if (m_kind == Kind::Array)
            return const_iterator(const_iterator::Mode::Arr, m_arr->begin(), ObjT::const_iterator{});
        if (m_kind == Kind::Object)
            return const_iterator(const_iterator::Mode::Obj, ArrT::const_iterator{}, m_obj->begin());
        return const_iterator();
    }
    const_iterator end() const {
        if (m_kind == Kind::Array)
            return const_iterator(const_iterator::Mode::Arr, m_arr->end(), ObjT::const_iterator{});
        if (m_kind == Kind::Object)
            return const_iterator(const_iterator::Mode::Obj, ArrT::const_iterator{}, m_obj->end());
        return const_iterator();
    }

    // ---- 元素访问 ----
    Json& operator[](const std::string& key) {
        if (m_kind != Kind::Object) {
            m_kind = Kind::Object;
            m_obj = std::make_shared<ObjT>();
        }
        return (*m_obj)[key];
    }
    const Json& operator[](const std::string& key) const {
        if (!is_object()) throw type_error("对非对象用字符串下标");
        auto it = m_obj->find(key);
        if (it == m_obj->end()) throw type_error("对象里没有键：" + key);
        return it->second;
    }
    Json& operator[](size_t i) {
        if (m_kind != Kind::Array) {
            m_kind = Kind::Array;
            m_arr = std::make_shared<ArrT>();
        }
        if (i >= m_arr->size()) m_arr->resize(i + 1);
        return (*m_arr)[i];
    }
    const Json& operator[](size_t i) const {
        if (!is_array()) throw type_error("对非数组用下标");
        if (i >= m_arr->size()) throw type_error("数组下标越界");
        return (*m_arr)[i];
    }

    void push_back(Json v) {
        if (m_kind != Kind::Array) {
            m_kind = Kind::Array;
            m_arr = std::make_shared<ArrT>();
        }
        m_arr->push_back(std::move(v));
    }

    // ---- 取值 ----
    // get<T>：只支持下面显式特化的类型（声明在类外、定义在 jdoc.cpp）
    template <typename T> T get() const;

    // value(key, 默认值)。
    // 字符串字面量走 const char* / std::string 重载（避免模板推导出
    // 数组类型的问题）；语义与 nlohmann 的 value() 对齐。
    std::string value(const std::string& key, const char* def) const;
    std::string value(const std::string& key, const std::string& def) const;
    Json value(const std::string& key, const Json& def) const;
    bool value(const std::string& key, bool def) const;
    int value(const std::string& key, int def) const;
    long long value(const std::string& key, long long def) const;
    double value(const std::string& key, double def) const;

    // ---- 序列化：dump() 紧凑；dump(2) 每层 2 空格（对齐 nlohmann）----
    std::string dump(int indent = -1) const;

    // 数字比较（p[0] != 10 之类的场景）
    friend bool operator==(const Json& a, const Json& b);
    friend bool operator!=(const Json& a, const Json& b) { return !(a == b); }

private:
    Kind m_kind = Kind::Null;
    bool m_bool = false;
    int64_t m_int = 0;
    uint64_t m_uint = 0;
    double m_double = 0;
    std::string m_str;
    std::shared_ptr<ArrT> m_arr;
    std::shared_ptr<ObjT> m_obj;

    // 深拷贝（与 nlohmann 的值语义一致；只拷贝本层 + 递归子树）
    void copyFrom(const Json& o) {
        if (this == &o) return;
        m_kind   = o.m_kind;
        m_bool   = o.m_bool;
        m_int    = o.m_int;
        m_uint   = o.m_uint;
        m_double = o.m_double;
        m_str    = o.m_str;
        if (o.m_arr) m_arr = std::make_shared<ArrT>(*o.m_arr);
        else         m_arr.reset();
        if (o.m_obj) m_obj = std::make_shared<ObjT>(*o.m_obj);
        else         m_obj.reset();
    }
};

} // namespace sb

// 显式特化声明（定义在 jdoc.cpp）：让其它翻译单元直接调用特化版本
namespace sb {
template <> std::string Json::get<std::string>() const;
template <> bool Json::get<bool>() const;
template <> int Json::get<int>() const;
template <> long long Json::get<long long>() const;
template <> unsigned long long Json::get<unsigned long long>() const;
template <> double Json::get<double>() const;
} // namespace sb