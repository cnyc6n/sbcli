#include "sb2.hpp"
#include "sb3_tables.hpp"
#include <cmath>
#include <map>
#include <regex>

namespace sb {
using json = Json;

static std::string extOf(const std::string& md5) {
    auto pos = md5.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string e = md5.substr(pos + 1);
    for (auto& c : e) c = (char)::tolower((unsigned char)c);
    return e;
}

static json primOf(const json& v) {
    json a = json::array();
    if (v.is_boolean()) { a.push_back(10); a.push_back(v.get<bool>() ? "true" : "false"); }
    else if (v.is_number()) { a.push_back(4); a.push_back(v); }
    else { a.push_back(10); a.push_back(v.is_string() ? v.get<std::string>() : v.dump()); }
    return a;
}

class Conv {
public:
    std::map<std::string, json> blocks;
    int counter = 0;

    std::string newId() { return "b" + std::to_string(counter++); }

    // sb2 opcode → sb3 opcode（refs/events/blocks 统一按 sb3 匹配）
    static const std::map<std::string, std::string>& opMap() {
        static const std::map<std::string, std::string> m = {
            // ---- 数据 ----
            {"setVar:to:", "data_setvariableto"},
            {"changeVar:by:", "data_changevariableby"},
            {"readVariable", "data_variable"},
            {"showVariable:", "data_showvariable"},
            {"hideVariable:", "data_hidevariable"},
            {"append:toList:", "data_addtolist"},
            {"deleteLine:ofList:", "data_deleteoflist"},
            {"insert:at:ofList:", "data_insertatlist"},
            {"setLine:ofList:to:", "data_replaceitemoflist"},
            {"getLine:ofList:", "data_itemoflist"},
            {"lineCountOfList:", "data_lengthoflist"},
            {"list:contains:", "data_listcontainsitem"},
            {"showList:", "data_showlist"},
            {"hideList:", "data_hidelist"},
            {"contentsOfList:", "data_listcontents"},
            // ---- 事件 ----
            {"broadcast:", "event_broadcast"},
            {"doBroadcastAndWait", "event_broadcastandwait"},
            {"whenIReceive", "event_whenbroadcastreceived"},
            {"whenGreenFlag", "event_whenflagclicked"},
            {"whenKeyPressed", "event_whenkeypressed"},
            {"whenClicked", "event_whenthisspriteclicked"},
            {"whenSceneStarts", "event_whenbackdropswitchesto"},
            {"whenSensorGreaterThan", "event_whengreaterthan"},
            // ---- 控制 ----
            {"wait:elapsed:from:", "control_wait"},
            {"doRepeat", "control_repeat"},
            {"doForever", "control_forever"},
            {"doIf", "control_if"},
            {"doIfElse", "control_if_else"},
            {"doWaitUntil", "control_wait_until"},
            {"doUntil", "control_repeat_until"},
            {"stopScripts", "control_stop"},
            {"createCloneOf", "control_create_clone_of"},
            {"whenCloned", "control_start_as_clone"},
            {"deleteClone", "control_delete_this_clone"},
            // ---- 动作 ----
            {"forward:", "motion_movesteps"},
            {"turnRight:", "motion_turnright"},
            {"turnLeft:", "motion_turnleft"},
            {"heading:", "motion_pointindirection"},
            {"pointTowards:", "motion_pointtowards"},
            {"gotoX:y:", "motion_gotoxy"},
            {"gotoSpriteOrMouse:", "motion_goto"},
            {"glideSecs:toX:y:elapsed:from:", "motion_glidesecstoxy"},
            {"glideSecs:toX:y:elapsed:from:", "motion_glidesecstoxy"},
            {"changeXposBy:", "motion_changexby"},
            {"xpos:", "motion_setx"},
            {"changeYposBy:", "motion_changeyby"},
            {"ypos:", "motion_sety"},
            {"bounceOffEdge", "motion_ifonedgebounce"},
            {"setRotationStyle", "motion_setrotationstyle"},
            {"xpos", "motion_xposition"},
            {"ypos", "motion_yposition"},
            {"heading", "motion_direction"},
            // ---- 外观 ----
            {"say:duration:elapsed:from:", "looks_sayforsecs"},
            {"say:", "looks_say"},
            {"think:duration:elapsed:from:", "looks_thinkforsecs"},
            {"think:", "looks_think"},
            {"show", "looks_show"},
            {"hide", "looks_hide"},
            {"looks_changeeffectby", "looks_changeeffectby"},
            {"looks_seteffectto", "looks_seteffectto"},
            {"looks_cleargraphiceffects", "looks_cleargraphiceffects"},
            {"looks_switchcostumeto", "looks_switchcostumeto"},
            {"looks_nextcostume", "looks_nextcostume"},
            {"looks_switchbackdropto", "looks_switchbackdropto"},
            {"looks_switchbackdroptoandwait", "looks_switchbackdroptoandwait"},
            {"looks_nextbackdrop", "looks_nextbackdrop"},
            {"changeSizeBy:", "looks_changesizeby"},
            {"setSizeTo:", "looks_setsizeto"},
            {"looks_changegraphicEffectby", "looks_changeeffectby"},
            {"looks_setgraphicEffectto", "looks_seteffectto"},
            {"looks_changeLayerTo:", "looks_gotofrontback"},
            {"looks_goForwardBackwardLayers:", "looks_goforwardbackward"},
            {"size", "looks_size"},
            {"costumeIndex", "looks_costumenumbername"},
            // ---- 声音 ----
            {"playSound:", "sound_play"},
            {"doPlaySoundAndWait", "sound_playuntildone"},
            {"stopAllSounds", "sound_stopallsounds"},
            {"playDrum", "sound_playdrumforbeats"},
            {"drum:duration:elapsed:from:", "sound_playdrumforbeats"},
            {"noteOn:duration:elapsed:from:", "sound_playnote"},
            {"rest:elapsed:from:", "sound_restforbeats"},
            {"changeVolumeBy:", "sound_changevolumeby"},
            {"setVolumeTo:", "sound_setvolumeto"},
            {"volume", "sound_volume"},
            {"changeTempoBy:", "sound_changetempoby"},
            {"setTempoTo:", "sound_settempo"},
            {"tempo", "sound_tempo"},
            // ---- 画笔 ----
            {"clearPenTrails", "pen_clear"},
            {"stampCostume", "pen_stamp"},
            {"putPenDown", "pen_penDown"},
            {"putPenUp", "pen_penUp"},
            {"penColor:", "pen_setPenColorToColor"},
            {"changePenHueBy:", "pen_changePenColorParamBy"},
            {"setPenHueTo:", "pen_setPenColorParamTo"},
            {"changePenShadeBy:", "pen_changePenColorParamBy"},
            {"setPenShadeTo:", "pen_setPenColorParamTo"},
            {"changePenSizeBy:", "pen_changePenSizeBy"},
            {"penSize:", "pen_setPenSizeTo"},
            // ---- 侦测 ----
            {"touching:", "sensing_touchingobject"},
            {"touchingColor:", "sensing_touchingcolor"},
            {"color:sees:", "sensing_coloristouchingcolor"},
            {"distanceTo:", "sensing_distanceto"},
            {"doAsk", "sensing_askandwait"},
            {"answer", "sensing_answer"},
            {"keyPressed:", "sensing_keypressed"},
            {"mousePressed", "sensing_mousedown"},
            {"mouseX", "sensing_mousex"},
            {"mouseY", "sensing_mousey"},
            {"soundLevel", "sensing_loudness"},
            {"timer", "sensing_timer"},
            {"timerReset", "sensing_resettimer"},
            {"getAttribute:of:", "sensing_of"},
            {"timeAndDate", "sensing_current"},
            {"timestamp", "sensing_dayssince2000"},
            {"getUserName", "sensing_username"},
            // ---- 运算 ----
            {"+", "operator_add"},
            {"-", "operator_subtract"},
            {"*", "operator_multiply"},
            {"/", "operator_divide"},
            {"randomFrom:to:", "operator_random"},
            {">", "operator_gt"},
            {"<", "operator_lt"},
            {"=", "operator_equals"},
            {"&", "operator_and"},
            {"|", "operator_or"},
            {"not", "operator_not"},
            {"concatenate:with:", "operator_join"},
            {"letter:of:", "operator_letter_of"},
            {"stringLength:", "operator_length"},
            {"%", "operator_mod"},
            {"rounded", "operator_round"},
            {"computeFunction:of:", "operator_mathop"},
            // ---- 自定义积木 ----
            {"getParam", "argument_reporter_string_number"},
            {"call", "procedures_call"},
            {"procDef", "procedures_definition"},
        };
        return m;
    }

    std::string convStack(const json& body, int x = 0, int y = 0, bool markTop = true) {
        std::vector<std::string> ids;
        if (body.is_array()) {
            for (auto& blk : body) {
                if (blk.is_array() && !blk.empty() && blk[0].is_string())
                    ids.push_back(convBlock(blk));
            }
        }
        for (size_t i = 0; i + 1 < ids.size(); ++i) {
            blocks[ids[i]]["next"] = ids[i+1];
            blocks[ids[i+1]]["parent"] = ids[i];
        }
        if (!ids.empty() && markTop) {
            blocks[ids[0]]["topLevel"] = true;
            blocks[ids[0]]["x"] = x;
            blocks[ids[0]]["y"] = y;
        }
        return ids.empty() ? "" : ids[0];
    }

    std::string convBlock(const json& blk) {
        std::string op = blk[0].get<std::string>();
        // sb2 → sb3 opcode 映射（refs/events/blocks 统一按 sb3 匹配）
        auto om = opMap().find(op);
        std::string op3 = (om != opMap().end()) ? om->second : op;
        std::string bid = newId();
        json b;
        b["opcode"]   = op3;
        b["inputs"]   = json::object();
        b["fields"]   = json::object();
        b["next"]     = nullptr;
        b["parent"]   = nullptr;
        b["shadow"]   = false;
        b["topLevel"] = false;
        blocks[bid] = b;

        if (op == "procDef") {
            blocks[bid]["fields"]["PROCCODE"] = blk.size() > 1 && blk[1].is_string()
                                                ? blk[1].get<std::string>() : "?";
            std::string args;
            if (blk.size() > 2 && blk[2].is_array()) {
                for (size_t i = 0; i < blk[2].size(); ++i) {
                    if (i) args += " ";
                    args += blk[2][i].is_string() ? blk[2][i].get<std::string>()
                                                  : blk[2][i].dump();
                }
            }
            blocks[bid]["fields"]["ARGS"] = args;
            return bid;
        }

        int n = 0, subI = 0;
        for (size_t i = 1; i < blk.size(); ++i) {
            ++n;
            const json& arg = blk[i];
            if (arg.is_array()) {
                if (!arg.empty() && arg[0].is_string()) {
                    std::string sub = convBlock(arg);
                    blocks[sub]["parent"] = bid;
                    blocks[bid]["inputs"]["__" + std::to_string(n)] = json::array({2, sub});
                } else {
                    std::string key = (subI == 0) ? "SUBSTACK" : "SUBSTACK2";
                    std::string cid = convStack(arg, 0, 0, false);
                    if (!cid.empty()) {
                        blocks[cid]["parent"] = bid;
                        blocks[bid]["inputs"][key] = json::array({2, cid});
                    }
                    ++subI;
                }
            } else {
                blocks[bid]["inputs"]["__" + std::to_string(n)] =
                    json::array({1, primOf(arg)});
            }
        }
        // 从 inputs 里移除指定键（Json 无 erase，重建对象跳过目标键）
        auto dropInput = [&](const std::string& key) {
            json ninp = json::object();
            for (auto it = blocks[bid]["inputs"].begin();
                 it != blocks[bid]["inputs"].end(); ++it)
                if (it.key() != key) ninp[it.key()] = it.value();
            blocks[bid]["inputs"] = std::move(ninp);
        };
        // 广播参数归一：sb2 的广播名在 inputs.__1（[1,[10,"名"]]），
        // 转成 sb3 期望的形态，供 refs/events/render 统一识别。
        if (op3 == "event_broadcast" || op3 == "event_broadcastandwait") {
            auto it = blocks[bid]["inputs"].find("__1");
            if (it != blocks[bid]["inputs"].end() && it.value().is_array() &&
                it.value().size() > 1 && it.value()[1].is_array() &&
                it.value()[1].size() > 1) {
                // 取出广播名（字符串 / 数字）
                const json& nm = it.value()[1][1];
                std::string name = nm.is_string() ? nm.get<std::string>() : nm.dump();
                // 转成 sb3 的 BROADCAST_INPUT（带 shadow 文本结构 [2,[10,"名"]]）
                json v = json::array();
                v.push_back(2);
                json inner = json::array();
                inner.push_back(10);
                inner.push_back(name);
                v.push_back(inner);
                blocks[bid]["inputs"]["BROADCAST_INPUT"] = std::move(v);
                dropInput("__1");
            }
        } else if (op3 == "event_whenbroadcastreceived") {
            auto it = blocks[bid]["inputs"].find("__1");
            if (it != blocks[bid]["inputs"].end() && it.value().is_array() &&
                it.value().size() > 1 && it.value()[1].is_array() &&
                it.value()[1].size() > 1) {
                const json& nm = it.value()[1][1];
                std::string name = nm.is_string() ? nm.get<std::string>() : nm.dump();
                json fld = json::array();
                fld.push_back(name);
                blocks[bid]["fields"]["BROADCAST_OPTION"] = std::move(fld);
                dropInput("__1");
            }
        }
        // 通用归一：sb2 的位置参数 __1/__2… → sb3 opcode 的真实参数名。
        // 依据 SB3_T 模板里的 {占位符} 顺序（如 operator_lt 的 {OPERAND1} {OPERAND2}）。
        // 各 opcode 特例（广播/变量/列表）已在上方处理并 drop 掉，这里只补剩下的。
        {
            auto tmplIt = SB3_T.find(op3);
            if (tmplIt != SB3_T.end()) {
                std::vector<std::string> names;
                const std::string& t = tmplIt->second;
                for (size_t i = 0; i < t.size(); ++i) {
                    if (t[i] == '{') {
                        size_t e = t.find('}', i);
                        if (e != std::string::npos) {
                            names.push_back(t.substr(i + 1, e - i - 1));
                            i = e;
                        }
                    }
                }
                // 按位置把 __1..__N 映射到 names[0..N-1]（已存在的键跳过）
                auto inps = blocks[bid]["inputs"];   // 拷贝一份遍历，避免边改边读
                for (size_t k = 0; k < names.size(); ++k) {
                    std::string oldKey = "__" + std::to_string(k + 1);
                    if (!inps.contains(oldKey)) continue;
                    if (names[k].empty()) continue;
                    // 该名已存在就不覆盖（避免冲突）
                    if (blocks[bid]["inputs"].contains(names[k])) continue;
                    blocks[bid]["inputs"][names[k]] = inps[oldKey];
                    dropInput(oldKey);
                }

                // 菜单字段搬迁：sb3 里这些是 **fields**（菜单），但 sb2 把它们放在 inputs。
                // 不搬的话渲染走 inputs 路径、不做本地化（如 control_stop 显示 "all" 而非 "全部脚本"）。
                // 权威菜单集合 = FIELD_MAP 的键（与 pack 同一来源）。
                // 注意：排除 VARIABLE/LIST（它们有自己的 [名,id] 结构，由上方专门逻辑处理，
                //       误搬会把 id 当成名字）。
                for (size_t k = 0; k < names.size(); ++k) {
                    const std::string& nm = names[k];
                    if (nm.empty() || !FIELD_MAP.count(nm)) continue;
                    if (nm == "VARIABLE" || nm == "LIST") continue;
                    if (blocks[bid]["fields"].contains(nm)) continue;
                    auto iv = blocks[bid]["inputs"].find(nm);
                    if (iv == blocks[bid]["inputs"].end()) continue;
                    // 形态 [1,[10,"值"]] → 取内层值写成 fields 的 [值]
                    const json& arr = iv.value();
                    if (arr.is_array() && arr.size() > 1 && arr[1].is_array() &&
                        arr[1].size() > 1) {
                        json fld = json::array();
                        fld.push_back(arr[1][1]);
                        blocks[bid]["fields"][nm] = std::move(fld);
                        dropInput(nm);
                    }
                }
            }
        }
        // 变量/列表块的字段归一：sb2 的名字在 inputs.__N（[1,[10,"名"]]），
        // 转成 sb3 的 fields.VARIABLE / fields.LIST，供 refs/valueOf 识别。
        auto nameFrom = [&](int n) -> std::string {
            auto it = blocks[bid]["inputs"].find("__" + std::to_string(n));
            if (it == blocks[bid]["inputs"].end() || !it.value().is_array() ||
                it.value().size() <= 1 || !it.value()[1].is_array() ||
                it.value()[1].size() <= 1) return "";
            const json& nm = it.value()[1][1];
            return nm.is_string() ? nm.get<std::string>() : nm.dump();
        };
        if (op3 == "data_variable" || op3 == "data_setvariableto" ||
            op3 == "data_changevariableby" || op3 == "data_showvariable" ||
            op3 == "data_hidevariable") {
            std::string vn = nameFrom(1);
            if (!vn.empty()) {
                json fld = json::array();
                fld.push_back(vn);
                blocks[bid]["fields"]["VARIABLE"] = std::move(fld);
                dropInput("__1");
            }
            // 变量块的值/增量归一成 sb3 的 VALUE 输入（供 script 渲染）
            if (op3 == "data_setvariableto" || op3 == "data_changevariableby") {
                auto vit = blocks[bid]["inputs"].find("__2");
                if (vit != blocks[bid]["inputs"].end() && !blocks[bid]["inputs"].contains("VALUE"))
                    blocks[bid]["inputs"]["VALUE"] = vit.value();
            }
        } else if (op3 == "data_addtolist" || op3 == "data_deleteoflist" ||
                   op3 == "data_insertatlist" || op3 == "data_replaceitemoflist" ||
                   op3 == "data_itemoflist" || op3 == "data_lengthoflist" ||
                   op3 == "data_listcontainsitem") {
            // 列表名位置因块而异：append __2、deleteLine __2、insert __3、
            // setLine __2、getLine __2、lineCount __1、list:contains __1
            int lnPos = (op == "append:toList:" || op == "deleteLine:ofList:" ||
                         op == "setLine:ofList:to:" || op == "getLine:ofList:")
                            ? 2
                        : (op == "insert:at:ofList:") ? 3 : 1;
            std::string ln = nameFrom(lnPos);
            if (!ln.empty()) {
                json fld = json::array();
                fld.push_back(ln);
                blocks[bid]["fields"]["LIST"] = std::move(fld);
            }
            // 值/索引位置归一成 sb3 的 ITEM/INDEX 输入名（供 script 渲染）
            auto moveInput = [&](int from, const std::string& to) {
                auto it = blocks[bid]["inputs"].find("__" + std::to_string(from));
                if (it != blocks[bid]["inputs"].end() && !blocks[bid]["inputs"].contains(to))
                    blocks[bid]["inputs"][to] = it.value();
            };
            if (op3 == "data_addtolist")            moveInput(1, "ITEM");
            else if (op3 == "data_deleteoflist")    moveInput(1, "INDEX");
            else if (op3 == "data_insertatlist")  { moveInput(1, "ITEM"); moveInput(2, "INDEX"); }
            else if (op3 == "data_replaceitemoflist") { moveInput(1, "INDEX"); moveInput(3, "ITEM"); }
            else if (op3 == "data_itemoflist")     moveInput(1, "INDEX");
            else if (op3 == "data_listcontainsitem") moveInput(2, "ITEM");
        } else if (op3 == "argument_reporter_string_number" ||
                   op3 == "argument_reporter_boolean") {
            // sb2 getParam：参数名在 __1（[1,[10,"名"]]），转成 sb3 的 fields.VALUE
            std::string pn = nameFrom(1);
            if (!pn.empty()) {
                json fld = json::array();
                fld.push_back(pn);
                blocks[bid]["fields"]["VALUE"] = std::move(fld);
                dropInput("__1");
                dropInput("__2");
            }
        }
        return bid;
    }
};

static json sb2Entries(const json& items) {
    json out = json::object();
    if (items.is_object()) {
        int i = 0;
        for (auto it = items.begin(); it != items.end(); ++it, ++i) {
            out["e" + std::to_string(i)] = json::array({it.key(), it.value()});
        }
        return out;
    }
    if (items.is_array()) {
        for (size_t i = 0; i < items.size(); ++i) {
            const json& v = items[i];
            if (v.is_object()) {
                std::string name = v.value("name",
                                   v.value("listName",
                                   v.value("variableName", "项" + std::to_string(i))));
                json val = v.contains("value") ? v["value"]
                          : (v.contains("contents") ? v["contents"] : json(""));
                out["e" + std::to_string(i)] = json::array({name, val});
            } else if (v.is_array() && v.size() >= 2) {
                out["e" + std::to_string(i)] = json::array({v[0], v[1]});
            }
        }
    }
    return out;
}

static json sb2Target(const json& obj, bool isStage, Conv& conv) {
    json t;
    t["isStage"]    = isStage;
    t["name"]       = obj.value("objName", isStage ? "Stage" : "未命名");
    t["variables"]  = sb2Entries(obj.value("variables", json::object()));
    t["lists"]      = sb2Entries(obj.value("lists", json::object()));
    t["broadcasts"] = json::object();
    t["blocks"]     = json::object();
    t["comments"]   = json::object();

    json costumes = json::array();
    if (obj.contains("costumes") && obj["costumes"].is_array()) {
        for (auto& c : obj["costumes"]) {
            json x;
            x["name"] = c.value("costumeName", c.value("name", ""));
            x["md5ext"] = c.value("baseLayerMD5", c.value("md5ext", ""));
            x["baseLayerID"] = c.value("baseLayerID", 0);
            std::string fmt = c.value("dataFormat", "");
            if (fmt.empty()) fmt = extOf(x["md5ext"].get<std::string>());
            x["dataFormat"] = fmt;
            costumes.push_back(x);
        }
    }
    t["costumes"] = costumes;

    json sounds = json::array();
    if (obj.contains("sounds") && obj["sounds"].is_array()) {
        for (auto& s : obj["sounds"]) {
            json x;
            x["name"] = s.value("soundName", s.value("name", ""));
            x["md5ext"] = s.value("md5", s.value("md5ext", ""));
            x["soundID"] = s.value("soundID", 0);
            std::string fmt = s.value("format", "");
            if (fmt.empty()) fmt = extOf(x["md5ext"].get<std::string>());
            x["dataFormat"] = fmt;
            sounds.push_back(x);
        }
    }
    t["sounds"] = sounds;

    if (!isStage) {
        t["x"] = obj.value("scratchX", 0);
        t["y"] = obj.value("scratchY", 0);
        double scale = 1.0;
        if (obj.contains("scale")) {
            if (obj["scale"].is_number()) scale = obj["scale"].get<double>();
            else if (obj["scale"].is_string()) {
                try { scale = std::stod(obj["scale"].get<std::string>()); }
                catch (...) { scale = 1.0; }
            }
        }
        t["size"] = (int)std::lround(scale * 100);
        t["direction"] = obj.value("direction", 90);
        t["visible"]   = obj.value("visible", true);
    }

    if (obj.contains("scripts") && obj["scripts"].is_array()) {
        for (auto& s : obj["scripts"]) {
            if (!s.is_array() || s.size() < 3) continue;
            int x = s[0].is_number() ? s[0].get<int>() : 0;
            int y = s[1].is_number() ? s[1].get<int>() : 0;
            const json& body = s[2];
            conv.convStack(body, x, y, true);
        }
    }

    // 合并 conv.blocks
    for (auto& kv : conv.blocks)
        t["blocks"][kv.first] = kv.second;
    conv.blocks.clear();

    // 广播
    for (auto it = t["blocks"].begin(); it != t["blocks"].end(); ++it) {
        std::string op = it.value().value("opcode", "");
        if (op == "event_broadcast" || op == "event_broadcastandwait") {
            // 广播名在 inputs.BROADCAST_INPUT（归一后的 [2,[10,"名"]]）
            auto& inp = it.value()["inputs"];
            if (inp.contains("BROADCAST_INPUT") && inp["BROADCAST_INPUT"].is_array() &&
                inp["BROADCAST_INPUT"].size() > 1 &&
                inp["BROADCAST_INPUT"][1].is_array() &&
                inp["BROADCAST_INPUT"][1].size() > 1) {
                std::string name = inp["BROADCAST_INPUT"][1][1].is_string()
                                   ? inp["BROADCAST_INPUT"][1][1].get<std::string>()
                                   : inp["BROADCAST_INPUT"][1][1].dump();
                t["broadcasts"][name] = name;
            }
        } else if (op == "event_whenbroadcastreceived") {
            // 广播名在 fields.BROADCAST_OPTION
            auto& fld = it.value()["fields"];
            if (fld.contains("BROADCAST_OPTION") && fld["BROADCAST_OPTION"].is_array() &&
                !fld["BROADCAST_OPTION"].empty()) {
                std::string name = fld["BROADCAST_OPTION"][0].is_string()
                                   ? fld["BROADCAST_OPTION"][0].get<std::string>()
                                   : fld["BROADCAST_OPTION"][0].dump();
                t["broadcasts"][name] = name;
            }
        }
    }
    return t;
}

json sb2Convert(const json& data) {
    Conv conv;
    json targets = json::array();
    targets.push_back(sb2Target(data, true, conv));
    if (data.contains("children") && data["children"].is_array()) {
        for (auto& ch : data["children"]) {
            if (!ch.is_object()) continue;
            if (!ch.contains("objName") && !ch.contains("scripts") &&
                !ch.contains("costumes") && !ch.contains("sounds")) continue;
            targets.push_back(sb2Target(ch, false, conv));
        }
    }
    return targets;
}

} // namespace sb