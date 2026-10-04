// src/sbcli_view.hpp
// `sb view` 的接口：把 sbcli 项目浏览成角色清单 + 中文脚本。
//
// 与读取 .sb3 的路径不同：这里输入是**文本 AST**（src/sbcli_parser），
// 所以翻译走"opcode → SB3_T/SB2_T 模板 → 填参"，不经过 Renderer
// （Renderer 吃的是 simdjson 的 project.json DOM，对这里不适用）。
#pragma once

#include <string>
#include <vector>

namespace sb {

struct ViewLine {
    int         indent = 0;      // 缩进层级（顶块为 0）
    std::string text;            // 中文译文
    std::string opcode;          // 原始 opcode（便于排查）
    int         srcLine = 0;     // block.sbcli 里的行号
};

struct ViewScript {
    std::string          hat;        // flag / broadcast / key / clone / click
    std::string          hatArg;     // 广播名 / 键名
    int                  line = 0;   // @script 所在行
    std::string          hatText;    // 帽子的中文描述
    std::vector<ViewLine> lines;     // 脚本正文（含嵌套，已带缩进层级）
};

struct ViewSprite {
    std::string            id;         // "stage" 或角色数字 id
    std::string            name;       // meta.sbcli 里的 name
    bool                   isStage = false;
    int                    scriptCount = 0;
    int                    blockCount = 0;
    int                    costumeCount = 0;
    int                    soundCount = 0;
    std::string            path;       // 项目相对路径
    std::vector<ViewScript> scripts;
    bool                   hasMeta = false;
    bool                   hasBlock = false;
};

struct ViewReport {
    std::string            root;
    std::string            projectName;
    std::vector<ViewSprite> sprites;
    std::string            error;      // 目录不可用时的顶层错误
};

ViewReport sbcliView(const std::string& rootOrDir);

} // namespace sb
