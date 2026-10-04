// src/squeak_tables.cpp —— Squeak 常量表（字段名 / MacRoman / 类 ID）
#include "squeak.hpp"
#include <cstdint>

namespace sb {
// ---- 字段名表 ----
const std::map<std::string, std::vector<std::string>> FIELD_NAMES = {
    {"Morph", {"bounds","owner","submorphs","color","flags","properties"}},
    {"ScriptableScratchMorph", {"bounds","owner","submorphs","color","flags",
                                "properties","name","variables","scripts",
                                "isClone","media","costume"}},
    {"BlockMorph", {"bounds","owner","submorphs","color","flags","properties",
                    "isSpecialForm","oldColor"}},
    {"CommandBlockMorph", {"bounds","owner","submorphs","color","flags",
                           "properties","isSpecialForm","oldColor",
                           "commandSpec","argMorphs","titleMorph","receiver",
                           "selector","isReporter","isTimed","wantsName",
                           "wantsPossession"}},
    {"VariableBlockMorph", {"bounds","owner","submorphs","color","flags",
                            "properties","isSpecialForm","oldColor",
                            "commandSpec","argMorphs","titleMorph","receiver",
                            "selector","isReporter","isTimed","wantsName",
                            "wantsPossession","isBoolean"}},
    {"ScratchMedia", {"name"}},
    {"Image", {"name"}},
    {"Sound", {"name"}},
};
const std::map<std::string, std::string> FIELD_ALIASES = {
    {"Sprite", "ScriptableScratchMorph"},
    {"Stage",  "ScriptableScratchMorph"},
    {"SetterBlockMorph", "CommandBlockMorph"},
    {"CBlockMorph", "CommandBlockMorph"},
    {"HatBlockMorph", "BlockMorph"},
    {"EventHatMorph", "BlockMorph"},
};

const Value* SqueakObject::named(const std::string& name) const {
    auto it = FIELD_NAMES.find(cls);
    if (it == FIELD_NAMES.end()) {
        auto ai = FIELD_ALIASES.find(cls);
        if (ai != FIELD_ALIASES.end()) it = FIELD_NAMES.find(ai->second);
    }
    if (it == FIELD_NAMES.end()) return nullptr;
    auto& keys = it->second;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == name) return at(i);
    return nullptr;
}

bool isBlockClass(const std::string& cls) {
    static const std::set<std::string> s = {
        "BlockMorph","CommandBlockMorph","CBlockMorph","HatBlockMorph",
        "EventHatMorph","VariableBlockMorph","SetterBlockMorph",
        "ReporterBlockMorph","KeyEventHatMorph","MouseClickEventHatMorph"
    };
    return s.count(cls) > 0;
}

// ---- 宏罗马字符表 ----
const uint8_t MAC_ROMAN_TO_LATIN[128] = {
    196,197,199,201,209,214,220,225,224,226,228,227,229,231,233,
    232,234,235,237,236,238,239,241,243,242,244,246,245,250,249,
    251,252,134,176,162,163,167,149,182,223,174,169,153,180,168,
    128,198,216,129,177,138,141,165,181,142,143,144,154,157,170,
    186,158,230,248,191,161,172,166,131,173,178,171,187,133,160,
    192,195,213,140,156,150,151,147,148,145,146,247,179,255,159,
    185,164,139,155,188,189,135,183,130,132,137,194,202,193,203,
    200,205,206,207,204,211,212,190,210,218,219,217,208,136,152,
    175,215,221,222,184,240,253,254
};

// ---- 类 ID 表 ----
const std::map<int, std::string> FIXED_CLASSES = {
    {9,"String"},{10,"Symbol"},{11,"ByteArray"},{12,"SoundBuffer"},
    {13,"Bitmap"},{14,"UTF8"},
    {20,"Array"},{21,"OrderedCollection"},{22,"Set"},{23,"IdentitySet"},
    {24,"Dictionary"},{25,"IdentityDictionary"},
    {30,"Color"},{31,"TranslucentColor"},
    {32,"Point"},{33,"Rectangle"},{34,"Form"},{35,"ColorForm"},
};
const std::map<int, std::string> USER_CLASSES = {
    {100,"Morph"},{101,"BorderedMorph"},{102,"RectangleMorph"},{103,"EllipseMorph"},
    {104,"AlignmentMorph"},{105,"StringMorph"},{106,"UpdatingStringMorph"},
    {107,"SimpleSliderMorph"},{108,"SimpleButtonMorph"},{109,"SampledSound"},
    {110,"ImageMorph"},{111,"SketchMorph"},{123,"SensorBoardMorph"},
    {124,"Sprite"},{125,"Stage"},
    {140,"ChoiceArgMorph"},{141,"ColorArgMorph"},{142,"ExpressionArgMorph"},
    {145,"SpriteArgMorph"},{147,"BlockMorph"},{148,"CommandBlockMorph"},
    {149,"CBlockMorph"},{151,"HatBlockMorph"},{153,"ScratchScriptsMorph"},
    {154,"ScratchSliderMorph"},{155,"WatcherMorph"},{157,"SetterBlockMorph"},
    {158,"EventHatMorph"},{160,"VariableBlockMorph"},{162,"Image"},
    {163,"MovieMedia"},{164,"Sound"},{165,"KeyEventHatMorph"},
    {166,"BooleanArgMorph"},{167,"EventTitleMorph"},{168,"MouseClickEventHatMorph"},
    {169,"ExpressionArgMorphWithMenu"},{170,"ReporterBlockMorph"},
    {171,"MultilineStringMorph"},{172,"ToggleButton"},
    {173,"WatcherReadoutFrameMorph"},{174,"WatcherSliderMorph"},
    {175,"ScratchListMorph"},{176,"ScrollingStringMorph"},
};
} // namespace sb

