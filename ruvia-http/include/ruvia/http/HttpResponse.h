#pragma once

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/response/HttpResponseBody.h"
#include "ruvia/http/HttpStatus.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ruvia {

class HttpResponse;
class HttpResponseHeaders;
struct HttpResponseHeader;

namespace detail {

struct HttpResponseBodyAccess;
struct HttpResponseFileAccess;
struct HttpResponseHeaderAccess;
struct HttpResponseHeadersAccess;
struct HttpResponseHeaderStateAccess;

}  // namespace detail

struct HttpResponseHeader {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return bytes == nullptr ? std::string_view{} : std::string_view(bytes, nameSize);
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return bytes == nullptr ? std::string_view{} : std::string_view(bytes + nameSize, valueSize);
    }

private:
    friend class HttpResponse;
    friend class HttpResponseHeaders;
    friend struct detail::HttpResponseHeaderAccess;

    HttpResponseHeader() noexcept = default;

    const char* bytes{nullptr};
    std::uint32_t nameSize{0};
    std::uint32_t valueSize{0};
    std::uint32_t knownBit{0};
    bool owned{false};
    bool append{false};
};

static_assert(std::is_trivially_copyable_v<HttpResponseHeader>);
static_assert(sizeof(HttpResponseHeader) <= 24);

class HttpResponseHeaders final {
public:
    using value_type = HttpResponseHeader;
    using const_iterator = const HttpResponseHeader*;

    ~HttpResponseHeaders();

    [[nodiscard]] const_iterator begin() const& noexcept {
        return data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept {
        return data() + size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return spilled_ ? heap_.size() : size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

private:
    friend class HttpResponse;
    friend struct detail::HttpResponseHeadersAccess;

    using iterator = HttpResponseHeader*;

    HttpResponseHeaders(detail::HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource);

    HttpResponseHeaders(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders& operator=(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders(HttpResponseHeaders&& other) noexcept;
    HttpResponseHeaders& operator=(HttpResponseHeaders&&) = delete;

    static constexpr std::size_t kInlineCapacity = 8;
    struct InlineStorage {
        alignas(HttpResponseHeader) std::byte bytes[sizeof(HttpResponseHeader)];
    };

    [[nodiscard]] iterator begin() & noexcept {
        return data();
    }

    [[nodiscard]] iterator end() & noexcept {
        return data() + size();
    }

    [[nodiscard]] HttpResponseHeader* inlineData() noexcept;
    [[nodiscard]] const HttpResponseHeader* inlineData() const noexcept;
    [[nodiscard]] HttpResponseHeader* data() noexcept;
    [[nodiscard]] const HttpResponseHeader* data() const noexcept;
    [[nodiscard]] HttpResponseHeader makeOwnedHeader(std::string_view name, std::string_view value, std::uint32_t knownBit);
    [[nodiscard]] HttpResponseHeader makeUninitializedHeader(std::string_view name, std::size_t valueSize, std::uint32_t knownBit);
    [[nodiscard]] static std::optional<HttpResponseHeader> makeStaticHeader(std::string_view name, std::string_view value, std::uint32_t knownBit) noexcept;
    [[nodiscard]] bool tryAssignOwnedInPlace(HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit) noexcept;
    HttpResponseHeader& appendHeader(HttpResponseHeader header);
    HttpResponseHeader& addStableView(std::string_view name, std::string_view value, std::uint32_t knownBit = 0);
    HttpResponseHeader& addUninitializedValue(std::string_view name, std::size_t valueSize, std::uint32_t knownBit = 0);
    HttpResponseHeader& add(std::string_view name, std::string_view value, std::uint32_t knownBit = 0);
    void assign(HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit);
    HttpResponseHeader& assignUninitializedValue(HttpResponseHeader& header, std::string_view name, std::size_t valueSize, std::uint32_t knownBit);
    void assignStableView(HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit);
    void releaseHeader(HttpResponseHeader& header) noexcept;
    void reserve(std::size_t count);
    HttpResponseHeader& appendPreparedHeader(HttpResponseHeader header) noexcept;
    void clear() noexcept;
    void spill(std::size_t minCapacity);
    void moveFrom(HttpResponseHeaders&& other) noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<HttpResponseHeader> heap_;
    std::array<InlineStorage, kInlineCapacity> inline_;
    std::size_t size_{0};
    bool spilled_{false};
};

class HttpResponse final {
public:
    struct HeaderOptions {
        bool append{false};
    };

    explicit HttpResponse(std::pmr::memory_resource* resource = nullptr);

    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;
    HttpResponse(HttpResponse&& other) noexcept;
    HttpResponse& operator=(HttpResponse&& other) noexcept;

    [[nodiscard]] HttpStatusCode status() const noexcept;
    [[nodiscard]] const HttpResponseHeaders& headers() const& noexcept;
    [[nodiscard]] const HttpResponseHeaders& headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // A generic HttpResponse is always final (200..599). Interim 1xx progress
    // messages use HttpInterimResponseHead; 101 uses a dedicated protocol driver.
    void status(HttpStatusCode statusCode);
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, HeaderOptions options);
    // Remove a header set by an earlier step. header(key, std::nullopt) meant
    // deletion; removal now has its own named entry point.
    void removeHeader(std::string_view key);
    void body(std::string_view value);

private:
    friend struct detail::HttpResponseBodyAccess;
    friend struct detail::HttpResponseFileAccess;
    friend struct detail::HttpResponseHeaderStateAccess;

    static constexpr std::size_t kKnownHeaderCount = 22;

    void setBodyBorrowedView(std::string_view value) noexcept;
    void setBodyStaticView(std::string_view value) noexcept;
    void setBodyOwned(std::pmr::string&& value);
    void replaceBodyWithContentEncoding(std::pmr::string&& value, std::string_view contentEncoding);
    void materializeBody();
    void setHeaderStableView(std::string_view key, std::string_view value);
    void setHeaderUnsigned(std::string_view key, std::uint64_t value, std::uint32_t knownBit);
    void setAllowHeader(std::uint32_t methodMask);
    void setContentRange(std::uint64_t offset, std::uint64_t length, std::uint64_t size);
    void setContentRangeUnsatisfied(std::uint64_t size);
    void setHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit);
    void appendHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit);
    HttpResponseHeader& appendHeaderUninitializedValue(std::string_view key, std::size_t valueSize, std::uint32_t knownBit);
    HttpResponseHeader& upsertSetCookieHeaderUninitializedValue(std::string_view wirePrefix, std::string_view cookieName, std::string_view path, std::string_view domain, std::size_t valueSize);
    void upsertSetCookieHeaderValidated(std::string_view value);
    [[nodiscard]] HttpResponseHeader* findSetCookieHeader(std::string_view wirePrefix, std::string_view cookieName, bool hasPath, std::string_view path, std::string_view domain) noexcept;
    void eraseLaterSetCookieHeaders(HttpResponseHeader& retained, std::string_view wirePrefix, std::string_view cookieName, bool hasPath, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] HttpResponseHeader& collapseResponseHeaders(HttpResponseHeader& retained, std::string_view key, std::uint32_t knownBit) noexcept;
    bool removeHeaderValidated(std::string_view key, std::uint32_t knownBit) noexcept;
    void rebuildKnownHeaderIndex() noexcept;
    void reserveHeaders(std::size_t count);
    HttpResponse(detail::HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource);
    void setFileBody(std::filesystem::path file, std::uint64_t size);
    void setFileBody(std::filesystem::path file, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    void setFileBody(std::filesystem::path file, std::uint64_t size, std::uint64_t offset, std::uint64_t length, detail::ResponseFileIdentity identity);
    void setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size);
    void setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    void setBorrowedNativeFileBody(const detail::HttpNativePathChar* file, std::uint64_t size);
    void setBorrowedNativeFileBody(const detail::HttpNativePathChar* file, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] std::string_view knownHeaderValue(std::uint32_t bit) const noexcept;
    [[nodiscard]] HttpResponseHeader* findHeaderForUpdate(std::string_view key, std::uint32_t knownBit) noexcept;
    [[nodiscard]] const HttpResponseHeader* findHeaderForRead(std::string_view key, std::uint32_t knownBit) const noexcept;
    HttpResponseHeader& prepareHeaderValueStorage(std::string_view key, std::size_t valueSize, std::uint32_t knownBit);
    void recordKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept;
    [[nodiscard]] HttpResponse cloneForTransaction() const;

    HttpStatusCode statusCode_{http_status::kOk};
    std::uint32_t knownHeaderBits_{0};
    std::array<std::int16_t, kKnownHeaderCount> knownHeaderIndexes_{};
    HttpResponseHeaders headers_;
    detail::HttpResponseBody body_;
};

}  // namespace ruvia
