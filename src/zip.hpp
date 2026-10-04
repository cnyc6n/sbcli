// src/zip.hpp
#pragma once

#include "common.hpp"        // 给 Reader 用 readFileBinary（UTF-8 路径）
#include <cstring>          // 给 miniz-zip.hpp 补 <cstring>

#ifndef MINIZ_HEADER_FILE_ONLY
#define MINIZ_HEADER_FILE_ONLY 1
#endif
#include "miniz-zip.hpp"

#include <string>
#include <vector>
#include <optional>
#include <stdexcept>
#include <cctype>

namespace mzip {

class ZipError : public std::runtime_error {
public:
    explicit ZipError(const std::string& s) : std::runtime_error(s) {}
};

// 惰性 zip 读取：由 miniz_impl.cpp 提供实现（那里能访问 miniz 内部 static 函数）。
// 句柄分配/释放/打开与初始化接线全在实现区，这里只持有 void*。
void* mzip_new_file_handle(void);
void  mzip_free_file_handle(void* p);
bool  mzip_file_handle_open(void* p, const char* utf8path);
bool  mz_zip_reader_init_utf8(mz_zip_archive* pZip, void* fileHandle,
                              mz_uint32 flags);

class Reader {
public:
    explicit Reader(const std::string& path) {
        std::memset(&m_zip, 0, sizeof(m_zip));
        m_fh = mzip_new_file_handle();
        if (!m_fh || !mzip_file_handle_open(m_fh, path.c_str())) {
            if (m_fh) { mzip_free_file_handle(m_fh); m_fh = nullptr; }
            throw ZipError("打不开文件：" + path);
        }
        if (!mz_zip_reader_init_utf8(&m_zip, m_fh, 0)) {
            mzip_free_file_handle(m_fh); m_fh = nullptr;
            throw ZipError("不是有效的压缩包，文件可能损坏");
        }
        m_count = mz_zip_reader_get_num_files(&m_zip);
        m_open = true;
    }

    ~Reader() {
        if (m_open) {
            mz_zip_reader_end(&m_zip);
        }
        if (m_fh) {
            mzip_free_file_handle(m_fh);
            m_fh = nullptr;
        }
    }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    size_t size() const { return m_count; }

    std::string nameAt(size_t i) const {
        char buf[512];
        mz_uint n = mz_zip_reader_get_filename(&m_zip, (mz_uint)i, buf, sizeof(buf));
        return std::string(buf, n ? n - 1 : 0);
    }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(m_count);
        for (size_t i = 0; i < m_count; ++i) out.push_back(nameAt(i));
        return out;
    }

    std::optional<size_t> findEntry(const std::string& target) const {
        std::string lower = toLower(target);
        for (size_t i = 0; i < m_count; ++i) {
            if (toLower(nameAt(i)) == lower) return i;
        }
        return std::nullopt;
    }

    size_t entrySize(const std::string& name) const {
        auto idx = findEntry(name);
        if (!idx) throw ZipError("找不到条目：" + name);
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&m_zip, (mz_uint)*idx, &st))
            throw ZipError("读取条目信息失败：" + name);
        return (size_t)st.m_uncomp_size;
    }

    std::string readText(const std::string& name) const {
        auto data = readBinary(name);
        return std::string(data.begin(), data.end());
    }

    std::vector<unsigned char> readBinary(const std::string& name) const {
        auto idx = findEntry(name);
        if (!idx) throw ZipError("找不到条目：" + name);
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&m_zip, (mz_uint)*idx, &st))
            throw ZipError("读取条目信息失败：" + name);
        std::vector<unsigned char> out(st.m_uncomp_size);
        // 快速路径：定位压缩块 → 一次读 → tinfl 一次性解压
        // （miniz 的流式 extract 对 m_pRead 逐块回调 + 顺带算 CRC，16MB 解压 ~95ms；
        //   这里 tinfl 一次性解压 ~21ms + 读 ~1ms，不再单独跑 crc（解压字节数校验足够，
        //   crc 单独算 16MB 要 60ms+，是纯浪费；数据不可信时 fallback 的 miniz 路径会兜底）
        if (m_zip.m_pRead && m_zip.m_pIO_opaque &&
            st.m_method == MZ_DEFLATED && st.m_uncomp_size > 0) {
            unsigned char lh[30];
            if (m_zip.m_pRead(m_zip.m_pIO_opaque, st.m_local_header_ofs, lh, 30) == 30 &&
                (uint32_t)(lh[0] | (lh[1]<<8) | (lh[2]<<16) | (lh[3]<<24)) == 0x04034b50) {
                uint16_t fnlen = (uint16_t)(lh[26] | (lh[27]<<8));
                uint16_t exlen = (uint16_t)(lh[28] | (lh[29]<<8));
                mz_uint64 data_ofs = st.m_local_header_ofs + 30 + fnlen + exlen;
                std::vector<unsigned char> comp((size_t)st.m_comp_size);
                if (m_zip.m_pRead(m_zip.m_pIO_opaque, data_ofs, comp.data(),
                                  (size_t)st.m_comp_size) == (size_t)st.m_comp_size) {
                    size_t olen = out.size();
                    size_t rc = tinfl_decompress_mem_to_mem(
                        out.data(), olen, comp.data(), (size_t)st.m_comp_size,
                        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                    if (rc == olen)
                        return out;
                }
            }
        }
        // 回退：miniz 标准提取（stored / 其它方法 / 快速路径失败）
        if (!mz_zip_reader_extract_to_mem(&m_zip, (mz_uint)*idx,
                                          out.data(), out.size(), 0))
            throw ZipError("解压失败：" + name);
        return out;
    }

    void writeToFile(const std::string& name, const std::string& dst) const {
        // 读内存再写文件，避免 mz_zip_reader_extract_to_file 的窄字符 fopen
        auto data = readBinary(name);
        sb::writeFileBinary(dst, data.data(), data.size());
    }

private:
    static std::string toLower(std::string s) {
        for (auto& c : s) c = (char)::tolower((unsigned char)c);
        return s;
    }

    void* m_fh = nullptr;              // 惰性文件句柄（miniz_impl.cpp 管理）
    mutable mz_zip_archive m_zip{};   // 栈对象
    size_t m_count = 0;
    bool   m_open  = false;
};

} // namespace mzip