# -*- coding: utf-8 -*-
"""Generate minimal .sb3 test fixtures for sbcli testing."""
import json, zipfile, os

STAGE = {
    "isStage": True, "name": "Stage", "variables": {}, "lists": {},
    "broadcasts": {"B1": "\u5e7f\u64ad1"},
    "blocks": {}, "comments": {}, "currentCostume": 0, "costumes": [],
    "sounds": [], "volume": 100, "layerOrder": 0, "tempo": 60,
    "videoTransparency": 50, "videoState": "on", "textToSpeechLanguage": None,
}

def make_sprite(extra_var_value=0, say_text="\u4f60\u597d\u4e16\u754c", steps="10", extra_block=False):
    blocks = {
        "a1": {"opcode": "event_whenflagclicked", "next": "a2", "parent": None,
               "inputs": {}, "fields": {}, "shadow": False, "topLevel": True},
        "a2": {"opcode": "looks_say", "next": "a3", "parent": "a1",
               "inputs": {"MESSAGE": [1, [10, say_text]]}, "fields": {},
               "shadow": False, "topLevel": False},
        "a3": {"opcode": "motion_movesteps", "next": None, "parent": "a2",
               "inputs": {"STEPS": [1, [4, steps]]}, "fields": {},
               "shadow": False, "topLevel": False},
        "a4": {"opcode": "event_broadcast", "next": None, "parent": None,
               "inputs": {"BROADCAST_INPUT": [1, [11, "B1"]]}, "fields": {},
               "shadow": False, "topLevel": True},
        "a5": {"opcode": "data_setvariableto", "next": None, "parent": None,
               "inputs": {"VALUE": [1, [4, str(extra_var_value)]]},
               "fields": {"VARIABLE": ["\u5206\u6570", "v1"]},
               "shadow": False, "topLevel": True},
    }
    if extra_block:
        blocks["a6"] = {"opcode": "looks_nextcostume", "next": None, "parent": None,
                        "inputs": {}, "fields": {}, "shadow": False, "topLevel": True}
    return {
        "isStage": False, "name": "\u5c0f\u732b", "variables": {"v1": ["\u5206\u6570", extra_var_value]},
        "lists": {"l1": ["\u540d\u5355", []]}, "broadcasts": {},
        "blocks": blocks, "comments": {},
        "currentCostume": 0,
        "costumes": [{"assetId": "00000000000000000000000000000000", "name": "\u9020\u578b1",
                      "md5ext": "00000000000000000000000000000000.svg", "dataFormat": "svg",
                      "rotationCenterX": 0, "rotationCenterY": 0}],
        "sounds": [], "volume": 100, "layerOrder": 1, "visible": True,
        "x": 0, "y": 0, "size": 100, "direction": 90, "draggable": False,
        "rotationStyle": "all around",
    }

def build(name, sprite, meta_agent):
    proj = {
        "targets": [STAGE, sprite],
        "monitors": [],
        "extensions": [],
        "meta": {"semver": "3.0.0", "vm": "0.2.0", "agent": meta_agent},
    }
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(proj, ensure_ascii=False))
        # fake costume asset so zip asset lookup can be exercised
        z.writestr("00000000000000000000000000000000.svg", "<svg xmlns='http://www.w3.org/2000/svg'/>")
    print("wrote", path, os.path.getsize(path), "bytes")

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)
build("fixture_origin.sb3", make_sprite(extra_var_value=0), "scratch")
build("fixture_v2.sb3", make_sprite(extra_var_value=9, say_text="\u6211\u662f\u6539\u7248", steps="25", extra_block=True), "scratch-v2")
