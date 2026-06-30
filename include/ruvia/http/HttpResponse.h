#pragma once

#include "ruvia/detail/NativePath.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/memory/PmrResource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ruvia {

class Context;
class HttpResponse;
class HttpResponseHeaders;
struct HttpResponseHeader;

namespace detail {

struct HttpResponseBodyAccess;
struct HttpResponseFileAccess;
struct HttpResponseHeaderAccess;
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
};

static_assert(std::is_trivially_copyable_v<HttpResponseHeader>);
static_assert(sizeof(HttpResponseHeader) <= 24);

class HttpResponseHeaders final {
public:
    using value_type = HttpResponseHeader;
    using const_iterator = const HttpResponseHeader*;

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

    using iterator = HttpResponseHeader*;

    explicit HttpResponseHeaders(std::pmr::memory_resource* resource = nullptr);
    HttpResponseHeaders(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);
    ~HttpResponseHeaders();

    HttpResponseHeaders(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders& operator=(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders(HttpResponseHeaders&& other) noexcept;
    HttpResponseHeaders& operator=(HttpResponseHeaders&& other) noexcept;

    static constexpr std::size_t kInlineCapacity = 8;
    struct InlineStorage {
        alignas(HttpResponseHeader) std::byte bytes[sizeof(HttpResponseHeader)];
    };

    [[nodiscard]] iterator begin() noexcept {
        return data();
    }

    [[nodiscard]] iterator end() noexcept {
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
    explicit HttpResponse(std::pmr::memory_resource* resource = nullptr);

    [[nodiscard]] std::uint16_t statusCode() const noexcept;
    [[nodiscard]] std::string_view statusText() const noexcept;
    [[nodiscard]] const HttpResponseHeaders& headers() const noexcept;
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    void setStatus(std::uint16_t statusCode, std::string_view statusText);
    void setHeader(std::string_view key, std::string_view value);
    void setBodyCopy(std::string_view value);
    void setBodyView(std::string_view value) noexcept;
    void setBodyOwned(std::pmr::string&& value);

private:
    friend struct detail::HttpResponseBodyAccess;
    friend struct detail::HttpResponseFileAccess;
    friend struct detail::HttpResponseHeaderStateAccess;

    enum class BodyKind : std::uint8_t {
        kEmpty,
        kBorrowed,
        kStaticBorrowed,
        kOwned,
        kFile
    };
    class FileBody final {
    public:
        using NativePathChar = detail::NativePathChar;
        using NativePathString = detail::NativePathString;

        FileBody(
            std::pmr::memory_resource* resource,
            std::filesystem::path file,
            std::uint64_t size,
            std::uint64_t offset,
            std::uint64_t length)
            : ownedPath_(resource),
              size_(size),
              offset_(offset),
              length_(length) {
            detail::assignNativePath(ownedPath_, file);
        }

        FileBody(
            std::pmr::memory_resource* resource,
            const NativePathChar* borrowedNativePath,
            std::uint64_t size,
            std::uint64_t offset,
            std::uint64_t length)
            : ownedPath_(resource),
              borrowedNativePath_(borrowedNativePath),
              size_(size),
              offset_(offset),
              length_(length) {}

    private:
        friend class HttpResponse;
        friend struct detail::HttpResponseFileAccess;

        [[nodiscard]] const NativePathChar* nativePathCStr() const noexcept {
            return borrowedNativePath_ == nullptr ? ownedPath_.c_str() : borrowedNativePath_;
        }

        NativePathString ownedPath_;
        const NativePathChar* borrowedNativePath_{nullptr};
        std::uint64_t size_{0};
        std::uint64_t offset_{0};
        std::uint64_t length_{0};
    };
    static constexpr std::size_t kKnownHeaderCount = 22;

    void setBodyStaticView(std::string_view value) noexcept;
    void materializeBody();
    void setHeaderStableView(std::string_view key, std::string_view value);
    void setHeaderUnsigned(std::string_view key, std::uint64_t value, std::uint32_t knownBit);
    void setAllowHeader(std::uint32_t methodMask);
    void setContentRange(std::uint64_t offset, std::uint64_t length, std::uint64_t size);
    void setContentRangeUnsatisfied(std::uint64_t size);
    void setHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit);
    void appendHeaderValidated(std::string_view key, std::string_view value, std::uint32_t knownBit);
    void reserveHeaders(std::size_t count);
    HttpResponse(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);
    void setFileBody(std::filesystem::path file, std::uint64_t size);
    void setFileBody(std::filesystem::path file, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    void setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size);
    void setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    void setBorrowedNativeFileBody(const FileBody::NativePathChar* file, std::uint64_t size);
    void setBorrowedNativeFileBody(
        const FileBody::NativePathChar* file,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length);
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] std::string_view bodyBytes() const noexcept;
    [[nodiscard]] std::size_t bodySize() const noexcept;
    [[nodiscard]] bool hasFileBody() const noexcept;
    [[nodiscard]] const FileBody& fileBody() const;
    [[nodiscard]] std::string_view knownHeaderValue(std::uint32_t bit) const noexcept;
    [[nodiscard]] HttpResponseHeader* findHeaderForUpdate(std::string_view key, std::uint32_t knownBit) noexcept;
    [[nodiscard]] const HttpResponseHeader* findHeaderForRead(std::string_view key, std::uint32_t knownBit) const noexcept;
    HttpResponseHeader& prepareHeaderValueStorage(std::string_view key, std::size_t valueSize, std::uint32_t knownBit);
    void recordKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept;

    std::uint16_t statusCode_{200};
    std::uint32_t knownHeaderBits_{0};
    std::array<std::int16_t, kKnownHeaderCount> knownHeaderIndexes_{};
    std::pmr::string statusText_;
    HttpResponseHeaders headers_;
    std::pmr::string body_;
    std::string_view bodyView_;
    BodyKind bodyKind_{BodyKind::kEmpty};
    std::optional<FileBody> fileBody_;
};

}  // namespace ruvia
