#include "ruvia/edge/detail/DiskCacheRecord.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace ruvia::edge {

namespace {

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

// The entry-file framing. Bump on any layout change so stale files from an older
// layout are rejected by the checksum-and-magic check instead of misparsed.
constexpr std::uint32_t kMagic = 0x52564334;  // 'RVC4'

}  // namespace

std::uint64_t diskCacheStableHash(std::string_view data) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::optional<std::string> serializeDiskCacheRecord(
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
    appendU64(out, diskCacheStableHash(out));
    return out;
}

// Parse a serialized entry. On success fills `key` and, if `entry` is non-null,
// the full payload; when `entry` is null only the key is decoded (used by the
// startup scan, which does not need to materialize bodies).
bool deserializeDiskCacheRecord(std::string_view data, std::string& key, CachedResponse* entry) {
    if (data.size() < 8) {
        return false;
    }
    std::uint64_t expectedChecksum = 0;
    Reader trailer(data.substr(data.size() - 8));
    if (!trailer.readU64(expectedChecksum) ||
        expectedChecksum != diskCacheStableHash(data.substr(0, data.size() - 8))) {
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

}  // namespace ruvia::edge
