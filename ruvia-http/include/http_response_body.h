#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "http_message.h"
#include "http_range.h"
#include "http_request.h"
#include "http_status.h"

namespace ruvia {

// Platform-native path text (wchar_t on Windows, char elsewhere), as carried
// by response file bodies.
using http_native_path_char_type = std::filesystem::path::value_type;
using http_native_path_string_type = std::basic_string<http_native_path_char_type>;
using http_native_path_view_type = std::basic_string_view<http_native_path_char_type>;

[[nodiscard]] inline http_native_path_view_type http_native_path_view(const std::filesystem::path& path) noexcept {
    return http_native_path_view_type(path.native());
}
http_native_path_view_type http_native_path_view(const std::filesystem::path&&) = delete;

inline void http_assign_native_path(http_native_path_string_type& output, const std::filesystem::path& path) {
    const auto native = http_native_path_view(path);
    output.assign(native.data(), native.size());
}

[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_char_type* path) {
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
}

[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_string_type& path) {
    return http_make_path_from_native_path(path.c_str());
}

// Opaque runtime-supplied identity of the file whose metadata was used to
// frame a response. HTTP transports this token; the runtime validates it after
// opening the actual file handle.
class http_response_file_identity final {
public:
    [[nodiscard]] static constexpr http_response_file_identity unchecked() noexcept {
        return http_response_file_identity({}, false);
    }

    [[nodiscard]] static constexpr http_response_file_identity checked(std::array<std::uint64_t, 4> words) noexcept {
        return http_response_file_identity(words, true);
    }

    [[nodiscard]] constexpr bool requires_validation() const noexcept {
        return checked_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const& noexcept {
        return words_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const&& = delete;

    friend constexpr bool operator==(const http_response_file_identity&, const http_response_file_identity&) noexcept = default;

private:
    constexpr http_response_file_identity(std::array<std::uint64_t, 4> words, bool checked) noexcept :
        words_(words),
        checked_(checked) {}

    std::array<std::uint64_t, 4> words_{};
    bool checked_{false};
};

// An open native file a runtime already framed a response from, carried by
// the response body so the transport sends from that same descriptor instead of
// resolving the path again. HTTP performs no I/O on it: it stores the native
// value and invokes the runtime-supplied release function exactly once when
// the owning body is destroyed or replaced.
class http_response_file_descriptor final {
public:
    using native_type = std::uintptr_t;
    using release_type = void (*)(native_type) noexcept;

    constexpr http_response_file_descriptor() noexcept = default;
    constexpr http_response_file_descriptor(native_type native_value, release_type release) noexcept :
        native_(native_value),
        release_(release) {}

    http_response_file_descriptor(const http_response_file_descriptor&) = delete;
    http_response_file_descriptor& operator=(const http_response_file_descriptor&) = delete;

    constexpr http_response_file_descriptor(http_response_file_descriptor&& other) noexcept :
        native_(std::exchange(other.native_, native_type{})),
        release_(std::exchange(other.release_, nullptr)) {}

    http_response_file_descriptor& operator=(http_response_file_descriptor&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        http_response_file_descriptor retired(std::move(other));
        std::swap(native_, retired.native_);
        std::swap(release_, retired.release_);
        // Commit both owners and the return reference before release can reenter.
        return *this;
    }

    ~http_response_file_descriptor() {
        reset();
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return release_ != nullptr;
    }

    // Borrowed while this descriptor is alive; meaningful only when engaged.
    [[nodiscard]] constexpr native_type native() const noexcept {
        return native_;
    }

private:
    void reset() noexcept {
        if (release_ != nullptr) {
            std::exchange(release_, nullptr)(std::exchange(native_, native_type{}));
        }
    }

    native_type native_{};
    release_type release_{nullptr};
};

// Read-only descriptor for a response file. Its path and any opened file are
// borrowed and remain valid for the lifetime of the response body from which
// it was obtained.
class http_response_file_view final {
public:
    constexpr http_response_file_view(const http_native_path_char_type* native_path, std::uint64_t size, std::uint64_t offset, std::uint64_t length, http_response_file_identity identity, const http_response_file_descriptor* opened = nullptr) noexcept :
        native_path_(native_path),
        opened_(opened != nullptr && *opened ? opened : nullptr),
        size_(size),
        offset_(offset),
        length_(length),
        identity_(identity) {}

    [[nodiscard]] constexpr const http_native_path_char_type* native_path_c_str() const noexcept {
        return native_path_;
    }
    [[nodiscard]] std::filesystem::path to_path() const {
        return std::filesystem::path(native_path_);
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
    // The descriptor the response was framed from, or null when the transport
    // must open native_path_c_str() and validate identity() itself.
    [[nodiscard]] constexpr const http_response_file_descriptor* opened_file() const noexcept {
        return opened_;
    }

private:
    const http_native_path_char_type* native_path_;
    const http_response_file_descriptor* opened_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
};

class http_response;

// A borrowed view of one response-body segment. Any bytes and file path remain
// valid only while the owning http_response is alive and unmodified.
struct http_response_body_segment_view final {
    std::string_view bytes_{};
    std::optional<http_response_file_view> file_{};
};

enum class http_response_content_semantics : std::uint8_t {
    informational,
    protocol_switch,
    connect_tunnel,
    without_content,
    with_content,
};

class http_response_body_plan final {
public:
    [[nodiscard]] constexpr http_known_method request_method() const noexcept {
        return request_method_;
    }
    [[nodiscard]] constexpr http_status_code response_status() const noexcept {
        return response_status_;
    }
    [[nodiscard]] constexpr http_response_content_semantics content_semantics() const noexcept {
        return semantics_;
    }
    [[nodiscard]] constexpr bool status_allows_body() const noexcept {
        return status_allows_body_;
    }
    [[nodiscard]] constexpr bool body_suppressed() const noexcept {
        return body_suppressed_;
    }
    [[nodiscard]] constexpr bool auto_content_length_allowed() const noexcept {
        return auto_content_length_allowed_;
    }
    [[nodiscard]] constexpr bool explicit_content_length_allowed() const noexcept {
        return explicit_content_length_allowed_;
    }
    [[nodiscard]] constexpr bool transfer_encoding_allowed() const noexcept {
        return transfer_encoding_allowed_;
    }
    [[nodiscard]] std::uint64_t buffered_representation_length(const http_response& response) const noexcept;

private:
    friend http_response_body_plan plan_http_response_body(http_known_method, http_status_code) noexcept;
    constexpr http_response_body_plan(http_known_method method, http_status_code status, http_response_content_semantics semantics, bool status_allows_body, bool body_suppressed, bool auto_content_length_allowed, bool explicit_content_length_allowed, bool transfer_encoding_allowed) noexcept :
        request_method_(method),
        response_status_(status),
        semantics_(semantics),
        status_allows_body_(status_allows_body),
        body_suppressed_(body_suppressed),
        auto_content_length_allowed_(auto_content_length_allowed),
        explicit_content_length_allowed_(explicit_content_length_allowed),
        transfer_encoding_allowed_(transfer_encoding_allowed) {}

    http_known_method request_method_;
    http_status_code response_status_;
    http_response_content_semantics semantics_;
    bool status_allows_body_;
    bool body_suppressed_;
    bool auto_content_length_allowed_;
    bool explicit_content_length_allowed_;
    bool transfer_encoding_allowed_;
};

[[nodiscard]] http_response_body_plan plan_http_response_body(http_known_method request_method, http_status_code response_status) noexcept;

namespace detail {

// A buffered response sends payload bytes only when its method/status permit a
// body and the representation is non-empty.
[[nodiscard]] constexpr bool http_buffered_response_sends_body(const http_response_body_plan& body_plan, std::uint64_t content_length) noexcept {
    return !body_plan.body_suppressed() && content_length != 0;
}

}  // namespace detail

class http_buffered_response_write_plan final {
public:
    [[nodiscard]] http_known_method request_method() const noexcept {
        return body_plan_.request_method();
    }
    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan_.response_status();
    }
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    [[nodiscard]] bool body_suppressed() const noexcept {
        return body_plan_.body_suppressed();
    }
    [[nodiscard]] bool status_allows_body() const noexcept {
        return body_plan_.status_allows_body();
    }
    [[nodiscard]] bool auto_content_length_allowed() const noexcept {
        return body_plan_.auto_content_length_allowed();
    }
    [[nodiscard]] bool explicit_content_length_allowed() const noexcept {
        return body_plan_.explicit_content_length_allowed();
    }
    [[nodiscard]] bool transfer_encoding_allowed() const noexcept {
        return body_plan_.transfer_encoding_allowed();
    }
    [[nodiscard]] bool send_body() const noexcept {
        return detail::http_buffered_response_sends_body(body_plan_, content_length_);
    }
    [[nodiscard]] bool matches_response(const http_response& response) const noexcept;

private:
    friend http_buffered_response_write_plan plan_buffered_http_response_write(http_known_method, const http_response&) noexcept;
    http_buffered_response_write_plan(http_response_body_plan body_plan, std::uint64_t content_length) noexcept :
        body_plan_(body_plan),
        content_length_(content_length) {}

    http_response_body_plan body_plan_;
    std::uint64_t content_length_;
};

[[nodiscard]] http_buffered_response_write_plan plan_buffered_http_response_write(http_known_method request_method, const http_response& response) noexcept;

namespace detail {

class http_response_body;

class http_empty_response_body final {
public:
    [[nodiscard]] constexpr bool head_metadata_only() const noexcept {
        return head_metadata_only_;
    }

private:
    friend class http_response_body;
    explicit constexpr http_empty_response_body(bool head_metadata_only = false) noexcept :
        head_metadata_only_(head_metadata_only) {}

    bool head_metadata_only_{false};
};

class http_static_response_bytes final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class http_response_body;

    explicit constexpr http_static_response_bytes(std::string_view bytes_value) noexcept :
        bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http_owned_response_bytes final {
public:
    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

    [[nodiscard]] constexpr bool file_representation() const noexcept {
        return file_representation_;
    }

private:
    friend class http_response_body;

    explicit http_owned_response_bytes(std::string_view bytes_value) :
        bytes_(bytes_value) {}

    http_owned_response_bytes(std::string&& bytes_value, bool file_representation) noexcept :
        bytes_(std::move(bytes_value)),
        file_representation_(file_representation) {}

    std::string bytes_;
    bool file_representation_{false};
};

class http_owned_response_file final {
public:
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

    [[nodiscard]] http_response_file_view view() const& noexcept {
        return http_response_file_view(native_path_.c_str(), size_, offset_, length_, identity_, &opened_);
    }
    [[nodiscard]] http_response_file_view view() const&& = delete;

private:
    friend class http_response_body;

    http_owned_response_file(http_native_path_string_type&& native_path, std::uint64_t size, std::uint64_t offset, std::uint64_t length, http_response_file_identity identity, http_response_file_descriptor opened) noexcept :
        native_path_(std::move(native_path)),
        size_(size),
        offset_(offset),
        length_(length),
        identity_(identity),
        opened_(std::move(opened)) {}

    http_owned_response_file(http_native_path_view_type file, std::uint64_t size, std::uint64_t offset, std::uint64_t length, http_response_file_identity identity, http_response_file_descriptor opened) :
        http_owned_response_file(http_native_path_string_type(file), size, offset, length, identity, std::move(opened)) {}
    http_native_path_string_type native_path_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
    http_response_file_descriptor opened_;
};

class http_multipart_response_body final {
public:
    http_multipart_response_body(http_native_path_view_type path, std::uint64_t size, http_response_file_identity identity, http_multipart_byte_range_plan plan, http_response_file_descriptor opened) :
        native_path_(path),
        plan_(std::move(plan)),
        size_(size),
        identity_(identity),
        opened_(std::move(opened)) {
        if (plan_.segments().empty()) {
            throw std::invalid_argument("multipart response plan must contain segments");
        }
        bool contains_file = false;
        for (const auto& segment : plan_.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                if (segment.file_length_ == 0 || segment.file_offset_ > size_ || segment.file_length_ > size_ - segment.file_offset_) {
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
        return {.file_ = http_response_file_view(native_path_.c_str(), size_, item.file_offset_, item.file_length_, identity_, &opened_)};
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return plan_.content_length();
    }
    [[nodiscard]] http_response_file_view file() const noexcept {
        return http_response_file_view(native_path_.c_str(), size_, 0, size_, identity_, &opened_);
    }

private:
    friend class http_response_body;

    http_multipart_response_body(http_native_path_string_type&& native_path, http_multipart_byte_range_plan&& plan, std::uint64_t size, http_response_file_identity identity, http_response_file_descriptor opened) noexcept :
        native_path_(std::move(native_path)),
        plan_(std::move(plan)),
        size_(size),
        identity_(identity),
        opened_(std::move(opened)) {}

    http_native_path_string_type native_path_;
    http_multipart_byte_range_plan plan_;
    std::uint64_t size_{};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    http_response_file_descriptor opened_;
};

// Owns exactly one legal buffered response-body representation. The common
// bytes()/file()/size() observations are derived from the active alternative.
class http_response_body final {
public:
    http_response_body() noexcept :
        value_(http_empty_response_body{}) {}

    http_response_body(const http_response_body&) = delete;
    http_response_body& operator=(const http_response_body&) = delete;
    http_response_body(http_response_body&& other) noexcept :
        value_(std::move(other.value_)) {
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

    [[nodiscard]] bool file_representation() const noexcept {
        if (const auto* body = owned_bytes()) {
            return body->file_representation();
        }
        return owned_file() != nullptr || multipart_body() != nullptr;
    }

    [[nodiscard]] bool head_metadata_only() const noexcept {
        if (const auto* body = std::get_if<http_empty_response_body>(&value_)) {
            return body->head_metadata_only();
        }
        return false;
    }

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
    friend struct http_response_body_access;

    using value_type = std::variant<http_empty_response_body, http_static_response_bytes, http_owned_response_bytes, http_owned_response_file, http_multipart_response_body>;
    static_assert(std::is_nothrow_move_constructible_v<value_type>);

    template <typename body_type>
    void replace(body_type&& body) noexcept {
        // Publish the complete alternative before a resource callback can reenter.
        value_type retired(std::move(value_));
        value_.emplace<std::remove_cvref_t<body_type>>(std::forward<body_type>(body));
    }

    void set_empty() noexcept {
        replace(http_empty_response_body{});
    }

    void set_head_metadata_only() noexcept {
        replace(http_empty_response_body(true));
    }

    void set_copy(std::string_view bytes_value) {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(bytes_value);
        replace(std::move(body));
    }

    void set_static(std::string_view bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        replace(http_static_response_bytes(bytes_value));
    }

    void set_owned(std::string&& bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(std::move(bytes_value), false);
        replace(std::move(body));
    }

    // Kept even when empty: an empty file is still a file representation.
    void set_file_bytes(std::string&& bytes_value) noexcept {
        http_owned_response_bytes body(std::move(bytes_value), true);
        replace(std::move(body));
    }

    void set_owned_file(http_native_path_view_type file, std::uint64_t size, std::uint64_t offset, std::uint64_t length, http_response_file_identity identity, http_response_file_descriptor opened) {
        http_owned_response_file body(file, size, offset, length, identity, std::move(opened));
        replace(std::move(body));
    }

    void set_multipart(http_native_path_view_type file, std::uint64_t size, http_response_file_identity identity, http_multipart_byte_range_plan plan, http_response_file_descriptor opened) {
        http_multipart_response_body replacement(file, size, identity, std::move(plan), std::move(opened));
        replace(std::move(replacement));
    }

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_owned_response_bytes>);
static_assert(std::is_nothrow_move_constructible_v<http_owned_response_file>);
static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);

}  // namespace detail

}  // namespace ruvia
