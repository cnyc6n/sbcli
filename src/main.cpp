// src/main.cpp
#include "commands.hpp"
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace sb;

static void printHelp() {
    std::cout <<
        "sbcli —— 读取 Scratch 作品（1.4 的 .sb/.sprite，2/3 的 .sb3/.sb2/.sprite3）\n"
        "\n"
        "用法：sb <命令> <文件> [选项]\n"
        "\n"
        "命令：\n"
        "  info      概要：格式、目标数、工程信息\n"
        "  sprites   列出所有角色和舞台\n"
        "  text      提取作品里的全部文字\n"
        "  script    把积木脚本翻译成可读文字\n"
        "  vars      列出变量和列表\n"
        "  assets    列出造型和声音，可导出\n"
        "  media     列出造型和声音；1.4 造型导出 PNG\n"
        "  blocks    积木使用统计\n"
        "  json      输出原始 project.json\n"
        "  raw       输出原始对象树（1.4 调试）\n"
        "  ls        扫描目录\n"
        "  find      在所有作品里搜索文字\n"
        "  refs      变量/列表的读写交叉引用（谁在哪个脚本里设置/读取/追加）\n"
        "  events    广播拓扑（谁发、谁收）\n"
        "  diff      版本对比（角色/积木/变量差异）\n"
        "  dup       相似角色检测（复制粘贴拼贴识别）\n"
        "  check     检查 sbcli 项目目录的脚本语法/结构/引用\n"
        "  fix       发现即声明：把脚本引用到的变量/列表/广播/造型/声音\n"
        "            自动登记进 meta（不改动 block.sbcli）\n"
        "  view      浏览 sbcli 项目：角色清单 + 脚本中文渲染\n"
        "  pack      把 sbcli 项目目录重新打包成 .sb3（sb pack <目录> <输出.sb3>）\n"
        "  unpack    把 .sb3 作品拆成 sbcli 项目目录（sb unpack <作品.sb3> <输出目录>）\n"
        "  search    语法查找手册：中文概念/英文 opcode → 模板+参数+示例\n"
        "            例：sb search 广播 / sb search looks_say --json\n"
        "  project   项目脚手架：init / add-sprite / add-costume / add-sound /\n"
        "            add-variable / add-list / add-broadcast\n"
        "\n"
        "通用选项（可放在文件前后任意位置）：\n"
        "  --json    输出 JSON\n"
        "  --limit N 显示条数上限\n"
        "  --sprite NAME  只看某角色\n"
        "  --extract DIR  导出目录\n"
        "  --regex   过滤参数按正则匹配（refs，如 ^Sprite1$）\n"
        "  --detail  diff 展开每个角色的积木明细\n"
        "  --by-sprite  refs 按角色视角反查（谁读/写了哪些变量）\n"
        "  --threshold N  dup 相似度阈值 %（0-100）\n"
        "\n"
        "示例：\n"
        "  sb info 作品.sb3 --json\n"
        "  sb script 作品.sb3 --json --sprite 角色名\n"
        "  sb find 关键字 --dir 目录 --jobs 8\n"
        "  sb refs 作品.sb3 系统日志     # 只看「系统日志」列表的读写\n"
        "  sb check 项目目录            # 检查 block.sbcli 的语法/结构/引用\n"
        "  sb check 项目目录 --json     # 结构化输出，有错误时退出码 1\n"
        "  sb view 项目目录             # 角色清单 + block.sbcli 的中文脚本\n"
        "  sb unpack 作品.sb3 目录     # 拆成 sbcli 项目（pack 的逆操作）\n"
        "\n";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // 控制台代码页切到 UTF-8：程序输出 UTF-8，直接跑不设环境变量也不乱码
    // （PowerShell/cmd 默认 GBK 解码会导致中文乱码）
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    // 关闭与 C stdio 的同步：逐行 std::cout 输出（script/text 等大量行）不再每次同步开销
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        std::vector<std::string> args;
        args.reserve((size_t)(argc > 0 ? argc - 1 : 0));
        for (int i = 1; i < argc; ++i) args.push_back(acpToUtf8(argv[i]));
        if (args.empty() || args[0] == "-h" || args[0] == "--help") {
            printHelp();
            return 0;
        }
        if (args[0] == "--version") {
            std::cout << "sbcli " << VERSION << "\n";
            return 0;
        }

        Args a;
        a.cmd = args[0];
        auto isOption = [](const std::string& s) {
            return !s.empty() && s[0] == '-';
        };
        bool positionalSet = false;

        // 解析剩余参数：选项可以出现在文件/路径/关键字的前后任意位置，
        // 第一个非选项参数是位置参数（文件 / 目录 / 关键字）。
        for (size_t i = 1; i < args.size(); ++i) {
            std::string k = args[i];
            auto nextVal = [&](std::string& out) {
                if (i + 1 < args.size()) out = args[++i];
            };
            if (k == "--json")            a.json = true;
            else if (k == "-h" || k == "--help") { printHelp(); return 0; }
            else if (k == "--strings")    a.strings = true;
            else if (k == "--dialog-only")a.dialogOnly = true;
            else if (k == "--drop-numbers") a.dropNumbers = true;
            else if (k == "--full")       a.full = true;
            else if (k == "--show-unknown") a.showUnknown = true;
            else if (k == "--by-sprite")  a.bySprite = true;
            else if (k == "--detail")    a.detail = true;
            else if (k == "--regex")     a.regex = true;
            else if (k == "--raw")        a.raw = true;
            else if (k == "--dry-run")    a.dryRun = true;
            else if (k == "--force" || k == "-f") a.force = true;
            else if (k == "--info-table") a.infoTable = true;
            else if (k == "--script")     a.script = true;
            else if (k == "--limit") {
                std::string v; nextVal(v);
                try { a.limit = std::stoi(v); a.limitSet = true; } catch (...) {}
            }
            else if (k == "--threshold") {
                std::string v; nextVal(v);
                try { a.threshold = std::stoi(v); } catch (...) {}
            }
            else if (k == "--depth") {
                std::string v; nextVal(v);
                try { a.depth = std::stoi(v); } catch (...) {}
            }
            else if (k == "--jobs") {
                std::string v; nextVal(v);
                try { a.jobs = std::stoi(v); } catch (...) {}
            }
            else if (k == "--sprite")    nextVal(a.sprite);
            else if (k == "--extract")   nextVal(a.extract);
            else if (k == "--dir")       nextVal(a.dir);
            else if (k == "--file")      nextVal(a.fileFilter);
            else if (k == "--sort")      nextVal(a.sort);
            else if (!isOption(k)) {
                // 位置参数：首个是主参数（文件/目录/关键字），
                // refs 的第二个位置参数是名字过滤（进 extra）。
                if (!positionalSet) {
                    positionalSet = true;
                    if (a.cmd == "ls")            a.path = k;
                    else if (a.cmd == "find")     a.keyword = k;
                    else                          a.file = k;
                } else {
                    a.extra.push_back(k);
                }
            }
            else {
                std::cerr << "未知选项：" << k << "\n";
                return 2;
            }
        }

        if      (a.cmd == "info")    return cmd_info(a);
        else if (a.cmd == "sprites") return cmd_sprites(a);
        else if (a.cmd == "text")    return cmd_text(a);
        else if (a.cmd == "script")  return cmd_script(a);
        else if (a.cmd == "vars")    return cmd_vars(a);
        else if (a.cmd == "assets")  return cmd_assets(a);
        else if (a.cmd == "media")   return cmd_media(a);
        else if (a.cmd == "blocks")  return cmd_blocks(a);
        else if (a.cmd == "json")    return cmd_json(a);
        else if (a.cmd == "raw")     return cmd_raw(a);
        else if (a.cmd == "ls")      return cmd_ls(a);
        else if (a.cmd == "find")    return cmd_find(a);
        else if (a.cmd == "refs")    return cmd_refs(a);
        else if (a.cmd == "events")  return cmd_events(a);
        else if (a.cmd == "diff")    return cmd_diff(a);
        else if (a.cmd == "dup")     return cmd_dup(a);
        else if (a.cmd == "check")   return cmd_check(a);
        else if (a.cmd == "fix")     return cmd_fix(a);
        else if (a.cmd == "view")    return cmd_view(a);
        else if (a.cmd == "search")  return cmd_search(a);
        else if (a.cmd == "project") return cmd_project(a);
        else if (a.cmd == "pack")    return cmd_pack(a);
        else if (a.cmd == "unpack")  return cmd_unpack(a);
        else {
            std::cerr << "未知命令：" << a.cmd << "\n";
            printHelp();
            return 2;
        }
    } catch (const Sb1Error& e) {
        std::cerr << "错误：" << e.what() << "\n";
        return 1;
    } catch (const Sb3Error& e) {
        std::cerr << "错误：" << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "错误：" << e.what() << "\n";
        return 1;
    }
    return 0;
}