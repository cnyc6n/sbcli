// src/commands.hpp
#pragma once
#include "common.hpp"
#include "sb1.hpp"
#include "sb3.hpp"

namespace sb {

struct Args {
    std::string cmd;
    std::string file;
    std::string path = ".";
    std::string keyword;
    std::string dir = ".";
    std::string sprite;
    std::string fileFilter;
    std::string extract;
    std::vector<std::string> extra;   // 额外位置参数（refs 的名字过滤 / diff 的第二文件等）
    int         limit = 40;
    bool        limitSet = false;   // 用户是否显式传了 --limit
    int         threshold = -1;     // dup：相似度阈值 %（0-100，-1=未设置）
    int         depth = 3;
    int         jobs = 4;
    bool        json = false;
    bool        strings = false;
    bool        dialogOnly = false;
    bool        dropNumbers = false;   // text：过滤纯数字/符号串（数值噪音）
    bool        full = false;
    bool        showUnknown = false;
    bool        bySprite = false;
    bool        detail = false;      // diff：展开积木明细（等价 --by-sprite）
    bool        regex = false;       // refs/find：过滤参数按正则匹配
    bool        raw = false;
    bool        dryRun = false;      // fix：只报告不写文件
    bool        force  = false;      // unpack：输出目录非空时覆盖
    bool        infoTable = false;
    bool        script = false;
    std::string sort = "name";
};

// 探测格式
std::string detectFormat(const std::string& path);  // "sb1" / "sb3"

int cmd_info   (Args& a);
int cmd_sprites(Args& a);
int cmd_text   (Args& a);
int cmd_script (Args& a);
int cmd_vars   (Args& a);
int cmd_assets (Args& a);
int cmd_media  (Args& a);
int cmd_blocks (Args& a);
int cmd_json   (Args& a);
int cmd_raw    (Args& a);
int cmd_ls     (Args& a);
int cmd_find   (Args& a);
int cmd_refs   (Args& a);
int cmd_events (Args& a);
int cmd_diff   (Args& a);
int cmd_dup    (Args& a);
int cmd_check  (Args& a);
int cmd_fix    (Args& a);
int cmd_view   (Args& a);
int cmd_search (Args& a);   // sb search <关键词> 语法查找
int cmd_project(Args& a);   // sb project init / add-*（单命令多子命令）
int cmd_pack   (Args& a);   // sb pack <项目目录> <输出.sb3>
int cmd_unpack (Args& a);   // sb unpack <作品.sb3> <输出目录>

} // namespace sb