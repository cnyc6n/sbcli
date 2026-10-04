// src/miniz_impl.cpp
// 唯一把 miniz 实现编译进来的翻译单元。
// 其它文件包含 miniz-zip.hpp 时会自动走 header-only 模式（见 common.hpp）。
#include <cstring>
#include "miniz-zip.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

// 惰性 zip 读取：宽字符文件句柄 + 自定义 m_pRead 回调。
//
// 设计要点：
//  * FileHandle 的所有权归调用方（mzip::Reader，见 zip.hpp），
//    mz_zip_reader_end 不释放自定义 m_pIO_opaque，由 Reader 析构时释放。
//  * 严格顺序：先设 m_pRead / m_pIO_opaque，再 mz_zip_reader_read_central_dir，
//    期间不能调用其它 mz_zip_reader_*（否则会走默认 mem 读取回调）。
//  * 只在 miniz 实现区编译（这里是唯一 include miniz 实现的地方），
//    因此可以访问内部的 mz_zip_reader_init_internal / read_central_dir。

namespace mzip {

namespace detail {

struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size{};

    FileHandle() = default;
    ~FileHandle() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    bool open(const char* utf8path) {
#ifdef _WIN32
        int wn = MultiByteToWideChar(CP_UTF8, 0, utf8path,
                                     (int)std::strlen(utf8path), nullptr, 0);
        if (wn <= 0) return false;
        std::wstring w(wn, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8path, (int)std::strlen(utf8path),
                            &w[0], wn);
        h = ::CreateFileW(w.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        ::GetFileSizeEx(h, &size);
        return true;
#else
        return false;
#endif
    }

    size_t readAt(mz_uint64 offset, void* pBuf, size_t n) const {
#ifdef _WIN32
        if (h == INVALID_HANDLE_VALUE) return 0;
        if (offset >= (mz_uint64)size.QuadPart) return 0;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)offset;
        if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return 0;
        DWORD toRead = (DWORD)MZ_MIN((mz_uint64)n,
                                     (mz_uint64)size.QuadPart - offset);
        DWORD got = 0;
        if (!::ReadFile(h, pBuf, toRead, &got, nullptr)) return 0;
        return (size_t)got;
#else
        return 0;
#endif
    }
};

size_t fileReadFunc(void* pOpaque, mz_uint64 file_ofs, void* pBuf, size_t n) {
    FileHandle* fh = static_cast<FileHandle*>(pOpaque);
    return fh->readAt(file_ofs, pBuf, n);
}

} // namespace detail

// 用调用方提供的 FileHandle* 初始化惰性读取。
// 返回 true 时，FileHandle* 的所有权仍属于调用方；false 时句柄由调用方自行关闭。
bool mz_zip_reader_init_utf8(mz_zip_archive* pZip, void* fileHandle,
                             mz_uint32 flags) {
    if (!pZip || !fileHandle) return false;
    detail::FileHandle* fh = static_cast<detail::FileHandle*>(fileHandle);

    if (!mz_zip_reader_init_internal(pZip, flags)) return false;

    // 顺序关键：read_central_dir 之前必须设好回调，否则 miniz 走默认 mem 读取。
    pZip->m_archive_size = (mz_uint64)fh->size.QuadPart;
    pZip->m_pRead = detail::fileReadFunc;
    pZip->m_pIO_opaque = fh;

    if (!mz_zip_reader_read_central_dir(pZip, flags)) {
        mz_zip_reader_end(pZip);
        return false;
    }
    return true;
}

} // namespace mzip

namespace mzip {

// 供 zip.hpp 使用的 FileHandle 分配/释放（避免类型泄漏到头文件）
void* mzip_new_file_handle(void) { return new mzip::detail::FileHandle(); }
void  mzip_free_file_handle(void* p) { delete static_cast<mzip::detail::FileHandle*>(p); }
bool  mzip_file_handle_open(void* p, const char* utf8path) {
    return static_cast<mzip::detail::FileHandle*>(p)->open(utf8path);
}

} // namespace mzip
