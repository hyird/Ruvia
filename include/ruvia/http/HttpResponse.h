#pragma once

#include "ruvia/http/FileToken.h"
#include "ruvia/http/HttpCommon.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ruvia {

struct HttpResponseHeader {
    const char* bytes{nullptr};
    std::uint32_t nameSize{0};
    std::uint32_t valueSize{0};
    std::uint32_t knownBit{0};
    bool owned{false};

    [[nodiscard]] std::string_view name() const noexcept {
        return {bytes, nameSize};
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return {bytes + nameSize, valueSize};
    }
};

static_assert(std::is_trivially_copyable_v<HttpResponseHeader>);
static_assert(sizeof(HttpResponseHeader) <= 24);

class HttpResponseHeaders final {
public:
    using value_type = HttpResponseHeader;
    using iterator = HttpResponseHeader*;
    using const_iterator = const HttpResponseHeader*;

    explicit HttpResponseHeaders(std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~HttpResponseHeaders();

    HttpResponseHeaders(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders& operator=(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders(HttpResponseHeaders&& other) noexcept;
    HttpResponseHeaders& operator=(HttpResponseHeaders&& other) noexcept;

    HttpResponseHeader& add(std::string_view name, std::string_view value, std::uint32_t knownBit = 0);
    void assign(HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit);

    void reserve(std::size_t count);

    [[nodiscard]] iterator begin() noexcept {
        return data();
    }

    [[nodiscard]] iterator end() noexcept {
        return data() + size();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return data() + size();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return begin();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return end();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return spilled_ ? heap_.size() : size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

private:
    friend class Context;
    friend class HttpResponse;

    static constexpr std::size_t kInlineCapacity = 8;
    struct InlineStorage {
        alignas(HttpResponseHeader) std::byte bytes[sizeof(HttpResponseHeader)];
    };

    [[nodiscard]] HttpResponseHeader* inlineData() noexcept;
    [[nodiscard]] const HttpResponseHeader* inlineData() const noexcept;
    [[nodiscard]] HttpResponseHeader* data() noexcept;
    [[nodiscard]] const HttpResponseHeader* data() const noexcept;
    [[nodiscard]] HttpResponseHeader makeOwnedHeader(std::string_view name, std::string_view value, std::uint32_t knownBit);
    [[nodiscard]] HttpResponseHeader makeUninitializedHeader(std::string_view name, std::size_t valueSize, std::uint32_t knownBit);
    [[nodiscard]] static std::optional<HttpResponseHeader> makeStaticHeader(std::string_view name, std::string_view value, std::uint32_t knownBit) noexcept;
    [[nodiscard]] bool tryAssignOwnedInPlace(HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit) noexcept;
    HttpResponseHeader& addStableView(std::string_view name, std::string_view value, std::uint32_t knownBit = 0);
    HttpResponseHeader& addUninitializedValue(std::string_view name, std::size_t valueSize, std::uint32_t knownBit = 0);
    HttpResponseHeader& assignUninitializedValue(HttpResponseHeader& header, std::string_view name, std::size_t valueSize, std::uint32_t knownBit);
    void assignStableView(HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit);
    void releaseHeader(HttpResponseHeader& header) noexcept;
    void clear() noexcept;
    void spill();
    void moveFrom(HttpResponseHeaders&& other) noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<HttpResponseHeader> heap_;
    std::array<InlineStorage, kInlineCapacity> inline_;
    std::size_t size_{0};
    bool spilled_{false};
};

struct FileBody {
    FileToken file;
    std::uint64_t size{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

enum class HttpResponseBodyKind : std::uint8_t {
    kEmpty,
    kBorrowed,
    kStaticBorrowed,
    kOwned,
    kFile
};

class HttpResponse;

namespace detail {

void setResponseHeaderStableView(HttpResponse& response, std::string_view key, std::string_view value);
void setResponseHeaderUnsigned(HttpResponse& response, std::string_view key, std::uint64_t value, std::uint32_t knownBit);
void setResponseAllowHeader(HttpResponse& response, std::uint32_t methodMask);
void setResponseBodyStaticView(HttpResponse& response, std::string_view value) noexcept;

}  // namespace detail

class HttpResponse final {
public:
    enum KnownHeaderBit : std::uint32_t {
        kKnownHeaderContentLength    = 1U << 0,
        kKnownHeaderContentEncoding  = 1U << 1,
        kKnownHeaderContentType      = 1U << 2,
        kKnownHeaderConnection       = 1U << 3,
        kKnownHeaderVary             = 1U << 4,
        kKnownHeaderDate             = 1U << 5,
        kKnownHeaderServer           = 1U << 6,
        kKnownHeaderCacheControl     = 1U << 7,
        kKnownHeaderTransferEncoding = 1U << 8,
        kKnownHeaderAllow            = 1U << 9,
        kKnownHeaderAccessControlAllowOrigin      = 1U << 10,
        kKnownHeaderAccessControlAllowCredentials = 1U << 11,
        kKnownHeaderAccessControlAllowMethods     = 1U << 12,
        kKnownHeaderAccessControlAllowHeaders     = 1U << 13,
        kKnownHeaderAccessControlMaxAge           = 1U << 14,
        kKnownHeaderAccessControlExposeHeaders    = 1U << 15,
        kKnownHeaderAcceptRanges                  = 1U << 16,
        kKnownHeaderContentRange                  = 1U << 17,
        kKnownHeaderEtag                          = 1U << 18,
        kKnownHeaderLastModified                  = 1U << 19,
        kKnownHeaderLocation                      = 1U << 20,
        kKnownHeaderSetCookie                     = 1U << 21,
    };
    static constexpr std::size_t kKnownHeaderCount = 22;

    explicit HttpResponse(std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] std::uint16_t statusCode() const noexcept;
    [[nodiscard]] std::string_view statusText() const noexcept;
    [[nodiscard]] const HttpResponseHeaders& headers() const noexcept;
    [[nodiscard]] HttpResponseBodyKind bodyKind() const noexcept;
    [[nodiscard]] std::string_view bodyBytes() const noexcept;
    [[nodiscard]] std::size_t bodySize() const noexcept;
    [[nodiscard]] bool bodyEmpty() const noexcept;
    [[nodiscard]] bool hasFileBody() const noexcept;
    [[nodiscard]] const FileBody& fileBody() const;
    [[nodiscard]] std::uint32_t knownHeaderBits() const noexcept { return knownHeaderBits_; }
    [[nodiscard]] bool hasKnownHeader(KnownHeaderBit bit) const noexcept {
        return (knownHeaderBits_ & static_cast<std::uint32_t>(bit)) != 0;
    }
    [[nodiscard]] std::string_view header(KnownHeaderBit bit) const noexcept;
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    void setStatus(std::uint16_t statusCode, std::string_view statusText);
    void setHeader(std::string_view key, std::string_view value);
    void appendHeader(std::string_view key, std::string_view value);
    void reserveHeaders(std::size_t count);
    void setBodyCopy(std::string_view value);
    void setBodyView(std::string_view value) noexcept;
    void setBody(std::pmr::string&& value);
    void materializeBody();

    [[nodiscard]] static std::uint32_t classifyKnownHeader(std::string_view name) noexcept;

private:
    friend class Context;
    friend void detail::setResponseHeaderStableView(HttpResponse& response, std::string_view key, std::string_view value);
    friend void detail::setResponseHeaderUnsigned(HttpResponse& response, std::string_view key, std::uint64_t value, std::uint32_t knownBit);
    friend void detail::setResponseAllowHeader(HttpResponse& response, std::uint32_t methodMask);
    friend void detail::setResponseBodyStaticView(HttpResponse& response, std::string_view value) noexcept;

    void setBodyStaticView(std::string_view value) noexcept;
    void setHeaderStableView(std::string_view key, std::string_view value);
    void setHeaderUnsigned(std::string_view key, std::uint64_t value, std::uint32_t knownBit);
    void setAllowHeader(std::uint32_t methodMask);
    void setContentRange(std::uint64_t offset, std::uint64_t length, std::uint64_t size);
    void setContentRangeUnsatisfied(std::uint64_t size);
    void setHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit);
    void appendHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit);
    void setFileBody(FileToken file, std::uint64_t size);
    void setFileBody(FileToken file, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    [[nodiscard]] static std::size_t knownHeaderSlot(std::uint32_t bit) noexcept;
    [[nodiscard]] HttpResponseHeader* findHeaderForUpdate(std::string_view key, std::uint32_t knownBit) noexcept;
    [[nodiscard]] const HttpResponseHeader* findHeaderForRead(std::string_view key, std::uint32_t knownBit) const noexcept;
    HttpResponseHeader& prepareHeaderValueStorage(std::string_view key, std::size_t valueSize, std::uint32_t knownBit);
    void recordKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept;

    std::uint16_t statusCode_{200};
    std::uint32_t knownHeaderBits_{0};
    std::array<std::int32_t, kKnownHeaderCount> knownHeaderIndexes_{
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1};
    std::pmr::string statusText_;
    HttpResponseHeaders headers_;
    std::pmr::string body_;
    std::string_view bodyView_;
    HttpResponseBodyKind bodyKind_{HttpResponseBodyKind::kEmpty};
    std::optional<FileBody> fileBody_;
};

}  // namespace ruvia
