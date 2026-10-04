// src/sb3_load.cpp —— Scratch 2/3 压缩包加载与目标解析
#include "sb3.hpp"
#include "sb2.hpp"
#include "sb3_internal.hpp"
#include <map>
#include <memory>
#include <string>

namespace sb {

namespace {

// 解析文本 → (解析器, 顶层元素)；失败抛 Sb3Error
std::pair<std::shared_ptr<simdjson::dom::parser>, simdjson::dom::element>
parseJsonText(const std::string& text) {
    auto p = std::make_shared<simdjson::dom::parser>();
    simdjson::dom::element root;
    simdjson::error_code err = p->parse(text).get(root);
    if (err)
        throw Sb3Error(std::string("project.json 解析失败：") +
                       simdjson::error_message(err));
    return {std::move(p), root};
}

bool hasKey(const simdjson::dom::element& e, std::string_view key) {
    return e.type() == simdjson::dom::element_type::OBJECT &&
           e[key].error() == simdjson::SUCCESS;
}

// 取 targets 数组；缺键/不是数组时包装一个 "[]"
Elem targetsArrayOf(const simdjson::dom::element& root,
                    std::shared_ptr<simdjson::dom::parser>& pT) {
    if (hasKey(root, "targets")) {
        Elem t = Elem(root).at("targets");
        if (t.is_array()) return t;
    }
    auto [p2, r2] = parseJsonText("[]");
    pT = std::move(p2);
    return Elem(r2);
}

} // namespace

// ---- 加载 ----

Sb3File sb3LoadAny(const std::string& path) {
    if (!isFile(path)) throw Sb3Error("找不到文件：" + path);
    std::string ext = extname(path);
    if (ext != ".sb3" && ext != ".sb2" && ext != ".sprite3" &&
        ext != ".sb" && ext != ".zip" && !ext.empty()) {
        throw Sb3Error("不认识的扩展名：" + ext);
    }

    Sb3File out;
    out.zip = std::make_shared<mzip::Reader>(path);
    return sb3LoadFromReader(out.zip, path);
}

Sb3File sb3LoadFromReader(std::shared_ptr<mzip::Reader> zip,
                          const std::string& path) {
    Sb3File out;
    out.zip = std::move(zip);   // 共享所有权，防止调用方持有的 shared_ptr 析构后悬垂

    auto names = out.zip->names();
    std::map<std::string, std::string> lower;
    for (auto& n : names) {
        std::string l = n;
        for (auto& c : l) c = (char)::tolower((unsigned char)c);
        lower[l] = n;
    }

    auto loadText = [&](const std::string& entry) {
        return out.zip->readText(entry);
    };

    if (lower.count("project.json")) {
        auto [p, root] = parseJsonText(loadText(lower["project.json"]));
        if (hasKey(root, "children") || hasKey(root, "objName")) {
            // sb2：转成 sb3 targets 数组（Json 中转构造，再回灌 simdjson DOM）
            out.kind = "sb2";
            Json conv = sb2Convert(toJson(Elem(root)));
            auto [p2, root2] = parseJsonText(conv.dump());
            out.p = std::move(p2);
            out.data = root2;
            out.targets = Elem(root2);   // sb2Convert 的结果就是 targets 数组
            return out;
        }
        if (hasKey(root, "blocks") && hasKey(root, "isStage")) {
            // 单角色文件（改名的 .sprite3 / .sb 等）：包装成单元素数组
            out.kind = "sprite";
            out.p = std::move(p);
            out.data = root;
            std::string arr = "[";
            arr += compactJson(root);
            arr += "]";
            auto [p2, root2] = parseJsonText(arr);
            out.pT = std::move(p2);
            out.targets = Elem(root2);
            return out;
        }
        out.kind = "project";
        out.p = std::move(p);
        out.data = root;
        out.targets = targetsArrayOf(root, out.pT);
        return out;
    }

    // 找其它 json
    std::string firstJson;
    for (auto& n : names) {
        std::string l = n;
        for (auto& c : l) c = (char)::tolower((unsigned char)c);
        if (l.size() > 5 && l.substr(l.size() - 5) == ".json" &&
            l.rfind("__macosx", 0) != 0) {
            firstJson = n; break;
        }
    }
    if (firstJson.empty()) throw Sb3Error("压缩包里没有 project.json");

    auto [p, root] = parseJsonText(loadText(firstJson));
    if (hasKey(root, "blocks") && hasKey(root, "isStage")) {
        out.kind = "sprite";
        out.p = std::move(p);
        out.data = root;
        std::string arr = "[";
        arr += compactJson(root);
        arr += "]";
        auto [p2, root2] = parseJsonText(arr);
        out.pT = std::move(p2);
        out.targets = Elem(root2);
        return out;
    }
    if (hasKey(root, "targets")) {
        out.kind = "project";
        out.p = std::move(p);
        out.data = root;
        out.targets = targetsArrayOf(root, out.pT);
        return out;
    }
    if (hasKey(root, "children") || hasKey(root, "objName")) {
        out.kind = "sb2";
        Json conv = sb2Convert(toJson(Elem(root)));
        auto [p2, root2] = parseJsonText(conv.dump());
        out.p = std::move(p2);
        out.data = root2;
        out.targets = Elem(root2);
        return out;
    }
    throw Sb3Error("这个 json 不是 Scratch 作品结构");
}

} // namespace sb