#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "borrowed_text.h"
#include "http_message.h"
#include "http_request.h"
#include "http_response_body.h"
#include "http_response_headers.h"
#include "http_status.h"

namespace ruvia {

enum class http_connect_udp_error : std::uint8_t;
class http1_response_head_plan;

// Composes one header block with one body. Header rules belong to
// http_response_headers; this type only publishes body and headers together.
class http_response final {
public:
    http_response() noexcept;

    http_response(const http_response&) = delete;
    http_response& operator=(const http_response&) = delete;
    // Moving transfers headers and body. The source remains reusable with
    // empty headers and body.
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
    void allow_methods(std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void remove_header(std::string_view key);
    void set_cookie(const set_cookie_plan& plan);
    // Copy the source's application headers with RFC-aware append and cookie semantics.
    // Assignment preserves the destination Content-Type.
    void transfer_headers_from(const http_response& source, http_response_header_transfer mode);
    // Publish a header-only transaction staged from headers().clone() without
    // allocation. The staged block is left empty and independent.
    void commit_headers(http_response_headers&& staged) noexcept;
    // Borrowed byte representation; file and empty bodies return an empty view.
    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_bytes();
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::string_view body_bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view body_bytes() const&& = delete;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const&& = delete;
    [[nodiscard]] bool has_multipart_file_body() const noexcept;
    // True for file_body(), multipart_file_body() and file_bytes_body() bodies:
    // the bytes are a file's representation, which a sender transmits as is.
    [[nodiscard]] bool has_file_representation() const noexcept;
    // 将正文替换为仅描述 HEAD 所选表示的空正文，保留首部并释放原正文资源。
    // 序列化 HEAD 时保留显式 Content-Length；未提供长度时不推断为零。
    // 标记随移动转移；重新设置正文会清除它，不影响 GET 等方法的正文规则。
    void head_metadata_body() noexcept;
    [[nodiscard]] bool has_head_metadata_body() const noexcept;
    // File segments retain their path and identity even when their length is
    // zero. Empty byte bodies have no segments; multipart bodies retain their
    // planned sequence of file ranges and boundary bytes.
    [[nodiscard]] std::size_t body_segment_count() const noexcept;
    [[nodiscard]] http_response_body_segment_view body_segment(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    http_response_body_segment_view body_segment(std::size_t index) const&& = delete;
    void body(std::string_view value);
    void owned_body(std::string&& value) noexcept;
    void static_body(std::string_view value) noexcept;
    // Own bytes read from a file: all of it, a byte range, or a multipart
    // byte-range payload of it. Unlike owned_body(), the body remains a file
    // representation, even when empty.
    void file_bytes_body(std::string&& value) noexcept;
    // Own the native path and preserve the
    // runtime's opaque identity token. `opened`, when engaged, is the descriptor the
    // runtime framed the response from; the body owns it and releases it on
    // destruction or replacement. Path and range validation precede body
    // replacement; a rejected call releases `opened`.
    void file_body(http_native_path_view_type file, http_content_length size, http_content_length offset, http_content_length length, http_response_file_identity identity, http_response_file_descriptor opened = {});
    void multipart_file_body(http_native_path_view_type file, http_content_length size, http_response_file_identity identity, http_multipart_byte_range_plan&& plan, http_response_file_descriptor opened = {});
    void content_range(http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve_headers(std::size_t count);
    // Apply a streaming coding, or atomically replace a buffered representation
    // and its coding-dependent metadata. The coding must be a nonempty HTTP coding list;
    // outer SP/HTAB are removed before storing its field value.
    // Both set Content-Encoding, weaken a strong ETag, drop an ETag that is not a
    // quoted entity-tag, and drop Accept-Ranges; the replacement sets
    // Content-Length, the streaming form removes it. A failed replacement leaves
    // this response unchanged.
    void apply_content_encoding(std::string_view content_encoding);
    void replace_body_with_content_encoding(std::string&& value, std::string_view content_encoding);

private:
    friend std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response, http_protocol_version);
    friend http1_response_head_plan prepare_http1_upgrade_response_head(http_response&, const http_request&);
    friend struct detail::http_response_body_access;

    void apply_encoded_representation(std::string_view content_encoding, std::string* body);

    http_status_code status_code_{http_status::ok};
    http_response_headers headers_;
    detail::http_response_body body_;
};

}  // namespace ruvia
