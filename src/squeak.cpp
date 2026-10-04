// src/squeak.cpp
#include "squeak.hpp"
#include "jdoc.hpp"
#include <cstring>
#include <cmath>
#include <functional>
#include <set>

namespace sb {

using json = Json;


// ---- 字段类型标记 ----
enum {
    F_NIL=1, F_TRUE=2, F_FALSE=3, F_SMALLINT=4, F_SMALLINT16=5,
    F_LARGE_POS=6, F_LARGE_NEG=7, F_FLOAT=8, F_REF=99
};

// ---- 二进制读取器 ----
namespace {

class ByteReader {
public:
    ByteReader(const std::vector<uint8_t>& blob) : m_blob(blob) {}

    uint8_t  u8()  { check(1); return m_blob[m_pos++]; }
    uint16_t u16() { check(2); uint16_t v = (m_blob[m_pos]<<8)|m_blob[m_pos+1]; m_pos+=2; return v; }
    int16_t  i16() { return (int16_t)u16(); }
    uint32_t u32() {
        check(4);
        uint32_t v = (uint32_t)m_blob[m_pos]<<24 | (uint32_t)m_blob[m_pos+1]<<16
                   | (uint32_t)m_blob[m_pos+2]<<8 | m_blob[m_pos+3];
        m_pos += 4; return v;
    }
    int32_t  i32() { return (int32_t)u32(); }
    double   f64() {
        check(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v<<8) | m_blob[m_pos+i];
        m_pos += 8;
        double d; std::memcpy(&d, &v, 8); return d;
    }
    std::vector<uint8_t> take(size_t n) {
        check(n);
        std::vector<uint8_t> out(m_blob.begin()+m_pos, m_blob.begin()+m_pos+n);
        m_pos += n; return out;
    }
    size_t pos() const { return m_pos; }
    void setPos(size_t p) { m_pos = p; }
    bool eof() const { return m_pos >= m_blob.size(); }

private:
    void check(size_t n) {
        if (m_pos + n > m_blob.size())
            throw Sb1Error("文件在偏移 " + std::to_string(m_pos) + " 处提前结束");
    }
    const std::vector<uint8_t>& m_blob;
    size_t m_pos = 0;
};

const char MAGIC[]     = "ScratchV02";
const char MAGIC_V01[] = "ScratchV01";
const char OBJ_HEADER[] = "ObjS\x01Stch\x01";

Value readField(ByteReader& r);

std::vector<Value> readFixedPayload(ByteReader& r, int cid) {
    std::vector<Value> out;
    if (cid == 9 || cid == 10 || cid == 14) {
        uint32_t n = r.u32();
        auto raw = r.take(n);
        if (cid == 14) {
            // UTF8：本身就是 UTF-8
            out.push_back(Value::makeStr(std::string(raw.begin(), raw.end())));
        } else {
            // Mac Roman → Latin-1 → UTF-8
            // （字符串在内存里统一用 UTF-8，和 sb3 一致；
            //   否则写文件名 / 打控制台时 0x80-0xFF 的 Latin-1 字节会被当成
            //   非法 UTF-8 变成替g换符）
            std::string s;
            s.reserve(raw.size() * 2);
            for (uint8_t b : raw) {
                uint8_t c = (b >= 0x80) ? MAC_ROMAN_TO_LATIN[b - 0x80] : b;
                if (c >= 0x80) {
                    s += (char)(0xC0 | (c >> 6));
                    s += (char)(0x80 | (c & 0x3F));
                } else {
                    s += (char)c;
                }
            }
            out.push_back(Value::makeStr(std::move(s)));
        }
        return out;
    }
    if (cid == 11) {
        uint32_t n = r.u32();
        out.push_back(Value::makeBytes(r.take(n)));
        return out;
    }
    if (cid == 12) {
        uint32_t n = r.u32();
        std::vector<uint8_t> raw;
        raw.reserve(n * 2);
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t v = r.u16();
            raw.push_back((uint8_t)(v >> 8));
            raw.push_back((uint8_t)(v & 0xFF));
        }
        out.push_back(Value::makeBytes(std::move(raw)));
        return out;
    }
    if (cid == 13) {
        uint32_t n = r.u32();
        out.push_back(Value::makeBytes(r.take((size_t)n * 4)));
        return out;
    }
    if (cid == 20 || cid == 21 || cid == 22 || cid == 23) {
        uint32_t n = r.u32();
        auto lst = std::make_shared<ValueList>();
        lst->reserve(n);
        for (uint32_t i = 0; i < n; ++i) lst->push_back(readField(r));
        Value v; v.kind = Value::Kind::List; v.list = lst;
        out.push_back(std::move(v));
        return out;
    }
    if (cid == 24 || cid == 25) {
        uint32_t n = r.u32();
        auto m = std::make_shared<ValueMap>();
        m->reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            Value k = readField(r);
            Value v = readField(r);
            m->emplace_back(std::move(k), std::move(v));
        }
        Value v; v.kind = Value::Kind::Map; v.map = m;
        out.push_back(std::move(v));
        return out;
    }
    if (cid == 30) {
        uint32_t v = r.u32();
        uint8_t r8 = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
        uint8_t g8 = (uint8_t)(((v >> 10) & 0x3FF) >> 2);
        uint8_t b8 = (uint8_t)((v & 0x3FF) >> 2);
        Value x; x.kind = Value::Kind::Color;
        x.color = (uint32_t)r8<<24 | (uint32_t)g8<<16 | (uint32_t)b8<<8 | 0xFF;
        out.push_back(x);
        return out;
    }
    if (cid == 31) {
        uint32_t v = r.u32();
        uint8_t a = r.u8();
        uint8_t r8 = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
        uint8_t g8 = (uint8_t)(((v >> 10) & 0x3FF) >> 2);
        uint8_t b8 = (uint8_t)((v & 0x3FF) >> 2);
        Value x; x.kind = Value::Kind::Color;
        x.color = (uint32_t)r8<<24 | (uint32_t)g8<<16 | (uint32_t)b8<<8 | a;
        out.push_back(x);
        return out;
    }
    if (cid == 32) {  // Point
        auto t = std::make_shared<std::vector<Value>>();
        t->push_back(readField(r));
        t->push_back(readField(r));
        Value v; v.kind = Value::Kind::Tuple; v.tuple = t;
        out.push_back(std::move(v));
        return out;
    }
    if (cid == 33) {  // Rectangle
        auto t = std::make_shared<std::vector<Value>>();
        for (int i = 0; i < 4; ++i) t->push_back(readField(r));
        Value v; v.kind = Value::Kind::Tuple; v.tuple = t;
        out.push_back(std::move(v));
        return out;
    }
    if (cid == 34) {  // Form
        auto t = std::make_shared<std::vector<Value>>();
        for (int i = 0; i < 5; ++i) t->push_back(readField(r));
        Value v; v.kind = Value::Kind::Tuple; v.tuple = t;
        out.push_back(std::move(v));
        return out;
    }
    if (cid == 35) {  // ColorForm
        auto t = std::make_shared<std::vector<Value>>();
        for (int i = 0; i < 6; ++i) t->push_back(readField(r));
        Value v; v.kind = Value::Kind::Tuple; v.tuple = t;
        out.push_back(std::move(v));
        return out;
    }
    throw Sb1Error("未知的固定对象类 ID " + std::to_string(cid) +
                   "（偏移 " + std::to_string(r.pos()) + "）");
}

Value readField(ByteReader& r) {
    size_t p = r.pos();
    uint8_t t = r.u8();
    switch (t) {
        case F_NIL:    return Value::makeNil();
        case F_TRUE:   return Value::makeBool(true);
        case F_FALSE:  return Value::makeBool(false);
        case F_SMALLINT:   return Value::makeInt(r.i32());
        case F_SMALLINT16: return Value::makeInt(r.i16());
        case F_LARGE_POS: case F_LARGE_NEG: {
            uint16_t n = r.u16();
            auto raw = r.take(n);
            // little endian
            __int128 v = 0;
            for (int i = (int)raw.size() - 1; i >= 0; --i) v = (v << 8) | raw[i];
            int64_t iv = (int64_t)v;
            if (t == F_LARGE_NEG) iv = -iv;
            return Value::makeInt(iv);
        }
        case F_FLOAT:  return Value::makeDouble(r.f64());
        case F_REF: {
            auto b = r.take(3);
            int idx = (b[0] << 16) | (b[1] << 8) | b[2];
            return Value::makeRef(idx);
        }
        default:
            throw Sb1Error("未知的字段类型标记 " + std::to_string(t) +
                           "（偏移 " + std::to_string(p) + "）");
    }
}

ObjTable readObjTable(ByteReader& r) {
    size_t start = r.pos();
    auto header = r.take(10);
    if (std::memcmp(header.data(), OBJ_HEADER, 10) != 0)
        throw Sb1Error("对象表头不对（偏移 " + std::to_string(start) + "）");
    uint32_t count = r.u32();
    ObjTable tbl;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t classId = r.u8();
        if (classId == 0)
            throw Sb1Error("遇到类 ID 0（偏移 " + std::to_string(r.pos() - 1) + "）");
        if (classId < 99) {
            tbl.add(classId, readFixedPayload(r, classId));
        } else {
            uint8_t version = r.u8();
            uint8_t n = r.u8();
            std::vector<Value> payload;
            payload.reserve(n);
            for (uint8_t k = 0; k < n; ++k) payload.push_back(readField(r));
            tbl.add(classId, std::move(payload));
        }
    }
    return tbl;
}

} // namespace

// ---- ObjTable ----

Value ObjTable::resolve(const Value& v, int depth) const {
    if (v.kind == Value::Kind::Ref) return get(v.ref.index, depth + 1);
    return v;
}

Value ObjTable::get(int index, int depth) const {
    if (index < 1 || (size_t)index > m_entries.size()) return Value::makeNil();
    auto it = m_cache.find(index);
    if (it != m_cache.end()) return it->second;
    if (depth > 200) return Value::makeNil();

    auto [classId, payload] = m_entries[index - 1];

    if (classId >= 99) {
        Value v; v.kind = Value::Kind::Obj;
        auto obj = std::make_shared<SqueakObject>();
        obj->cls = USER_CLASSES.count(classId) ? USER_CLASSES.at(classId)
                                               : ("class" + std::to_string(classId));
        obj->classId = classId;
        obj->oid = index;
        // 先占位：把 obj 直接放进缓存（与 Python 的 _cache[index] = obj 一致）。
        // 循环引用（owner/submorphs 互指）命中缓存时拿到的是同一个对象，
        // 字段随后填满，不会出现 null 空壳导致遍历丢字段。
        v.obj = obj;
        m_cache[index] = v;
        obj->fields.reserve(payload.size());
        for (auto& x : payload) obj->fields.push_back(resolve(x, depth + 1));
        return v;
    }

    // readFixedPayload 把集合类（20/21/22/23）整个读成 Value::Kind::List，
    // 放在 payload[0]。Dictionary（24/25）同理放在 payload[0]（Value::Kind::Map）。
    // 所以这里从 payload[0] 取出来，只对内部元素做 resolve（处理内层 Ref）。

    if (classId == 20 || classId == 21 || classId == 22 || classId == 23) {
        Value v = payload.empty() ? Value::makeNil() : payload[0];
        if (v.kind != Value::Kind::List || !v.list) {
            v.kind = Value::Kind::List;
            v.list = std::make_shared<ValueList>();
        }
        auto out = std::make_shared<ValueList>();
        out->reserve(v.list->size());
        for (auto& x : *v.list) out->push_back(resolve(x, depth + 1));
        v.list = out;
        m_cache[index] = v;
        return v;
    }

    if (classId == 24 || classId == 25) {
        Value v = payload.empty() ? Value::makeNil() : payload[0];
        if (v.kind != Value::Kind::Map || !v.map) {
            v.kind = Value::Kind::Map;
            v.map = std::make_shared<ValueMap>();
        }
        auto out = std::make_shared<ValueMap>();
        out->reserve(v.map->size());
        for (auto& kv : *v.map) {
            out->emplace_back(resolve(kv.first, depth + 1),
                              resolve(kv.second, depth + 1));
        }
        v.map = out;
        m_cache[index] = v;
        return v;
    }

    // 固定格式：materialize
    Value v = materialize(classId, payload, index, depth);
    m_cache[index] = v;
    return v;
}

Value ObjTable::materialize(int classId, const std::vector<Value>& payload,
                            int oid, int depth) const {
    // readFixedPayload 已经把固定格式对象读成"单个 Value"，放在 payload[0]：
    //   9/10/14  → Value::Kind::Str
    //   11/13    → Value::Kind::Bytes
    //   12       → Value::Kind::Obj("SoundBuffer")
    //   30/31    → Value::Kind::Color
    //   32/33    → Value::Kind::Tuple
    //   34       → Value::Kind::Tuple（Form 的 5 字段）
    //   35       → Value::Kind::Tuple（ColorForm 的 6 字段）
    // 所以直接用 payload[0]，只对内部的 Ref 做 resolve。

    auto resolveValue = [&](const Value& x) { return resolve(x, depth + 1); };

    if (classId == 9 || classId == 10 || classId == 14) {
        return payload.empty() ? Value::makeStr("") : payload[0];
    }
    if (classId == 11) {
        return payload.empty() ? Value::makeBytes({}) : payload[0];
    }
    if (classId == 30 || classId == 31) {
        return payload.empty() ? Value::makeNil() : payload[0];
    }

    if (classId == 12) {
        // SoundBuffer：字段是字节流，payload[0] 是 Bytes
        Value v = payload.empty() ? Value::makeBytes({}) : payload[0];
        Value out; out.kind = Value::Kind::Obj;
        auto obj = std::make_shared<SqueakObject>();
        obj->cls = "SoundBuffer"; obj->classId = classId; obj->oid = oid;
        obj->fields.push_back(resolveValue(v));
        out.obj = obj;
        return out;
    }
    if (classId == 13) {
        // Bitmap：同上，payload[0] 是 Bytes
        Value v = payload.empty() ? Value::makeBytes({}) : payload[0];
        Value out; out.kind = Value::Kind::Obj;
        auto obj = std::make_shared<SqueakObject>();
        obj->cls = "Bitmap"; obj->classId = classId; obj->oid = oid;
        obj->fields.push_back(resolveValue(v));
        out.obj = obj;
        return out;
    }

    if (classId == 32 || classId == 33) {
        // Point / Rectangle：payload[0] 已经是 Value::Kind::Tuple
        Value v = payload.empty() ? Value::makeNil() : payload[0];
        if (v.kind != Value::Kind::Tuple || !v.tuple) {
            v.kind = Value::Kind::Tuple;
            v.tuple = std::make_shared<std::vector<Value>>();
        }
        auto out = std::make_shared<std::vector<Value>>();
        out->reserve(v.tuple->size());
        for (auto& x : *v.tuple) out->push_back(resolveValue(x));
        v.tuple = out;
        return v;
    }

    if (classId == 34 || classId == 35) {
        // Form / ColorForm：payload[0] 是 Tuple（5 或 6 个字段）
        const char* cls = (classId == 34) ? "Form" : "ColorForm";
        Value v = payload.empty() ? Value::makeNil() : payload[0];
        Value out; out.kind = Value::Kind::Obj;
        auto obj = std::make_shared<SqueakObject>();
        obj->cls = cls; obj->classId = classId; obj->oid = oid;
        if (v.kind == Value::Kind::Tuple && v.tuple) {
            obj->fields.reserve(v.tuple->size());
            for (auto& x : *v.tuple) obj->fields.push_back(resolveValue(x));
        } else {
            obj->fields.push_back(resolveValue(v));
        }
        out.obj = obj;
        return out;
    }

    // 兜底：不认识的固定类
    Value v = payload.empty() ? Value::makeNil() : payload[0];
    Value out; out.kind = Value::Kind::Obj;
    auto obj = std::make_shared<SqueakObject>();
    obj->cls = FIXED_CLASSES.count(classId) ? FIXED_CLASSES.at(classId)
                                            : ("class" + std::to_string(classId));
    obj->classId = classId; obj->oid = oid;
    obj->fields.push_back(resolveValue(v));
    out.obj = obj;
    return out;
}

// ---- 文件入口 ----

Sb1File sb1Load(const std::string& path) {
    auto blob = readFileBinary(path);
    size_t pos = 0;
    bool spriteStyle = false;
    if (blob.size() >= 10 &&
        (std::memcmp(blob.data(), MAGIC, 10) == 0 ||
         std::memcmp(blob.data(), MAGIC_V01, 10) == 0)) {
        pos = 10;
    } else if (blob.size() >= 10 &&
               std::memcmp(blob.data(), OBJ_HEADER, 10) == 0) {
        pos = 0;
        spriteStyle = true;
    } else {
        throw Sb1Error("不是 Scratch 1.x 文件（既没有 ScratchV01/V02 头，"
                       "也不是 .sprite 的对象表）");
    }

    ByteReader r(blob);
    r.setPos(pos);

    Sb1File out;
    out.table = std::make_shared<ObjTable>();

    if (spriteStyle) {
        *out.table = readObjTable(r);
        return out;
    }
    out.infoSize = r.u32();
    auto infoTbl = readObjTable(r);
    out.info = std::make_shared<ObjTable>(std::move(infoTbl));
    auto mainTbl = readObjTable(r);
    *out.table = std::move(mainTbl);
    return out;
}

// ---- 工具 ----

Value borrowObj(const SqueakObject* o) {
    Value v;
    v.kind = Value::Kind::Obj;
    // 借用指针，不拥有所有权（no-op deleter），避免重复释放
    v.obj = std::shared_ptr<SqueakObject>(const_cast<SqueakObject*>(o),
                                          [](SqueakObject*) {});
    return v;
}

std::string nameOf(const Value& v) {
    if (v.kind == Value::Kind::Str) return v.s;
    if (v.kind == Value::Kind::Obj && v.obj) {
        auto n = v.obj->named("name");
        if (n && n->kind == Value::Kind::Str && !n->s.empty()) return n->s;
    }
    return "";
}

void walkObjects(const Value& root,
                 const std::function<void(const SqueakObject&)>& fn,
                 size_t maxNodes) {
    std::set<int> seen;
    std::vector<const Value*> stack{&root};
    size_t n = 0;
    while (!stack.empty() && n < maxNodes) {
        const Value* x = stack.back(); stack.pop_back();
        ++n;
        if (x->kind == Value::Kind::Obj && x->obj) {
            if (x->obj->oid > 0) {
                if (seen.count(x->obj->oid)) continue;
                seen.insert(x->obj->oid);
            }
            fn(*x->obj);
            for (auto it = x->obj->fields.rbegin();
                 it != x->obj->fields.rend(); ++it)
                stack.push_back(&*it);
        } else if (x->kind == Value::Kind::List && x->list) {
            for (auto it = x->list->rbegin(); it != x->list->rend(); ++it)
                stack.push_back(&*it);
        } else if (x->kind == Value::Kind::Map && x->map) {
            for (auto it = x->map->rbegin(); it != x->map->rend(); ++it)
                stack.push_back(&it->second);
        }
    }
}

std::vector<const SqueakObject*> findAll(const Value& root,
                                         const std::string& cls) {
    std::vector<const SqueakObject*> out;
    walkObjects(root, [&](const SqueakObject& o) {
        if (o.cls == cls) out.push_back(&o);
    });
    return out;
}

json asJson(const Value& v, int maxDepth, int depth, std::set<int>* seen) {
    if (depth > maxDepth) return "…";
    switch (v.kind) {
        case Value::Kind::Nil:    return nullptr;
        case Value::Kind::Bool:   return v.b;
        case Value::Kind::Int:    return v.i;
        case Value::Kind::Double: return v.d;
        case Value::Kind::Str:    return v.s;
        case Value::Kind::Bytes:  return "<bytes " + std::to_string(v.bytes.size()) + ">";
        case Value::Kind::Color: {
            char buf[16];
            uint32_t c = v.color;
            if ((c & 0xFF) == 0xFF)
                std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                              (c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF);
            else
                std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                              (c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF,
                              c & 0xFF);
            return buf;
        }
        case Value::Kind::Ref:    return "Ref(" + std::to_string(v.ref.index) + ")";
        case Value::Kind::Tuple: {
            json a = json::array();
            for (auto& x : *v.tuple) a.push_back(asJson(x, maxDepth, depth+1, seen));
            return a;
        }
        case Value::Kind::List: {
            json a = json::array();
            for (auto& x : *v.list) a.push_back(asJson(x, maxDepth, depth+1, seen));
            return a;
        }
        case Value::Kind::Map: {
            json o = json::object();
            for (auto& kv : *v.map) {
                std::string k = (kv.first.kind == Value::Kind::Str)
                                ? kv.first.s
                                : asJson(kv.first, 0, depth+1, seen).dump();
                o[k] = asJson(kv.second, maxDepth, depth+1, seen);
            }
            return o;
        }
        case Value::Kind::Obj: {
            if (!v.obj) return nullptr;
            if (seen && v.obj->oid > 0) {
                if (seen->count(v.obj->oid))
                    return "↺" + v.obj->cls + "#" + std::to_string(v.obj->oid);
                seen->insert(v.obj->oid);
            }
            auto it = FIELD_NAMES.find(v.obj->cls);
            if (it == FIELD_NAMES.end()) {
                auto ai = FIELD_ALIASES.find(v.obj->cls);
                if (ai != FIELD_ALIASES.end()) it = FIELD_NAMES.find(ai->second);
            }
            json body;
            if (it != FIELD_NAMES.end()) {
                body = json::object();
                auto& keys = it->second;
                for (size_t i = 0; i < v.obj->fields.size(); ++i) {
                    std::string nm = i < keys.size() ? keys[i]
                                                     : ("undefined-" + std::to_string(i));
                    body[nm] = asJson(v.obj->fields[i], maxDepth, depth+1, seen);
                }
            } else {
                body = json::array();
                for (auto& f : v.obj->fields)
                    body.push_back(asJson(f, maxDepth, depth+1, seen));
            }
            json o;
            o["_class"] = v.obj->cls;
            o["_id"]    = v.obj->oid;
            o["_fields"] = body;
            return o;
        }
    }
    return nullptr;
}

} // namespace sb
