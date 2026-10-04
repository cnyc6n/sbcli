// src/squeak.hpp
#pragma once
#include "common.hpp"
#include <memory>
#include <map>
#include <variant>
#include "jdoc.hpp"
#include <set>

namespace sb {

// ---- 对象模型 ----

struct Ref { int index; };

struct SqueakObject;
using ObjPtr = std::shared_ptr<SqueakObject>;

// 字段值：null / bool / int / double / string / bytes / Ref / 列表 / 字典 / 对象
struct Value;
using ValueList = std::vector<Value>;
using ValueMap  = std::vector<std::pair<Value, Value>>; // 保序，键可能不是字符串

struct Value {
    enum class Kind { Nil, Bool, Int, Double, Str, Bytes, Ref, List, Map, Obj, Tuple, Color } kind = Kind::Nil;
    bool        b = false;
    int64_t     i = 0;
    double      d = 0;
    std::string s;                  // Str
    std::vector<uint8_t> bytes;     // Bytes
    Ref         ref{0};
    std::shared_ptr<ValueList> list;
    std::shared_ptr<ValueMap>  map;
    ObjPtr      obj;
    std::shared_ptr<std::vector<Value>> tuple;  // Point/Rectangle 用
    uint32_t    color = 0;          // RGBA，用于 #rrggbb[aa]

    static Value makeNil() { return {}; }
    static Value makeBool(bool v) { Value x; x.kind = Kind::Bool; x.b = v; return x; }
    static Value makeInt(int64_t v) { Value x; x.kind = Kind::Int; x.i = v; return x; }
    static Value makeDouble(double v) { Value x; x.kind = Kind::Double; x.d = v; return x; }
    static Value makeStr(std::string v) { Value x; x.kind = Kind::Str; x.s = std::move(v); return x; }
    static Value makeBytes(std::vector<uint8_t> v) { Value x; x.kind = Kind::Bytes; x.bytes = std::move(v); return x; }
    static Value makeRef(int idx) { Value x; x.kind = Kind::Ref; x.ref = {idx}; return x; }
};

struct SqueakObject {
    std::string cls;
    int classId = 0;
    int oid = 0;
    std::vector<Value> fields;

    const Value* at(size_t i) const {
        return i < fields.size() ? &fields[i] : nullptr;
    }
    // 按字段名查（依赖 FIELD_NAMES 表）
    const Value* named(const std::string& name) const;
};

// ---- 字段名表（照搬 Python 版） ----
extern const std::map<std::string, std::vector<std::string>> FIELD_NAMES;
extern const std::map<std::string, std::string> FIELD_ALIASES;

// ---- 常量表（定义在 squeak_tables.cpp） ----
extern const uint8_t MAC_ROMAN_TO_LATIN[128];
extern const std::map<int, std::string> FIXED_CLASSES;
extern const std::map<int, std::string> USER_CLASSES;

bool isBlockClass(const std::string& cls);

// ---- 对象表 ----
class ObjTable {
public:
    void add(int classId, std::vector<Value> payload) {
        m_entries.emplace_back(classId, std::move(payload));
    }
    size_t size() const { return m_entries.size(); }

    // 取值：把 Ref 解析成实际值
    Value get(int index, int depth = 0) const;

private:
    std::vector<std::pair<int, std::vector<Value>>> m_entries;
    mutable std::map<int, Value> m_cache;

    Value resolve(const Value& v, int depth) const;
    Value materialize(int classId, const std::vector<Value>& payload,
                      int oid, int depth) const;
};

// ---- 文件读取 ----
struct Sb1File {
    std::shared_ptr<ObjTable> table;
    std::shared_ptr<ObjTable> info;   // 可能为空
    int infoSize = 0;
};

Sb1File sb1Load(const std::string& path);

// 借用 SqueakObject* 构造一个 Value（不拥有所有权，析构不释放源对象）。
// 用于把裸指针塞进 Value::Obj 参与遍历/取名字，避免重复释放。
Value borrowObj(const SqueakObject* o);

// 从对象里取名字（字符串 / 有 name 字段的对象）
std::string nameOf(const Value& v);

// 遍历对象树
void walkObjects(const Value& root,
                 const std::function<void(const SqueakObject&)>& fn,
                 size_t maxNodes = 200000);

// 收集所有指定类名的对象
std::vector<const SqueakObject*> findAll(const Value& root, const std::string& cls);

// root → JSON（调试用）
Json asJson(const Value& v, int maxDepth = 3, int depth = 0,
                      std::set<int>* seen = nullptr);

} // namespace sb