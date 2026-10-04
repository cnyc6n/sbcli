// src/sbcli_search.cpp
// `sb search <关键词>` 的实现（format.md §2 的配套查找手册）。
//
// 设计：
//   · 中文概念 → 相关 opcode 的映射表（CONCEPTS）。
//   · opcode 的 category / 一行示例 是手工精选的元数据（与 SB3_T 一致、可直接照抄）。
//   · 参数清单（字段名 + 类型）从 SB3_T 模板的 {X} 占位符抽取（复用
//     sbcli_parser 的 sbcOpcodeFields），类型用 FIELD_MAP（菜单）+ 命名启发式判定。
//   · 匹配：关键词命中某个中文概念，或作为 opcode 子串（不区分大小写）。
//   · 无匹配：列出 opcode 子串命中 / 概念名子串命中的相近建议。
#include "sbcli_search.hpp"
#include "sbcli_parser.hpp"
#include "sb3_tables.hpp"
#include "commands.hpp"   // Args
#include "jdoc.hpp"      // Json
#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace sb {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

// ------------------------------------------------------------------ 概念映射
// 中文概念词 → 相关 opcode 列表。覆盖 format.md §2 高频块 + 常用扩展。
struct ConceptRow { const char* concept; const char* opcodes; };
const ConceptRow CONCEPTS[] = {
    {"广播", "event_broadcast event_broadcastandwait event_whenbroadcastreceived"},
    {"变量", "data_setvariableto data_changevariableby data_variable data_showvariable data_hidevariable"},
    {"列表", "data_addtolist data_deleteoflist data_itemoflist data_lengthoflist data_insertatlist data_replaceitemoflist data_deletealloflist data_listcontainsitem data_showlist data_hidelist"},
    {"如果", "control_if control_if_else"},
    {"循环", "control_repeat control_forever control_repeat_until control_while control_for_each"},
    {"等待", "control_wait control_wait_until"},
    {"移动", "motion_movesteps motion_goto motion_gotoxy motion_glideto motion_glidesecstoxy motion_changexby motion_setx motion_changeyby motion_sety"},
    {"转向", "motion_turnright motion_turnleft motion_pointindirection motion_pointtowards"},
    {"说", "looks_say looks_sayforsecs"},
    {"思考", "looks_think looks_thinkforsecs"},
    {"造型", "looks_switchcostumeto looks_nextcostume looks_switchbackdropto looks_nextbackdrop looks_changeeffectby looks_seteffectto looks_show looks_hide"},
    {"大小", "looks_changesizeby looks_setsizeto"},
    {"声音", "sound_play sound_playuntildone sound_stopallsounds sound_changevolumeby sound_setvolumeto"},
    {"克隆", "control_create_clone_of control_start_as_clone control_delete_this_clone"},
    {"停止", "control_stop"},
    {"绿旗", "event_whenflagclicked"},
    {"按键", "event_whenkeypressed event_whenthisspriteclicked event_whenstageclicked"},
    {"点击", "event_whenthisspriteclicked event_whenstageclicked"},
    {"背景", "looks_switchbackdropto looks_nextbackdrop event_whenbackdropswitchesto"},
    {"运算", "operator_add operator_subtract operator_multiply operator_divide operator_mod operator_random operator_gt operator_lt operator_equals operator_and operator_or operator_not operator_join operator_letter_of operator_length operator_contains operator_round operator_mathop"},
    {"侦测", "sensing_touchingobject sensing_touchingcolor sensing_keypressed sensing_distanceto sensing_askandwait sensing_timer sensing_mousedown sensing_answer sensing_of sensing_current"},
    {"计时器", "sensing_timer sensing_resettimer"},
    {"询问", "sensing_askandwait"},
    {"画笔", "pen_penDown pen_penUp pen_stamp pen_clear pen_setPenColorToColor pen_changePenSizeBy pen_setPenSizeTo pen_changePenColorParamBy pen_setPenColorParamTo"},
    {"自定义积木", "procedures_definition procedures_call argument_reporter_string_number argument_reporter_boolean"},
    {"旋转", "motion_setrotationstyle"},
    {"反弹", "motion_ifonedgebounce"},
    {"显示", "looks_show data_showvariable data_showlist"},
    {"隐藏", "looks_hide data_hidevariable data_hidelist"},
    // ---- 补全：常用同义词 / 单独概念（避免中文查询落空）----
    {"重复", "control_repeat control_forever control_repeat_until control_while control_for_each"},
    {"重复执行", "control_forever"},
    {"重复直到", "control_repeat_until"},
    {"永远", "control_forever"},
    {"碰到", "sensing_touchingobject"},
    {"接触", "sensing_touchingobject"},
    {"碰到颜色", "sensing_touchingcolor sensing_coloristouchingcolor"},
    {"颜色", "sensing_touchingcolor sensing_coloristouchingcolor pen_setPenColorToColor looks_seteffectto"},
    {"距离", "sensing_distanceto"},
    {"大于", "operator_gt"},
    {"小于", "operator_lt"},
    {"等于", "operator_equals"},
    {"加", "operator_add data_changevariableby"},
    {"减", "operator_subtract"},
    {"乘", "operator_multiply"},
    {"除", "operator_divide"},
    {"取余", "operator_mod"},
    {"随机", "operator_random"},
    {"并且", "operator_and"},
    {"或者", "operator_or"},
    {"不成立", "operator_not"},
    {"连接", "operator_join"},
    {"第几个字符", "operator_letter_of"},
    {"长度", "operator_length data_lengthoflist"},
    {"包含", "operator_contains data_listcontainsitem"},
    {"四舍五入", "operator_round operator_mathop"},
    {"鼠标", "sensing_mousedown sensing_distanceto sensing_touchingobject"},
    {"回答", "sensing_answer sensing_askandwait"},
    {"坐标", "motion_gotoxy motion_glidesecstoxy motion_setx motion_sety"},
    {"x坐标", "motion_setx motion_changexby motion_xposition"},
    {"y坐标", "motion_sety motion_changeyby motion_yposition"},
    {"方向", "motion_pointindirection motion_pointtowards motion_direction"},
    {"朝向", "motion_pointtowards"},
    {"滑行", "motion_glidesecstoxy motion_glideto"},
    {"跳到", "motion_goto motion_gotoxy"},
    {"下一个造型", "looks_nextcostume"},
    {"背景换成", "looks_switchbackdropto"},
    {"特效", "looks_seteffectto looks_changeeffectby"},
    {"图章", "pen_stamp"},
    {"落笔", "pen_penDown"},
    {"抬笔", "pen_penUp"},
    {"清空", "pen_clear data_deletealloflist"},
    {"加入列表", "data_addtolist"},
    {"删除", "data_deleteoflist data_deletealloflist"},
    {"插入", "data_insertatlist"},
    {"替换", "data_replaceitemoflist"},
    {"输出", "looks_say looks_sayforsecs"},
    {"音量", "sound_setvolumeto sound_changevolumeby"},
    {"播放", "sound_play sound_playuntildone"},
    {"克隆自己", "control_create_clone_of control_start_as_clone"},
    {"当作为克隆体启动", "control_start_as_clone"},
    {"删除克隆体", "control_delete_this_clone"},
    {"停止全部", "control_stop"},
    {"当角色被点击", "event_whenthisspriteclicked"},
    {"当按下", "event_whenkeypressed"},
    {"广播并等待", "event_broadcastandwait"},
    {"当接收到", "event_whenbroadcastreceived"},
};

// ------------------------------------------------------------------ opcode 元数据
// category + 一行示例（精选，值与 SB3_T 模板语义一致，可直接照抄）。
struct OpMeta { const char* category; const char* example; };
const std::map<std::string, OpMeta> OP_META = {
    // 事件
    {"event_whenflagclicked",   {"事件", "event_whenflagclicked"}},
    {"event_whenbroadcastreceived", {"事件", "event_whenbroadcastreceived BROADCAST_OPTION=游戏开始"}},
    {"event_whenkeypressed",    {"事件", "event_whenkeypressed KEY_OPTION=space"}},
    {"event_whenthisspriteclicked", {"事件", "event_whenthisspriteclicked"}},
    {"event_whenstageclicked",  {"事件", "event_whenstageclicked"}},
    {"event_whenbackdropswitchesto", {"事件", "event_whenbackdropswitchesto BACKDROP=背景1"}},
    {"event_whengreaterthan",   {"事件", "event_whengreaterthan WHENGREATERTHANMENU=计时器 VALUE=10"}},
    {"event_broadcast",         {"事件", "event_broadcast BROADCAST_INPUT=游戏开始"}},
    {"event_broadcastandwait",  {"事件", "event_broadcastandwait BROADCAST_INPUT=游戏开始"}},
    // 动作
    {"motion_movesteps",        {"动作", "motion_movesteps STEPS=10"}},
    {"motion_turnright",        {"动作", "motion_turnright DEGREES=15"}},
    {"motion_turnleft",         {"动作", "motion_turnleft DEGREES=15"}},
    {"motion_goto",             {"动作", "motion_goto TO=_random_"}},
    {"motion_gotoxy",           {"动作", "motion_gotoxy X=0 Y=0"}},
    {"motion_glideto",          {"动作", "motion_glideto SECS=1 TO=_mouse_"}},
    {"motion_glidesecstoxy",    {"动作", "motion_glidesecstoxy SECS=0.5 X=0 Y=0"}},
    {"motion_pointindirection", {"动作", "motion_pointindirection DIRECTION=90"}},
    {"motion_pointtowards",     {"动作", "motion_pointtowards TOWARDS=_mouse_"}},
    {"motion_changexby",        {"动作", "motion_changexby DX=10"}},
    {"motion_setx",             {"动作", "motion_setx X=0"}},
    {"motion_changeyby",        {"动作", "motion_changeyby DY=10"}},
    {"motion_sety",             {"动作", "motion_sety Y=0"}},
    {"motion_ifonedgebounce",   {"动作", "motion_ifonedgebounce"}},
    {"motion_setrotationstyle", {"动作", "motion_setrotationstyle STYLE=all_around"}},
    // 外观
    {"looks_say",               {"外观", R"(looks_say MESSAGE="你好")"}},
    {"looks_sayforsecs",        {"外观", R"(looks_sayforsecs MESSAGE="你好" SECS=2)"}},
    {"looks_think",             {"外观", R"(looks_think MESSAGE="嗯")"}},
    {"looks_thinkforsecs",      {"外观", R"(looks_thinkforsecs MESSAGE="嗯" SECS=2)"}},
    {"looks_switchcostumeto",   {"外观", "looks_switchcostumeto COSTUME=造型1"}},
    {"looks_nextcostume",       {"外观", "looks_nextcostume"}},
    {"looks_switchbackdropto",  {"外观", "looks_switchbackdropto BACKDROP=背景1"}},
    {"looks_nextbackdrop",      {"外观", "looks_nextbackdrop"}},
    {"looks_changesizeby",      {"外观", "looks_changesizeby CHANGE=10"}},
    {"looks_setsizeto",         {"外观", "looks_setsizeto SIZE=100"}},
    {"looks_changeeffectby",    {"外观", "looks_changeeffectby EFFECT=颜色 CHANGE=25"}},
    {"looks_seteffectto",       {"外观", "looks_seteffectto EFFECT=颜色 VALUE=0"}},
    {"looks_cleargraphiceffects", {"外观", "looks_cleargraphiceffects"}},
    {"looks_show",              {"外观", "looks_show"}},
    {"looks_hide",              {"外观", "looks_hide"}},
    // 声音
    {"sound_play",              {"声音", "sound_play SOUND_MENU=音效1"}},
    {"sound_playuntildone",     {"声音", "sound_playuntildone SOUND_MENU=音效1"}},
    {"sound_stopallsounds",     {"声音", "sound_stopallsounds"}},
    {"sound_changeeffectby",    {"声音", "sound_changeeffectby EFFECT=音高 VALUE=10"}},
    {"sound_seteffectto",       {"声音", "sound_seteffectto EFFECT=音高 VALUE=0"}},
    {"sound_cleareffects",      {"声音", "sound_cleareffects"}},
    {"sound_changevolumeby",    {"声音", "sound_changevolumeby VOLUME=10"}},
    {"sound_setvolumeto",       {"声音", "sound_setvolumeto VOLUME=100"}},
    // 控制
    {"control_wait",            {"控制", "control_wait DURATION=1"}},
    {"control_repeat",          {"控制", "control_repeat TIMES=10"}},
    {"control_forever",         {"控制", "control_forever"}},
    {"control_if",              {"控制", "control_if CONDITION=<operator_gt OPERAND1=(data_variable VARIABLE=分数) OPERAND2=100>"}},
    {"control_if_else",         {"控制", "control_if_else CONDITION=<sensing_touchingobject TOUCHINGOBJECTMENU=苹果>"}},
    {"control_wait_until",      {"控制", "control_wait_until CONDITION=<sensing_mousedown>"}},
    {"control_repeat_until",    {"控制", "control_repeat_until CONDITION=<operator_not OPERAND=<sensing_keypressed KEY_OPTION=space>>"}},
    {"control_while",           {"控制", "control_while CONDITION=<sensing_touchingobject TOUCHINGOBJECTMENU=苹果>"}},
    {"control_for_each",        {"控制", "control_for_each VARIABLE=项 VALUE=[苹果, 香蕉]"}},
    {"control_stop",            {"控制", "control_stop STOP_OPTION=all"}},
    {"control_start_as_clone",  {"控制", "control_start_as_clone"}},
    {"control_create_clone_of", {"控制", "control_create_clone_of CLONE_OPTION=_myself_"}},
    {"control_delete_this_clone", {"控制", "control_delete_this_clone"}},
    // 数据
    {"data_variable",           {"数据", "data_variable VARIABLE=分数"}},
    {"data_setvariableto",      {"数据", "data_setvariableto VARIABLE=分数 VALUE=0"}},
    {"data_changevariableby",   {"数据", "data_changevariableby VARIABLE=分数 VALUE=1"}},
    {"data_showvariable",       {"数据", "data_showvariable VARIABLE=分数"}},
    {"data_hidevariable",       {"数据", "data_hidevariable VARIABLE=分数"}},
    {"data_listcontents",       {"数据", "data_listcontents LIST=道具"}},
    {"data_addtolist",          {"数据", "data_addtolist LIST=道具 ITEM=苹果"}},
    {"data_deleteoflist",       {"数据", "data_deleteoflist LIST=道具 INDEX=1"}},
    {"data_deletealloflist",    {"数据", "data_deletealloflist LIST=道具"}},
    {"data_insertatlist",       {"数据", "data_insertatlist LIST=道具 ITEM=苹果 INDEX=1"}},
    {"data_replaceitemoflist",  {"数据", "data_replaceitemoflist LIST=道具 INDEX=1 ITEM=苹果"}},
    {"data_itemoflist",         {"数据", "data_itemoflist LIST=道具 INDEX=1"}},
    {"data_itemnumoflist",      {"数据", "data_itemnumoflist ITEM=苹果 LIST=道具"}},
    {"data_lengthoflist",       {"数据", "data_lengthoflist LIST=道具"}},
    {"data_listcontainsitem",   {"数据", "data_listcontainsitem LIST=道具 ITEM=苹果"}},
    {"data_showlist",           {"数据", "data_showlist LIST=道具"}},
    {"data_hidelist",           {"数据", "data_hidelist LIST=道具"}},
    // 运算
    {"operator_add",            {"运算", "operator_add NUM1=1 NUM2=2"}},
    {"operator_subtract",       {"运算", "operator_subtract NUM1=3 NUM2=1"}},
    {"operator_multiply",       {"运算", "operator_multiply NUM1=3 NUM2=4"}},
    {"operator_divide",         {"运算", "operator_divide NUM1=8 NUM2=2"}},
    {"operator_mod",            {"运算", "operator_mod NUM1=10 NUM2=3"}},
    {"operator_random",         {"运算", "operator_random FROM=1 TO=10"}},
    {"operator_gt",             {"运算", "operator_gt OPERAND1=1 OPERAND2=2"}},
    {"operator_lt",             {"运算", "operator_lt OPERAND1=1 OPERAND2=2"}},
    {"operator_equals",         {"运算", "operator_equals OPERAND1=1 OPERAND2=1"}},
    {"operator_and",            {"运算", "operator_and OPERAND1=<...> OPERAND2=<...>"}},
    {"operator_or",             {"运算", "operator_or OPERAND1=<...> OPERAND2=<...>"}},
    {"operator_not",            {"运算", "operator_not OPERAND=<sensing_keypressed KEY_OPTION=space>"}},
    {"operator_join",           {"运算", R"(operator_join STRING1="你好" STRING2="世界")"}},
    {"operator_letter_of",      {"运算", "operator_letter_of LETTER=1 STRING=\"abc\""}},
    {"operator_length",         {"运算", R"(operator_length STRING="你好")"}},
    {"operator_contains",       {"运算", R"(operator_contains STRING1="你好" STRING2="好")"}},
    {"operator_round",          {"运算", "operator_round NUM=3.14"}},
    {"operator_mathop",         {"运算", "operator_mathop NUM=9 OPERATOR=sqrt"}},
    // 侦测
    {"sensing_touchingobject",  {"侦测", "sensing_touchingobject TOUCHINGOBJECTMENU=苹果"}},
    {"sensing_touchingcolor",   {"侦测", "sensing_touchingcolor COLOR=#ff0000"}},
    {"sensing_coloristouchingcolor", {"侦测", "sensing_coloristouchingcolor COLOR=#ff0000 COLOR2=#00ff00"}},
    {"sensing_distanceto",      {"侦测", "sensing_distanceto DISTANCETOMENU=_mouse_"}},
    {"sensing_askandwait",      {"侦测", R"(sensing_askandwait QUESTION="你叫什么？")"}},
    {"sensing_answer",          {"侦测", "sensing_answer"}},
    {"sensing_keypressed",      {"侦测", "sensing_keypressed KEY_OPTION=space"}},
    {"sensing_mousedown",       {"侦测", "sensing_mousedown"}},
    {"sensing_mousex",          {"侦测", "sensing_mousex"}},
    {"sensing_mousey",          {"侦测", "sensing_mousey"}},
    {"sensing_loudness",        {"侦测", "sensing_loudness"}},
    {"sensing_timer",           {"侦测", "sensing_timer"}},
    {"sensing_resettimer",      {"侦测", "sensing_resettimer"}},
    {"sensing_of",              {"侦测", "sensing_of OBJECT=角色1 PROPERTY=x position"}},
    {"sensing_current",         {"侦测", "sensing_current CURRENTMENU=年"}},
    {"sensing_dayssince2000",   {"侦测", "sensing_dayssince2000"}},
    {"sensing_username",        {"侦测", "sensing_username"}},
    {"sensing_setdragmode",     {"侦测", "sensing_setdragmode DRAG_MODE=draggable"}},
    // 自定义积木
    {"procedures_definition",   {"自定义积木", R"(procedures_definition PROCCODE="移动 %s 步" ARGS=[步数])"}},
    {"procedures_call",         {"自定义积木", R"(procedures_call PROCCODE="移动 %s 步" ARG1=10)"}},
    {"argument_reporter_string_number", {"自定义积木", "argument_reporter_string_number VALUE=步数"}},
    {"argument_reporter_boolean", {"自定义积木", "argument_reporter_boolean VALUE=开关"}},
    // 画笔
    {"pen_clear",               {"画笔", "pen_clear"}},
    {"pen_stamp",               {"画笔", "pen_stamp"}},
    {"pen_penDown",             {"画笔", "pen_penDown"}},
    {"pen_penUp",               {"画笔", "pen_penUp"}},
    {"pen_setPenColorToColor",  {"画笔", "pen_setPenColorToColor COLOR=#ff0000"}},
    {"pen_changePenColorParamBy", {"画笔", "pen_changePenColorParamBy COLOR_PARAM=色调 VALUE=10"}},
    {"pen_setPenColorParamTo",  {"画笔", "pen_setPenColorParamTo COLOR_PARAM=色调 VALUE=0"}},
    {"pen_changePenSizeBy",     {"画笔", "pen_changePenSizeBy SIZE=1"}},
    {"pen_setPenSizeTo",        {"画笔", "pen_setPenSizeTo SIZE=1"}},
    {"pen_setPenShadeToNumber", {"画笔", "pen_setPenShadeToNumber SHADE=0"}},
    {"pen_changePenShadeBy",    {"画笔", "pen_changePenShadeBy SHADE=10"}},
};

// 字段类型启发式
const std::set<std::string> NUMERIC_FIELDS = {
    "STEPS", "DEGREES", "DIRECTION", "X", "Y", "SECS", "DURATION", "TIMES",
    "DX", "DY", "CHANGE", "SIZE", "VOLUME", "FROM", "TO", "LETTER", "INDEX",
    "NUM", "SHADE", "NUM1", "NUM2", "OPERAND1", "OPERAND2",
};
// 布尔条件输入（<...>）：opcode:field
const std::set<std::string> BOOL_PAIRS = {
    "control_if:CONDITION", "control_if_else:CONDITION",
    "control_wait_until:CONDITION", "control_repeat_until:CONDITION",
    "control_while:CONDITION", "operator_not:OPERAND",
};

std::string classifyType(const std::string& opcode, const std::string& field) {
    if (FIELD_MAP.count(field))   return "menu";
    if (BOOL_PAIRS.count(opcode + ":" + field)) return "boolean";
    if (NUMERIC_FIELDS.count(field)) return "number";
    return "text";
}

} // namespace

SearchResult sbSearch(const std::string& keywordRaw) {
    SearchResult r;
    r.keyword = keywordRaw;
    std::string kw = toLower(keywordRaw);

    // 1) 命中中文概念表
    std::vector<std::string> matchedOps;
    for (const auto& c : CONCEPTS) {
        if (kw == toLower(c.concept) ||
            (kw.size() >= 2 && toLower(c.concept).find(kw) != std::string::npos)) {
            std::stringstream ss(c.opcodes);
            std::string o;
            while (ss >> o) matchedOps.push_back(o);
        }
    }
    // 2) opcode 子串命中（不区分大小写）
    for (const auto& kv : OP_META) {
        if (kv.first.find(kw) != std::string::npos) {
            if (std::find(matchedOps.begin(), matchedOps.end(), kv.first) == matchedOps.end())
                matchedOps.push_back(kv.first);
        }
    }

    // 按概念表顺序 / 字母序稳定输出
    std::stable_sort(matchedOps.begin(), matchedOps.end());
    matchedOps.erase(std::unique(matchedOps.begin(), matchedOps.end()), matchedOps.end());

    for (const auto& op : matchedOps) {
        SearchMatch m;
        m.opcode = op;
        m.category = OP_META.count(op) ? OP_META.at(op).category : "其他";
        m.templateText = sbcOpcodeTemplate(op);
        if (m.templateText.empty()) m.templateText = "(无翻译模板)";
        m.example = OP_META.count(op) ? OP_META.at(op).example : op;

        std::vector<std::string> fields = sbcOpcodeFields(op);
        for (const auto& f : fields) {
            SearchParam p;
            p.name = f;
            p.type = classifyType(op, f);
            m.params.push_back(std::move(p));
        }
        r.matches.push_back(std::move(m));
    }

    // 3) 无匹配 → 相近建议
    if (r.matches.empty()) {
        // opcode 子串建议
        for (const auto& kv : OP_META)
            if (kv.first.find(kw) != std::string::npos)
                r.suggestions.push_back(kv.first);
        // 模板中文里含关键词的建议（kw 为英文时意义不大，但中文概念可能落到这里）
        for (const auto& kv : OP_META) {
            if (toLower(kv.second.example).find(kw) != std::string::npos)
                if (std::find(r.suggestions.begin(), r.suggestions.end(), kv.first) == r.suggestions.end())
                    r.suggestions.push_back(kv.first);
        }
        // 概念名子串建议
        for (const auto& c : CONCEPTS) {
            if (toLower(c.concept).find(kw) != std::string::npos) {
                std::stringstream ss(c.opcodes);
                std::string o;
                while (ss >> o)
                    if (std::find(r.suggestions.begin(), r.suggestions.end(), o) == r.suggestions.end())
                        r.suggestions.push_back(o);
            }
        }
        std::stable_sort(r.suggestions.begin(), r.suggestions.end());
        r.suggestions.erase(std::unique(r.suggestions.begin(), r.suggestions.end()),
                            r.suggestions.end());
        if (r.suggestions.size() > 12) r.suggestions.resize(12);
    }
    return r;
}

int cmd_search(Args& a) {
    std::string kw = a.file.empty() ? a.path : a.file;
    // 首个 extra 也当作关键词（多词查询：sb search 广播 开始）
    for (const auto& e : a.extra) kw += " " + e;
    if (kw.empty()) {
        std::cerr << "用法：sb search <关键词>  [--json]\n"
                  << "  关键词可以是中文概念（广播 / 变量 / 如果 / 移动）或英文 opcode（looks_say）\n";
        return 2;
    }

    SearchResult r = sbSearch(kw);

    if (a.json) {
        Json out = Json::object();
        out["keyword"] = r.keyword;
        Json matches = Json::array();
        for (const auto& m : r.matches) {
            Json o = Json::object();
            o["opcode"] = m.opcode;
            o["category"] = m.category;
            o["template"] = m.templateText;
            o["example"] = m.example;
            Json ps = Json::array();
            for (const auto& p : m.params) {
                Json pp = Json::object();
                pp["name"] = p.name;
                pp["type"] = p.type;
                ps.push_back(std::move(pp));
            }
            o["params"] = std::move(ps);
            matches.push_back(std::move(o));
        }
        out["matches"] = std::move(matches);
        Json sugg = Json::array();
        for (const auto& s : r.suggestions) sugg.push_back(s);
        out["suggestions"] = std::move(sugg);
        std::cout << out.dump(2) << "\n";
        return r.matches.empty() ? 1 : 0;
    }

    // 文本模式
    if (r.matches.empty()) {
        std::cout << "没有匹配「" << r.keyword << "」的语法块。\n";
        if (!r.suggestions.empty()) {
            std::cout << "相近的 opcode / 概念：\n";
            for (const auto& s : r.suggestions) {
                auto it = OP_META.find(s);
                std::string tag = (it != OP_META.end())
                    ? std::string("  （") + it->second.category + "）"
                    : std::string();
                std::cout << "  " << s << tag << "\n";
            }
        } else {
            std::cout << "提示：试试更简单的关键词，如 广播 / 变量 / 如果 / 移动 / looks_say。\n";
        }
        return 1;
    }

    std::cout << "「" << r.keyword << "」匹配 " << r.matches.size() << " 个语法块：\n\n";
    for (size_t i = 0; i < r.matches.size(); ++i) {
        const auto& m = r.matches[i];
        if (i) std::cout << "\n";
        std::cout << "▸ " << m.opcode << "  [" << m.category << "]\n";
        std::cout << "    模板：" << m.templateText << "\n";
        if (m.params.empty()) {
            std::cout << "    参数：（无）\n";
        } else {
            std::cout << "    参数：\n";
            for (const auto& p : m.params)
                std::cout << "      · " << p.name << "  <" << p.type << ">\n";
        }
        std::cout << "    示例：" << m.example << "\n";
    }
    return 0;
}

} // namespace sb
