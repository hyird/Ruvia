#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/attributes.h"
#include "ruvia/http/detail/response/http_response_body.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_native_path.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_response_file.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

enum class http_connect_udp_error : std::uint8_t;
class http_response;
class http_response_headers;

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
    constexpr http_response_body_plan(http_known_method method, http_status_code status, http_response_content_semantics semantics,
        bool status_allows_body, bool body_suppressed, bool auto_content_length_allowed,
        bool explicit_content_length_allowed, bool transfer_encoding_allowed) noexcept
        : request_method_(method),
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

[[nodiscard]] http_response_body_plan plan_http_response_body(
    http_known_method request_method, http_status_code response_status) noexcept;

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
        return !body_suppressed() && content_length_ != 0;
    }
    [[nodiscard]] bool matches_response(const http_response& response) const noexcept;

private:
    friend http_buffered_response_write_plan plan_buffered_http_response_write(
        http_known_method, const http_response&) noexcept;
    http_buffered_response_write_plan(http_response_body_plan body_plan, std::uint64_t content_length) noexcept
        : body_plan_(body_plan),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    std::uint64_t content_length_;
};

[[nodiscard]] http_buffered_response_write_plan plan_buffered_http_response_write(
    http_known_method request_method, const http_response& response) noexcept;

class http_response_headers;
class set_cookie_plan;
struct http_response_header;

enum class http_response_header_mode : std::uint8_t {
    replace,
    append,
};

enum class http_response_header_transfer : std::uint8_t {
    merge,
    assign,
    apply,
};

// Content-Type values whose field lines are constants of this library. Setting
// one shares a static descriptor: no allocation and no grammar re-validation.
enum class http_content_type : std::uint8_t {
    text_plain_utf8,   // text/plain; charset=UTF-8
    text_html_utf8,    // text/html; charset=UTF-8
    application_json,  // application/json
};

struct http_response_header_options {
    http_response_header_mode mode_{http_response_header_mode::replace};
};

namespace detail {

struct http_response_body_access;
struct http_response_header_access;
struct http_response_header_state_access;

}  // namespace detail

struct http_response_header {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return bytes_ == nullptr ? std::string_view{} : std::string_view(bytes_, name_size_);
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return bytes_ == nullptr ? std::string_view{}
                                 : std::string_view(bytes_ + name_size_, value_size_);
    }

private:
    friend class http_response;
    friend class http_response_headers;
    friend struct detail::http_response_header_access;

    http_response_header() noexcept = default;

    const char* bytes_{nullptr};
    std::uint32_t name_size_{0};
    std::uint32_t value_size_{0};
    std::uint32_t known_bit_{0};
    bool owned_{false};
    bool append_{false};
};

static_assert(std::is_trivially_copyable_v<http_response_header>);
static_assert(sizeof(http_response_header) <= 24);

// Owns a response header block: descriptor storage, the known-header index, every
// field mutation rule, and descriptor staging. A clone is a header-only
// transaction: edit it around external operations, then publish it with
// http_response::commit_headers.
class http_response_headers final {
public:
    using value_type = http_response_header;
    using const_iterator = const http_response_header*;

    // Moving transfers every descriptor and the index. The source keeps its
    // resource and remains usable, independent, and empty.
    http_response_headers(http_response_headers&& other) noexcept;
    http_response_headers(const http_response_headers&) = delete;
    http_response_headers& operator=(const http_response_headers&) = delete;
    http_response_headers& operator=(http_response_headers&&) = delete;
    ~http_response_headers();

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return data() + size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return spilled_ ? heap_.size() : size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // Inputs may borrow current header storage. Refresh views after mutation.
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    // Validated like header(). Name and value may be retained as borrowed views
    // without copying. Keep their bytes valid and unchanged while this block or
    // any clone retains them.
    void header_stable_view(std::string_view key, std::string_view value);
    // Replaces Content-Type with a library constant.
    void content_type(http_content_type type);
    void remove_header(std::string_view key);
    // Replace cookies with the same name, Domain/Path scope, and Partitioned
    // state; partitioned and unpartitioned cookies may coexist.
    void set_cookie(const set_cookie_plan& plan);
    // Owned bytes are copied so either block may change or be destroyed;
    // stable views are shared. Reserves room for additional_headers more fields.
    [[nodiscard]] http_response_headers clone(std::size_t additional_headers = 0) const;

private:
    friend class http_response;
    friend struct detail::http_response_header_state_access;

    using iterator = http_response_header*;

    static constexpr std::size_t known_header_count = 22;
    static constexpr std::size_t inline_capacity = 8;
    struct inline_storage_type {
        alignas(http_response_header) std::byte bytes_[sizeof(http_response_header)];
    };

    // Stages up to three owned descriptors without touching the table and
    // publishes them later without allocation or failure.
    class descriptor_stage;

    http_response_headers(detail::http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource);

    // Mutable iteration for the owning block and its friends. Named apart from
    // the public const begin()/end() so a non-const block still iterates
    // through the public overloads.
    [[nodiscard]] iterator mutable_begin() & noexcept {
        return data();
    }

    [[nodiscard]] iterator mutable_end() & noexcept {
        return data() + size();
    }

    // Header-only transaction publication. `staged` must be a clone of a block
    // sharing this resource; it is left empty and independent.
    void commit(http_response_headers&& staged) noexcept;
    void transfer_from(const http_response_headers& source, http_response_header_transfer mode);
    // Appends the source's fields in place when none can interact with an
    // existing field. Returns false, leaving this block unchanged, otherwise.
    [[nodiscard]] bool try_append_disjoint(const http_response_headers& source, http_response_header_transfer mode);
    void set_static_header(http_response_header header);

    // Field rules and the known-header index.
    void record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept;
    void rebuild_known_header_index() noexcept;
    [[nodiscard]] std::string_view known_header_value(std::uint32_t bit) const noexcept;
    [[nodiscard]] http_response_header* find_header_for_update(
        std::string_view key, std::uint32_t known_bit) noexcept;
    [[nodiscard]] const http_response_header* find_header_for_read(
        std::string_view key, std::uint32_t known_bit) const noexcept;
    http_response_header& prepare_header_value_storage(
        std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    void set_header_validated(std::string_view key, std::string_view value, std::uint32_t known_bit);
    void append_header_validated(
        std::string_view key, std::string_view value, std::uint32_t known_bit);
    http_response_header& append_header_uninitialized_value(
        std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] http_response_header& collapse_response_headers(
        http_response_header& retained, std::uint32_t known_bit) noexcept;
    bool remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept;
    void upsert_set_cookie_header_validated(std::string_view value);
    [[nodiscard]] http_response_header* find_set_cookie_header(std::string_view wire_prefix,
        std::string_view cookie_name, bool has_path, std::string_view path,
        std::string_view domain, bool partitioned) noexcept;
    void erase_later_set_cookie_headers(http_response_header& retained,
        std::string_view cookie_name, bool has_path, std::string_view path,
        std::string_view domain, bool partitioned) noexcept;
    void allow_methods(std::uint32_t method_mask, std::span<const std::string_view> extension_methods);
    void set_content_range(std::uint64_t offset, std::uint64_t length, std::uint64_t size);
    void set_content_range_unsatisfied(std::uint64_t size);

    // Descriptor storage.
    [[nodiscard]] http_response_header* inline_data() noexcept;
    [[nodiscard]] const http_response_header* inline_data() const noexcept;
    [[nodiscard]] http_response_header* data() noexcept;
    [[nodiscard]] const http_response_header* data() const noexcept;
    [[nodiscard]] http_response_header make_owned_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit);
    [[nodiscard]] http_response_header make_uninitialized_header(
        std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] static std::optional<http_response_header> make_static_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept;
    [[nodiscard]] bool try_assign_owned_in_place(http_response_header& header, std::string_view name,
        std::string_view value, std::uint32_t known_bit) noexcept;
    http_response_header& append_header(http_response_header header);
    http_response_header& add_stable_view(
        std::string_view name, std::string_view value, std::uint32_t known_bit);
    http_response_header& add_uninitialized_value(
        std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    http_response_header& add(
        std::string_view name, std::string_view value, std::uint32_t known_bit);
    void assign(http_response_header& header, std::string_view name, std::string_view value,
        std::uint32_t known_bit);
    http_response_header& assign_uninitialized_value(http_response_header& header, std::string_view name,
        std::size_t value_size, std::uint32_t known_bit);
    void assign_stable_view(http_response_header& header, std::string_view name, std::string_view value,
        std::uint32_t known_bit);
    void release_header(http_response_header& header) noexcept;
    // Drops every header from new_end onward; the dropped headers must
    // already be released.
    void truncate(iterator new_end) noexcept;
    void reserve(std::size_t count);
    http_response_header& append_prepared_header(http_response_header header) noexcept;
    void clear() noexcept;
    void spill(std::size_t min_capacity);
    void move_from(http_response_headers&& other) noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<http_response_header> heap_;
    std::array<inline_storage_type, inline_capacity> inline_;
    std::size_t size_{0};
    bool spilled_{false};
    std::uint32_t known_header_bits_{0};
    std::array<std::int16_t, known_header_count> known_header_indexes_{};
};

// Composes one header block with one body. Header rules belong to
// http_response_headers; this type only publishes body and headers together.
class http_response final {
public:
    struct options_type final {
        std::pmr::memory_resource* resource_{nullptr};
    };

    http_response();
    explicit http_response(options_type options);

    http_response(const http_response&) = delete;
    http_response& operator=(const http_response&) = delete;
    // Moving transfers headers and body. The source keeps its resource and
    // remains reusable with empty headers and body.
    http_response(http_response&& other) noexcept;
    http_response& operator=(http_response&& other) noexcept;

    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] const http_response_headers& headers() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response_headers& headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // A generic http_response is always final (200..599). Interim 1xx progress
    // messages use http_interim_response_head; 101 uses a dedicated protocol driver.
    void status(http_status_code status_code);
    // Header mutations follow http_response_headers.
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    void header_stable_view(std::string_view key, std::string_view value);
    void content_type(http_content_type type);
    // Format Allow from known-method bits followed by extension method values.
    // Extension methods must be nonempty HTTP tokens and may borrow current headers.
    // The resulting field is owned by this response; refresh views after mutation.
    void allow_methods(
        std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void remove_header(std::string_view key);
    void set_cookie(const set_cookie_plan& plan);
    // Copy the source's application headers with RFC-aware append and cookie semantics.
    // Assignment preserves the destination Content-Type.
    void transfer_headers_from(const http_response& source, http_response_header_transfer mode);
    // Publish a header-only transaction staged from headers().clone() without
    // allocation. The staged block is left empty and independent.
    void commit_headers(http_response_headers&& staged) noexcept;
    // Borrowed byte representation; file and empty bodies return an empty view.
    [[nodiscard]] std::string_view body_bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view body_bytes() const&& = delete;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const& noexcept;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const&& = delete;
    [[nodiscard]] bool has_multipart_file_body() const noexcept;
    // True for file_body(), multipart_file_body() and file_bytes_body() bodies:
    // the bytes are a file's representation, which a sender transmits as is.
    [[nodiscard]] bool has_file_representation() const noexcept;
    [[nodiscard]] std::size_t body_segment_count() const noexcept;
    [[nodiscard]] http_response_body_segment_view body_segment(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    http_response_body_segment_view body_segment(std::size_t index) const&& = delete;
    void body(std::string_view value);
    void owned_body(std::pmr::string&& value);
    void static_body(std::string_view value) noexcept;
    // Own bytes read from a file: all of it, a byte range, or a multipart
    // byte-range payload of it. Unlike owned_body(), the body remains a file
    // representation, even when empty.
    void file_bytes_body(std::pmr::string&& value);
    // Own the native path in this response's PMR storage and preserve the
    // runtime's opaque identity token. `opened`, when engaged, is the handle the
    // runtime framed the response from; the body owns it and releases it on
    // destruction or replacement. Path and range validation precede body
    // replacement; a rejected call releases `opened`.
    void file_body(http_native_path_view_type file, std::uint64_t size, std::uint64_t offset,
        std::uint64_t length, http_response_file_identity identity, http_response_file_handle opened = {});
    void multipart_file_body(http_native_path_view_type file, std::uint64_t size,
        http_response_file_identity identity, http_multipart_byte_range_plan&& plan,
        http_response_file_handle opened = {});
    void content_range(std::uint64_t offset, std::uint64_t length, std::uint64_t size);
    void content_range_unsatisfied(std::uint64_t size);
    void add_vary_token(std::string_view token);
    void reserve_headers(std::size_t count);
    [[nodiscard]] std::pmr::memory_resource* memory_resource() const noexcept;
    // Apply a streaming coding, or atomically replace a buffered representation
    // and its coding-dependent metadata. The coding must be a nonempty HTTP coding list.
    // A failed replacement leaves this
    // response unchanged.
    void apply_content_encoding(std::string_view content_encoding);
    void replace_body_with_content_encoding(
        std::pmr::string&& value, std::string_view content_encoding);

private:
    friend std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response, http_protocol_version);
    friend struct detail::http_response_body_access;

    void apply_encoded_representation(std::string_view content_encoding, std::pmr::string* body);
    http_response(detail::http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource);

    http_status_code status_code_{http_status::ok};
    http_response_headers headers_;
    detail::http_response_body body_;
};

}  // namespace ruvia
