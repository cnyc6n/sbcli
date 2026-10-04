// src/sb1_image.cpp —— Scratch 1.4 位图解码 + PNG 写出
#include "sb1.hpp"
#include "zip.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace sb {
using json = Json;
// ---- 位图解码 + PNG 写出 ----

std::vector<std::array<uint8_t,4>> defaultColormap() {
    auto g8 = [](double x) -> uint8_t {
        return (uint8_t)(((int)std::lround(x * 1023) & 0x3FF) >> 2);
    };
    auto gray = [&](double x) {
        uint8_t v = g8(x);
        return std::array<uint8_t,4>{v,v,v,255};
    };
    std::vector<std::array<uint8_t,4>> cm;
    cm.reserve(256);
    cm.push_back({255,255,255,255});
    cm.push_back({0,0,0,255});
    cm.push_back({255,255,255,255});
    cm.push_back(gray(0.5));
    cm.push_back({255,0,0,255});
    cm.push_back({0,255,0,255});
    cm.push_back({0,0,255,255});
    cm.push_back({0,255,255,255});
    cm.push_back({255,255,0,255});
    cm.push_back({255,0,255,255});
    cm.push_back(gray(0.125)); cm.push_back(gray(0.25));  cm.push_back(gray(0.375));
    cm.push_back(gray(0.625)); cm.push_back(gray(0.75));  cm.push_back(gray(0.875));
    for (int v = 1; v < 32; ++v) {
        if (v % 4 == 0) continue;
        cm.push_back(gray(v / 32.0));
    }
    for (int red = 0; red < 6; ++red)
        for (int blue = 0; blue < 6; ++blue)
            for (int green = 0; green < 6; ++green)
                cm.push_back({g8(red/5.0), g8(green/5.0), g8(blue/5.0), 255});
    return cm;
}

static std::pair<long long, size_t>
readSqueakInt(const std::vector<uint8_t>& buf, size_t pos) {
    uint8_t v = buf[pos++];
    if (v <= 223) return {v, pos};
    if (v <= 254) return {(long long)(v - 224) * 256 + buf[pos], pos + 1};
    long long x = ((long long)buf[pos]<<24) | ((long long)buf[pos+1]<<16)
                | ((long long)buf[pos+2]<<8) | buf[pos+3];
    return {x, pos + 4};
}

static std::vector<uint8_t> squeakDecompress(const std::vector<uint8_t>& raw) {
    size_t pos = 0;
    auto [total, p0] = readSqueakInt(raw, pos);
    pos = p0;
    std::vector<uint8_t> out;
    while (pos < raw.size() && (long long)out.size() < total) {
        auto [v, p1] = readSqueakInt(raw, pos);
        pos = p1;
        int code = (int)(v % 4);
        long long run = (v - code) / 4;
        if (code == 0) {
            for (long long i = 0; i < run; ++i) out.insert(out.end(), {0,0,0,0});
        } else if (code == 1) {
            uint8_t b = raw[pos++];
            for (long long i = 0; i < run; ++i) out.insert(out.end(), {b,b,b,b});
        } else if (code == 2) {
            for (long long i = 0; i < run; ++i)
                out.insert(out.end(), raw.begin()+pos, raw.begin()+pos+4);
            pos += 4;
        } else {
            size_t n = (size_t)run * 4;
            out.insert(out.end(), raw.begin()+pos, raw.begin()+pos+n);
            pos += n;
        }
    }
    if (out.size() > (size_t)total) out.resize((size_t)total);
    return out;
}

static bool parseColorStr(const std::string& s, std::array<uint8_t,4>& out) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int vals[4] = {0,0,0,255};
    for (int i = 0; i < (int)(s.size()-1)/2; ++i) {
        int hi = hex(s[1 + i*2]), lo = hex(s[2 + i*2]);
        if (hi < 0 || lo < 0) return false;
        vals[i] = hi * 16 + lo;
    }
    out = {(uint8_t)vals[0], (uint8_t)vals[1], (uint8_t)vals[2], (uint8_t)vals[3]};
    return true;
}

static std::vector<std::array<uint8_t,4>>
formPalette(const SqueakObject& form) {
    if (form.cls != "ColorForm" || form.fields.size() < 6) return {};
    const Value& cols = form.fields[5];
    if (cols.kind != Value::Kind::List || !cols.list || cols.list->empty()) return {};
    std::vector<std::array<uint8_t,4>> pal;
    for (auto& c : *cols.list) {
        // 颜色可能是 "#rrggbb[aa]" 字符串，也可能是 Value::Kind::Color
        // （RGBA 打包在 uint32 里：r<<24 | g<<16 | b<<8 | a）
        std::array<uint8_t,4> col{};
        if (c.kind == Value::Kind::Color) {
            uint32_t cc = c.color;
            col = {(uint8_t)(cc>>24), (uint8_t)(cc>>16),
                   (uint8_t)(cc>>8),  (uint8_t)(cc & 0xFF)};
        } else if (c.kind == Value::Kind::Str) {
            if (!parseColorStr(c.s, col)) return {};
        } else {
            return {};
        }
        pal.push_back(col);
    }
    return pal;
}

bool formToRGBA(const SqueakObject& form, RGBAImage& out) {
    if (form.fields.size() < 5) return false;
    const Value& wv = form.fields[0];
    const Value& hv = form.fields[1];
    const Value& dv = form.fields[2];
    const Value& bv = form.fields[4];
    if (wv.kind != Value::Kind::Int || hv.kind != Value::Kind::Int) return false;
    int w = (int)wv.i, h = (int)hv.i;
    if (w <= 0 || h <= 0) return false;
    int depth = (dv.kind == Value::Kind::Int) ? (int)dv.i : 0;

    std::vector<uint8_t> raw;
    if (bv.kind == Value::Kind::Obj && bv.obj && bv.obj->cls == "Bitmap") {
        if (bv.obj->fields.empty()) return false;
        const Value& data = bv.obj->fields[0];
        if (data.kind != Value::Kind::Bytes) return false;
        raw = data.bytes;
    } else if (bv.kind == Value::Kind::Bytes) {
        raw = squeakDecompress(bv.bytes);
    } else {
        return false;
    }

    out.w = w; out.h = h;
    out.rgba.clear();
    out.rgba.reserve((size_t)w * h * 4);

    if (depth == 32) {
        size_t need = (size_t)w * h * 4;
        size_t end = std::min(raw.size(), need);
        // 先判断 alpha 通道是否被使用
        bool alphaUsed = false;
        for (size_t i = 0; i + 3 < end; i += 4)
            if (raw[i] != 0) { alphaUsed = true; break; }
        for (size_t i = 0; i + 3 < end; i += 4) {
            uint8_t a = raw[i], r = raw[i+1], g = raw[i+2], b = raw[i+3];
            if (!alphaUsed && (r || g || b)) a = 255;
            if (a == 0) { out.rgba.insert(out.rgba.end(), {0,0,0,0}); }
            else        { out.rgba.insert(out.rgba.end(), {r,g,b,a}); }
        }
        // 补齐
        while (out.rgba.size() < need) out.rgba.insert(out.rgba.end(), {0,0,0,0});
        out.rgba.resize(need);
        return true;
    }

    if (depth == 16) {
        for (int i = 0; i < w * h; ++i) {
            size_t j = (size_t)i * 2;
            if (j + 2 > raw.size()) break;
            uint16_t v = (uint16_t)((raw[j]<<8) | raw[j+1]);
            uint8_t r = (v >> 10) & 31;
            uint8_t g = (v >> 5) & 31;
            uint8_t b = v & 31;
            if (r == 0 && g == 0) {
                if (b == 0) { out.rgba.insert(out.rgba.end(), {0,0,0,0}); continue; }
                if (b == 1) { out.rgba.insert(out.rgba.end(), {0,0,0,255}); continue; }
            }
            out.rgba.insert(out.rgba.end(), {
                (uint8_t)((r * 33) >> 2),
                (uint8_t)((g * 33) >> 2),
                (uint8_t)((b * 33) >> 2), 255});
        }
        size_t need = (size_t)w * h * 4;
        while (out.rgba.size() < need) out.rgba.insert(out.rgba.end(), {0,0,0,0});
        out.rgba.resize(need);
        return true;
    }

    if (depth == 1 || depth == 2 || depth == 4 || depth == 8) {
        auto cm = formPalette(form);
        if (cm.empty()) {
            static std::vector<std::array<uint8_t,4>> cached;
            if (cached.empty()) cached = defaultColormap();
            cm = cached;
        }
        if (depth > 1) cm[0] = {0,0,0,0};
        int rowBytes = ((w * depth + 31) / 32) * 4;
        int mask = (1 << depth) - 1;
        for (int y = 0; y < h; ++y) {
            size_t rowOff = (size_t)y * rowBytes;
            for (int x = 0; x < w; ++x) {
                int bit = x * depth;
                size_t idx = rowOff + (bit >> 3);
                uint8_t byte = (idx < raw.size()) ? raw[idx] : 0;
                int shift = 8 - depth - (bit & 7);
                int pixel = (byte >> shift) & mask;
                std::array<uint8_t,4> c = (pixel < (int)cm.size()) ? cm[pixel]
                                                    : std::array<uint8_t,4>{0,0,0,0};
                out.rgba.insert(out.rgba.end(), {c[0], c[1], c[2], c[3]});
            }
        }
        return true;
    }
    return false;
}

// 用 miniz 写 PNG（mz_compress2 / mz_crc32 声明在 zip.hpp）
static void writeBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((v>>24)&0xFF); out.push_back((v>>16)&0xFF);
    out.push_back((v>>8)&0xFF);  out.push_back(v&0xFF);
}

static void pngChunk(std::vector<uint8_t>& out, const char* tag,
                     const std::vector<uint8_t>& data) {
    writeBE32(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = mz_crc32(MZ_CRC32_INIT, out.data() + start, (size_t)(out.size() - start));
    writeBE32(out, crc);
}

bool writePNG(const std::string& path, int w, int h,
              const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(),
                   rgba.begin() + (size_t)y * w * 4,
                   rgba.begin() + (size_t)(y + 1) * w * 4);
    }
    mz_ulong bound = mz_compressBound((mz_ulong)raw.size());
    std::vector<uint8_t> comp(bound);
    if (mz_compress2(comp.data(), &bound, raw.data(), (mz_ulong)raw.size(), 9) != MZ_OK)
        return false;
    comp.resize(bound);

    std::vector<uint8_t> png = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    {
        std::vector<uint8_t> ihdr;
        writeBE32(ihdr, (uint32_t)w); writeBE32(ihdr, (uint32_t)h);
        ihdr.push_back(8); ihdr.push_back(6);
        ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
        pngChunk(png, "IHDR", ihdr);
    }
    pngChunk(png, "IDAT", comp);
    pngChunk(png, "IEND", {});
    writeFileBinary(path, png.data(), png.size());
    return true;
}

} // namespace sb

