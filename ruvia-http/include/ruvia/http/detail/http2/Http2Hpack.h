#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ruvia::detail {

enum class HpackDecodeError {
    kNeedMore,
    kIntegerOverflow,
    kInvalidIndex,
    kInvalidString,
    kInvalidHuffman,
    kDynamicTableSize,
    kCallbackRejected
};

class HpackDecoded final {
private:
    friend class HpackDecodeResult;

    constexpr HpackDecoded() noexcept = default;
};

class HpackDecodeFailure final {
public:
    [[nodiscard]] constexpr HpackDecodeError error() const noexcept {
        return error_;
    }

private:
    friend class HpackDecodeResult;

    explicit constexpr HpackDecodeFailure(HpackDecodeError error) noexcept
        : error_(error) {}

    HpackDecodeError error_;
};

class HpackDecodeResult final {
public:
    [[nodiscard]] constexpr const HpackDecoded* decoded() const & noexcept {
        return std::get_if<HpackDecoded>(&state_);
    }
    [[nodiscard]] constexpr const HpackDecoded* decoded() const && = delete;

    [[nodiscard]] constexpr const HpackDecodeFailure* failure() const & noexcept {
        return std::get_if<HpackDecodeFailure>(&state_);
    }
    [[nodiscard]] constexpr const HpackDecodeFailure* failure() const && = delete;

private:
    friend class HpackDecoder;

    constexpr HpackDecodeResult() noexcept
        : state_(HpackDecoded()) {}

    explicit constexpr HpackDecodeResult(HpackDecodeError error) noexcept
        : state_(HpackDecodeFailure(error)) {}

    std::variant<HpackDecoded, HpackDecodeFailure> state_;
};

struct HpackStaticIndex final {
    static constexpr std::uint32_t kStatus200 = 8;
    static constexpr std::uint32_t kStatus204 = 9;
    static constexpr std::uint32_t kStatus206 = 10;
    static constexpr std::uint32_t kStatus304 = 11;
    static constexpr std::uint32_t kStatus400 = 12;
    static constexpr std::uint32_t kStatus404 = 13;
    static constexpr std::uint32_t kStatus500 = 14;
    static constexpr std::uint32_t kAcceptRanges = 18;
    static constexpr std::uint32_t kAccessControlAllowOrigin = 20;
    static constexpr std::uint32_t kAllow = 22;
    static constexpr std::uint32_t kCacheControl = 24;
    static constexpr std::uint32_t kContentEncoding = 26;
    static constexpr std::uint32_t kContentLength = 28;
    static constexpr std::uint32_t kContentRange = 30;
    static constexpr std::uint32_t kContentType = 31;
    static constexpr std::uint32_t kDate = 33;
    static constexpr std::uint32_t kEtag = 34;
    static constexpr std::uint32_t kLastModified = 44;
    static constexpr std::uint32_t kLocation = 46;
    static constexpr std::uint32_t kServer = 54;
    static constexpr std::uint32_t kSetCookie = 55;
    static constexpr std::uint32_t kVary = 59;
};

struct HpackHuffmanNode final {
    std::int16_t child[2]{-1, -1};
    std::int16_t symbol{-1};
};

// HPACK literal representation prefixes (RFC 7541 §6.2).
inline constexpr std::uint8_t kHpackLiteralWithoutIndexing = 0x00;  // §6.2.2
inline constexpr std::uint8_t kHpackLiteralNeverIndexed = 0x10;     // §6.2.3

// Single source of truth for which header fields must never be committed to an
// HPACK dynamic table. RFC 7541 §7.1.3: credential-bearing fields SHOULD use the
// never-indexed literal so that an intermediary along the path never places them
// in a shared dynamic table (a compression side-channel, cf. CRIME). Names must
// already be lowercased (both encode call sites lowercase before dispatch).
[[nodiscard]] inline bool hpackHeaderNameIsSensitive(std::string_view name) noexcept {
    return name == "authorization" || name == "proxy-authorization" ||
        name == "cookie" || name == "set-cookie";
}

class HpackDecoder final {
public:
    using HeaderCallback = bool (*)(void*, std::string_view, std::string_view);

    explicit HpackDecoder(std::pmr::memory_resource* resource);

    HpackDecoder(const HpackDecoder&) = delete;
    HpackDecoder& operator=(const HpackDecoder&) = delete;

    void setMaxDynamicTableSize(std::size_t bytes);
    [[nodiscard]] HpackDecodeResult decode(std::string_view block, void* target, HeaderCallback callback);

private:
    using StepResult = std::optional<HpackDecodeError>;

    struct Entry final {
        std::pmr::string name;
        std::pmr::string value;
    };

    struct HeaderView final {
        std::string_view name;
        std::string_view value;
    };

    [[nodiscard]] static std::size_t entrySize(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] StepResult decodeInteger(
        const unsigned char*& cursor,
        const unsigned char* end,
        std::uint8_t prefixBits,
        std::uint32_t& value) const noexcept;
    [[nodiscard]] StepResult decodeString(
        const unsigned char*& cursor,
        const unsigned char* end,
        std::pmr::string& scratch,
        std::string_view& value);
    // `rejected` is an in/out latch: once a callback has returned false, no further
    // callbacks fire, BUT the block keeps decoding and dynamic-table insertions still
    // apply (including this entry's) so the connection-global table stays consistent
    // -- RFC 7541 requires the whole field block to be processed. Set true here when a
    // fresh callback rejects; the caller reports it after finishing the block.
    [[nodiscard]] StepResult decodeLiteralHeader(
        const unsigned char*& cursor,
        const unsigned char* end,
        std::uint8_t nameIndexPrefixBits,
        bool indexIntoDynamic,
        void* target,
        HeaderCallback callback,
        bool& rejected);
    [[nodiscard]] StepResult decodeHuffman(
        std::string_view encoded,
        std::pmr::string& output);
    void releaseScratch();
    [[nodiscard]] StepResult indexedHeader(
        std::uint32_t index,
        HeaderView& header) const noexcept;
    [[nodiscard]] StepResult indexedName(
        std::uint32_t index,
        std::string_view& name) const noexcept;
    [[nodiscard]] std::size_t dynamicEntryCount() const noexcept;
    [[nodiscard]] const Entry& dynamicEntryByNewestIndex(std::size_t newestIndex) const noexcept;
    void addDynamic(std::string_view name, std::string_view value);
    void clearDynamic() noexcept;
    void evictDynamicToFit(std::size_t entrySize);
    void evictDynamic();
    void compactDynamic();

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> dynamic_;
    std::pmr::string nameScratch_;
    std::pmr::string valueScratch_;
    std::size_t dynamicSize_{0};
    std::size_t dynamicOffset_{0};
    std::size_t maxDynamicSize_{4096};
    std::size_t allowedDynamicSize_{4096};
};

class HpackEncoder final {
public:
    static void encodeIndexed(std::pmr::string& out, std::uint32_t index);
    static void encodeHeader(std::pmr::string& out, std::string_view name, std::string_view value);
    static void encodeHeaderWithNameIndex(
        std::pmr::string& out,
        std::uint32_t nameIndex,
        std::string_view value,
        bool neverIndexed = false);
    static void encodeStatus(std::pmr::string& out, std::uint16_t status);

private:
    static void encodeInteger(
        std::pmr::string& out,
        std::uint8_t firstByteMask,
        std::uint8_t prefixBits,
        std::uint32_t value);
    static void encodeString(std::pmr::string& out, std::string_view value);
};

}  // namespace ruvia::detail
