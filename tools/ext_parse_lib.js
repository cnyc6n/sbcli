// tools/ext_parse_lib.js —— TurboWarp 扩展解析库
// 导出 makeScratch / makeSandbox / fetchText / parseExtension
const vm = require('vm');

function makeScratch() {
  const scratch = {
    BlockType: { COMMAND: 'command', REPORTER: 'reporter', BOOLEAN: 'boolean',
                 HAT: 'hat', CONDITIONAL: 'conditional' },
    ArgumentType: { STRING: 'string', NUMBER: 'number', BOOLEAN: 'boolean',
                    COLOR: 'color', MATRIX: 'matrix', ANGLE: 'angle',
                    IMAGE: 'image', NOTE: 'note' },
    Cast: { toNumber: x => Number(x), toString: x => String(x), toBoolean: x => !!x },
    extensions: { register: () => {}, unsandboxed: true },
    // 块过滤器：标注积木适用角色类型（Sprite 专用等）
    TargetType: { SPRITE: 'sprite', STAGE: 'stage' },
  };
  const tf = function (x) { return x; };
  tf.setup = () => {};
  tf.setLocale = () => {};
  tf.translate = x => x;
  scratch.translate = tf;
  return scratch;
}

function makeSandbox(scratch) {
  const noop = () => {};
  // 万能桩：可当函数调用、可当对象取任意属性、可当枚举值（既是函数也是 Proxy）
  // 解决扩展在模块顶层读取 renderer.SPRITE / BitmapSkin / runtime.postData 等真实类/常量
  const anyStub = new Proxy(function () {}, {
    get: (t, k) => {
      if (k === Symbol.toPrimitive) return () => 0;
      if (k === 'toString') return () => '';
      if (k === 'valueOf') return () => 0;
      if (k === Symbol.toStringTag) return 'AnyStub';
      return anyStub;
    },
    set: () => true,
    apply: () => anyStub,
    construct: () => anyStub,
    has: () => true,
  });
  const fakeEl = new Proxy({}, {
    get: (t, k) => {
      if (k === 'style' || k === 'classList' || k === 'dataset') return fakeEl;
      if (['appendChild','removeChild','addEventListener','removeEventListener',
           'setAttribute','getAttribute','append','remove','focus','click',
           'getContext','toDataURL','drawImage'].includes(k)) return noop;
      if (k === 'getBoundingClientRect') return () => ({width:0,height:0,top:0,left:0});
      if (k === 'children' || k === 'childNodes') return [];
      if (k === 'parentNode' || k === 'parentElement') return null;
      if (['innerHTML','textContent','className','id','width','height'].includes(k)) return '';
      return anyStub;
    },
    set: () => true,
  });
  const fakeDocument = new Proxy({}, {
    get: (t, k) => {
      if (k === 'createElement' || k === 'getElementById' || k === 'querySelector' ||
          k === 'querySelectorAll') return () => fakeEl;
      if (k === 'addEventListener' || k === 'removeEventListener') return noop;
      if (k === 'body' || k === 'head' || k === 'documentElement') return fakeEl;
      if (k === 'location') return { href: 'https://turbowarp.org/' };
      return anyStub;
    },
    set: () => true,
  });
  const fakeNavigator = { userAgent: 'Node', language: 'en', languages: ['en'], platform: 'Node' };
  const runtimeKnown = {
    on: noop, off: noop, once: noop, emit: noop,
    targets: [], ioDevices: anyStub, runtimeOptions: {},
    requestRedraw: noop, requestUpdateMonitor: noop, start: noop, stopAll: noop,
    setInterpolation: noop, setTurboMode: noop, setFramerate: noop,
    startHats: noop, getTargetForStage: () => null, getEditingTarget: () => null,
    formatMessage: () => ({}), getFormatMessage: () => (() => ({})),
  };
  const fakeRuntime = new Proxy(runtimeKnown, {
    get: (t, k) => (k in t ? t[k] : anyStub),
    set: () => true,
  });
  const fakeRenderer = new Proxy({
    on: noop, off: noop, emit: noop, canvas: fakeEl, gl: anyStub, _gl: anyStub,
    getNativeSize: () => [480, 360], draw: noop, requestRedraw: noop,
    createDrawable: () => fakeEl, destroyDrawable: noop,
    updateDrawableSkinId: noop, updateDrawablePosition: noop,
  }, {
    // 未知属性（SPRITE / BitmapSkin / MASK / …）返回万能桩
    get: (t, k) => (k in t ? t[k] : anyStub),
    set: () => true,
  });
  const fakeVM = new Proxy({
    on: noop, off: noop, once: noop, emit: noop,
    runtime: fakeRuntime, renderer: fakeRenderer,
    extensionManager: { loadExtensionURL: noop, isExtensionLoaded: () => false },
  }, {
    get: (t, k) => (k in t ? t[k] : anyStub),
    set: () => true,
  });
  const fakeWindow = new Proxy({}, {
    get: (t, k) => {
      if (['addEventListener','removeEventListener','postMessage','focus','open','close',
           'alert','confirm','prompt'].includes(k)) return noop;
      if (k === 'document') return fakeDocument;
      if (k === 'location') return { href: 'https://turbowarp.org/', origin: 'https://turbowarp.org' };
      if (k === 'localStorage' || k === 'sessionStorage')
        return { getItem: () => null, setItem: noop, removeItem: noop };
      if (k === 'innerWidth' || k === 'innerHeight') return 480;
      if (k === 'navigator') return fakeNavigator;
      if (k === 'devicePixelRatio') return 1;
      if (k === 'Scratch') return scratch;
      return anyStub;
    },
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
    alert: noop, confirm: () => false, prompt: () => null,
    matchMedia: () => ({ matches: false, addEventListener: noop, addListener: noop }),
    getComputedStyle: () => ({}),
    ResizeObserver: function () { return { observe: noop, unobserve: noop, disconnect: noop }; },
    MutationObserver: function () { return { observe: noop, disconnect: noop, takeRecords: () => [] }; },
    IntersectionObserver: function () { return { observe: noop, unobserve: noop, disconnect: noop }; },
    DOMException: function () {},
    Event: function () {}, CustomEvent: function () {},
    AudioContext: function () { return anyStub; },
    webkitAudioContext: function () { return anyStub; },
    OffscreenCanvas: function () { return anyStub; },
    Path2D: function () { return anyStub; },
    createImageBitmap: () => Promise.resolve(anyStub),
    Uint8Array, Uint8ClampedArray, Float32Array, ArrayBuffer, DataView, Int32Array,
    Math, JSON, Date, RegExp, Error, TypeError, Map, Set, WeakMap, WeakSet, Symbol,
    Promise, Object, Array, String, Number, Boolean, Function,
    process: undefined,
  };
  if (scratch.vm === undefined) scratch.vm = fakeVM;
  // 部分扩展直接访问 Scratch.renderer（等价 vm.renderer）
  if (scratch.renderer === undefined) scratch.renderer = fakeRenderer;
  if (scratch.runtime === undefined) scratch.runtime = fakeRuntime;
  if (scratch.canvas === undefined) scratch.canvas = fakeEl;
  if (scratch.RenderedTarget === undefined) {
    scratch.RenderedTarget = function () {};
    scratch.RenderedTarget.prototype = {};
  }
  scratch.Target = scratch.RenderedTarget;
  vm.createContext(sandbox);
  return sandbox;
}

// 支持 data:（含 base64）/ http(s)
function fetchText(url) {
  if (url.startsWith('data:')) {
    const comma = url.indexOf(',');
    if (comma < 0) return Promise.reject(new Error('非法 data URL'));
    const meta = url.slice(5, comma);
    const body = url.slice(comma + 1);
    if (/;base64/i.test(meta)) {
      return Promise.resolve(Buffer.from(body, 'base64').toString('utf-8'));
    }
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
      if (res.statusCode !== 200) { reject(new Error(`HTTP ${res.statusCode}`)); return; }
      let data = '';
      res.setEncoding('utf8');
      res.on('data', c => data += c);
      res.on('end', () => resolve(data));
    });
    req.on('error', reject);
    req.setTimeout(15000, () => req.destroy(new Error('timeout')));
  });
}

function parseExtension(code) {
  const scratch = makeScratch();
  const sandbox = makeSandbox(scratch);
  let registered = null;
  sandbox.Scratch.extensions.register = cls => { registered = cls; };
  let execError = null;
  try {
    vm.runInContext(code, sandbox, { timeout: 10000 });
  } catch (e) {
    execError = e;
  }
  if (!registered) {
    throw new Error(execError ? String(execError.message) : '未调用 register');
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

module.exports = { makeScratch, makeSandbox, fetchText, parseExtension };
