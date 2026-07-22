#include "ruvia/edge/detail/cache/DiskCacheFiles.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <string>
#include <cstring>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ruvia::edge {

namespace {

[[nodiscard]] bool isLowerHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

}  // namespace

[[nodiscard]] bool readEntryFile(
    const std::filesystem::path& path,
    std::size_t maxBytes,
    std::string& out) {
    std::error_code ec;
    const auto fileBytes = std::filesystem::file_size(path, ec);
    if (ec || fileBytes > maxBytes ||
        fileBytes > static_cast<std::uintmax_t>(
            (std::numeric_limits<std::streamsize>::max)())) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    const auto size = static_cast<std::size_t>(fileBytes);
    out.resize(size);
    if (size > 0 && !in.read(out.data(), static_cast<std::streamsize>(size))) {
        return false;
    }
    // Reject a file that grew after file_size(): it did not come from this
    // cache's atomic writer (or another writer violated the directory lease).
    return in.peek() == std::char_traits<char>::eof();
}

[[nodiscard]] bool isCommittedEntryName(std::string_view name) noexcept {
    return name.size() == 20 &&
        std::all_of(name.begin(), name.begin() + 16, isLowerHex) &&
        name.substr(16) == ".rvc";
}

[[nodiscard]] bool isOwnedTempName(std::string_view name) noexcept {
    return name.size() > 24 &&
        std::all_of(name.begin(), name.begin() + 16, isLowerHex) &&
        (name.substr(16).starts_with(".rvc.tmp") ||
         name.substr(16).starts_with(".rvc.delete"));
}

void syncDirectoryBestEffort(const std::filesystem::path& directory) noexcept {
#if !defined(_WIN32)
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(directory.c_str(), flags);
    if (descriptor < 0) {
        return;
    }
    (void)::fsync(descriptor);
    (void)::close(descriptor);
#else
    (void)directory;
#endif
}

[[nodiscard]] bool flushFileToDisk(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    const HANDLE file = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool flushed = ::FlushFileBuffers(file) != FALSE;
    const bool closed = ::CloseHandle(file) != FALSE;
    return flushed && closed;
#else
    int flags = O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return false;
    }
    const bool flushed = ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    return flushed && closed;
#endif
}

[[nodiscard]] bool commitReplacement(
    const std::filesystem::path& temporary,
    const std::filesystem::path& finalPath) noexcept {
#if defined(_WIN32)
    return ::MoveFileExW(
               temporary.c_str(),
               finalPath.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, finalPath, ec);
    if (ec) {
        return false;
    }
    syncDirectoryBestEffort(finalPath.parent_path());
    return true;
#endif
}

void removeOwnedFileBestEffort(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    if (!ec) {
        syncDirectoryBestEffort(path.parent_path());
    }
}

}  // namespace ruvia::edge
