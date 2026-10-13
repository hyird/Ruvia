#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_native_path.h"
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
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

    [[nodiscard]] http_response_file_view view() const& noexcept {
        return http_response_file_view(native_path_->c_str(), size_, offset_, length_, identity_, &opened_);
    }
    [[nodiscard]] http_response_file_view view() const&& = delete;

private:
    friend class http_response_body;

    http_owned_response_file(std::pmr::memory_resource* resource, http_native_path_view_type file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_handle opened)
        : native_path_(make_http_pmr_object<http_native_path_string_type>(resource, file, resource)),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity),
          opened_(std::move(opened)) {}

    std::unique_ptr<http_native_path_string_type, http_pmr_object_deleter<http_native_path_string_type>> native_path_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
    http_response_file_handle opened_;
};

class http_multipart_response_body final {
public:
    http_multipart_response_body(std::pmr::memory_resource* resource, http_native_path_view_type path,
        std::uint64_t size, http_response_file_identity identity, http_multipart_byte_range_plan plan,
        http_response_file_handle opened)
        : native_path_(make_http_pmr_object<http_native_path_string_type>(resource, path, resource)),
          plan_(plan.resource() == resource ? std::move(plan) : plan.clone(resource)),
          size_(size),
          identity_(identity),
          opened_(std::move(opened)) {
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
                    item.file_length_, identity_, &opened_)};
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return plan_.content_length();
    }
    [[nodiscard]] http_response_file_view file() const noexcept {
        return http_response_file_view(native_path_->c_str(), size_, 0, size_, identity_, &opened_);
    }

private:
    friend class http_response_body;
    std::unique_ptr<http_native_path_string_type, http_pmr_object_deleter<http_native_path_string_type>> native_path_;
    http_multipart_byte_range_plan plan_;
    std::uint64_t size_{};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    http_response_file_handle opened_;
};

// Owns exactly one legal buffered response-body representation. The common
// bytes()/file()/size() observations are derived from the active alternative.
class http_response_body final {
public:
    http_response_body() noexcept
        : value_(http_empty_response_body{}) {}

    http_response_body(const http_response_body&) = delete;
    http_response_body& operator=(const http_response_body&) = delete;
    http_response_body(http_response_body&& other) noexcept
        : value_(std::move(other.value_)) {
        other.set_empty();
    }
    http_response_body& operator=(http_response_body&&) = delete;

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

    [[nodiscard]] const http_multipart_response_body* multipart_body() const& noexcept {
        return std::get_if<http_multipart_response_body>(&value_);
    }
    [[nodiscard]] const http_multipart_response_body* multipart_body() const&& = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
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
            return body->view();
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
        if (const auto* body = multipart_body()) {
            return body->content_length();
        }
        return static_cast<std::uint64_t>(bytes().size());
    }

private:
    friend class ::ruvia::http_response;

    using value_type = std::variant<http_empty_response_body, http_static_response_bytes,
        http_owned_response_bytes, http_owned_response_file, http_multipart_response_body>;
    static_assert(std::is_nothrow_move_constructible_v<value_type>);

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

    void set_owned_file(std::pmr::memory_resource* resource, http_native_path_view_type file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_handle opened) {
        http_owned_response_file body(resource, file, size, offset, length, identity, std::move(opened));
        value_.emplace<http_owned_response_file>(std::move(body));
    }

    void set_multipart(std::pmr::memory_resource* resource, http_native_path_view_type file,
        std::uint64_t size, http_response_file_identity identity,
        http_multipart_byte_range_plan plan, http_response_file_handle opened) {
        http_multipart_response_body replacement(
            resource, file, size, identity, std::move(plan), std::move(opened));
        static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);
        value_.emplace<http_multipart_response_body>(std::move(replacement));
    }

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_owned_response_bytes>);
static_assert(std::is_nothrow_move_constructible_v<http_owned_response_file>);
static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);

}  // namespace detail
}  // namespace ruvia
