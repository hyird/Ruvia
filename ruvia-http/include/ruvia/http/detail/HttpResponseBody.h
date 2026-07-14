#pragma once

#include "ruvia/http/detail/HttpResponseFileBody.h"
#include "ruvia/http/detail/NativePath.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia {

class HttpResponse;

namespace detail {

class HttpResponseBody;

class HttpEmptyResponseBody final {
private:
    friend class HttpResponseBody;
    constexpr HttpEmptyResponseBody() noexcept = default;
};

class HttpBorrowedResponseBytes final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class HttpResponseBody;

    explicit constexpr HttpBorrowedResponseBytes(
        std::string_view bytes) noexcept
        : bytes_(bytes) {}

    std::string_view bytes_;
};

class HttpStaticResponseBytes final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class HttpResponseBody;

    explicit constexpr HttpStaticResponseBytes(
        std::string_view bytes) noexcept
        : bytes_(bytes) {}

    std::string_view bytes_;
};

class HttpOwnedResponseBytes final {
public:
    [[nodiscard]] std::string_view bytes() const & noexcept {
        return std::string_view(bytes_.data(), bytes_.size());
    }
    [[nodiscard]] std::string_view bytes() const && = delete;

private:
    friend class HttpResponseBody;

    HttpOwnedResponseBytes(
        std::pmr::memory_resource* resource,
        std::string_view bytes)
        : bytes_(bytes.data(), bytes.size(), resource) {}

    HttpOwnedResponseBytes(
        std::pmr::memory_resource* resource,
        std::pmr::string&& bytes)
        : bytes_(resource) {
        bytes_ = std::move(bytes);
    }

    std::pmr::string bytes_;
};

class HttpOwnedResponseFile final {
public:
    [[nodiscard]] const HttpNativePathChar*
    nativePathCStr() const & noexcept {
        return nativePath_.c_str();
    }
    [[nodiscard]] const HttpNativePathChar*
    nativePathCStr() const && = delete;

    [[nodiscard]] constexpr std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class HttpResponseBody;

    HttpOwnedResponseFile(
        std::pmr::memory_resource* resource,
        const std::filesystem::path& file,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length)
        : nativePath_(resource),
          size_(size),
          offset_(offset),
          length_(length) {
        assignHttpNativePath(nativePath_, file);
    }

    HttpNativePathString nativePath_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

class HttpBorrowedResponseFile final {
public:
    [[nodiscard]] constexpr const HttpNativePathChar* nativePathCStr()
        const noexcept {
        return nativePath_;
    }

    [[nodiscard]] constexpr std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class HttpResponseBody;

    constexpr HttpBorrowedResponseFile(
        const HttpNativePathChar* nativePath,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) noexcept
        : nativePath_(nativePath),
          size_(size),
          offset_(offset),
          length_(length) {}

    const HttpNativePathChar* nativePath_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

// Owns exactly one legal buffered response-body representation. The common
// bytes()/file()/size() observations are derived from the active alternative;
// callers never need a separate kind enum or has-file side channel.
class HttpResponseBody final {
public:
    HttpResponseBody() noexcept
        : value_(HttpEmptyResponseBody{}) {}

    HttpResponseBody(const HttpResponseBody&) = delete;
    HttpResponseBody& operator=(const HttpResponseBody&) = delete;
    HttpResponseBody(HttpResponseBody&&) = default;
    HttpResponseBody& operator=(HttpResponseBody&&) = delete;

    [[nodiscard]] const HttpEmptyResponseBody* empty() const & noexcept {
        return std::get_if<HttpEmptyResponseBody>(&value_);
    }
    [[nodiscard]] const HttpEmptyResponseBody* empty() const && = delete;

    [[nodiscard]] const HttpBorrowedResponseBytes* borrowedBytes()
        const & noexcept {
        return std::get_if<HttpBorrowedResponseBytes>(&value_);
    }
    [[nodiscard]] const HttpBorrowedResponseBytes*
    borrowedBytes() const && = delete;

    [[nodiscard]] const HttpStaticResponseBytes* staticBytes()
        const & noexcept {
        return std::get_if<HttpStaticResponseBytes>(&value_);
    }
    [[nodiscard]] const HttpStaticResponseBytes*
    staticBytes() const && = delete;

    [[nodiscard]] const HttpOwnedResponseBytes*
    ownedBytes() const & noexcept {
        return std::get_if<HttpOwnedResponseBytes>(&value_);
    }
    [[nodiscard]] const HttpOwnedResponseBytes*
    ownedBytes() const && = delete;

    [[nodiscard]] const HttpOwnedResponseFile* ownedFile() const & noexcept {
        return std::get_if<HttpOwnedResponseFile>(&value_);
    }
    [[nodiscard]] const HttpOwnedResponseFile* ownedFile() const && = delete;

    [[nodiscard]] const HttpBorrowedResponseFile* borrowedFile()
        const & noexcept {
        return std::get_if<HttpBorrowedResponseFile>(&value_);
    }
    [[nodiscard]] const HttpBorrowedResponseFile*
    borrowedFile() const && = delete;

    [[nodiscard]] std::string_view bytes() const & noexcept {
        if (const auto* body = borrowedBytes()) {
            return body->bytes();
        }
        if (const auto* body = staticBytes()) {
            return body->bytes();
        }
        if (const auto* body = ownedBytes()) {
            return body->bytes();
        }
        return {};
    }
    [[nodiscard]] std::string_view bytes() const && = delete;

    [[nodiscard]] std::optional<ResponseFileBody> file() const & noexcept {
        if (const auto* body = ownedFile()) {
            return ResponseFileBody(
                body->nativePathCStr(),
                body->size(),
                body->offset(),
                body->length());
        }
        if (const auto* body = borrowedFile()) {
            return ResponseFileBody(
                body->nativePathCStr(),
                body->size(),
                body->offset(),
                body->length());
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ResponseFileBody> file() const && = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        if (const auto* body = ownedFile()) {
            return static_cast<std::size_t>(body->length());
        }
        if (const auto* body = borrowedFile()) {
            return static_cast<std::size_t>(body->length());
        }
        return bytes().size();
    }

private:
    friend class ::ruvia::HttpResponse;

    using Value = std::variant<
        HttpEmptyResponseBody,
        HttpBorrowedResponseBytes,
        HttpStaticResponseBytes,
        HttpOwnedResponseBytes,
        HttpOwnedResponseFile,
        HttpBorrowedResponseFile>;

    void setEmpty() noexcept {
        value_.emplace<HttpEmptyResponseBody>(HttpEmptyResponseBody{});
    }

    void setCopy(
        std::pmr::memory_resource* resource,
        std::string_view bytes) {
        if (bytes.empty()) {
            setEmpty();
            return;
        }
        HttpOwnedResponseBytes body(resource, bytes);
        value_.emplace<HttpOwnedResponseBytes>(std::move(body));
    }

    void setBorrowed(std::string_view bytes) noexcept {
        if (bytes.empty()) {
            setEmpty();
            return;
        }
        value_.emplace<HttpBorrowedResponseBytes>(
            HttpBorrowedResponseBytes(bytes));
    }

    void setStatic(std::string_view bytes) noexcept {
        if (bytes.empty()) {
            setEmpty();
            return;
        }
        value_.emplace<HttpStaticResponseBytes>(
            HttpStaticResponseBytes(bytes));
    }

    void setOwned(
        std::pmr::memory_resource* resource,
        std::pmr::string&& bytes) {
        if (bytes.empty()) {
            setEmpty();
            return;
        }
        HttpOwnedResponseBytes body(resource, std::move(bytes));
        value_.emplace<HttpOwnedResponseBytes>(std::move(body));
    }

    void materialize(std::pmr::memory_resource* resource) {
        const auto* borrowed = borrowedBytes();
        if (borrowed == nullptr) {
            return;
        }
        HttpOwnedResponseBytes body(resource, borrowed->bytes());
        value_.emplace<HttpOwnedResponseBytes>(std::move(body));
    }

    void setOwnedFile(
        std::pmr::memory_resource* resource,
        const std::filesystem::path& file,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) {
        HttpOwnedResponseFile body(resource, file, size, offset, length);
        value_.emplace<HttpOwnedResponseFile>(std::move(body));
    }

    void setBorrowedFile(
        const HttpNativePathChar* file,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) noexcept {
        value_.emplace<HttpBorrowedResponseFile>(
            HttpBorrowedResponseFile(file, size, offset, length));
    }

    Value value_;
};

static_assert(std::is_nothrow_move_constructible_v<HttpOwnedResponseBytes>);
static_assert(std::is_nothrow_move_constructible_v<HttpOwnedResponseFile>);

}  // namespace detail
}  // namespace ruvia
