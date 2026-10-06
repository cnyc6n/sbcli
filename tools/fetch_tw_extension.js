// tools/fetch_tw_extension.js
// 自动联网寻找并解析 TurboWarp 扩展源码：
//   输入：扩展 id（如 "Encoding"）或完整 URL
//   输出：getInfo() 的块类型表（opcode → type + name + args）
// 用法：node tools/fetch_tw_extension.js <扩展id 或 URL> [缓存目录]
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const LIB = require(path.join(__dirname, 'ext_parse_lib.js'));
// 用 lib 的实现（单一权威），本地旧副本禁用
const makeScratch = LIB.makeScratch, makeSandbox = LIB.makeSandbox,
      fetchText = LIB.fetchText, parseExtension = LIB.parseExtension;

// ---------- Scratch API 模拟 ----------
/* 本地旧实现已由 ext_parse_lib.js 取代

function makeScratch() {
  const scratch = {
    BlockType: { COMMAND: 'command', REPORTER: 'reporter', BOOLEAN: 'boolean',
                 HAT: 'hat', CONDITIONAL: 'conditional' },
    ArgumentType: { STRING: 'string', NUMBER: 'number', BOOLEAN: 'boolean',
                    COLOR: 'color', MATRIX: 'matrix', ANGLE: 'angle',
                    IMAGE: 'image', NOTE: 'note' },
    Cast: { toNumber: x => Number(x), toString: x => String(x), toBoolean: x => !!x },
    extensions: { register: () => {}, unsandboxed: true },
  };
  const tf = function (x) { return x; };
  tf.setup = () => {};
  tf.setLocale = () => {};
  tf.translate = x => x;
  scratch.translate = tf;
  return scratch;
}
*/


/* 本地旧实现已由 ext_parse_lib.js 取代

function makeSandbox(scratch) {
  // 非沙盒扩展常在模块顶层访问 window/document/Scratch.vm.runtime，
  // 这里提供最小可用桩（只为让 getInfo() 可被调用，不真正执行功能）
  const noop = () => {};
  const fakeEl = new Proxy({}, {
    get: (t, k) => {
      if (k === 'style' || k === 'classList' || k === 'dataset') return fakeEl;
      if (k === 'appendChild' || k === 'removeChild' || k === 'addEventListener' ||
          k === 'removeEventListener' || k === 'setAttribute' || k === 'getAttribute' ||
          k === 'append' || k === 'remove' || k === 'focus' || k === 'click') return noop;
      if (k === 'children' || k === 'childNodes') return [];
      if (k === 'parentNode' || k === 'parentElement') return null;
      if (k === 'innerHTML' || k === 'textContent' || k === 'className' || k === 'id') return '';
      return fakeEl;
    },
    set: () => true,
  });
  const fakeWindow = new Proxy({}, {
    get: (t, k) => {
      if (k === 'addEventListener' || k === 'removeEventListener' ||
          k === 'postMessage' || k === 'focus' || k === 'open' || k === 'close') return noop;
      if (k === 'document') return fakeDocument;
      if (k === 'location') return { href: 'https://turbowarp.org/', origin: 'https://turbowarp.org' };
      if (k === 'localStorage' || k === 'sessionStorage')
        return { getItem: () => null, setItem: noop, removeItem: noop };
      if (k === 'innerWidth' || k === 'innerHeight') return 480;
      if (k === 'navigator') return fakeNavigator;
      if (k === 'devicePixelRatio') return 1;
      return undefined;
    },
    set: () => true,
  });
  const fakeDocument = new Proxy({}, {
    get: (t, k) => {
      if (k === 'createElement' || k === 'getElementById' || k === 'querySelector') return () => fakeEl;
      if (k === 'addEventListener' || k === 'removeEventListener') return noop;
      if (k === 'body' || k === 'head' || k === 'documentElement') return fakeEl;
      if (k === 'location') return { href: 'https://turbowarp.org/' };
      return undefined;
    },
    set: () => true,
  });
  const fakeNavigator = { userAgent: 'Node', language: 'en', languages: ['en'], platform: 'Node' };
  // Scratch.vm.runtime 桩：非沙盒扩展常用它注册帧回调、调 setInterpolation 等。
  // 未知方法一律返回 noop，避免 "xxx is not a function"。
  const runtimeKnown = {
    on: noop, off: noop, once: noop, emit: noop,
    targets: [], ioDevices: {}, runtimeOptions: {},
    requestRedraw: noop, requestUpdateMonitor: noop, start: noop, stopAll: noop,
    setInterpolation: noop, setTurboMode: noop, setFramerate: noop,
    startHats: noop, getTargetForStage: () => null, getEditingTarget: () => null,
  };
  const fakeRuntime = new Proxy(runtimeKnown, {
    get: (t, k) => (k in t ? t[k] : (typeof k === 'string' ? noop : undefined)),
    set: () => true,
  });
  const sandbox = {
    Scratch: scratch,
    console, setTimeout, clearTimeout, setInterval, clearInterval,
    window: fakeWindow, document: fakeDocument, navigator: fakeNavigator,
    location: { href: 'https://turbowarp.org/' },
    fetch: () => Promise.resolve({ ok: true, text: () => Promise.resolve(''), json: () => Promise.resolve({}) }),
    WebSocket: function () {}, XMLHttpRequest: function () {},
    TextEncoder: class { encode(s) { return Buffer.from(s, 'utf-8'); } },
    TextDecoder: class { decode(b) { return Buffer.from(b).toString('utf-8'); } },
    btoa: s => Buffer.from(s, 'binary').toString('base64'),
    atob: s => Buffer.from(s, 'base64').toString('binary'),
    crypto: { getRandomValues: arr => { for (let i = 0; i < arr.length; i++) arr[i] = (Math.random() * 256) | 0; return arr; } },
    URL: URL, URLSearchParams,
    Blob: function () {}, FileReader: function () {},
    Image: function () {}, HTMLImageElement: function () {}, HTMLCanvasElement: function () {},
    requestAnimationFrame: () => 0, cancelAnimationFrame: noop,
    performance: { now: () => Date.now() },
    localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
    process: undefined, // 防止逃逸
  };
  // renderer 桩：非沙盒扩展常用 renderer.on / renderer.canvas 等
  const fakeRenderer = new Proxy({
    on: noop, off: noop, emit: noop,
    canvas: fakeEl, gl: null, _gl: null,
    getNativeSize: () => [480, 360],
    draw: noop, requestRedraw: noop,
    createDrawable: () => fakeEl, destroyDrawable: noop,
    updateDrawableSkinId: noop, updateDrawablePosition: noop,
  }, {
    get: (t, k) => (k in t ? t[k] : (typeof k === 'string' ? noop : undefined)),
    set: () => true,
  });
  // VM 对象：on/off/emit + runtime + renderer + 各类 setter
  const fakeVM = new Proxy({
    on: noop, off: noop, once: noop, emit: noop,
    runtime: fakeRuntime, renderer: fakeRenderer,
    extensionManager: { loadExtensionURL: noop, isExtensionLoaded: () => false },
  }, {
    get: (t, k) => (k in t ? t[k] : (typeof k === 'string' ? noop : undefined)),
    set: () => true,
  });
  if (scratch.vm === undefined) scratch.vm = fakeVM;
  // Scratch.RenderedTarget（部分扩展做 instanceof 检查）
  if (scratch.RenderedTarget === undefined) {
    scratch.RenderedTarget = function () {};
    scratch.RenderedTarget.prototype = {};
  }
  scratch.Target = scratch.RenderedTarget;
  vm.createContext(sandbox);
  return sandbox;
}
*/


// ---------- 下载（支持 data: 内嵌 + http(s)）----------
/* 本地旧实现已由 ext_parse_lib.js 取代

function fetchText(url) {
  // data:application/javascript,<urlencoded code> —— sb3 内嵌自定义插件
  if (url.startsWith('data:')) {
    const comma = url.indexOf(',');
    if (comma < 0) return Promise.reject(new Error('非法 data URL'));
    const body = url.slice(comma + 1);
    // 可能 percent-encoded
    try { return Promise.resolve(decodeURIComponent(body)); }
    catch (e) { return Promise.resolve(body); }
  }
  return new Promise((resolve, reject) => {
    const http = url.startsWith('https') ? require('https') : require('http');
    const req = http.get(url, { headers: { 'User-Agent': 'sbcli-tool' } }, res => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        fetchText(res.headers.location).then(resolve).catch(reject);
        return;
      }
      if (res.statusCode !== 200) { reject(new Error(`HTTP ${res.statusCode} for ${url}`)); return; }
      let data = '';
      res.setEncoding('utf8');
      res.on('data', c => data += c);
      res.on('end', () => resolve(data));
    });
    req.on('error', reject);
    req.setTimeout(15000, () => req.destroy(new Error('timeout')));
  });
}
*/


// ---------- 解析 ----------
/* 本地旧实现已由 ext_parse_lib.js 取代

function parseExtension(code) {
  const scratch = makeScratch();
  const sandbox = makeSandbox(scratch);
  let registered = null;
  sandbox.Scratch.extensions.register = cls => { registered = cls; };
  // 扩展可能在 register 之后继续做 VM 初始化并抛错（非沙盒扩展常见）。
  // 只要注册已发生，就视为成功：吞掉后续异常。
  let execError = null;
  try {
    vm.runInContext(code, sandbox, { timeout: 10000 });
  } catch (e) {
    execError = e;
  }
  if (!registered) {
    throw new Error(execError ? String(execError.message) : '扩展未调用 Scratch.extensions.register');
  }
  const info = registered.getInfo();
  const blocks = [];
  for (const b of info.blocks) {
    if (typeof b === 'string') continue;
    const args = {};
    for (const [k, v] of Object.entries(b.arguments || {})) {
      args[k] = { type: String(v.type), menu: v.menu || null };
    }
    blocks.push({ opcode: b.opcode, type: String(b.blockType), text: String(b.text), args });
  }
  return { id: info.id, name: info.name, blocks, _warn: execError ? String(execError.message).slice(0, 80) : null };
}
*/


// ---------- 主流程 ----------
// 从已抓取的清单（F:\temp\tw_ext_list.json）找 id 对应的 URL：
// 忽略大小写匹配路径尾段（如 id "Encoding" ↔ 清单 "encoding"）
function findUrlFromManifest(id) {
  try {
    const list = JSON.parse(fs.readFileSync('F:/temp/tw_ext_list.json', 'utf-8'));
    const target = id.toLowerCase();
    for (const entry of list) {
      const base = entry.split('/').pop().toLowerCase();
      if (base === target) return `https://extensions.turbowarp.org/${entry}.js`;
    }
  } catch (e) { /* 清单缺失时忽略 */ }
  return null;
}

// 从 sb3 文件读 extensionURLs（id → URL），逐个下载+解析
async function processSb3(filePath) {
  const zip = require('child_process');
  // 用 python 快速解出 project.json（避免引入 zip 依赖）
  const { execFileSync } = require('child_process');
  const py = `
import json, zipfile, sys
p = ${JSON.stringify(filePath)}
z = zipfile.ZipFile(p)
d = json.loads(z.read("project.json").decode("utf-8"))
print(json.dumps(d.get("extensionURLs", {})))
`;
  let urls = {};
  try {
    const out = execFileSync('python', ['-c', py], { encoding: 'utf8' }).trim();
    urls = JSON.parse(out);
  } catch (e) {
    console.error('读取 sb3 extensionURLs 失败:', String(e.message).slice(0, 80));
  }
  console.error(`sb3 扩展 URL 映射: ${JSON.stringify(urls)}`);
  const results = {};
  for (const [id, url] of Object.entries(urls)) {
    console.error(`\n[${id}] ${url.slice(0, 60)}...`);
    try {
      const code = await fetchText(url);
      console.error(`  下载 ${code.length} 字节`);
      const info = parseExtension(code);
      console.error(`  解析成功 id=${info.id} blocks=${info.blocks.length}`);
      results[id] = info;
    } catch (e) {
      console.error(`  失败: ${String(e.message).slice(0, 80)}`);
    }
  }
  return results;
}

async function main() {
  const input = process.argv[2];
  if (!input) { console.error('用法: node fetch_tw_extension.js <id|URL|sb3文件>'); process.exit(2); }

  // sb3 文件模式：读 extensionURLs 批量解析
  if (input.endsWith('.sb3')) {
    const results = await processSb3(input);
    console.log('___RESULT___' + JSON.stringify(results));
    process.exit(0);
  }

  // 本地扩展源码文件模式（extensions/*.js）：直接读文件解析
  if (input.endsWith('.js') || fs.existsSync(input)) {
    let code;
    try {
      code = fs.readFileSync(input, 'utf-8');
    } catch (e) {
      console.error(`读取失败: ${e.message}`);
      console.log('___RESULT___{"error":true,"message":"read failed"}');
      process.exit(1);
    }
    try {
      const info = parseExtension(code);
      console.error(`解析成功: id=${info.id} blocks=${info.blocks.length}`);
      console.log('___RESULT___' + JSON.stringify(info));
      process.exit(0);
    } catch (e) {
      console.error(`解析失败: ${e.message}`);
      console.log('___RESULT___{"error":true,"message":"' + String(e.message).slice(0, 100) + '"}');
      process.exit(1);
    }
  }

  let url = input;
  if (!/^https?:/.test(input)) {
    url = findUrlFromManifest(input) || `https://extensions.turbowarp.org/${input}.js`;
  }
  console.error(`下载 ${url} ...`);
  let code;
  try {
    code = await fetchText(url);
  } catch (e) {
    console.error(`下载失败: ${e.message}`);
    console.log('___RESULT___{"error":true,"message":"' + String(e.message).slice(0, 100) + '"}');
    process.exit(1);
  }
  console.error(`下载成功 ${code.length} 字节，解析 getInfo ...`);
  try {
    const info = parseExtension(code);
    // 输出：extended JSON（含 id/name/blocks）
    const out = JSON.stringify(info);
    console.error(`解析成功: id=${info.id} blocks=${info.blocks.length}`);
    console.log('___RESULT___' + out);
  } catch (e) {
    console.error(`解析失败: ${e.message}`);
    console.log('___RESULT___{"error":true,"message":"' + String(e.message).slice(0, 100) + '"}');
    process.exit(1);
  }
}

main();
