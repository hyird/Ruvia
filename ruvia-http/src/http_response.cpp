#include "ruvia/http/http_response.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/multipart_parser.h"
#include "ruvia/http/url_encoding.h"

#include "http_connection_fields.h"
#include "http_websocket_permessage_deflate.h"
#include "http_zlib_stream.h"

namespace ruvia {

// Owns only newly staged descriptors, never copies the existing header block.
// Staging and reservation may throw and leave the block unchanged; publication
// transfers every descriptor without allocation; unwinding releases the rest.
class http_response_headers::descriptor_stage final {
public:
    explicit descriptor_stage(http_response_headers& owner_value) noexcept
        : owner_(owner_value) {}
    descriptor_stage(const descriptor_stage&) = delete;
    descriptor_stage& operator=(const descriptor_stage&) = delete;

    ~descriptor_stage() noexcept {
        for (std::size_t slot = 0; slot < count_; ++slot) {
            owner_.release_header(prepared_[slot]);
        }
    }

    // Each staged field must be distinct.
    void stage(std::string_view name, std::string_view value, std::uint32_t known_bit) {
        const auto builtin = make_static_header(name, value, known_bit);
        prepared_[count_] = builtin ? *builtin : owner_.make_owned_header(name, value, known_bit);
        ++count_;
    }

    void stage_weak_etag(std::string_view strong_etag) {
        auto prepared = owner_.make_uninitialized_header(
            "ETag", strong_etag.size() + 2U, detail::response_header_etag);
        auto* const out = detail::http_response_header_access::value_begin(prepared);
        out[0] = 'W';
        out[1] = '/';
        std::memcpy(out + 2, strong_etag.data(), strong_etag.size());
        prepared_[count_++] = prepared;
    }

    // Reserve a table slot for every staged field the block lacks, so publish
    // cannot allocate. Removing fields before publish keeps the reservation valid.
    void reserve() {
        std::size_t missing_headers = 0;
        for (std::size_t slot = 0; slot < count_; ++slot) {
            const auto& prepared = prepared_[slot];
            if (owner_.find_header_for_read(prepared.name(), detail::http_response_header_access::known_bit(prepared)) ==
                nullptr) {
                ++missing_headers;
            }
        }
        owner_.reserve(owner_.size() + missing_headers);
    }

    // Replace each field in place, collapsing earlier appended lines, or append it.
    void publish() noexcept {
        for (std::size_t slot = 0; slot < count_; ++slot) {
            // reserve() made room for every appended field, so this cannot throw.
            owner_.publish_header(prepared_[slot]);
        }
        count_ = 0;
    }

private:
    static constexpr std::size_t capacity = 3;

    http_response_headers& owner_;
    std::array<http_response_header, capacity> prepared_{};
    std::size_t count_{0};
};

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
    return body.file().has_value() || body.size() != 0 ? 1 : 0;
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

http_response::http_response() noexcept = default;

http_response::http_response(http_response&& other) noexcept
    : status_code_(other.status_code_),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)) {}

http_response& http_response::operator=(http_response&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    http_response retired(std::move(*this));
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
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
    if (!status_code.is_final()) {
        throw std::invalid_argument("invalid final HTTP status code");
    }
    status_code_ = status_code;
}

void http_response::transfer_headers_from(
    const http_response& source_value, http_response_header_transfer mode) {
    headers_.transfer_from(source_value.headers_, mode);
}

void http_response::body(std::string_view value) {
    body_.set_copy(value);
}

void http_response::owned_body(std::string&& value) noexcept {
    body_.set_owned(std::move(value));
}

void http_response::static_body(std::string_view value) noexcept {
    body_.set_static(value);
}

void http_response::file_bytes_body(std::string&& value) noexcept {
    body_.set_file_bytes(std::move(value));
}

void http_response::apply_content_encoding(std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, nullptr);
}

void http_response::replace_body_with_content_encoding(
    std::string&& value, std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, &value);
}

void http_response::apply_encoded_representation(
    std::string_view content_encoding, std::string* body) {
    detail::validate_response_header_storage_size(std::string_view("Content-Encoding").size(), content_encoding.size());
    // Coding-list parsing permits field-line OWS; publish normalized field
    // content so the same response can be emitted by HTTP/1 and HTTP/2.
    content_encoding = http_trim_ows(content_encoding);
    detail::response_field_state fields;
    if (detail::validate_response_field(detail::http_field_kind::content_encoding, content_encoding,
            fields) != detail::response_field_status::ok) {
        throw std::invalid_argument("invalid HTTP Content-Encoding header");
    }
    // RFC 9110 §8.8.3: the coded representation is not byte-identical to the
    // identity one, so a strong validator becomes weak. A value that is not an
    // entity-tag cannot be weakened unambiguously and is dropped (nginx gzip
    // filter, Pingora weaken_or_clear_etag).
    const auto current_etag = detail::http_response_header_state_access::known_header_value(headers_, detail::response_header_etag);
    const auto etag_opaque = detail::http_trim_weak_etag_prefix(current_etag);
    const bool etag_well_formed = etag_opaque.size() >= 2 && etag_opaque.front() == '"' && etag_opaque.back() == '"';
    http_response_headers::descriptor_stage update(headers_);
    update.stage("Content-Encoding", content_encoding, detail::response_header_content_encoding);
    if (etag_well_formed && !detail::http_is_weak_etag(current_etag)) {
        update.stage_weak_etag(current_etag);
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
        body_.set_owned(std::move(*body));
    } else {
        if (body_.size() == 0 && !body_.file_representation()) {
            head_metadata_body();
        }
        (void)headers_.remove_header_validated("Content-Length", detail::response_header_content_length);
    }
    if (!etag_well_formed) {
        (void)headers_.remove_header_validated("ETag", detail::response_header_etag);
    }
    // Byte ranges of the identity representation do not address the coded
    // one, and the framework never serves ranges of a body it encoded
    // (RFC 9110 §14.3).
    (void)headers_.remove_header_validated("Accept-Ranges", detail::response_header_accept_ranges);
    update.publish();
}

void http_response::file_body(http_native_path_view_type file, http_content_length size,
    http_content_length offset, http_content_length length, http_response_file_identity identity,
    http_response_file_descriptor opened) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset.value() > size.value() || length.value() > size.value() - offset.value()) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.set_owned_file(file, size.value(), offset.value(), length.value(),
        identity, std::move(opened));
}

void http_response::multipart_file_body(http_native_path_view_type file, http_content_length size,
    http_response_file_identity identity, http_multipart_byte_range_plan&& plan,
    http_response_file_descriptor opened) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    body_.set_multipart(
        file, size.value(), identity, std::move(plan), std::move(opened));
}

void http_response::content_range(
    http_content_length offset, http_content_length length, http_content_length size) {
    headers_.content_range(offset, length, size);
}

void http_response::content_range_unsatisfied(http_content_length size) {
    headers_.content_range_unsatisfied(size);
}

void http_response::add_vary_token(std::string_view token) {
    headers_.add_vary_token(token);
}

}  // namespace ruvia

namespace ruvia {
namespace {

constexpr std::string_view crlf = "\r\n";

struct http1_interim_header_facts final {
    http_field_section_budget section_;
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
};

[[nodiscard]] const char* common_validation_message(detail::http_interim_response_header_validation_status status) noexcept {
    switch (status) {
        case detail::http_interim_response_header_validation_status::content_length_forbidden:
            return "Content-Length is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::transfer_encoding_forbidden:
            return "Transfer-Encoding is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::trailer_forbidden:
            return "Trailer is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::repeated_singleton:
            return "repeated singleton interim response header";
        case detail::http_interim_response_header_validation_status::invalid_header:
        case detail::http_interim_response_header_validation_status::ok:
            break;
    }
    return "invalid HTTP/1 interim response header";
}

void analyze_http1_fields(const http_interim_response_head& response, http1_interim_header_facts& facts) {
    detail::http_interim_response_sender_validator validator;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        const auto value = header.value();
        auto kind = detail::http_field_kind::other;
        const auto status = validator.validate(name, value, &kind);
        if (status != detail::http_interim_response_header_validation_status::ok) {
            throw http1_response_head_error(common_validation_message(status));
        }
        switch (detail::parse_http_response_control_field(
            kind, value, facts.connection_options_, facts.upgrade_protocols_)) {
            case detail::http_response_control_field_status::invalid_connection:
                throw http1_response_head_error("invalid HTTP/1 interim response Connection field");
            case detail::http_response_control_field_status::invalid_upgrade:
                throw http1_response_head_error("invalid HTTP/1 interim response Upgrade field");
            case detail::http_response_control_field_status::te_forbidden:
                throw http1_response_head_error("TE is not a response field");
            case detail::http_response_control_field_status::ok:
                break;
        }

        if (!facts.section_.add_field(name, value)) {
            throw http1_response_head_error("HTTP/1 interim response head is too large");
        }
    }
    if (!detail::http_response_upgrade_pair_valid(facts.connection_options_, facts.upgrade_protocols_)) {
        throw http1_response_head_error("HTTP/1 interim Upgrade requires Connection: Upgrade");
    }
}

[[nodiscard]] char* write_interim_response_head(
    char* output, const http_interim_response_head& response, std::string_view reason_phrase) noexcept {
    detail::http_raw_wire_sink sink(output);
    detail::emit_http1_status_line(sink, response.status(), reason_phrase);
    for (const auto& header : response.headers()) {
        detail::emit_http1_field_line(sink, header.name(), header.value());
    }
    sink.append(crlf);
    return sink.position();
}

}  // namespace

http_interim_response_head::http_interim_response_head(
    http_status_code status_code, borrowed_http_headers headers)
    : status_code_(status_code),
      headers_(headers.view()) {
    if (!detail::http_interim_status_code_valid(status_code)) {
        throw std::invalid_argument(status_code == http_status::switching_protocols
                                        ? "Switching Protocols requires a dedicated protocol driver"
                                        : "invalid interim HTTP status code");
    }
}

std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer) {
    const auto reason_phrase = http_reason_phrase(response.status());
    http1_interim_header_facts facts;
    analyze_http1_fields(response, facts);
    if (!facts.section_.add(detail::http1_status_line_size(reason_phrase)) || !facts.section_.add(crlf.size())) {
        throw http1_response_head_error("HTTP/1 interim response head is too large");
    }
    const auto head_bytes = facts.section_.bytes();
    if (head_buffer.size() < head_bytes) {
        return http1_interim_response_buffer_too_small(head_bytes);
    }

    const auto output = head_buffer.first(head_bytes);
    detail::write_http_wire(output,
        detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), response.headers()),
        [&](char* destination) noexcept {
            return write_interim_response_head(destination, response, reason_phrase);
        });

    return prepared_http1_interim_response(std::string_view(head_buffer.data(), head_bytes),
        facts.connection_options_.close()
            ? http1_interim_connection_disposition::close_after_interim_response
            : http1_interim_connection_disposition::unchanged);
}

}  // namespace ruvia

namespace ruvia {

const http_representation_response_plan::full_type* http_representation_response_plan::full() const& noexcept {
    return std::get_if<full_type>(&value_);
}

const http_representation_response_plan::not_modified_type*
http_representation_response_plan::not_modified() const& noexcept {
    return std::get_if<not_modified_type>(&value_);
}

const http_representation_response_plan::precondition_failed_type*
http_representation_response_plan::precondition_failed() const& noexcept {
    return std::get_if<precondition_failed_type>(&value_);
}

const http_representation_response_plan::range_unsatisfiable_type*
http_representation_response_plan::range_unsatisfiable() const& noexcept {
    return std::get_if<range_unsatisfiable_type>(&value_);
}

const http_representation_response_plan::partial_type* http_representation_response_plan::partial() const& noexcept {
    return std::get_if<partial_type>(&value_);
}

const http_representation_response_plan::multipart_type*
http_representation_response_plan::multipart_ranges() const& noexcept {
    return std::get_if<multipart_type>(&value_);
}

http_status_code http_representation_response_plan::status() const noexcept {
    if (const auto* outcome = full()) {
        return outcome->status_;
    }
    if (not_modified() != nullptr) {
        return http_status::not_modified;
    }
    if (precondition_failed() != nullptr) {
        return http_status::precondition_failed;
    }
    if (range_unsatisfiable() != nullptr) {
        return http_status::range_not_satisfiable;
    }
    return http_status::partial_content;
}

http_representation_response_plan plan_http_representation_response(
    const http_request& request, http_selected_representation_metadata representation,
    http_representation_response_options options) {
    switch (options.range_policy_) {
        case http_range_request_policy::ignore:
        case http_range_request_policy::honor_byte_ranges:
            break;
        default:
            throw std::invalid_argument("unknown HTTP range request policy");
    }

    const auto make_full = [&]() noexcept {
        return http_representation_response_plan(http_representation_response_plan::full_type{options.normal_status_});
    };
    auto method = get_http_conditional_method_plan(request.known_method());
    if (request.known_method() == http_known_method::unknown &&
        !request.method().empty() && request.method() != "TRACE") {
        method.evaluates_preconditions_ = true;
    }
    if (!method.evaluates_preconditions_ ||
        (!options.normal_status_.is_successful() && options.normal_status_ != http_status::precondition_failed)) {
        return make_full();
    }

    const auto headers = get_http_conditional_headers(request);
    const auto etags = get_http_etag_preconditions(request, representation.etag_);
    if (etags.if_match_.present_) {
        if (!etags.if_match_.matches()) {
            return http_representation_response_plan(
                http_representation_response_plan::precondition_failed_type{});
        }
    } else if (representation.last_modified_ && !headers.if_unmodified_since_.empty() &&
               !http_date_unmodified(headers.if_unmodified_since_, *representation.last_modified_)) {
        return http_representation_response_plan(http_representation_response_plan::precondition_failed_type{});
    }

    if (etags.if_none_match_.present_) {
        if (etags.if_none_match_.matches()) {
            if (method.uses_not_modified_response_) {
                return http_representation_response_plan(http_representation_response_plan::not_modified_type{});
            }
            return http_representation_response_plan(http_representation_response_plan::precondition_failed_type{});
        }
    } else if (method.evaluates_if_modified_since_ && representation.last_modified_ &&
               !headers.if_modified_since_.empty() &&
               http_date_not_modified(headers.if_modified_since_, *representation.last_modified_)) {
        return http_representation_response_plan(http_representation_response_plan::not_modified_type{});
    }

    if (options.normal_status_ != http_status::ok ||
        !method.evaluates_range_ ||
        options.range_policy_ != http_range_request_policy::honor_byte_ranges || headers.range_.empty()) {
        return make_full();
    }

    if (headers.has_if_range_ && !http_if_range_allows(headers.if_range_, representation.etag_,
                                     representation.last_modified_, representation.strong_date_validator_)) {
        return make_full();
    }

    const auto ranges = resolve_http_byte_range_set(headers.range_, representation.length_);
    if (ranges.ignored()) {
        return make_full();
    }
    if (ranges.unsatisfiable()) {
        return http_representation_response_plan(
            http_representation_response_plan::range_unsatisfiable_type{});
    }
    if (ranges.size() == 1) {
        return http_representation_response_plan(ranges[0], headers.has_if_range_);
    }
    if (representation.content_coded_) {
        return make_full();
    }
    return http_representation_response_plan(ranges, headers.has_if_range_);
}

}  // namespace ruvia

namespace ruvia {

[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(
    http_response response, http_response_stream_commit_plan commit_plan) {
    if (response.status() != commit_plan.response_status()) {
        throw std::invalid_argument("response stream commit plan status does not match response");
    }
    if (!commit_plan.trailer_intent_allowed()) {
        throw std::invalid_argument("response stream status does not permit trailers");
    }
    const auto framing = commit_plan.framing();
    if (!detail::response_transfer_encoding_replaceable(response)) {
        if (framing == http_response_stream_framing::http2_frames ||
            framing == http_response_stream_framing::http3_frames) {
            throw std::invalid_argument("HTTP/2 or HTTP/3 response cannot carry a Transfer-Encoding other than chunked");
        }
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }
    const auto body_plan = commit_plan.body_plan();
    const bool writer_owns_http1_chunked = framing == http_response_stream_framing::http1_chunked &&
                                           body_plan.transfer_encoding_allowed() &&
                                           !body_plan.body_suppressed();

    // Keep the prepared response metadata consistent with the wire plan. The
    // framework's chunk writer is the only Transfer-Encoding producer, so the
    // handler's chunked hint survives only as that writer's own field; HTTP/2
    // and HTTP/3 frames and an HTTP/1.0 close-delimited body carry none. An HTTP/1.0
    // close-delimited body cannot retain Content-Length either. HEAD/304 may
    // retain Content-Length metadata because their body is suppressed.
    if (writer_owns_http1_chunked) {
        response.remove_header("Content-Length");
    } else {
        response.remove_header("Transfer-Encoding");
    }
    if (framing == http_response_stream_framing::http1_close_delimited && !body_plan.body_suppressed()) {
        response.remove_header("Content-Length");
    }
    // Without a chunked body the trailer section is discarded, so the head must
    // not announce trailer fields that will never follow (RFC 9110 6.6.2).
    if (!commit_plan.trailers_available()) {
        response.remove_header("Trailer");
    }

    if (writer_owns_http1_chunked) {
        if (!detail::response_has_known_header(response, detail::response_header_transfer_encoding)) {
            response.reserve_headers(response.headers().size() + 1);
        }
        response.header_stable_view("Transfer-Encoding", "chunked");
    }

    return http_response_stream_head(std::move(response), commit_plan);
}

}  // namespace ruvia

namespace ruvia {

http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
    const http_byte_range_set& ranges, http_content_length representation_length_value,
    std::string_view media_type, std::string_view boundary) {
    const auto representation_length = representation_length_value.value();
    if (ranges.ignored() || ranges.unsatisfiable() || ranges.size() < 2 ||
        ranges.size() > http_byte_range_set::capacity || representation_length == 0 ||
        !is_valid_http_content_type_field_value(media_type) ||
        !multipart_boundary::try_create(boundary).has_value()) {
        throw std::invalid_argument("invalid multipart byte-range plan input");
    }

    const auto representation_digits = http_unsigned_decimal_size(representation_length);
    std::size_t metadata_size = 2U + boundary.size() + 4U;
    std::uint64_t total_content_length = metadata_size;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        if (range.length_ == 0 || range.offset_ > representation_length ||
            range.length_ > representation_length - range.offset_) {
            throw std::invalid_argument("multipart range is outside the representation");
        }
        const auto part_metadata_size =
            2U + boundary.size() + 16U + media_type.size() + 23U +
            http_unsigned_decimal_size(range.offset_) + 1U +
            http_unsigned_decimal_size(range.offset_ + range.length_ - 1) + 1U +
            representation_digits + 6U;
        if (part_metadata_size > (std::numeric_limits<std::size_t>::max)() - metadata_size) {
            throw std::length_error("multipart response metadata size overflows size_t");
        }
        metadata_size += part_metadata_size;
        if (total_content_length > (std::numeric_limits<std::uint64_t>::max)() - part_metadata_size ||
            total_content_length + part_metadata_size >
                (std::numeric_limits<std::uint64_t>::max)() - range.length_) {
            throw std::length_error("multipart response content length overflows uint64_t");
        }
        total_content_length += part_metadata_size + range.length_;
    }

    http_multipart_byte_range_plan plan;
    auto& storage = *plan.state_;
    auto& metadata = storage.metadata_;
    static constexpr std::string_view content_type_prefix = "multipart/byteranges; boundary=";
    const bool boundary_is_token = http_token_prefix_size(boundary) == boundary.size();
    storage.content_type_.reserve(
        content_type_prefix.size() + boundary.size() + (boundary_is_token ? 0U : 2U));
    storage.content_type_.append(content_type_prefix);
    if (!boundary_is_token) {
        storage.content_type_.push_back('"');
    }
    storage.content_type_.append(boundary);
    if (!boundary_is_token) {
        storage.content_type_.push_back('"');
    }
    metadata.reserve(metadata_size);
    const auto record_metadata = [&storage](std::size_t offset) noexcept {
        const auto length = storage.metadata_.size() - offset;
        if (storage.segment_count_ != 0) {
            auto& previous = storage.segments_[storage.segment_count_ - 1];
            if (previous.kind_ == http_multipart_byte_range_plan::segment_kind::metadata &&
                previous.metadata_offset_ + previous.metadata_length_ == offset) {
                previous.metadata_length_ = storage.metadata_.size() - previous.metadata_offset_;
                return;
            }
        }
        storage.segments_[storage.segment_count_++] = {
            http_multipart_byte_range_plan::segment_kind::metadata, offset, length, 0, 0};
    };

    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        const auto part_offset = metadata.size();
        metadata.append("--");
        metadata.append(boundary);
        metadata.append("\r\nContent-Type: ");
        metadata.append(media_type);
        metadata.append("\r\nContent-Range: bytes ");
        http_append_unsigned(metadata, range.offset_);
        metadata.push_back('-');
        http_append_unsigned(metadata, range.offset_ + range.length_ - 1);
        metadata.push_back('/');
        http_append_unsigned(metadata, representation_length);
        metadata.append("\r\n\r\n");
        record_metadata(part_offset);

        storage.segments_[storage.segment_count_++] = {
            http_multipart_byte_range_plan::segment_kind::file, 0, 0, range.offset_, range.length_};
        const auto separator_offset = metadata.size();
        metadata.append("\r\n");
        record_metadata(separator_offset);
    }

    const auto closing_offset = metadata.size();
    metadata.append("--");
    metadata.append(boundary);
    metadata.append("--\r\n");
    record_metadata(closing_offset);
    storage.content_length_ = total_content_length;
    return plan;
}

http_multipart_byte_range_plan http_multipart_byte_range_plan::clone() const {
    if (state_ == nullptr) {
        throw std::invalid_argument("multipart response plan must contain segments");
    }
    http_multipart_byte_range_plan copy;
    copy.state_->content_type_.assign(state_->content_type_);
    copy.state_->metadata_.assign(state_->metadata_);
    copy.state_->segments_ = state_->segments_;
    copy.state_->segment_count_ = state_->segment_count_;
    copy.state_->content_length_ = state_->content_length_;
    return copy;
}

http_byte_range_set resolve_http_byte_range_set(std::string_view field_value,
    http_content_length representation_length_value, http_byte_range_options options) {
    const auto representation_length = representation_length_value.value();
    http_byte_range_set result;
    if (options.max_ranges_ == 0) {
        return result;
    }
    field_value = http_trim_ows(field_value);
    constexpr std::string_view unit = "bytes";
    if (field_value.size() <= unit.size() || field_value[unit.size()] != '=' ||
        !http_ascii_equals_ignore_case(field_value.substr(0, unit.size()), unit)) {
        return result;
    }

    const auto parse_number = [](std::string_view digits, std::uint64_t& value) noexcept {
        if (digits.empty()) {
            return false;
        }
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (end != digits.data() + digits.size()) {
            return false;
        }
        if (error == std::errc::result_out_of_range) {
            value = (std::numeric_limits<std::uint64_t>::max)();
            return true;
        }
        return error == std::errc{};
    };

    const bool strict = options.strict_order_ ||
                        options.order_policy_ == http_byte_range_order_policy::strict_ascending_disjoint;
    const auto add_range = [&](std::uint64_t offset, std::uint64_t length) -> bool {
        if (strict) {
            if (result.count_ > 0) {
                const auto prev = result[result.count_ - 1];
                if (offset < prev.offset_ + prev.length_) {
                    return false;
                }
            }
            if (result.count_ == options.max_ranges_) {
                return false;
            }
            result.append_slot({offset, length});
            return true;
        }
        if (result.count_ == options.max_ranges_) {
            return false;
        }
        auto start = offset;
        auto end = offset + length;
        auto target = result.count_;
        std::size_t kept = 0;
        for (std::size_t index = 0; index < result.count_; ++index) {
            const auto current = result[index];
            const auto current_end = current.offset_ + current.length_;
            if (current.offset_ <= end && start <= current_end) {
                start = (std::min)(start, current.offset_);
                end = (std::max)(end, current_end);
                if (target == result.count_) {
                    target = kept++;
                }
                continue;
            }
            result.mutable_at(kept++) = current;
        }
        if (target == result.count_) {
            result.append_slot({start, end - start});
        } else {
            result.resize_slots(kept);
            result.mutable_at(target) = {start, end - start};
        }
        return true;
    };

    auto specs = http_trim_ows(field_value.substr(unit.size() + 1));
    if (specs.empty()) {
        return result;
    }
    result.ignored_ = false;
    std::size_t empty_members = 0;
    bool saw_range_spec = false;
    bool has_more = false;
    do {
        const auto comma = specs.find(',');
        auto spec = http_trim_ows(specs.substr(0, comma));
        has_more = comma != std::string_view::npos;
        specs = has_more ? specs.substr(comma + 1) : std::string_view{};
        if (spec.empty()) {
            if (++empty_members > 32) {
                return http_byte_range_set{};
            }
            continue;
        }
        saw_range_spec = true;
        const auto dash = spec.find('-');
        if (dash == std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos) {
            return http_byte_range_set{};
        }
        const auto first = spec.substr(0, dash);
        const auto last = spec.substr(dash + 1);
        std::uint64_t start{};
        std::uint64_t end{};
        if (first.empty()) {
            std::uint64_t suffix{};
            if (!parse_number(last, suffix)) {
                return http_byte_range_set{};
            }
            if (suffix == 0 || representation_length == 0) {
                continue;
            }
            const auto length = (std::min)(suffix, representation_length);
            if (!add_range(representation_length - length, length)) {
                return http_byte_range_set{};
            }
            continue;
        }
        if (!parse_number(first, start) || (!last.empty() && !parse_number(last, end))) {
            return http_byte_range_set{};
        }
        if (!last.empty()) {
            if (start > end) {
                return http_byte_range_set{};
            }
            if (start == (std::numeric_limits<std::uint64_t>::max)() && end == start) {
                auto normalized_first = first;
                auto normalized_last = last;
                while (normalized_first.size() > 1 && normalized_first.front() == '0') {
                    normalized_first.remove_prefix(1);
                }
                while (normalized_last.size() > 1 && normalized_last.front() == '0') {
                    normalized_last.remove_prefix(1);
                }
                if (normalized_first.size() > normalized_last.size() ||
                    (normalized_first.size() == normalized_last.size() &&
                        normalized_first > normalized_last)) {
                    return http_byte_range_set{};
                }
            }
        }
        if (start >= representation_length) {
            continue;
        }
        const auto clamped_end =
            last.empty() ? representation_length - 1 : (std::min)(end, representation_length - 1);
        if (!add_range(start, clamped_end - start + 1)) {
            return http_byte_range_set{};
        }
    } while (has_more);
    if (result.count_ == 0 && !saw_range_spec) {
        return http_byte_range_set{};
    }
    return result;
}

std::optional<http_content_range> parse_http_content_range(std::string_view field_value) noexcept {
    field_value = http_trim_ows(field_value);
    constexpr std::string_view prefix = "bytes ";
    if (field_value.size() <= prefix.size() ||
        !http_ascii_equals_ignore_case(field_value.substr(0, 5), "bytes") ||
        (field_value[5] != ' ' && field_value[5] != '\t')) {
        return std::nullopt;
    }
    const auto spec = http_trim_ows(field_value.substr(5));
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto slash = spec.find('/', dash + 1);
    if (slash == std::string_view::npos || spec.find('/', slash + 1) != std::string_view::npos ||
        spec.find('-', dash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto parse_exact = [](std::string_view digits, std::uint64_t& out) noexcept {
        if (digits.empty()) {
            return false;
        }
        const auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), out);
        return err == std::errc{} && end == digits.data() + digits.size();
    };
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::uint64_t total = 0;
    if (!parse_exact(spec.substr(0, dash), first) ||
        !parse_exact(spec.substr(dash + 1, slash - dash - 1), last) ||
        !parse_exact(spec.substr(slash + 1), total)) {
        return std::nullopt;
    }
    if (first > last || last >= total) {
        return std::nullopt;
    }
    return http_content_range{
        .first_ = first,
        .last_ = last,
        .total_ = total,
    };
}

std::optional<std::uint64_t> compute_http_byte_range_content_length(
    std::span<const http_byte_range> ranges, http_content_length representation_length_value,
    std::string_view media_type, std::string_view boundary) noexcept {
    const auto representation_length = representation_length_value.value();
    if (ranges.empty() || representation_length == 0) {
        return std::nullopt;
    }
    for (const auto& range : ranges) {
        if (range.length_ == 0 || range.offset_ > representation_length ||
            range.length_ > representation_length - range.offset_) {
            return std::nullopt;
        }
    }
    if (ranges.size() == 1) {
        return ranges.front().length_;
    }
    if (!is_valid_http_content_type_field_value(media_type) ||
        !multipart_boundary::try_create(boundary).has_value()) {
        return std::nullopt;
    }
    const auto representation_digits = http_unsigned_decimal_size(representation_length);
    std::uint64_t total = 2U + boundary.size() + 4U;
    for (const auto& range : ranges) {
        const std::uint64_t part_metadata =
            2U + boundary.size() + 16U + media_type.size() + 23U +
            http_unsigned_decimal_size(range.offset_) + 1U +
            http_unsigned_decimal_size(range.offset_ + range.length_ - 1) + 1U +
            representation_digits + 6U;
        constexpr auto max_u64 = (std::numeric_limits<std::uint64_t>::max)();
        if (total > max_u64 - part_metadata ||
            total + part_metadata > max_u64 - range.length_) {
            return std::nullopt;
        }
        total += part_metadata + range.length_;
    }
    return total;
}

std::optional<std::uint64_t> compute_http_byte_range_content_length(
    const http_byte_range_set& ranges, http_content_length representation_length,
    std::string_view media_type, std::string_view boundary) noexcept {
    if (ranges.ignored() || ranges.unsatisfiable()) {
        return std::nullopt;
    }
    return compute_http_byte_range_content_length(
        ranges.ranges(), representation_length, media_type, boundary);
}

http_byte_range_slicer::http_byte_range_slicer(const http_byte_range_set& ranges,
    http_content_length representation_length, std::string_view media_type,
    std::string_view boundary)
    : http_byte_range_slicer(
          ranges.ignored() ? std::span<const http_byte_range>{} : ranges.ranges(),
          representation_length, media_type, boundary) {}

http_byte_range_slicer::http_byte_range_slicer(std::span<const http_byte_range> ranges,
    http_content_length representation_length_value, std::string_view media_type,
    std::string_view boundary)
    : ranges_(ranges.begin(), ranges.end()),
      media_type_(media_type),
      boundary_(boundary),
      content_type_(),
      content_range_(),
      representation_length_(representation_length_value.value()) {
    if (ranges_.empty() || representation_length_ == 0) {
        throw std::invalid_argument("HTTP byte-range slicer requires at least one range");
    }
    for (std::size_t index = 0; index < ranges_.size(); ++index) {
        const auto range = ranges_[index];
        if (range.length_ == 0 || range.offset_ > representation_length_ ||
            range.length_ > representation_length_ - range.offset_) {
            throw std::invalid_argument("HTTP byte range is outside representation");
        }
        if (index > 0) {
            const auto prev = ranges_[index - 1];
            if (range.offset_ < prev.offset_ + prev.length_) {
                throw std::invalid_argument(
                    "HTTP byte-range slicer requires ascending non-overlapping ranges");
            }
        }
    }
    if (ranges_.size() == 1) {
        if (!media_type.empty() && !is_valid_http_content_type_field_value(media_type)) {
            throw std::invalid_argument("invalid HTTP media type for byte-range slicer");
        }
        content_type_.assign(media_type);
        const auto single = ranges_.front();
        content_range_.append("bytes ");
        http_append_unsigned(content_range_, single.offset_);
        content_range_.push_back('-');
        http_append_unsigned(content_range_, single.offset_ + single.length_ - 1);
        content_range_.push_back('/');
        http_append_unsigned(content_range_, representation_length_);
        content_length_ = single.length_;
        return;
    }
    if (!is_valid_http_content_type_field_value(media_type) ||
        !multipart_boundary::try_create(boundary).has_value()) {
        throw std::invalid_argument("invalid multipart media type or boundary for byte-range slicer");
    }
    const auto computed = compute_http_byte_range_content_length(
        ranges_, representation_length_value, media_type, boundary);
    if (!computed.has_value()) {
        throw std::length_error("multipart byte-range response content length overflows uint64_t");
    }
    content_length_ = *computed;
    static constexpr std::string_view content_type_prefix = "multipart/byteranges; boundary=";
    const bool boundary_is_token = http_token_prefix_size(boundary) == boundary.size();
    content_type_.reserve(
        content_type_prefix.size() + boundary.size() + (boundary_is_token ? 0U : 2U));
    content_type_.append(content_type_prefix);
    if (!boundary_is_token) {
        content_type_.push_back('"');
    }
    content_type_.append(boundary);
    if (!boundary_is_token) {
        content_type_.push_back('"');
    }
}

void http_byte_range_slicer::emit_part_header(
    std::size_t index, std::string& output) const {
    const auto range = ranges_[index];
    output.append("--");
    output.append(boundary_);
    output.append("\r\nContent-Type: ");
    output.append(media_type_);
    output.append("\r\nContent-Range: bytes ");
    http_append_unsigned(output, range.offset_);
    output.push_back('-');
    http_append_unsigned(output, range.offset_ + range.length_ - 1);
    output.push_back('/');
    http_append_unsigned(output, representation_length_);
    output.append("\r\n\r\n");
}

void http_byte_range_slicer::write(std::string_view chunk, std::string& output) {
    write(current_offset_, chunk, output);
}

void http_byte_range_slicer::write(
    std::uint64_t chunk_offset, std::string_view chunk, std::string& output) {
    if (chunk_offset < current_offset_ || chunk_offset > representation_length_ ||
        chunk.size() > representation_length_ - chunk_offset) {
        throw std::invalid_argument("HTTP byte-range slicer chunk offset is out of bounds");
    }
    if (complete_) {
        current_offset_ = chunk_offset + chunk.size();
        return;
    }
    const auto needed_offset = ranges_[range_index_].offset_ + range_progress_;
    if (chunk_offset > needed_offset) {
        throw std::invalid_argument(
            "HTTP byte-range slicer chunk skipped over required range bytes");
    }
    const std::uint64_t chunk_end = chunk_offset + chunk.size();
    current_offset_ = chunk_end;
    if (chunk.empty()) {
        return;
    }

    while (range_index_ < ranges_.size()) {
        const auto range = ranges_[range_index_];
        const std::uint64_t next_needed = range.offset_ + range_progress_;
        if (chunk_end <= next_needed) {
            break;
        }
        if (range_progress_ == 0 && multipart()) {
            emit_part_header(range_index_, output);
        }
        const std::size_t slice_start = static_cast<std::size_t>(next_needed - chunk_offset);
        const std::uint64_t remaining_in_range = range.length_ - range_progress_;
        const std::uint64_t available_in_chunk = chunk.size() - slice_start;
        const auto copy_bytes =
            static_cast<std::size_t>((std::min)(remaining_in_range, available_in_chunk));
        output.append(chunk.data() + slice_start, copy_bytes);
        range_progress_ += copy_bytes;
        if (range_progress_ == range.length_) {
            if (multipart()) {
                output.append("\r\n");
            }
            ++range_index_;
            range_progress_ = 0;
            if (range_index_ == ranges_.size()) {
                if (multipart()) {
                    output.append("--");
                    output.append(boundary_);
                    output.append("--\r\n");
                }
                complete_ = true;
                break;
            }
        } else {
            break;
        }
    }
}

void http_byte_range_slicer::finish(std::string&) {
    if (!complete_) {
        throw std::logic_error("HTTP byte-range slicer finished before all ranges completed");
    }
}

void http_byte_range_slicer::reset(std::uint64_t start_offset) noexcept {
    current_offset_ = start_offset;
    range_progress_ = 0;
    range_index_ = 0;
    complete_ = false;
}

}  // namespace ruvia

namespace ruvia::detail {

template <std::size_t n>
[[nodiscard]] consteval http_response_header static_response_header(
    std::string_view name, const char (&bytes_value)[n], std::uint32_t known_bit) {
    static_assert(n > 1 && response_header_storage_size_fits(0, n - 1));
    if (name.empty() || name.size() > n - 1 || bytes_value[n - 1] != '\0' ||
        !std::string_view(bytes_value, n - 1).starts_with(name)) {
        throw std::logic_error("invalid static response header storage");
    }
    if (known_bit != 0 && known_bit != response_header_upgrade &&
        response_known_header_slot(known_bit) == response_known_header_count) {
        throw std::logic_error("static response header requires one known header bit");
    }
    return http_response_header_access::make(
        bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(n - 1 - name.size()), known_bit, false);
}

template <std::size_t... sizes>
[[nodiscard]] consteval auto static_response_headers(
    std::string_view name, std::uint32_t known_bit, const char (&... bytes_values)[sizes]) {
    return std::array{static_response_header(name, bytes_values, known_bit)...};
}

[[nodiscard]] inline std::optional<http_response_header> find_static_response_header(
    std::span<const http_response_header> headers, std::string_view value) noexcept {
    for (const auto& header_value : headers) {
        if (header_value.value() == value) {
            return header_value;
        }
    }
    return std::nullopt;
}

inline constexpr auto content_type_headers = static_response_headers("Content-Type", response_header_content_type,
    "Content-Typetext/plain; charset=UTF-8",
    "Content-Typeapplication/json",
    "Content-Typetext/html; charset=UTF-8",
    "Content-Typetext/plain; charset=utf-8",
    "Content-Typeapplication/json; charset=utf-8",
    "Content-Typetext/html; charset=utf-8",
    "Content-Typetext/css; charset=utf-8",
    "Content-Typetext/javascript; charset=utf-8",
    "Content-Typetext/event-stream",
    "Content-Typeimage/png",
    "Content-Typeimage/jpeg",
    "Content-Typeimage/gif",
    "Content-Typeimage/svg+xml",
    "Content-Typeapplication/wasm",
    "Content-Typeapplication/octet-stream",
    "Content-Typeapplication/problem+json",
    "Content-Typeimage/apng",
    "Content-Typeimage/avif",
    "Content-Typeimage/webp",
    "Content-Typeimage/bmp",
    "Content-Typeimage/vnd.microsoft.icon",
    "Content-Typefont/woff",
    "Content-Typefont/woff2",
    "Content-Typefont/ttf",
    "Content-Typefont/otf",
    "Content-Typeapplication/vnd.ms-fontobject",
    "Content-Typetext/xml; charset=utf-8",
    "Content-Typeapplication/manifest+json; charset=utf-8");

[[nodiscard]] inline http_response_header static_content_type_header(http_content_type type) {
    switch (type) {
        case http_content_type::text_plain_utf8:
            return content_type_headers[0];
        case http_content_type::text_html_utf8:
            return content_type_headers[2];
        case http_content_type::application_json:
            return content_type_headers[1];
    }
    throw std::invalid_argument("invalid HTTP content type");
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_response_header(
    std::uint32_t known_bit, std::string_view value) noexcept {
    switch (known_bit) {
        case response_header_content_type:
            return find_static_response_header(content_type_headers, value);
        case response_header_connection: {
            static constexpr auto headers = static_response_headers("Connection", response_header_connection,
                "Connectionclose", "Connectionkeep-alive", "ConnectionUpgrade");
            return find_static_response_header(headers, value);
        }
        case response_header_accept_ranges: {
            static constexpr auto headers = static_response_headers("Accept-Ranges", response_header_accept_ranges,
                "Accept-Rangesbytes");
            return find_static_response_header(headers, value);
        }
        case response_header_content_encoding: {
            static constexpr auto headers = static_response_headers("Content-Encoding", response_header_content_encoding,
                "Content-Encodinggzip", "Content-Encodingbr", "Content-Encodingzstd", "Content-Encodingdeflate");
            return find_static_response_header(headers, value);
        }
        case response_header_transfer_encoding: {
            static constexpr auto headers = static_response_headers("Transfer-Encoding", response_header_transfer_encoding,
                "Transfer-Encodingchunked");
            return find_static_response_header(headers, value);
        }
        case response_header_cache_control: {
            static constexpr auto headers = static_response_headers("Cache-Control", response_header_cache_control,
                "Cache-Controlno-store", "Cache-Controlno-cache", "Cache-Controlprivate");
            return find_static_response_header(headers, value);
        }
        case response_header_vary: {
            static constexpr auto headers = static_response_headers("Vary", response_header_vary,
                "Vary*", "VaryAccept-Encoding", "VaryOrigin", "VaryAccess-Control-Request-Headers", "VaryAccess-Control-Request-Method");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_origin: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Origin", response_header_access_control_allow_origin,
                "Access-Control-Allow-Origin*");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_credentials: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Credentials", response_header_access_control_allow_credentials,
                "Access-Control-Allow-Credentialstrue");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_methods: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Methods", response_header_access_control_allow_methods,
                "Access-Control-Allow-MethodsGET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
            return find_static_response_header(headers, value);
        }
        case response_header_upgrade: {
            static constexpr auto headers = static_response_headers("Upgrade", response_header_upgrade,
                "Upgradewebsocket", "Upgradeconnect-udp");
            return find_static_response_header(headers, value);
        }
        default:
            return std::nullopt;
    }
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_custom_response_header(
    std::string_view name, std::string_view value) noexcept {
    switch (name.size()) {
        case std::string_view("Retry-After").size():
            if (name == "Retry-After") {
                static constexpr auto headers = static_response_headers("Retry-After", 0, "Retry-After1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Frame-Options").size():
            if (name == "X-Frame-Options") {
                static constexpr auto headers = static_response_headers("X-Frame-Options", 0,
                    "X-Frame-OptionsDENY", "X-Frame-OptionsSAMEORIGIN");
                return find_static_response_header(headers, value);
            }
            if (name == "Referrer-Policy") {
                static constexpr auto headers = static_response_headers("Referrer-Policy", 0,
                    "Referrer-Policystrict-origin-when-cross-origin", "Referrer-Policyno-referrer");
                return find_static_response_header(headers, value);
            }
            if (name == "Accept-Encoding") {
                static constexpr auto headers = static_response_headers("Accept-Encoding", 0,
                    "Accept-Encodinggzip, deflate, br, zstd", "Accept-Encodingidentity");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-XSS-Protection").size():
            if (name == "X-XSS-Protection") {
                static constexpr auto headers = static_response_headers("X-XSS-Protection", 0, "X-XSS-Protection0");
                return find_static_response_header(headers, value);
            }
            if (name == "Capsule-Protocol") {
                static constexpr auto headers = static_response_headers("Capsule-Protocol", 0, "Capsule-Protocol?1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Reset").size():
            if (name == "X-RateLimit-Reset") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Reset", 0, "X-RateLimit-Reset1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Permissions-Policy").size():
            if (name == "Permissions-Policy") {
                static constexpr auto headers = static_response_headers("Permissions-Policy", 0,
                    "Permissions-Policygeolocation=(), microphone=(), camera=()");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Remaining").size():
            if (name == "X-RateLimit-Remaining") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Remaining", 0, "X-RateLimit-Remaining0");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Content-Type-Options").size():
            if (name == "X-Content-Type-Options") {
                static constexpr auto headers = static_response_headers("X-Content-Type-Options", 0, "X-Content-Type-Optionsnosniff");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Content-Security-Policy").size():
            if (name == "Content-Security-Policy") {
                static constexpr auto headers = static_response_headers("Content-Security-Policy", 0,
                    "Content-Security-Policydefault-src 'self'", "Content-Security-Policydefault-src 'none'");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Strict-Transport-Security").size():
            if (name == "Strict-Transport-Security") {
                static constexpr auto headers = static_response_headers("Strict-Transport-Security", 0,
                    "Strict-Transport-Securitymax-age=31536000; includeSubDomains");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

http_response_headers::http_response_headers() noexcept = default;

http_response_headers::~http_response_headers() {
    clear();
}

http_response_headers::http_response_headers(http_response_headers&& other) noexcept {
    move_from(std::move(other));
}

void http_response_headers::reserve(std::size_t count) {
    if (count <= inline_capacity) {
        return;
    }

    if (!spilled_) {
        spill(count);
        return;
    }
    if (count <= heap_.capacity()) {
        return;
    }
    // Only descriptors move; field bytes retain their existing ownership.
    std::vector<http_response_header> replacement;
    replacement.reserve(count);
    replacement.insert(replacement.end(), heap_.begin(), heap_.end());
    heap_.swap(replacement);
}

http_response_header& http_response_headers::append_prepared_header(http_response_header header_value) noexcept {
    if (!spilled_) {
        if (size_ == inline_capacity) {
            std::terminate();
        }
        auto* const target = inline_data() + size_;
        *target = header_value;
        ++size_;
        return *target;
    }

    if (heap_.size() == heap_.capacity()) {
        std::terminate();
    }
    heap_.push_back(header_value);
    return heap_.back();
}

http_response_header* http_response_headers::inline_data() noexcept {
    return reinterpret_cast<http_response_header*>(inline_.data());
}

const http_response_header* http_response_headers::inline_data() const noexcept {
    return reinterpret_cast<const http_response_header*>(inline_.data());
}

http_response_header* http_response_headers::data() noexcept {
    return spilled_ ? heap_.data() : inline_data();
}

const http_response_header* http_response_headers::data() const noexcept {
    return spilled_ ? heap_.data() : inline_data();
}

void http_response_headers::clear() noexcept {
    auto* items = data();
    const auto count = size();
    for (std::size_t i = 0; i < count; ++i) {
        release_header(items[i]);
    }
    if (spilled_) {
        heap_.clear();
    }
    size_ = 0;
}

void http_response_headers::spill(std::size_t min_capacity) {
    heap_.clear();
    heap_.reserve(std::max<std::size_t>(inline_capacity * 2, min_capacity));
    auto* items = inline_data();
    for (std::size_t i = 0; i < size_; ++i) {
        heap_.push_back(items[i]);
    }
    size_ = heap_.size();
    spilled_ = true;
}

void http_response_headers::move_from(http_response_headers&& other) noexcept {
    known_header_bits_ = std::exchange(other.known_header_bits_, 0U);
    if (known_header_bits_ != 0) {
        known_header_indexes_ = other.known_header_indexes_;
    }
    if (other.spilled_) {
        spilled_ = true;
        heap_ = std::move(other.heap_);
        size_ = heap_.size();
        other.spilled_ = false;
        other.size_ = 0;
        return;
    }

    if (other.size_ > 0) {
        std::memcpy(inline_.data(), other.inline_.data(), other.size_ * sizeof(inline_storage_type));
    }
    size_ = other.size_;
    other.size_ = 0;
}

bool http_response_headers::borrows_owned_header(std::string_view name, std::string_view value) const noexcept {
    for (const auto& existing : *this) {
        if (existing.owned_ &&
            (detail::response_header_storage_overlaps(existing, name) ||
                detail::response_header_storage_overlaps(existing, value))) {
            return true;
        }
    }
    return false;
}
http_response_header http_response_headers::make_owned_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value.size());
    const auto total = name.size() + value.size();
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = new char[total];
        std::memcpy(bytes_value, name.data(), name.size());
        std::memcpy(bytes_value + name.size(), value.data(), value.size());
    }
    return detail::http_response_header_access::make(bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(value.size()), known_bit, true);
}

http_response_header http_response_headers::make_uninitialized_header(
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = new char[total];
        std::memcpy(bytes_value, name.data(), name.size());
    }
    return detail::http_response_header_access::make(bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(value_size), known_bit, true);
}

std::optional<http_response_header> http_response_headers::make_static_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept {
    if (known_bit == 0) {
        return detail::builtin_static_custom_response_header(name, value);
    }
    auto header_value = detail::builtin_static_response_header(known_bit, value);
    if (!header_value || header_value->name() != name) {
        return std::nullopt;
    }
    return header_value;
}

std::optional<http_response_header> http_response_headers::make_shared_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) const noexcept {
    if (const auto static_header = make_static_header(name, value, known_bit)) {
        return static_header;
    }
    if (stable_view && !borrows_owned_header(name, value) &&
        detail::response_header_views_contiguous(name, value) &&
        detail::response_header_storage_size_fits(name.size(), value.size())) {
        return detail::http_response_header_access::make(name.data(),
            static_cast<std::uint32_t>(name.size()), static_cast<std::uint32_t>(value.size()),
            known_bit, false);
    }
    return std::nullopt;
}

void http_response_headers::release_header(http_response_header& header_value) noexcept {
    if (header_value.owned_ && header_value.bytes_ != nullptr) {
        delete[] const_cast<char*>(header_value.bytes_);
    }
    header_value.bytes_ = nullptr;
    header_value.name_size_ = 0;
    header_value.value_size_ = 0;
    header_value.known_bit_ = 0;
    header_value.owned_ = false;
    header_value.append_ = false;
}

void http_response_headers::truncate(iterator new_end) noexcept {
    if (spilled_) {
        heap_.erase(heap_.begin() + (new_end - heap_.data()), heap_.end());
        return;
    }
    size_ = static_cast<std::size_t>(new_end - inline_data());
}

http_response_header& http_response_headers::append_header(http_response_header header_value) {
    // `header` may own a separately allocated name/value block.  The vector
    // stores the small descriptor by value, so an exception while spilling or
    // appending must release that block here; otherwise a failed heap
    // allocation leaks the response header and leaves the next retry with a
    // different ownership picture.
    try {
        if (!spilled_ && size_ == inline_capacity) {
            spill(size_ + 1);
        }
        if (!spilled_) {
            auto* target = inline_data() + size_;
            *target = header_value;
            ++size_;
            return *target;
        }
        heap_.push_back(header_value);
        return heap_.back();
    } catch (...) {
        release_header(header_value);
        throw;
    }
}

http_response_header& http_response_headers::add(
    std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) {
    const auto shared_header = make_shared_header(name, value, known_bit, stable_view);
    return append_header(shared_header ? *shared_header : make_owned_header(name, value, known_bit));
}

http_response_header& http_response_headers::add_uninitialized_value(
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    return append_header(make_uninitialized_header(name, value_size, known_bit));
}

http_response_header& http_response_headers::assign_uninitialized_value(http_response_header& header_value,
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    if (header_value.owned_ && header_value.bytes_ != nullptr && !detail::response_header_storage_overlaps(header_value, name) &&
        total == static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        auto* const bytes_value = const_cast<char*>(header_value.bytes_);
        std::memcpy(bytes_value, name.data(), name.size());
        header_value.name_size_ = static_cast<std::uint32_t>(name.size());
        header_value.value_size_ = static_cast<std::uint32_t>(value_size);
        header_value.known_bit_ = known_bit;
        header_value.append_ = false;
        return header_value;
    }

    const auto replacement = make_uninitialized_header(name, value_size, known_bit);
    release_header(header_value);
    header_value = replacement;
    return header_value;
}

bool http_response_headers::try_assign_owned_in_place(http_response_header& header_value, std::string_view name,
    std::string_view value, std::uint32_t known_bit) noexcept {
    if (!detail::response_header_storage_size_fits(name.size(), value.size())) {
        return false;
    }
    const auto total = name.size() + value.size();
    if (!header_value.owned_ || header_value.bytes_ == nullptr || detail::response_header_storage_overlaps(header_value, name) ||
        detail::response_header_storage_overlaps(header_value, value) ||
        total != static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        return false;
    }
    auto* const bytes_value = const_cast<char*>(header_value.bytes_);
    std::memcpy(bytes_value, name.data(), name.size());
    std::memcpy(bytes_value + name.size(), value.data(), value.size());
    header_value.name_size_ = static_cast<std::uint32_t>(name.size());
    header_value.value_size_ = static_cast<std::uint32_t>(value.size());
    header_value.known_bit_ = known_bit;
    header_value.append_ = false;
    return true;
}

void http_response_headers::assign(http_response_header& header_value, std::string_view name,
    std::string_view value, std::uint32_t known_bit, bool stable_view) {
    if (const auto shared_header = make_shared_header(name, value, known_bit, stable_view)) {
        release_header(header_value);
        header_value = *shared_header;
        return;
    }
    if (try_assign_owned_in_place(header_value, name, value, known_bit)) {
        return;
    }
    const auto replacement = make_owned_header(name, value, known_bit);
    release_header(header_value);
    header_value = replacement;
}

namespace {

inline constexpr std::size_t allow_header_method_slots = static_cast<std::size_t>(http_known_method::unknown);

void write_content_range_header_value(
    http_response_header& header_value, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes ");
    sink.append_unsigned(offset);
    sink.append('-');
    sink.append_unsigned(offset + length - 1);
    sink.append('/');
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

void write_content_range_unsatisfied_header_value(http_response_header& header_value, std::uint64_t size) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes */");
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

[[nodiscard]] std::size_t allow_header_value_size(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    std::size_t size = 0;
    std::size_t count = 0;
    const auto add_method = [&](std::size_t token_size) {
        const auto separator_size = count == 0 ? std::size_t{0} : std::size_t{2};
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, separator_size);
        size += separator_size;
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, token_size);
        size += token_size;
        ++count;
    };
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        add_method(known_http_method_token(static_cast<http_known_method>(i)).size());
    }
    for (const auto token : extension_methods) {
        add_method(token.size());
    }
    return size;
}

void write_allow_header_value(http_response_header& header_value, std::uint32_t method_mask,
    std::span<const std::string_view> extension_methods) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    bool first = true;
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(known_http_method_token(static_cast<http_known_method>(i)));
    }
    for (const auto token : extension_methods) {
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(token);
    }
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Allow header");
    }
}

[[nodiscard]] bool set_cookie_wire_name_matches(
    std::string_view value, std::string_view wire_prefix, std::string_view cookie_name) noexcept {
    if (value.size() != wire_prefix.size() + cookie_name.size()) {
        return false;
    }
    if (!value.starts_with(wire_prefix)) {
        return false;
    }
    return value.substr(wire_prefix.size()) == cookie_name;
}

[[nodiscard]] bool set_cookie_value_matches_storage_key(std::string_view value,
    std::string_view wire_prefix, std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    const auto parsed_value = parse_set_cookie(value);
    return parsed_value.has_value() && is_valid_http_header_name(parsed_value->name()) &&
           set_cookie_wire_name_matches(parsed_value->name(), wire_prefix, cookie_name) &&
           !parsed_value->path().empty() == has_path &&
           parsed_value->has(http_set_cookie_attribute::partitioned) == partitioned &&
           (!has_path || parsed_value->path() == path) &&
           http_ascii_equals_ignore_case(parsed_value->domain(), domain);
}

}  // namespace

void http_response_headers::allow_methods(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    const auto value_size = allow_header_value_size(method_mask, extension_methods);
    for (const auto token : extension_methods) {
        if (!is_valid_http_method_token(token)) {
            throw std::invalid_argument("invalid HTTP Allow method");
        }
    }
    if (!extension_methods.empty()) {
        if (auto* const retained = find_header_for_update("Allow", detail::response_header_allow)) {
            bool borrows_allow = false;
            const auto overlaps_retained = [&](std::string_view token) noexcept {
                if (!detail::http_response_header_access::append(*retained)) {
                    return detail::response_header_storage_overlaps(*retained, token);
                }
                for (const auto& existing : *this) {
                    if (detail::http_response_header_access::known_bit(existing) == detail::response_header_allow &&
                        detail::response_header_storage_overlaps(existing, token)) {
                        return true;
                    }
                }
                return false;
            };
            for (const auto token : extension_methods) {
                if (overlaps_retained(token)) {
                    borrows_allow = true;
                    break;
                }
            }
            if (borrows_allow) {
                auto prepared = make_uninitialized_header(
                    "Allow", value_size, detail::response_header_allow);
                try {
                    write_allow_header_value(prepared, method_mask, extension_methods);
                } catch (...) {
                    release_header(prepared);
                    throw;
                }
                (void)replace_header(*retained, prepared);
                return;
            }
        }
    }
    auto& header_value = prepare_header_value_storage(
        "Allow", value_size, detail::response_header_allow);
    write_allow_header_value(header_value, method_mask, extension_methods);
}

void http_response_headers::content_range(
    http_content_length offset_value, http_content_length length_value, http_content_length size_value) {
    const auto offset = offset_value.value();
    const auto length = length_value.value();
    const auto size = size_value.value();
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    if (offset > size || length > size - offset) {
        throw std::logic_error("file response byte range is outside the representation");
    }
    const auto end_offset = offset + length - 1;
    const auto value_size =
        std::string_view("bytes ").size() + http_unsigned_decimal_size(offset) + 1 +
        http_unsigned_decimal_size(end_offset) + 1 + http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_header_value(header_value, offset, length, size);
}

void http_response_headers::content_range_unsatisfied(http_content_length size_value) {
    const auto size = size_value.value();
    const auto value_size =
        std::string_view("bytes */").size() + http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_unsatisfied_header_value(header_value, size);
}

void http_response_headers::set_cookie(const set_cookie_plan& plan) {
    auto* retained = find_set_cookie_header(
        plan.wire_prefix(), plan.name(), !plan.path().empty(), plan.path(), plan.domain(), plan.partitioned_);
    if (retained == nullptr) {
        auto& header_value = append_header_uninitialized_value(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        plan.write(detail::http_response_header_access::value_begin(header_value));
        return;
    }

    const bool borrows_retained =
        detail::response_header_storage_overlaps(*retained, plan.name_) ||
        detail::response_header_storage_overlaps(*retained, plan.value_) ||
        detail::response_header_storage_overlaps(*retained, plan.path_) ||
        detail::response_header_storage_overlaps(*retained, plan.domain_);
    set_cookie_plan::written_fields written;
    if (borrows_retained) {
        auto prepared = make_uninitialized_header(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(prepared));
        release_header(*retained);
        *retained = prepared;
    } else {
        assign_uninitialized_value(
            *retained, "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(*retained));
    }
    detail::http_response_header_access::set_append(*retained);
    erase_later_set_cookie_headers(*retained, written.wire_name_, !written.path_.empty(),
        written.path_, written.domain_, plan.partitioned_);
}

void http_response_headers::upsert_set_cookie_header_validated(std::string_view value) {
    const auto parsed_value = parse_set_cookie(value);
    const auto cookie_name = parsed_value.has_value() && is_valid_http_header_name(parsed_value->name())
                                 ? parsed_value->name()
                                 : std::string_view{};
    if (cookie_name.empty()) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto has_path = !parsed_value->path().empty();
    const auto partitioned = parsed_value->has(http_set_cookie_attribute::partitioned);
    auto* retained = find_set_cookie_header({}, cookie_name, has_path, parsed_value->path(), parsed_value->domain(), partitioned);
    if (retained == nullptr) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto name_size = cookie_name.size();
    const auto path_size = parsed_value->path().size();
    const auto domain_size = parsed_value->domain().size();
    const auto name_offset = static_cast<std::size_t>(cookie_name.data() - value.data());
    const auto path_offset = parsed_value->path().empty() ? std::size_t{0}
                                                          : static_cast<std::size_t>(parsed_value->path().data() - value.data());
    const auto domain_offset = parsed_value->domain().empty() ? std::size_t{0}
                                                              : static_cast<std::size_t>(parsed_value->domain().data() - value.data());
    assign(*retained, "Set-Cookie", value, detail::response_header_set_cookie);
    detail::http_response_header_access::set_append(*retained);
    const auto copied_value = retained->value();
    erase_later_set_cookie_headers(*retained, copied_value.substr(name_offset, name_size), has_path,
        copied_value.substr(path_offset, path_size), copied_value.substr(domain_offset, domain_size), partitioned);
}

http_response_header* http_response_headers::find_set_cookie_header(std::string_view wire_prefix,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    for (auto* header = mutable_begin(); header != mutable_end(); ++header) {
        if (detail::http_response_header_access::known_bit(*header) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                header->value(), wire_prefix, cookie_name, has_path, path, domain, partitioned)) {
            return header;
        }
    }
    return nullptr;
}

void http_response_headers::erase_later_set_cookie_headers(http_response_header& retained,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    auto* const end_value = mutable_end();
    auto* write = &retained + 1;
    for (auto* read = write; read != end_value; ++read) {
        if (detail::http_response_header_access::known_bit(*read) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                read->value(), {}, cookie_name, has_path, path, domain, partitioned)) {
            release_header(*read);
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end_value) {
        truncate(write);
        rebuild_known_header_index();
    }
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Every insertion path validates one field line completely, so a serialized
// head only re-checks the rules that span several lines. Returns the field's
// known bit.
[[nodiscard]] std::uint32_t validate_response_header_field(std::string_view key, std::string_view value) {
    // Check the descriptor's representable storage before any grammar scan;
    // callers may provide a view over a bounded buffer with a hostile length.
    detail::validate_response_header_storage_size(key.size(), value.size());
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!is_valid_http_header_value(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto field = detail::classify_http_field_name(key);
    const auto known_bit = field.response_bit_;
    const auto kind = field.kind_;
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    switch (detail::parse_http_response_control_field(kind, value, connection_options, upgrade_protocols)) {
        case detail::http_response_control_field_status::invalid_connection:
            throw std::invalid_argument("invalid HTTP Connection header");
        case detail::http_response_control_field_status::invalid_upgrade:
            throw std::invalid_argument("invalid HTTP Upgrade header");
        case detail::http_response_control_field_status::te_forbidden:
            throw std::invalid_argument("TE is not a response field");
        case detail::http_response_control_field_status::ok:
            break;
    }
    detail::response_field_state fields;
    if (detail::validate_response_field(kind, value, fields) != detail::response_field_status::ok) {
        throw std::invalid_argument("invalid HTTP response field value");
    }
    return known_bit;
}

[[nodiscard]] bool response_header_mode_appends(http_response_header_mode mode) {
    switch (mode) {
        case http_response_header_mode::replace:
            return false;
        case http_response_header_mode::append:
            return true;
    }
    throw std::invalid_argument("invalid HTTP response header mode");
}

}  // namespace

http_response_headers http_response_headers::clone(std::size_t additional_headers) const {
    const auto existing = size();
    if (additional_headers > heap_.max_size() - existing) {
        throw std::length_error("HTTP response header table is too large");
    }
    http_response_headers copy;
    copy.reserve(existing + additional_headers);
    for (const auto& header_value : *this) {
        // Static descriptors can be shared; owning descriptors must keep an
        // independent allocation so either block may be changed or destroyed.
        auto descriptor = header_value.owned_
                              ? copy.make_owned_header(header_value.name(), header_value.value(), header_value.known_bit_)
                              : header_value;
        descriptor.append_ = header_value.append_;
        (void)copy.append_prepared_header(descriptor);
    }
    copy.known_header_bits_ = known_header_bits_;
    if (known_header_bits_ != 0) {
        copy.known_header_indexes_ = known_header_indexes_;
    }
    return copy;
}

void http_response_headers::commit(http_response_headers&& staged) noexcept {
    if (this == &staged) {
        return;
    }
    // Reconstruction adopts the staged table without allocating; the move
    // leaves `staged` empty with its own resource and a cleared index.
    std::destroy_at(this);
    std::construct_at(this, std::move(staged));
}

bool http_response_headers::try_append_disjoint(
    const http_response_headers& source, http_response_header_transfer mode) {
    if (&source == this) {
        return false;
    }
    const auto skips = [mode](std::uint32_t known_bit) noexcept {
        return mode == http_response_header_transfer::assign && known_bit == detail::response_header_content_type;
    };
    // A source block already holds each field under its own rules: one line per
    // single-valued field and key-unique cookies. When no source field names a
    // field this block holds, every transfer mode keeps each source line as is.
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (skips(known_bit)) {
            continue;
        }
        if (known_bit != 0 ? (known_header_bits_ & known_bit) != 0
                           : find_header_for_read(header_value.name(), 0) != nullptr) {
            return false;
        }
    }

    reserve(size() + source.size());
    const auto first_index = size();
    try {
        for (const auto& header_value : source) {
            const auto known_bit = header_value.known_bit_;
            if (skips(known_bit)) {
                continue;
            }
            // Static descriptors are shared; owning descriptors get an
            // independent allocation, as in clone().
            auto descriptor = header_value.owned_
                                  ? make_owned_header(header_value.name(), header_value.value(), known_bit)
                                  : header_value;
            // Transferred cookies are appended occurrences, like the staged path.
            descriptor.append_ = header_value.append_ || known_bit == detail::response_header_set_cookie;
            (void)append_prepared_header(descriptor);
        }
    } catch (...) {
        auto* const first = data() + first_index;
        for (auto* cursor_value = first; cursor_value != end(); ++cursor_value) {
            release_header(*cursor_value);
        }
        truncate(first);
        throw;
    }
    const auto* const items = data();
    for (auto index = first_index; index < size(); ++index) {
        if (const auto known_bit = items[index].known_bit_; known_bit != 0) {
            record_known_header_index(known_bit, index);
        }
    }
    return true;
}

void http_response_headers::transfer_from(
    const http_response_headers& source, http_response_header_transfer mode) {
    switch (mode) {
        case http_response_header_transfer::merge:
        case http_response_header_transfer::assign:
        case http_response_header_transfer::apply:
            break;
        default:
            throw std::invalid_argument("invalid HTTP response header transfer mode");
    }
    if (try_append_disjoint(source, mode)) {
        return;
    }
    // Fields that meet existing ones are resolved on a staged copy, so a
    // failure leaves this block unchanged.
    auto staged = clone(source.size());
    const auto original_size = staged.size();
    bool removed_cookies = false;
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (mode == http_response_header_transfer::assign &&
            known_bit == detail::response_header_content_type) {
            continue;
        }
        const auto name = header_value.name();
        const auto value = header_value.value();
        if (known_bit == detail::response_header_set_cookie) {
            if (mode == http_response_header_transfer::assign && !removed_cookies) {
                (void)staged.remove_header_validated("Set-Cookie", detail::response_header_set_cookie);
                removed_cookies = true;
            }
            staged.upsert_set_cookie_header_validated(value);
        } else if (detail::http_response_header_access::append(header_value)) {
            const auto matches_header = [&](const http_response_header& candidate) noexcept {
                return detail::response_header_matches(candidate, name, known_bit) && candidate.value() == value;
            };
            const auto occurrence_through = [&] {
                std::size_t count = 0;
                for (const auto& candidate : source) {
                    if (matches_header(candidate)) {
                        ++count;
                    }
                    if (&candidate == &header_value) {
                        break;
                    }
                }
                return count;
            }();
            const auto existing_count = [&] {
                std::size_t count = 0;
                const auto* const items = staged.data();
                for (std::size_t index = 0, limit = std::min(original_size, staged.size());
                    index < limit; ++index) {
                    if (matches_header(items[index])) {
                        ++count;
                    }
                }
                return count;
            }();
            if (existing_count < occurrence_through) {
                staged.append_header_validated(name, value, known_bit);
            }
        } else if (mode == http_response_header_transfer::merge) {
            if (staged.find_header_for_read(name, known_bit) == nullptr) {
                if (!header_value.owned_) {
                    staged.publish_header(header_value);
                } else {
                    staged.set_header_validated(name, value, known_bit);
                }
            }
        } else if (!header_value.owned_) {
            staged.publish_header(header_value);
        } else {
            staged.set_header_validated(name, value, known_bit);
        }
    }
    commit(std::move(staged));
}

void http_response_headers::record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept {
    if (known_bit == 0 || (known_header_bits_ & known_bit) != 0) {
        return;
    }
    known_header_bits_ |= known_bit;
    const auto slot = detail::response_known_header_slot(known_bit);
    if (slot >= known_header_indexes_.size()) {
        return;
    }
    // A slot holds the first occurrence's index + 1; 0 (unset) and -1 (index
    // beyond int16) both fall back to a scan.
    known_header_indexes_[slot] = index < static_cast<std::size_t>((std::numeric_limits<std::int16_t>::max)())
                                      ? static_cast<std::int16_t>(index + 1)
                                      : std::int16_t{-1};
}

http_response_header* http_response_headers::find_header_for_update(
    std::string_view key, std::uint32_t known_bit) noexcept {
    return const_cast<http_response_header*>(std::as_const(*this).find_header_for_read(key, known_bit));
}

const http_response_header* http_response_headers::find_header_for_read(
    std::string_view key, std::uint32_t known_bit) const noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return nullptr;
    }
    if (const auto slot = detail::response_known_header_slot(known_bit);
        slot < known_header_indexes_.size() && known_header_indexes_[slot] > 0) {
        return begin() + (known_header_indexes_[slot] - 1);
    }
    for (const auto& header_value : *this) {
        if (detail::response_header_matches(header_value, key, known_bit)) {
            return &header_value;
        }
    }
    return nullptr;
}

template <typename Mutation>
http_response_header& http_response_headers::upsert_header(
    std::string_view key, std::uint32_t known_bit, Mutation&& mutate) {
    if (auto* const existing = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::http_response_header_access::append(*existing);
        auto& header_value = mutate(existing);
        return was_appended ? collapse_response_headers(header_value, known_bit) : header_value;
    }
    const auto index = size();
    auto& header_value = mutate(nullptr);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response_headers::prepare_header_value_storage(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    return upsert_header(key, known_bit, [&](http_response_header* existing) -> http_response_header& {
        return existing != nullptr
                   ? assign_uninitialized_value(*existing, key, value_size, known_bit)
                   : add_uninitialized_value(key, value_size, known_bit);
    });
}

std::optional<std::string_view> http_response_headers::header(std::string_view name) const& noexcept {
    if (const auto* const found =
            find_header_for_read(name, detail::classify_response_header_name(name))) {
        return found->value();
    }
    return std::nullopt;
}

void http_response_headers::rebuild_known_header_index() noexcept {
    known_header_bits_ = 0;
    const auto* const first = data();
    const auto* const last = first + size();
    for (auto* cursor_value = first; cursor_value != last; ++cursor_value) {
        const auto known_bit = detail::http_response_header_access::known_bit(*cursor_value);
        if (known_bit != 0) {
            record_known_header_index(known_bit, static_cast<std::size_t>(cursor_value - first));
        }
    }
}

void http_response_headers::header(std::string_view key, std::string_view value) {
    header(key, value, {});
}

void http_response_headers::content_type(http_content_type type) {
    publish_header(detail::static_content_type_header(type));
}

http_response_header& http_response_headers::replace_header(
    http_response_header& existing, http_response_header prepared) noexcept {
    const bool was_appended = detail::http_response_header_access::append(existing);
    release_header(existing);
    existing = prepared;
    return was_appended ? collapse_response_headers(existing, prepared.known_bit_) : existing;
}

void http_response_headers::publish_header(http_response_header prepared) {
    (void)upsert_header(prepared.name(), prepared.known_bit_, [&](http_response_header* existing) -> http_response_header& {
        if (existing != nullptr) {
            release_header(*existing);
            *existing = prepared;
            return *existing;
        }
        return append_header(prepared);
    });
}

void http_response_headers::header(
    std::string_view key, std::string_view value, http_response_header_options options) {
    const auto known_bit = validate_response_header_field(key, value);
    if (response_header_mode_appends(options.mode_)) {
        if (detail::response_header_append_forbidden(known_bit)) {
            throw std::invalid_argument("HTTP response header cannot be appended");
        }
        if (known_bit == detail::response_header_set_cookie) {
            upsert_set_cookie_header_validated(value);
        } else {
            append_header_validated(key, value, known_bit);
        }
    } else {
        set_header_validated(key, value, known_bit);
    }
}

void http_response_headers::remove_header(std::string_view key) {
    detail::validate_response_header_storage_size(key.size(), 0);
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    (void)remove_header_validated(key, detail::classify_response_header_name(key));
}

void http_response_headers::set_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit, bool stable_view) {
    (void)upsert_header(key, known_bit, [&](http_response_header* existing) -> http_response_header& {
        if (existing != nullptr) {
            assign(*existing, key, value, known_bit, stable_view);
            return *existing;
        }
        return add(key, value, known_bit, stable_view);
    });
}

void http_response_headers::append_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit) {
    auto& header_value = append_header_uninitialized_value(key, value.size(), known_bit);
    // Appending can move descriptors, but existing header byte blocks stay
    // stable, including any bytes borrowed by value.
    if (!value.empty()) {
        std::memcpy(detail::http_response_header_access::value_begin(header_value), value.data(), value.size());
    }
}

http_response_header& http_response_headers::append_header_uninitialized_value(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(key.size(), value_size);
    if (detail::response_header_append_forbidden(known_bit)) {
        throw std::invalid_argument("HTTP response header cannot be appended");
    }
    // The index cache intentionally points at the first occurrence. Mark that
    // retained slot as plural too, so a later plain set can detect multiplicity
    // in O(1) and collapse the field without scanning every normal update. Do
    // not set it until the new descriptor has been published: the new header
    // owns bytes before it may allocate the backing table, and a failed append
    // must leave the existing block exactly as it was.
    const auto* const existing = find_header_for_read(key, known_bit);
    const auto existing_index = existing == nullptr
                                    ? std::optional<std::size_t>{}
                                    : std::optional{static_cast<std::size_t>(existing - data())};
    const auto index = size();
    auto& header_value = add_uninitialized_value(key, value_size, known_bit);
    if (existing_index) {
        // Appending may move the descriptor table. Preserve an index, not a pointer,
        // to mark the existing entry without repeating the header lookup.
        detail::http_response_header_access::set_append(data()[*existing_index]);
    }
    // Mark the append flag so a later merge of this block keeps every appended
    // value instead of treating the field as single-valued and dropping all but the
    // first.
    detail::http_response_header_access::set_append(header_value);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response_headers::collapse_response_headers(
    http_response_header& retained, std::uint32_t known_bit) noexcept {
    const auto key = retained.name();
    auto* const first = data();
    auto* const last = first + size();
    auto* const retained_address = &retained;
    auto* collapsed_retained = retained_address;
    auto* write = first;
    for (auto* read = first; read != last; ++read) {
        if (read != retained_address && detail::response_header_matches(*read, key, known_bit)) {
            release_header(*read);
            continue;
        }
        if (read == retained_address) {
            collapsed_retained = write;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != last) {
        truncate(write);
    }
    rebuild_known_header_index();
    return *collapsed_retained;
}

bool http_response_headers::remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return false;
    }
    auto* const first = data();
    auto* const last = first + size();
    auto* write = first;
    bool removed = false;
    http_response_header retired_key_header;

    for (auto* read = first; read != last; ++read) {
        if (detail::response_header_matches(*read, key, known_bit)) {
            if (!removed) {
                retired_key_header = std::exchange(*read, http_response_header{});
                key = retired_key_header.name();
            } else {
                release_header(*read);
            }
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (!removed) {
        return false;
    }

    truncate(write);
    rebuild_known_header_index();
    release_header(retired_key_header);
    return true;
}

void http_response_headers::header_stable_view(std::string_view key, std::string_view value) {
    const auto known_bit = validate_response_header_field(key, value);
    set_header_validated(key, value, known_bit, true);
}

void http_response_headers::add_vary_token(std::string_view token) {
    // Vary is a list field (RFC 9110 section 12.5.5): wildcard and membership
    // are decided across every line, not only the indexed first occurrence.
    token = http_trim_ows(token);
    if (token.empty()) {
        return;
    }
    const bool wildcard_token = token == "*";
    if (!wildcard_token && !is_valid_http_header_name(token)) {
        throw std::invalid_argument("invalid HTTP Vary header token");
    }

    std::size_t value_size = token.size();
    constexpr auto max_value_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) - 4;
    detail::validate_response_header_storage_size(4, value_size);
    if ((known_header_bits_ & detail::response_header_vary) != 0) {
        for (const auto& header_value : *this) {
            if (detail::http_response_header_access::known_bit(header_value) != detail::response_header_vary) {
                continue;
            }
            const auto value = header_value.value();
            if (detail::http_has_token(value, "*") || (!wildcard_token && detail::http_has_token(value, token))) {
                return;
            }
            if (!wildcard_token && !http_trim_ows(value).empty()) {
                if (value.size() > max_value_size - value_size ||
                    max_value_size - value_size - value.size() < 2) {
                    throw std::length_error("HTTP response header is too large");
                }
                value_size += value.size() + 2;
            }
        }
    }

    if (wildcard_token) {
        header("Vary", "*");
        return;
    }
    if (value_size == token.size()) {
        header("Vary", token);
        return;
    }

    // Build independently: token may borrow a field that publication retires.
    auto prepared = make_uninitialized_header("Vary", value_size, detail::response_header_vary);
    auto* output = detail::http_response_header_access::value_begin(prepared);
    for (const auto& header_value : *this) {
        if (detail::http_response_header_access::known_bit(header_value) != detail::response_header_vary) {
            continue;
        }
        const auto value = header_value.value();
        if (!http_trim_ows(value).empty()) {
            std::memcpy(output, value.data(), value.size());
            output += value.size();
            *output++ = ',';
            *output++ = ' ';
        }
    }
    std::memcpy(output, token.data(), token.size());
    // Existing lines and token are validated; comma-joining preserves validity.
    publish_header(prepared);
}

}  // namespace ruvia
