#include "ruvia/edge/DiskCache.h"

#include <array>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

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
constexpr std::uint32_t kMagic = 0x52564332;  // 'RVC2'

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

private:
    [[nodiscard]] std::uint64_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] unsigned char byte(std::size_t off) const noexcept {
        return static_cast<unsigned char>(data_[pos_ + off]);
    }

    std::string_view data_;
    std::size_t pos_{0};
};

[[nodiscard]] std::string serialize(std::string_view key, const CachedResponse& entry) {
    std::string out;
    out.reserve(64 + key.size() + entry.body.size());
    appendU32(out, kMagic);
    appendU32(out, static_cast<std::uint32_t>(key.size()));
    out.append(key);
    appendU16(out, entry.status);
    appendU64(out, static_cast<std::uint64_t>(entry.storedAt));
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
    return out;
}

// Parse a serialized entry. On success fills `key` and, if `entry` is non-null,
// the full payload; when `entry` is null only the key is decoded (used by the
// startup scan, which does not need to materialize bodies).
[[nodiscard]] bool deserialize(std::string_view data, std::string& key, CachedResponse* entry) {
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
    key.assign(keyView);
    if (entry == nullptr) {
        return true;
    }

    std::uint16_t status = 0;
    std::uint64_t storedAt = 0;
    std::uint64_t expiresAt = 0;
    std::uint64_t swr = 0;
    std::uint64_t sie = 0;
    std::uint32_t headerCount = 0;
    if (!reader.readU16(status) || !reader.readU64(storedAt) || !reader.readU64(expiresAt) ||
        !reader.readU64(swr) || !reader.readU64(sie) || !reader.readU32(headerCount)) {
        return false;
    }
    entry->status = status;
    entry->storedAt = static_cast<std::time_t>(storedAt);
    entry->expiresAt = static_cast<std::time_t>(expiresAt);
    entry->staleWhileRevalidate = swr;
    entry->staleIfError = sie;
    entry->headers.clear();
    entry->headers.reserve(headerCount);
    for (std::uint32_t i = 0; i < headerCount; ++i) {
        std::uint32_t nameLen = 0;
        std::string_view name;
        std::uint32_t valueLen = 0;
        std::string_view value;
        if (!reader.readU32(nameLen) || !reader.readBytes(nameLen, name) ||
            !reader.readU32(valueLen) || !reader.readBytes(valueLen, value)) {
            return false;
        }
        entry->headers.emplace_back(std::string(name), std::string(value));
    }
    std::uint64_t bodyLen = 0;
    std::string_view body;
    if (!reader.readU64(bodyLen) || !reader.readBytes(bodyLen, body)) {
        return false;
    }
    entry->body.assign(body);
    return true;
}

[[nodiscard]] bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(out.data(), size)) {
        return false;
    }
    return true;
}

}  // namespace

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
    // Adopt every well-formed entry file already present so the cache is warm
    // across restarts. Malformed or stale-format files are ignored (left on
    // disk); only files we can parse count against the budget.
    std::error_code ec;
    for (const auto& dirEntry : std::filesystem::directory_iterator(directory_, ec)) {
        if (ec || !dirEntry.is_regular_file()) {
            continue;
        }
        std::string data;
        if (!readFile(dirEntry.path(), data)) {
            continue;
        }
        std::string key;
        if (!deserialize(data, key, nullptr)) {
            continue;
        }
        const std::size_t bytes = data.size();
        auto [it, inserted] = index_.try_emplace(key, Entry{dirEntry.path().filename().string(), bytes});
        if (!inserted) {
            continue;  // hash collision already claimed: keep the first
        }
        recency_.push_front(key);
        lru_.emplace(key, recency_.begin());
        totalBytes_ += bytes;
    }
    evictWhileOverBudget();
}

std::shared_ptr<const CachedResponse> DiskCache::lookup(std::string_view key) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = index_.find(std::string(key));
    if (it == index_.end()) {
        return nullptr;
    }
    std::string data;
    if (!readFile(directory_ / it->second.fileName, data)) {
        removeLocked(it);
        return nullptr;
    }
    auto entry = std::make_shared<CachedResponse>();
    std::string storedKey;
    if (!deserialize(data, storedKey, entry.get()) || storedKey != key) {
        return nullptr;  // corrupt or a hash collision: treat as a miss
    }
    // Promote to most-recently-used.
    if (const auto lruIt = lru_.find(it->first); lruIt != lru_.end()) {
        recency_.splice(recency_.begin(), recency_, lruIt->second);
    }
    return entry;
}

bool DiskCache::store(std::string_view key, const CachedResponse& entry) {
    const std::string serialized = serialize(key, entry);
    if (serialized.size() > maxBytes_) {
        return false;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    const std::string fileName = fileNameFor(key);
    const std::filesystem::path finalPath = directory_ / fileName;
    const std::filesystem::path tempPath = directory_ / (fileName + ".tmp" + std::to_string(tempCounter_++));
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()))) {
            std::error_code ignore;
            std::filesystem::remove(tempPath, ignore);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    const std::string keyStr(key);
    if (const auto it = index_.find(keyStr); it != index_.end()) {
        totalBytes_ -= it->second.bytes;
        it->second = Entry{fileName, serialized.size()};
        totalBytes_ += serialized.size();
        if (const auto lruIt = lru_.find(it->first); lruIt != lru_.end()) {
            recency_.splice(recency_.begin(), recency_, lruIt->second);
        }
    } else {
        index_.emplace(keyStr, Entry{fileName, serialized.size()});
        recency_.push_front(keyStr);
        lru_.emplace(keyStr, recency_.begin());
        totalBytes_ += serialized.size();
    }
    evictWhileOverBudget();
    return true;
}

bool DiskCache::purge(std::string_view key) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = index_.find(std::string(key));
    if (it == index_.end()) {
        return false;
    }
    removeLocked(it);
    return true;
}

std::size_t DiskCache::purgePrefix(std::string_view prefix) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::size_t removed = 0;
    for (auto it = index_.begin(); it != index_.end();) {
        if (std::string_view(it->first).starts_with(prefix)) {
            const auto next = std::next(it);
            removeLocked(it);
            it = next;
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void DiskCache::clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::error_code ec;
    for (const auto& [key, entry] : index_) {
        std::filesystem::remove(directory_ / entry.fileName, ec);
    }
    index_.clear();
    lru_.clear();
    recency_.clear();
    totalBytes_ = 0;
}

std::size_t DiskCache::entryCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return index_.size();
}

std::size_t DiskCache::byteSize() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return totalBytes_;
}

void DiskCache::removeLocked(std::unordered_map<std::string, Entry>::iterator it) noexcept {
    std::error_code ec;
    std::filesystem::remove(directory_ / it->second.fileName, ec);
    totalBytes_ -= it->second.bytes;
    if (const auto lruIt = lru_.find(it->first); lruIt != lru_.end()) {
        recency_.erase(lruIt->second);
        lru_.erase(lruIt);
    }
    index_.erase(it);
}

void DiskCache::evictWhileOverBudget() noexcept {
    while (totalBytes_ > maxBytes_ && !recency_.empty()) {
        const std::string victim = recency_.back();
        if (const auto it = index_.find(victim); it != index_.end()) {
            removeLocked(it);
        } else {
            recency_.pop_back();  // orphaned recency node; drop it
        }
    }
}

}  // namespace ruvia::edge
