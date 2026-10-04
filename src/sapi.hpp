// src/sapi.hpp
// simdjson 访问层：
//   · Elem —— simdjson::dom::element 的轻量包装（值语义、带有效性），
//     读取代码统一用它，避免悬垂与无效元素崩溃。
//   · toJson —— DOM → 自有 Json（输出 JSON / 子树复制用）。
//   · compactJson / prettyJson —— DOM 序列化（键排序、数字/转义规则与
//     nlohmann dump 对齐，保证 --json 输出逐字节一致）。
#pragma once
#include "jdoc.hpp"
#include <cstddef>
#include <string>
#include <string_view>

// 需要 simdjson 的迭代器类型（obj()/arr()），所以直接引入
#include "simdjson.h"

namespace sb {

class Elem {
public:
    Elem() = default;
    explicit Elem(const simdjson::dom::element& e) : m_e(e), m_ok(true) {}

    bool ok() const { return m_ok; }

    // ---- 类型判断（无效元素一律 false）----
    bool is_object()  const;
    bool is_array()   const;
    bool is_string()  const;
    bool is_bool()    const;
    bool is_number()  const;
    bool is_integer() const;
    bool is_float()   const;
    bool is_null()    const;

    simdjson::dom::element_type type() const { return m_e.type(); }
    const simdjson::dom::element& raw() const { return m_e; }

    // ---- 取子节点 / 数组下标 ----
    Elem at(std::string_view key) const;      // 对象按键取；缺键/非对象 → 无效
    Elem op(size_t i) const;                  // 数组按下标；越界/非数组 → 无效
    bool contains(std::string_view key) const;
    size_t size() const;                      // 数组/对象长度，其它 0
    bool empty() const { return size() == 0; }

    // ---- 值 ----
    std::string_view sv() const;              // 字符串视图；非字符串 → ""
    std::string str() const;                  // 复制的字符串；非字符串 → ""
    long long i64() const;                    // 数值转 long long；非数值 → 0
    double f64() const;                       // 数值转 double；非数值 → 0
    bool b() const;                           // bool；非 bool → false
    long long i64At(std::string_view key, long long def = 0) const;
    double f64At(std::string_view key, double def = 0) const;
    bool bAt(std::string_view key, bool def = false) const;

    // ---- 原生迭代（调用前确保 is_object() / is_array()）----
    simdjson::dom::object obj() const { return m_e.get_object().value(); }
    simdjson::dom::array  arr() const { return m_e.get_array().value(); }

private:
    simdjson::dom::element m_e;
    bool m_ok = false;
};

// DOM → Json（深拷贝）
Json toJson(const Elem& e);

// DOM 序列化：键排序 + nlohmann 风格转义/数字格式
std::string compactJson(const simdjson::dom::element& e);   // 紧凑
std::string prettyJson(const simdjson::dom::element& e, int indent);  // 缩进

// 字符串转义（写成 JSON 字符串字面量，含引号）
void writeJsonString(std::string& out, std::string_view s);

// 对象键的排序列表（模拟 nlohmann std::map 的迭代顺序：字节序）。
// 只有"遍历顺序影响输出"的循环才需要它。
std::vector<std::string> sortedKeysOf(const simdjson::dom::object& o);

// 对象按键排序的 (键视图, 元素) 列表：一遍线性扫描 + 排序。
// 用于遍历大对象（如 blocks）时避免对每个键 at() 造成 O(n²)。
// 键视图引用解析缓冲区，使用时缓冲区必须存活。
std::vector<std::pair<std::string_view, simdjson::dom::element>>
sortedEntriesOf(const simdjson::dom::object& o);

} // namespace sb