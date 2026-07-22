#include "ruvia/edge/detail/DiskCache.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace ruvia::edge {
namespace {

// A stable 64-bit key hash (FNV-1a) for deriving on-disk file names. Stable
// across runs and platforms so a scanned directory keeps naming its entries the
// same way, unlike std::hash.
[[nodiscard]] std::uint64_t stableHash(std::string_view key) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char c : key) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// The entry-file framing. Bump on any layout change so stale files from an older
// format are ignored rather than misread.
constexpr std::uint32_t kMagic = 0x52564334;  // 'RVC4'

void appendU16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void appendU32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}

void appendU64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}

// A bounds-checked little-endian reader over a byte buffer.
class Reader final {
public:
    explicit Reader(std::string_view data) noexcept : data_(data) {}

    [[nodiscard]] bool readU16(std::uint16_t& out) noexcept {
        if (remaining() < 2) {
            return false;
        }
        out = static_cast<std::uint16_t>(byte(0)) |
              static_cast<std::uint16_t>(byte(1) << 8);
        pos_ += 2;
        return true;
    }

    [[nodiscard]] bool readU32(std::uint32_t& out) noexcept {
        if (remaining() < 4) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            out |= static_cast<std::uint32_t>(byte(i)) << (8 * i);
        }
        pos_ += 4;
        return true;
    }

    [[nodiscard]] bool readU64(std::uint64_t& out) noexcept {
        if (remaining() < 8) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out |= static_cast<std::uint64_t>(byte(i)) << (8 * i);
        }
        pos_ += 8;
        return true;
    }

    [[nodiscard]] bool readBytes(std::uint64_t n, std::string_view& out) noexcept {
        if (remaining() < n) {
            return false;
        }
        out = data_.substr(pos_, n);
        pos_ += n;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return data_.size() - pos_;
    }

private:
    [[nodiscard]] unsigned char byte(std::size_t off) const noexcept {
        return static_cast<unsigned char>(data_[pos_ + off]);
    }

    std::string_view data_;
    std::size_t pos_{0};
};

[[nodiscard]] std::optional<std::string> serialize(
    std::string_view key,
    const CachedResponse& entry) {
    constexpr auto kMaxU32 = (std::numeric_limits<std::uint32_t>::max)();
    if (key.size() > kMaxU32 || entry.headers.size() > kMaxU32) {
        return std::nullopt;
    }
    for (const auto& [name, value] : entry.headers) {
        if (name.size() > kMaxU32 || value.size() > kMaxU32) {
            return std::nullopt;
        }
    }

    std::string out;
    out.reserve(72 + key.size() + entry.body.size());
    appendU32(out, kMagic);
    appendU32(out, static_cast<std::uint32_t>(key.size()));
    out.append(key);
    appendU16(out, entry.status);
    appendU64(out, static_cast<std::uint64_t>(entry.storedAt));
    appendU64(out, entry.initialAge);
    appendU64(out, static_cast<std::uint64_t>(entry.expiresAt));
    appendU64(out, entry.staleWhileRevalidate);
    appendU64(out, entry.staleIfError);
    appendU32(out, static_cast<std::uint32_t>(entry.headers.size()));
    for (const auto& [name, value] : entry.headers) {
        appendU32(out, static_cast<std::uint32_t>(name.size()));
        out.append(name);
        appendU32(out, static_cast<std::uint32_t>(value.size()));
        out.append(value);
    }
    appendU64(out, entry.body.size());
    out.append(entry.body);
    // Detect truncated/torn writes and silent payload corruption during the
    // next startup scan. This is an integrity checksum, not an authenticity
    // primitive: callers that can modify the cache directory are trusted.
    appendU64(out, stableHash(out));
    return out;
}

// Parse a serialized entry. On success fills `key` and, if `entry` is non-null,
// the full payload; when `entry` is null only the key is decoded (used by the
// startup scan, which does not need to materialize bodies).
[[nodiscard]] bool deserialize(std::string_view data, std::string& key, CachedResponse* entry) {
    if (data.size() < 8) {
        return false;
    }
    std::uint64_t expectedChecksum = 0;
    Reader trailer(data.substr(data.size() - 8));
    if (!trailer.readU64(expectedChecksum) ||
        expectedChecksum != stableHash(data.substr(0, data.size() - 8))) {
        return false;
    }

    Reader reader(data);
    std::uint32_t magic = 0;
    if (!reader.readU32(magic) || magic != kMagic) {
        return false;
    }
    std::uint32_t keyLen = 0;
    std::string_view keyView;
    if (!reader.readU32(keyLen) || !reader.readBytes(keyLen, keyView)) {
        return false;
    }
    std::uint16_t status = 0;
    std::uint64_t storedAt = 0;
    std::uint64_t initialAge = 0;
    std::uint64_t expiresAt = 0;
    std::uint64_t swr = 0;
    std::uint64_t sie = 0;
    std::uint32_t headerCount = 0;
    if (!reader.readU16(status) || !reader.readU64(storedAt) ||
        !reader.readU64(initialAge) || !reader.readU64(expiresAt) ||
        !reader.readU64(swr) || !reader.readU64(sie) || !reader.readU32(headerCount)) {
        return false;
    }
    if (headerCount > reader.remaining() / 8) {
        return false;  // every header needs at least two encoded lengths
    }

    CachedResponse parsed;
    parsed.status = status;
    parsed.storedAt = static_cast<std::time_t>(storedAt);
    parsed.initialAge = initialAge;
    parsed.expiresAt = static_cast<std::time_t>(expiresAt);
    parsed.staleWhileRevalidate = swr;
    parsed.staleIfError = sie;
    if (entry != nullptr) {
        parsed.headers.reserve(headerCount);
    }
    for (std::uint32_t i = 0; i < headerCount; ++i) {
        std::uint32_t nameLen = 0;
        std::string_view name;
        std::uint32_t valueLen = 0;
        std::string_view value;
        if (!reader.readU32(nameLen) || !reader.readBytes(nameLen, name) ||
            !reader.readU32(valueLen) || !reader.readBytes(valueLen, value)) {
            return false;
        }
        if (entry != nullptr) {
            parsed.headers.emplace_back(std::string(name), std::string(value));
        }
    }
    std::uint64_t bodyLen = 0;
    std::string_view body;
    if (!reader.readU64(bodyLen) || !reader.readBytes(bodyLen, body)) {
        return false;
    }
    std::uint64_t checksum = 0;
    if (!reader.readU64(checksum) || reader.remaining() != 0 ||
        checksum != expectedChecksum) {
        return false;
    }

    key.assign(keyView);
    if (entry != nullptr) {
        parsed.body.assign(body);
        *entry = std::move(parsed);
    }
    return true;
}

[[nodiscard]] bool readFile(
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

[[nodiscard]] bool isLowerHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

[[nodiscard]] bool isCommittedFileName(std::string_view name) noexcept {
    return name.size() == 20 &&
        std::all_of(name.begin(), name.begin() + 16, isLowerHex) &&
        name.substr(16) == ".rvc";
}

[[nodiscard]] bool isOwnedTransientFileName(std::string_view name) noexcept {
    return name.size() > 24 &&
        std::all_of(name.begin(), name.begin() + 16, isLowerHex) &&
        (name.substr(16).starts_with(".rvc.tmp") ||
         name.substr(16).starts_with(".rvc.delete"));
}

void syncDirectoryBestEffort(const std::filesystem::path& directory) noexcept {
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
}

[[nodiscard]] bool flushFileToDisk(const std::filesystem::path& path) noexcept {
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
}

[[nodiscard]] bool commitReplacement(
    const std::filesystem::path& temporary,
    const std::filesystem::path& finalPath) noexcept {
    std::error_code ec;
    std::filesystem::rename(temporary, finalPath, ec);
    if (ec) {
        return false;
    }
    syncDirectoryBestEffort(finalPath.parent_path());
    return true;
}

void removeOwnedFileBestEffort(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    if (!ec) {
        syncDirectoryBestEffort(path.parent_path());
    }
}

}  // namespace

class DiskCache::DirectoryLease final {
public:
    explicit DirectoryLease(const std::filesystem::path& directory)
        : path_(directory / ".ruvia-cache.lock") {
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
    }

    ~DirectoryLease() {
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
    }

    DirectoryLease(const DirectoryLease&) = delete;
    DirectoryLease& operator=(const DirectoryLease&) = delete;

private:
    std::filesystem::path path_;
    int descriptor_{-1};
};

std::string DiskCache::fileNameFor(std::string_view key) {
    std::array<char, 16> hex{};
    std::uint64_t h = stableHash(key);
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
            isOwnedTransientFileName(fileName)) {
            removeOwnedFileBestEffort(path);
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status) ||
            !isCommittedFileName(fileName)) {
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }

        std::string data;
        if (!readFile(path, maxBytes_, data)) {
            removeOwnedFileBestEffort(path);
            current.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }
        std::string key;
        if (!deserialize(data, key, nullptr) || fileNameFor(key) != fileName) {
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
    if (!readFile(directory_ / it->second.fileName, maxBytes_, data)) {
        (void)removeLocked(it);
        return std::nullopt;
    }
    CachedResponse entry;
    std::string storedKey;
    if (!deserialize(data, storedKey, &entry) || storedKey != key) {
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
    const auto serialized = serialize(key, entry);
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
