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

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/native_path.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_response_file.h"

namespace ruvia {

class http_response;

namespace detail {

class http_response_body;

class http_empty_response_body final {
private:
    friend class http_response_body;
    constexpr http_empty_response_body() noexcept = default;
};

class http_borrowed_response_bytes final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class http_response_body;

    explicit constexpr http_borrowed_response_bytes(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http_static_response_bytes final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class http_response_body;

    explicit constexpr http_static_response_bytes(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http_owned_response_bytes final {
public:
    [[nodiscard]] std::string_view bytes() const& noexcept {
        return *bytes_;
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

private:
    friend class http_response_body;

    http_owned_response_bytes(std::pmr::memory_resource* resource, std::string_view bytes_value)
        : bytes_(make_http_pmr_object<std::pmr::string>(resource, bytes_value, resource)) {}

    http_owned_response_bytes(std::pmr::memory_resource* resource, std::pmr::string&& bytes_value)
        : bytes_(make_http_pmr_object<std::pmr::string>(resource, std::move(bytes_value), resource)) {}

    std::unique_ptr<std::pmr::string, http_pmr_object_deleter<std::pmr::string>> bytes_;
};

class http_owned_response_file final {
public:
    [[nodiscard]] const http_native_path_char_type* native_path_c_str() const& noexcept {
        return native_path_->c_str();
    }
    [[nodiscard]] const http_native_path_char_type* native_path_c_str() const&& = delete;

    [[nodiscard]] constexpr std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

    [[nodiscard]] constexpr http_response_file_identity identity() const noexcept {
        return identity_;
    }

private:
    friend class http_response_body;

    http_owned_response_file(std::pmr::memory_resource* resource, const std::filesystem::path& file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity)
        : native_path_(make_http_pmr_object<http_native_path_string_type>(
              resource, std::basic_string_view<http_native_path_char_type>{}, resource)),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {
        assign_http_native_path(*native_path_, file);
    }

    std::unique_ptr<http_native_path_string_type, http_pmr_object_deleter<http_native_path_string_type>> native_path_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
};

class http_borrowed_response_file final {
public:
    [[nodiscard]] constexpr const http_native_path_char_type* native_path_c_str() const noexcept {
        return native_path_;
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

    [[nodiscard]] constexpr http_response_file_identity identity() const noexcept {
        return identity_;
    }

private:
    friend class http_response_body;

    constexpr http_borrowed_response_file(const http_native_path_char_type* native_path, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, http_response_file_identity identity) noexcept
        : native_path_(native_path),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    const http_native_path_char_type* native_path_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
};

class http_multipart_response_body final {
public:
    http_multipart_response_body(std::pmr::memory_resource* resource,
        const std::filesystem::path& path, std::uint64_t size,
        http_response_file_identity identity, http_multipart_byte_range_plan plan)
        : native_path_(make_http_pmr_object<http_native_path_string_type>(
              resource, std::basic_string_view<http_native_path_char_type>{}, resource)),
          plan_(plan.resource() == resource ? std::move(plan) : plan.clone(resource)),
          size_(size),
          identity_(identity) {
        assign_http_native_path(*native_path_, path);
        if (plan_.segments().empty()) {
            throw std::invalid_argument("multipart response plan must contain segments");
        }
        bool contains_file = false;
        for (const auto& segment : plan_.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                if (segment.file_length_ == 0 || segment.file_offset_ > size_ ||
                    segment.file_length_ > size_ - segment.file_offset_) {
                    throw std::invalid_argument("multipart response file segment is out of range");
                }
                contains_file = true;
            }
        }
        if (!contains_file) {
            throw std::invalid_argument("multipart response plan must contain a file segment");
        }
    }

    [[nodiscard]] std::size_t segment_count() const noexcept {
        return plan_.segments().size();
    }
    [[nodiscard]] http_response_body_segment_view segment(std::size_t index) const {
        const auto& item = plan_.segments()[index];
        if (item.kind_ == http_multipart_byte_range_plan::segment_kind::metadata) {
            return {.bytes_ = plan_.metadata().substr(item.metadata_offset_, item.metadata_length_)};
        }
        return {.file_ = http_response_file_view(native_path_->c_str(), size_, item.file_offset_,
                    item.file_length_, identity_)};
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return plan_.content_length();
    }
    [[nodiscard]] http_response_file_view file() const noexcept {
        return http_response_file_view(native_path_->c_str(), size_, 0, size_, identity_);
    }
    [[nodiscard]] std::uint64_t file_size() const noexcept {
        return size_;
    }
    [[nodiscard]] http_response_file_identity identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const http_multipart_byte_range_plan& plan() const& noexcept {
        return plan_;
    }

private:
    friend class http_response_body;
    std::unique_ptr<http_native_path_string_type, http_pmr_object_deleter<http_native_path_string_type>> native_path_;
    http_multipart_byte_range_plan plan_;
    std::uint64_t size_{};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
};

// Owns exactly one legal buffered response-body representation. The common
// bytes()/file()/size() observations are derived from the active alternative.
class http_response_body final {
public:
    http_response_body() noexcept
        : value_(http_empty_response_body{}) {}

    http_response_body(const http_response_body&) = delete;
    http_response_body& operator=(const http_response_body&) = delete;
    http_response_body(http_response_body&&) = default;
    http_response_body& operator=(http_response_body&&) = delete;

    [[nodiscard]] const http_empty_response_body* empty() const& noexcept {
        return std::get_if<http_empty_response_body>(&value_);
    }
    [[nodiscard]] const http_empty_response_body* empty() const&& = delete;

    [[nodiscard]] const http_borrowed_response_bytes* borrowed_bytes() const& noexcept {
        return std::get_if<http_borrowed_response_bytes>(&value_);
    }
    [[nodiscard]] const http_borrowed_response_bytes* borrowed_bytes() const&& = delete;

    [[nodiscard]] const http_static_response_bytes* static_bytes() const& noexcept {
        return std::get_if<http_static_response_bytes>(&value_);
    }
    [[nodiscard]] const http_static_response_bytes* static_bytes() const&& = delete;

    [[nodiscard]] const http_owned_response_bytes* owned_bytes() const& noexcept {
        return std::get_if<http_owned_response_bytes>(&value_);
    }
    [[nodiscard]] const http_owned_response_bytes* owned_bytes() const&& = delete;

    [[nodiscard]] const http_owned_response_file* owned_file() const& noexcept {
        return std::get_if<http_owned_response_file>(&value_);
    }
    [[nodiscard]] const http_owned_response_file* owned_file() const&& = delete;

    [[nodiscard]] const http_borrowed_response_file* borrowed_file() const& noexcept {
        return std::get_if<http_borrowed_response_file>(&value_);
    }
    [[nodiscard]] const http_borrowed_response_file* borrowed_file() const&& = delete;
    [[nodiscard]] const http_multipart_response_body* multipart_body() const& noexcept {
        return std::get_if<http_multipart_response_body>(&value_);
    }
    [[nodiscard]] const http_multipart_response_body* multipart_body() const&& = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
        if (const auto* body = borrowed_bytes()) {
            return body->bytes();
        }
        if (const auto* body = static_bytes()) {
            return body->bytes();
        }
        if (const auto* body = owned_bytes()) {
            return body->bytes();
        }
        return {};
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

    [[nodiscard]] std::optional<http_response_file_view> file() const& noexcept {
        if (const auto* body = owned_file()) {
            return http_response_file_view(body->native_path_c_str(), body->size(), body->offset(),
                body->length(), body->identity());
        }
        if (const auto* body = borrowed_file()) {
            return http_response_file_view(body->native_path_c_str(), body->size(), body->offset(),
                body->length(), body->identity());
        }
        if (const auto* body = multipart_body()) {
            return body->file();
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<http_response_file_view> file() const&& = delete;

    [[nodiscard]] std::uint64_t size() const noexcept {
        if (const auto* body = owned_file()) {
            return body->length();
        }
        if (const auto* body = borrowed_file()) {
            return body->length();
        }
        if (const auto* body = multipart_body()) {
            return body->content_length();
        }
        return static_cast<std::uint64_t>(bytes().size());
    }

private:
    friend class ::ruvia::http_response;

    using value_type =
        std::variant<http_empty_response_body, http_borrowed_response_bytes, http_static_response_bytes,
            http_owned_response_bytes, http_owned_response_file, http_borrowed_response_file,
            http_multipart_response_body>;

    void set_empty() noexcept {
        value_.emplace<http_empty_response_body>(http_empty_response_body{});
    }

    void set_copy(std::pmr::memory_resource* resource, std::string_view bytes_value) {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(resource, bytes_value);
        value_.emplace<http_owned_response_bytes>(std::move(body));
    }

    void set_borrowed(std::string_view bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        value_.emplace<http_borrowed_response_bytes>(http_borrowed_response_bytes(bytes_value));
    }

    void set_static(std::string_view bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        value_.emplace<http_static_response_bytes>(http_static_response_bytes(bytes_value));
    }

    void set_owned(std::pmr::memory_resource* resource, std::pmr::string&& bytes_value) {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(resource, std::move(bytes_value));
        value_.emplace<http_owned_response_bytes>(std::move(body));
    }

    void materialize(std::pmr::memory_resource* resource) {
        const auto* borrowed = borrowed_bytes();
        if (borrowed == nullptr) {
            return;
        }
        http_owned_response_bytes body(resource, borrowed->bytes());
        value_.emplace<http_owned_response_bytes>(std::move(body));
    }

    void set_owned_file(std::pmr::memory_resource* resource, const std::filesystem::path& file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity = http_response_file_identity::unchecked()) {
        http_owned_response_file body(resource, file, size, offset, length, identity);
        value_.emplace<http_owned_response_file>(std::move(body));
    }

    void set_multipart(std::pmr::memory_resource* resource, const std::filesystem::path& file,
        std::uint64_t size, http_response_file_identity identity,
        http_multipart_byte_range_plan plan) {
        http_multipart_response_body replacement(
            resource, file, size, identity, std::move(plan));
        static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);
        value_.emplace<http_multipart_response_body>(std::move(replacement));
    }

    void set_borrowed_file(const http_native_path_char_type* file, std::uint64_t size, std::uint64_t offset,
        std::uint64_t length,
        http_response_file_identity identity = http_response_file_identity::unchecked()) noexcept {
        value_.emplace<http_borrowed_response_file>(
            http_borrowed_response_file(file, size, offset, length, identity));
    }

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_owned_response_bytes>);
static_assert(std::is_nothrow_move_constructible_v<http_owned_response_file>);
static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);

}  // namespace detail
}  // namespace ruvia
