#include "ruvia/http/http_response.h"

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"
#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_status.h"

#include "field/http_entity_tag.h"
#include "field/response_field_validation.h"
#include "response/http_response_descriptor_stage.h"
#include "response/http_response_header_access.h"
#include "response/response_header_utils.h"

namespace ruvia {

std::string_view http_response::body_bytes() const& noexcept {
    return detail::response_body(*this).bytes();
}

std::optional<http_response_file_view> http_response::file_body() const& noexcept {
    return detail::response_body(*this).file();
}

bool http_response::has_multipart_file_body() const noexcept {
    return detail::response_body(*this).multipart_body() != nullptr;
}

bool http_response::has_file_representation() const noexcept {
    return detail::response_body(*this).file_representation();
}

void http_response::head_metadata_body() noexcept {
    body_.set_head_metadata_only();
}

bool http_response::has_head_metadata_body() const noexcept {
    return body_.head_metadata_only();
}

std::size_t http_response::body_segment_count() const noexcept {
    const auto& body = detail::response_body(*this);
    if (const auto* multipart = body.multipart_body()) {
        return multipart->segment_count();
    }
    return body.size() == 0 ? 0 : 1;
}

http_response_body_segment_view http_response::body_segment(std::size_t index) const& {
    const auto& body = detail::response_body(*this);
    if (const auto* multipart = body.multipart_body()) {
        if (index >= multipart->segment_count()) {
            throw std::out_of_range("response body segment index is out of range");
        }
        return multipart->segment(index);
    }
    if (index != 0) {
        throw std::out_of_range("response body segment index is out of range");
    }
    if (const auto file = body.file()) {
        return {.file_ = file};
    }
    return {.bytes_ = body.bytes()};
}

std::uint64_t http_response_body_plan::buffered_representation_length(const http_response& response) const noexcept {
    if (!status_allows_body() || content_semantics() == http_response_content_semantics::connect_tunnel) {
        return 0;
    }
    return static_cast<std::uint64_t>(detail::response_body(response).size());
}

bool http_buffered_response_write_plan::matches_response(const http_response& response) const noexcept {
    return response.status() == body_plan_.response_status() &&
           content_length_ == body_plan_.buffered_representation_length(response);
}

http_buffered_response_write_plan plan_buffered_http_response_write(
    http_known_method request_method, const http_response& response) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response.status());
    return http_buffered_response_write_plan(body_plan, body_plan.buffered_representation_length(response));
}

http_response_body_plan plan_http_response_body(
    http_known_method request_method, http_status_code response_status) noexcept {
    const bool body_allowed = !response_status.is_informational() &&
                              response_status != http_status::no_content &&
                              response_status != http_status::reset_content &&
                              response_status != http_status::not_modified;
    const bool auto_content_length_allowed = body_allowed || response_status == http_status::reset_content;
    const bool explicit_content_length_allowed = body_allowed || response_status == http_status::not_modified;
    const auto semantics = detail::classify_http_response_content_semantics(request_method, response_status);
    const bool connect_tunnel = semantics == http_response_content_semantics::connect_tunnel;
    return http_response_body_plan(request_method, response_status, semantics, body_allowed,
        !body_allowed || semantics != http_response_content_semantics::with_content,
        !connect_tunnel && auto_content_length_allowed,
        !connect_tunnel && explicit_content_length_allowed,
        !connect_tunnel && body_allowed);
}

namespace {

inline constexpr std::size_t inline_weak_etag_capacity = 128;

}  // namespace
http_response::http_response(std::pmr::memory_resource* resource)
    : http_response(detail::http_resolved_pmr_resource_tag{},
          detail::http_pmr_resource_or_default(resource)) {}

http_response::http_response(detail::http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
    : headers_(detail::http_resolved_pmr_resource_tag{}, resource) {}

http_response::http_response(http_response&& other) noexcept
    : status_code_(other.status_code_),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)) {}

http_response& http_response::operator=(http_response&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    // A response is one resource domain. Member-wise assignment would retain the
    // target allocator in PMR alternatives while http_response_headers follows the
    // source resource, leaving one response split across unrelated request arenas.
    // Reconstructing transfers every owning alternative together and does not
    // allocate on the response hot path.
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
}

std::pmr::memory_resource* http_response::memory_resource() const noexcept {
    return headers_.resource_;
}

void http_response::commit_headers(http_response_headers&& staged) noexcept {
    headers_.commit(std::move(staged));
}

http_status_code http_response::status() const noexcept {
    return status_code_;
}

const http_response_headers& http_response::headers() const& noexcept {
    return headers_;
}

std::optional<std::string_view> http_response::header(std::string_view name) const& noexcept {
    return headers_.header(name);
}

void http_response::header(std::string_view key, std::string_view value) {
    headers_.header(key, value);
}

void http_response::header(std::string_view key, std::string_view value, http_response_header_options options) {
    headers_.header(key, value, options);
}

void http_response::header_stable_view(std::string_view key, std::string_view value) {
    headers_.header_stable_view(key, value);
}

void http_response::content_type(http_content_type type) {
    headers_.content_type(type);
}

void http_response::allow_methods(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    headers_.allow_methods(method_mask, extension_methods);
}

void http_response::remove_header(std::string_view key) {
    headers_.remove_header(key);
}

void http_response::set_cookie(const set_cookie_plan& plan) {
    headers_.set_cookie(plan);
}

void http_response::reserve_headers(std::size_t count) {
    headers_.reserve(count);
}

void http_response::status(http_status_code status_code) {
    if (status_code == http_status::switching_protocols) {
        throw std::invalid_argument("Switching Protocols requires a dedicated protocol driver");
    }
    if (!detail::http_final_status_code_valid(status_code)) {
        throw std::invalid_argument("invalid final HTTP status code");
    }
    status_code_ = status_code;
}

void http_response::transfer_headers_from(
    const http_response& source_value, http_response_header_transfer mode) {
    headers_.transfer_from(source_value.headers_, mode);
}

void http_response::body(std::string_view value) {
    body_.set_copy(memory_resource(), value);
}

void http_response::owned_body(std::pmr::string&& value) {
    body_.set_owned(memory_resource(), std::move(value));
}

void http_response::static_body(std::string_view value) noexcept {
    body_.set_static(value);
}

void http_response::file_bytes_body(std::pmr::string&& value) {
    body_.set_file_bytes(memory_resource(), std::move(value));
}

void http_response::apply_content_encoding(std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, nullptr);
}

void http_response::replace_body_with_content_encoding(
    std::pmr::string&& value, std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, &value);
}

void http_response::apply_encoded_representation(
    std::string_view content_encoding, std::pmr::string* body) {
    detail::validate_response_header_storage_size(std::string_view("Content-Encoding").size(), content_encoding.size());
    // Coding-list parsing permits field-line OWS; publish normalized field
    // content so the same response can be emitted by HTTP/1 and HTTP/2.
    content_encoding = http_trim_ows(content_encoding);
    detail::response_field_state fields;
    if (detail::validate_response_field(detail::request_header_kind::content_encoding, content_encoding,
            detail::http_field_list_role::sender, fields) != detail::response_field_status::ok) {
        throw std::invalid_argument("invalid HTTP Content-Encoding header");
    }
    const auto current_etag = headers_.known_header_value(detail::response_header_etag);
    std::array<char, inline_weak_etag_capacity> inline_weak_etag;
    std::pmr::string heap_weak_etag(memory_resource());
    std::string_view weak_etag{};
    if (detail::http_is_strong_etag(current_etag)) {
        const std::size_t total_size = current_etag.size() + 2U;
        if (total_size <= inline_weak_etag.size()) {
            inline_weak_etag[0] = 'W';
            inline_weak_etag[1] = '/';
            std::memcpy(inline_weak_etag.data() + 2, current_etag.data(), current_etag.size());
            weak_etag = std::string_view(inline_weak_etag.data(), total_size);
        } else {
            heap_weak_etag.reserve(total_size);
            heap_weak_etag.append("W/");
            heap_weak_etag.append(current_etag.data(), current_etag.size());
            weak_etag = heap_weak_etag;
        }
    }

    http_response_headers::descriptor_stage update(headers_);
    update.stage("Content-Encoding", content_encoding, detail::response_header_content_encoding);
    if (!weak_etag.empty()) {
        update.stage("ETag", weak_etag, detail::response_header_etag);
    }
    std::array<char, http_max_unsigned_decimal_size> length_digits;
    if (body != nullptr) {
        update.stage("Content-Length", http_format_unsigned(length_digits, body->size()),
            detail::response_header_content_length);
    }
    update.reserve();
    // Every descriptor and table slot exists before the body changes. set_owned
    // has a strong failure guarantee; everything after it is no-throw publication.
    if (body != nullptr) {
        body_.set_owned(memory_resource(), std::move(*body));
    } else {
        if (body_.size() == 0 && !body_.file_representation()) {
            head_metadata_body();
        }
        (void)headers_.remove_header_validated("Content-Length", detail::response_header_content_length);
    }
    update.publish();
}

void http_response::file_body(http_native_path_view_type file, std::uint64_t size,
    std::uint64_t offset, std::uint64_t length, http_response_file_identity identity,
    http_response_file_handle opened) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.set_owned_file(memory_resource(), file, size, offset, length, identity, std::move(opened));
}

void http_response::multipart_file_body(http_native_path_view_type file, std::uint64_t size,
    http_response_file_identity identity, http_multipart_byte_range_plan&& plan,
    http_response_file_handle opened) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    body_.set_multipart(memory_resource(), file, size, identity, std::move(plan), std::move(opened));
}

void http_response::content_range(
    std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    headers_.set_content_range(offset, length, size);
}

void http_response::content_range_unsatisfied(std::uint64_t size) {
    headers_.set_content_range_unsatisfied(size);
}

void http_response::add_vary_token(std::string_view token) {
    detail::add_vary_token(*this, token);
}

}  // namespace ruvia
