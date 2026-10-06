#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/HttpResponseFile.h"
#include "ruvia/http/detail/util/HttpPmrObject.h"
#include "ruvia/http/detail/util/NativePath.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"

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

    explicit constexpr HttpBorrowedResponseBytes(std::string_view bytes) noexcept
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

    explicit constexpr HttpStaticResponseBytes(std::string_view bytes) noexcept
        : bytes_(bytes) {}

    std::string_view bytes_;
};

class HttpOwnedResponseBytes final {
public:
    [[nodiscard]] std::string_view bytes() const& noexcept {
        return *bytes_;
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

private:
    friend class HttpResponseBody;

    HttpOwnedResponseBytes(std::pmr::memory_resource* resource, std::string_view bytes)
        : bytes_(makeHttpPmrObject<std::pmr::string>(resource, bytes, resource)) {}

    HttpOwnedResponseBytes(std::pmr::memory_resource* resource, std::pmr::string&& bytes)
        : bytes_(makeHttpPmrObject<std::pmr::string>(resource, std::move(bytes), resource)) {}

    std::unique_ptr<std::pmr::string, HttpPmrObjectDeleter<std::pmr::string>> bytes_;
};

class HttpOwnedResponseFile final {
public:
    [[nodiscard]] const HttpNativePathChar* nativePathCStr() const& noexcept {
        return nativePath_->c_str();
    }
    [[nodiscard]] const HttpNativePathChar* nativePathCStr() const&& = delete;

    [[nodiscard]] constexpr std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

    [[nodiscard]] constexpr HttpResponseFileIdentity identity() const noexcept {
        return identity_;
    }

private:
    friend class HttpResponseBody;

    HttpOwnedResponseFile(std::pmr::memory_resource* resource, const std::filesystem::path& file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        HttpResponseFileIdentity identity)
        : nativePath_(makeHttpPmrObject<HttpNativePathString>(
              resource, std::basic_string_view<HttpNativePathChar>{}, resource)),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {
        assignHttpNativePath(*nativePath_, file);
    }

    std::unique_ptr<HttpNativePathString, HttpPmrObjectDeleter<HttpNativePathString>> nativePath_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    HttpResponseFileIdentity identity_;
};

class HttpBorrowedResponseFile final {
public:
    [[nodiscard]] constexpr const HttpNativePathChar* nativePathCStr() const noexcept {
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

    [[nodiscard]] constexpr HttpResponseFileIdentity identity() const noexcept {
        return identity_;
    }

private:
    friend class HttpResponseBody;

    constexpr HttpBorrowedResponseFile(const HttpNativePathChar* nativePath, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, HttpResponseFileIdentity identity) noexcept
        : nativePath_(nativePath),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    const HttpNativePathChar* nativePath_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    HttpResponseFileIdentity identity_;
};

class http_multipart_response_body final {
public:
    http_multipart_response_body(std::pmr::memory_resource* resource,
        const std::filesystem::path& path, std::uint64_t size,
        HttpResponseFileIdentity identity, http_multipart_byte_range_plan plan)
        : nativePath_(makeHttpPmrObject<HttpNativePathString>(
              resource, std::basic_string_view<HttpNativePathChar>{}, resource)),
          plan_(plan.resource() == resource ? std::move(plan) : plan.clone(resource)),
          size_(size),
          identity_(identity) {
        assignHttpNativePath(*nativePath_, path);
        if (plan_.segments().empty()) {
            throw std::invalid_argument("multipart response plan must contain segments");
        }
        bool containsFile = false;
        for (const auto& segment : plan_.segments()) {
            if (segment.kind == http_multipart_byte_range_plan::segment_kind::file) {
                if (segment.file_length == 0 || segment.file_offset > size_ ||
                    segment.file_length > size_ - segment.file_offset) {
                    throw std::invalid_argument("multipart response file segment is out of range");
                }
                containsFile = true;
            }
        }
        if (!containsFile) {
            throw std::invalid_argument("multipart response plan must contain a file segment");
        }
    }

    [[nodiscard]] std::size_t segment_count() const noexcept {
        return plan_.segments().size();
    }
    [[nodiscard]] http_response_body_segment_view segment(std::size_t index) const {
        const auto& item = plan_.segments()[index];
        if (item.kind == http_multipart_byte_range_plan::segment_kind::metadata) {
            return {.bytes_ = plan_.metadata().substr(item.metadata_offset, item.metadata_length)};
        }
        return {.file_ = HttpResponseFileView(nativePath_->c_str(), size_, item.file_offset,
                    item.file_length, identity_)};
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return plan_.content_length();
    }
    [[nodiscard]] HttpResponseFileView file() const noexcept {
        return HttpResponseFileView(nativePath_->c_str(), size_, 0, size_, identity_);
    }
    [[nodiscard]] std::uint64_t file_size() const noexcept {
        return size_;
    }
    [[nodiscard]] HttpResponseFileIdentity identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const http_multipart_byte_range_plan& plan() const& noexcept {
        return plan_;
    }

private:
    friend class HttpResponseBody;
    std::unique_ptr<HttpNativePathString, HttpPmrObjectDeleter<HttpNativePathString>> nativePath_;
    http_multipart_byte_range_plan plan_;
    std::uint64_t size_{};
    HttpResponseFileIdentity identity_{HttpResponseFileIdentity::unchecked()};
};

// Owns exactly one legal buffered response-body representation. The common
// bytes()/file()/size() observations are derived from the active alternative.
class HttpResponseBody final {
public:
    HttpResponseBody() noexcept
        : value_(HttpEmptyResponseBody{}) {}

    HttpResponseBody(const HttpResponseBody&) = delete;
    HttpResponseBody& operator=(const HttpResponseBody&) = delete;
    HttpResponseBody(HttpResponseBody&&) = default;
    HttpResponseBody& operator=(HttpResponseBody&&) = delete;

    [[nodiscard]] const HttpEmptyResponseBody* empty() const& noexcept {
        return std::get_if<HttpEmptyResponseBody>(&value_);
    }
    [[nodiscard]] const HttpEmptyResponseBody* empty() const&& = delete;

    [[nodiscard]] const HttpBorrowedResponseBytes* borrowedBytes() const& noexcept {
        return std::get_if<HttpBorrowedResponseBytes>(&value_);
    }
    [[nodiscard]] const HttpBorrowedResponseBytes* borrowedBytes() const&& = delete;

    [[nodiscard]] const HttpStaticResponseBytes* staticBytes() const& noexcept {
        return std::get_if<HttpStaticResponseBytes>(&value_);
    }
    [[nodiscard]] const HttpStaticResponseBytes* staticBytes() const&& = delete;

    [[nodiscard]] const HttpOwnedResponseBytes* ownedBytes() const& noexcept {
        return std::get_if<HttpOwnedResponseBytes>(&value_);
    }
    [[nodiscard]] const HttpOwnedResponseBytes* ownedBytes() const&& = delete;

    [[nodiscard]] const HttpOwnedResponseFile* ownedFile() const& noexcept {
        return std::get_if<HttpOwnedResponseFile>(&value_);
    }
    [[nodiscard]] const HttpOwnedResponseFile* ownedFile() const&& = delete;

    [[nodiscard]] const HttpBorrowedResponseFile* borrowedFile() const& noexcept {
        return std::get_if<HttpBorrowedResponseFile>(&value_);
    }
    [[nodiscard]] const HttpBorrowedResponseFile* borrowedFile() const&& = delete;
    [[nodiscard]] const http_multipart_response_body* multipart_body() const& noexcept {
        return std::get_if<http_multipart_response_body>(&value_);
    }
    [[nodiscard]] const http_multipart_response_body* multipart_body() const&& = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
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
    [[nodiscard]] std::string_view bytes() const&& = delete;

    [[nodiscard]] std::optional<HttpResponseFileView> file() const& noexcept {
        if (const auto* body = ownedFile()) {
            return HttpResponseFileView(body->nativePathCStr(), body->size(), body->offset(),
                body->length(), body->identity());
        }
        if (const auto* body = borrowedFile()) {
            return HttpResponseFileView(body->nativePathCStr(), body->size(), body->offset(),
                body->length(), body->identity());
        }
        if (const auto* body = multipart_body()) {
            return body->file();
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<HttpResponseFileView> file() const&& = delete;

    [[nodiscard]] std::uint64_t size() const noexcept {
        if (const auto* body = ownedFile()) {
            return body->length();
        }
        if (const auto* body = borrowedFile()) {
            return body->length();
        }
        if (const auto* body = multipart_body()) {
            return body->content_length();
        }
        return static_cast<std::uint64_t>(bytes().size());
    }

private:
    friend class ::ruvia::HttpResponse;

    using Value =
        std::variant<HttpEmptyResponseBody, HttpBorrowedResponseBytes, HttpStaticResponseBytes,
            HttpOwnedResponseBytes, HttpOwnedResponseFile, HttpBorrowedResponseFile,
            http_multipart_response_body>;

    void setEmpty() noexcept {
        value_.emplace<HttpEmptyResponseBody>(HttpEmptyResponseBody{});
    }

    void setCopy(std::pmr::memory_resource* resource, std::string_view bytes) {
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
        value_.emplace<HttpBorrowedResponseBytes>(HttpBorrowedResponseBytes(bytes));
    }

    void setStatic(std::string_view bytes) noexcept {
        if (bytes.empty()) {
            setEmpty();
            return;
        }
        value_.emplace<HttpStaticResponseBytes>(HttpStaticResponseBytes(bytes));
    }

    void setOwned(std::pmr::memory_resource* resource, std::pmr::string&& bytes) {
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

    void setOwnedFile(std::pmr::memory_resource* resource, const std::filesystem::path& file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        HttpResponseFileIdentity identity = HttpResponseFileIdentity::unchecked()) {
        HttpOwnedResponseFile body(resource, file, size, offset, length, identity);
        value_.emplace<HttpOwnedResponseFile>(std::move(body));
    }

    void set_multipart(std::pmr::memory_resource* resource, const std::filesystem::path& file,
        std::uint64_t size, HttpResponseFileIdentity identity,
        http_multipart_byte_range_plan plan) {
        http_multipart_response_body replacement(
            resource, file, size, identity, std::move(plan));
        static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);
        value_.emplace<http_multipart_response_body>(std::move(replacement));
    }

    void setBorrowedFile(const HttpNativePathChar* file, std::uint64_t size, std::uint64_t offset,
        std::uint64_t length,
        HttpResponseFileIdentity identity = HttpResponseFileIdentity::unchecked()) noexcept {
        value_.emplace<HttpBorrowedResponseFile>(
            HttpBorrowedResponseFile(file, size, offset, length, identity));
    }

    Value value_;
};

static_assert(std::is_nothrow_move_constructible_v<HttpOwnedResponseBytes>);
static_assert(std::is_nothrow_move_constructible_v<HttpOwnedResponseFile>);
static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);

}  // namespace detail
}  // namespace ruvia
