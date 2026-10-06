// tools/build_ext_tables.js —— 批量解析扩展，生成 C++ 静态表
// 输入：F:/temp/all_ext_urls.json（id → URL，来自扫描作品）
// 输出：stdout（C++ 表片段），供人工/脚本写入 src/sb3_tables.hpp
// 用法: node tools/build_ext_tables.js [urls.json]
const fs = require('fs');
const path = require('path');
const { parseExtension, fetchText } = require('./ext_parse_lib.js');

async function main() {
  const urlsFile = process.argv[2] || 'F:/temp/all_ext_urls.json';
  const urls = JSON.parse(fs.readFileSync(urlsFile, 'utf-8'));
  // 扩展加载时会 console.log，重定向到 stderr，保持 stdout 只有 C++ 表
  const realLog = console.log;
  console.log = (...args) => process.stderr.write(args.join(' ') + '\n');
  const types = [];
  const names = [];
  let ok = 0, fail = 0;
  const failed = [];
  for (const [id, url] of Object.entries(urls)) {
    try {
      const code = await fetchText(url);
      const info = parseExtension(code);
      // 用作品的扩展 id（而非扩展自报 id），因为 opcode 前缀以作品 id 为准
      names.push([id, info.name]);
      for (const b of info.blocks) {
        const t = { command: 0, reporter: 1, boolean: 2, hat: 3 }[b.type] ?? 0;
        types.push([`${id}_${b.opcode}`, t]);
      }
      // 菜单桩（<id>_menu_<menu名>）：按 reporter 处理
      const menus = new Set();
      for (const b of info.blocks) {
        for (const a of Object.values(b.args || {})) {
          if (a && a.menu) menus.add(a.menu);
        }
      }
      for (const m of menus) types.push([`${id}_menu_${m}`, 1]);
      ok++;
      process.stderr.write(`OK   ${id} (${info.blocks.length} 块)\n`);
    } catch (e) {
      fail++;
      failed.push([id, String(e.message).slice(0, 60)]);
      process.stderr.write(`FAIL ${id}: ${String(e.message).slice(0, 60)}\n`);
    }
  }
  // 输出 C++ 片段（恢复真实 stdout）
  console.log = realLog;
  console.log('// === 自动解析的扩展积木表（tools/build_ext_tables.js 生成）===');
  console.log('inline const std::map<std::string, int> AUTO_EXT_BLOCK_TYPES = {');
  for (const [k, v] of types.sort((a, b) => a[0] < b[0] ? -1 : 1)) {
    console.log(`    {"${k}", ${v}},`);
  }
  console.log('};');
  console.log('');
  console.log('inline const std::map<std::string, std::string> AUTO_EXT_NAMES = {');
  for (const [k, v] of names.sort((a, b) => a[0] < b[0] ? -1 : 1)) {
    console.log(`    {"${k}", "${String(v).replace(/"/g, '\\"')}"},`);
  }
  console.log('};');
  process.stderr.write(`\n成功 ${ok} / 失败 ${fail}\n`);
  if (failed.length) {
    process.stderr.write('失败列表:\n');
    for (const [id, msg] of failed) process.stderr.write(`  ${id}: ${msg}\n`);
  }
}

main();
