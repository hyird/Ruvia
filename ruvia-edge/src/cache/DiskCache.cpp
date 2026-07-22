#include "ruvia/edge/detail/cache/DiskCache.h"

#include "ruvia/edge/detail/cache/DiskCacheFiles.h"
#include "ruvia/edge/detail/cache/DiskCacheRecord.h"

#include <array>
#include <cerrno>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

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

// A stable 64-bit key hash (FNV-1a) for deriving on-disk file names. Stable
// across runs and platforms so a scanned directory keeps naming its entries the
// same way, unlike std::hash.
}  // namespace

class DiskCache::DirectoryLease final {
public:
    explicit DirectoryLease(const std::filesystem::path& directory)
        : path_(directory / ".ruvia-cache.lock") {
#if defined(_WIN32)
        handle_ = ::CreateFileW(
            path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = std::error_code(
                static_cast<int>(::GetLastError()), std::system_category());
            throw std::filesystem::filesystem_error(
                "failed to acquire disk cache directory lease", path_, error);
        }
#else
        int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        descriptor_ = ::open(path_.c_str(), flags, 0600);
        if (descriptor_ < 0) {
            const auto error = std::error_code(errno, std::generic_category());
            throw std::filesystem::filesystem_error(
                "failed to open disk cache directory lease", path_, error);
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const auto error = std::error_code(errno, std::generic_category());
            (void)::close(descriptor_);
            descriptor_ = -1;
            throw std::filesystem::filesystem_error(
                "disk cache directory is already in use", path_, error);
        }
#endif
    }

    ~DirectoryLease() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

    DirectoryLease(const DirectoryLease&) = delete;
    DirectoryLease& operator=(const DirectoryLease&) = delete;

private:
    std::filesystem::path path_;
#if defined(_WIN32)
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

std::string DiskCache::fileNameFor(std::string_view key) {
    std::array<char, 16> hex{};
    std::uint64_t h = diskRecordHash(key);
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        hex[static_cast<std::size_t>(i)] = kDigits[h & 0xf];
        h >>= 4;
    }
    return std::string(hex.data(), hex.size()) + ".rvc";
}

DiskCache::DiskCache(std::filesystem::path directory, std::size_t maxBytes)
    : directory_(std::move(directory)), maxBytes_(maxBytes) {
    std::filesystem::create_directories(directory_);
    directoryLease_ = std::make_unique<DirectoryLease>(directory_);

    // Adopt only fully committed, canonical, checksum-valid records. A crash
    // may leave a complete temporary file behind, but it was never renamed and
    // therefore must never become visible after restart.
    std::error_code ec;
    std::filesystem::directory_iterator current(directory_, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        throw std::filesystem::filesystem_error(
            "failed to scan disk cache directory", directory_, ec);
    }
    while (current != end) {
        const auto path = current->path();
        const auto status = current->symlink_status(ec);
        if (ec) {
            throw std::filesystem::filesystem_error(
                "failed to inspect disk cache entry", path, ec);
        }

        std::string fileName;
        try {
            fileName = path.filename().string();
        } catch (...) {
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }

        if (std::filesystem::is_regular_file(status) &&
            isOwnedTempName(fileName)) {
            removeOwnedFileBestEffort(path);
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status) ||
            !isCommittedEntryName(fileName)) {
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }

        std::string data;
        if (!readEntryFile(path, maxBytes_, data)) {
            removeOwnedFileBestEffort(path);
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }
        std::string key;
        if (!decodeDiskRecord(data, key, nullptr) || fileNameFor(key) != fileName) {
            removeOwnedFileBestEffort(path);
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }
        const std::size_t bytes = data.size();
        const bool inserted =
            index_.try_emplace(key, Entry{fileName, bytes}).second;
        if (!inserted) {
            removeOwnedFileBestEffort(path);
        } else {
            fileOwners_.emplace(fileName, key);
            recency_.push_front(key);
            lru_.emplace(key, recency_.begin());
            totalBytes_ += bytes;
        }
        current.increment(ec);
        if (ec) {
            break;
        }
    }
    if (ec) {
        throw std::filesystem::filesystem_error(
            "failed to scan disk cache directory", directory_, ec);
    }
    if (!evictWhileOverBudget()) {
        throw std::runtime_error(
            "failed to enforce disk cache byte budget during startup");
    }
}

DiskCache::~DiskCache() = default;

std::optional<CachedResponse> DiskCache::lookup(std::string_view key) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = index_.find(std::string(key));
    if (it == index_.end()) {
        return std::nullopt;
    }
    std::string data;
    if (!readEntryFile(directory_ / it->second.fileName, maxBytes_, data)) {
        (void)removeLocked(it);
        return std::nullopt;
    }
    CachedResponse entry;
    std::string storedKey;
    if (!decodeDiskRecord(data, storedKey, &entry) || storedKey != key) {
        (void)removeLocked(it);
        return std::nullopt;
    }
    // Promote to most-recently-used.
    if (const auto lruIt = lru_.find(it->first); lruIt != lru_.end()) {
        recency_.splice(recency_.begin(), recency_, lruIt->second);
    }
    return entry;
}

bool DiskCache::store(std::string_view key, const CachedResponse& entry) {
    const auto serialized = encodeDiskRecord(key, entry);
    if (!serialized || serialized->size() > maxBytes_) {
        return false;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    const std::string fileName = fileNameFor(key);
    const std::string keyStr(key);
    const std::filesystem::path finalPath = directory_ / fileName;

    // FNV-1a file names can theoretically collide. Delete the previous logical
    // owner before committing this value; a collision is a miss for the old
    // key, never an alias to another response.
    if (const auto owner = fileOwners_.find(fileName);
        owner != fileOwners_.end() && owner->second != keyStr) {
        const auto collided = index_.find(owner->second);
        if (collided != index_.end()) {
            if (!removeLocked(collided)) {
                return false;
            }
        } else {
            fileOwners_.erase(owner);
        }
    }

    const auto existing = index_.find(keyStr);
    const std::size_t replacedBytes =
        existing == index_.end() ? 0 : existing->second.bytes;
    const std::size_t availableWithoutReplacement =
        maxBytes_ - serialized->size();
    while (totalBytes_ - replacedBytes > availableWithoutReplacement) {
        const auto victim = std::find_if(
            recency_.rbegin(),
            recency_.rend(),
            [&keyStr](const std::string& candidate) {
                return candidate != keyStr;
            });
        if (victim == recency_.rend()) {
            return false;
        }
        const auto victimEntry = index_.find(*victim);
        if (victimEntry == index_.end()) {
            recency_.erase(std::next(victim).base());
            continue;
        }
        if (!removeLocked(victimEntry)) {
            return false;
        }
    }

    const std::filesystem::path tempPath =
        directory_ / (fileName + ".tmp" + std::to_string(tempCounter_++));
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out ||
            !out.write(
                serialized->data(),
                static_cast<std::streamsize>(serialized->size()))) {
            removeOwnedFileBestEffort(tempPath);
            return false;
        }
        out.flush();
        out.close();
        if (!out) {
            removeOwnedFileBestEffort(tempPath);
            return false;
        }
    }
    if (!flushFileToDisk(tempPath) || !commitReplacement(tempPath, finalPath)) {
        removeOwnedFileBestEffort(tempPath);
        return false;
    }

    if (const auto it = index_.find(keyStr); it != index_.end()) {
        totalBytes_ -= it->second.bytes;
        it->second = Entry{fileName, serialized->size()};
        totalBytes_ += serialized->size();
        if (const auto lruIt = lru_.find(it->first); lruIt != lru_.end()) {
            recency_.splice(recency_.begin(), recency_, lruIt->second);
        }
    } else {
        index_.emplace(keyStr, Entry{fileName, serialized->size()});
        recency_.push_front(keyStr);
        lru_.emplace(keyStr, recency_.begin());
        totalBytes_ += serialized->size();
    }
    fileOwners_.insert_or_assign(fileName, keyStr);
    return true;
}

bool DiskCache::purge(std::string_view key) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = index_.find(std::string(key));
    if (it == index_.end()) {
        return false;
    }
    return removeLocked(it);
}

DiskCache::PurgeResult DiskCache::purgePrefix(std::string_view prefix) {
    std::lock_guard<std::mutex> guard(mutex_);
    PurgeResult result;
    for (auto it = index_.begin(); it != index_.end();) {
        if (std::string_view(it->first).starts_with(prefix)) {
            const auto next = std::next(it);
            if (removeLocked(it)) {
                ++result.removed;
            } else {
                result.complete = false;
            }
            it = next;
        } else {
            ++it;
        }
    }
    return result;
}

bool DiskCache::clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    bool complete = true;
    for (auto it = index_.begin(); it != index_.end();) {
        const auto next = std::next(it);
        if (!removeLocked(it)) {
            complete = false;
        }
        it = next;
    }
    return complete;
}

std::size_t DiskCache::entryCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return index_.size();
}

std::size_t DiskCache::byteSize() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return totalBytes_;
}

bool DiskCache::removeLocked(
    std::unordered_map<std::string, Entry>::iterator it) noexcept {
    std::error_code ec;
    const auto path = directory_ / it->second.fileName;
    (void)std::filesystem::remove(path, ec);
    if (ec) {
        return false;
    }
    syncDirectoryBestEffort(directory_);
    eraseIndexLocked(it);
    return true;
}

void DiskCache::eraseIndexLocked(
    std::unordered_map<std::string, Entry>::iterator it) noexcept {
    if (totalBytes_ >= it->second.bytes) {
        totalBytes_ -= it->second.bytes;
    } else {
        totalBytes_ = 0;
    }
    if (const auto owner = fileOwners_.find(it->second.fileName);
        owner != fileOwners_.end() && owner->second == it->first) {
        fileOwners_.erase(owner);
    }
    if (const auto lruIt = lru_.find(it->first); lruIt != lru_.end()) {
        recency_.erase(lruIt->second);
        lru_.erase(lruIt);
    }
    index_.erase(it);
}

bool DiskCache::evictWhileOverBudget() noexcept {
    while (totalBytes_ > maxBytes_ && !recency_.empty()) {
        const std::string victim = recency_.back();
        if (const auto it = index_.find(victim); it != index_.end()) {
            if (!removeLocked(it)) {
                return false;
            }
        } else {
            recency_.pop_back();  // orphaned recency node; drop it
        }
    }
    return totalBytes_ <= maxBytes_;
}

}  // namespace ruvia::edge
