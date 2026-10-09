#include "http3/http3_client_request_write.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_request_writer.h"

#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"
#include "http3/http3_client_sans_io_session_engine.h"

namespace ruvia::detail {

// variant's in-place constructor has no noexcept specification. The commit
// relies on the selected alternative's constructor, without moving the cursor.
static_assert(std::is_nothrow_constructible_v<http3_client_request_write,
    http3_client_request_write::prepared_tag_type, std::pmr::memory_resource*, http_client_request_storage&&,
    std::pmr::string&&, std::pmr::string&&, std::pmr::vector<char>&&, http3_data_write_plan,
    http3_field_section_limits>);

http3_client_request_write::http3_client_request_write(prepared_tag_type,
    std::pmr::memory_resource* resource, http_client_request_storage&& request,
    std::pmr::string&& scheme,
    std::pmr::string&& authority, std::pmr::vector<char>&& headers,
    http3_data_write_plan data_plan, http3_field_section_limits limits) noexcept
    : worker_pool_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      field_limits_(limits),
      request_(std::move(request)),
      scheme_(std::move(scheme)),
      authority_(std::move(authority)),
      headers_(std::move(headers)),
      data_plan_(std::move(data_plan)) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(request_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(scheme_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(authority_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(headers_)>);
    static_assert(std::is_nothrow_move_constructible_v<http3_data_write_plan>);
    static_assert(std::is_nothrow_constructible_v<decltype(data_plan_), http3_data_write_plan&&>);
}

http3_client_request_write& http3_client_request_write::require_no_outstanding_segment(
    http3_client_request_write& other) {
    if (other.offered_) {
        throw std::logic_error("cannot move an HTTP/3 request cursor with an outstanding span");
    }
    return other;
}

http3_client_request_write::http3_client_request_write(http3_client_request_write&& other)
    : worker_pool_(require_no_outstanding_segment(other).worker_pool_),
      field_limits_(other.field_limits_),
      request_(std::move(other.request_)),
      scheme_(std::move(other.scheme_), worker_pool_),
      authority_(std::move(other.authority_), worker_pool_),
      headers_(std::move(other.headers_), worker_pool_),
      data_plan_(std::move(other.data_plan_)),
      chunk_(other.chunk_),
      segment_offset_(other.segment_offset_),
      body_offset_(other.body_offset_),
      state_(other.state_),
      chunk_pending_(other.chunk_pending_),
      request_taken_(other.request_taken_) {
    if (chunk_pending_) {
        chunk_.payload_ = (request_.output() != nullptr ? std::string_view(request_.output()->chunk_) : request_.body()).substr(body_offset_, chunk_.payload_.size());
    }
    other.state_ = state_type::failed;
    other.request_taken_ = true;
}

std::optional<http_client_request_storage> http3_client_request_write::take_request_after_retirement() {
    if (request_taken_) {
        return std::nullopt;
    }
    state_ = state_type::failed;
    offered_ = false;
    chunk_pending_ = false;
    chunk_ = {};
    data_plan_.reset();
    request_taken_ = true;
    return std::optional<http_client_request_storage>(std::in_place, std::move(request_));
}

std::variant<http3_client_request_write, http3_client_request_write::error_type>
http3_client_request_write::create(http_client_request_storage&& request, std::string_view scheme,
    std::string_view authority, std::pmr::memory_resource* worker_pool,
    http3_field_section_limits limits) noexcept {
    auto* resource = worker_pool != nullptr ? worker_pool : std::pmr::get_default_resource();
    try {
        std::optional<http_client_request_storage> normalized_request;
        const http_client_request_storage* prepared_request = &request;
        if (request.resource() != resource) {
            // into_resource copies when resources differ and leaves its source
            // untouched if any normalization allocation fails.
            normalized_request.emplace(std::move(request).into_resource(resource));
            prepared_request = &*normalized_request;
        }

        std::pmr::string owned_scheme(scheme, resource);
        std::pmr::string owned_authority(authority, resource);
        std::pmr::vector<http3_field_section_field_view> fields(resource);
        fields.reserve(http_client_request_storage_access::headers(*prepared_request).size());
        for (const auto& field : http_client_request_storage_access::headers(*prepared_request)) {
            fields.push_back({field.name_, field.value_, false});
        }
        const auto* upload = prepared_request->upload();
        if (upload != nullptr && upload->config_.expectation_ == http_client_request_expectation::continue_value) {
            if (std::ranges::any_of(fields, [](const auto& field) { return field.name_ == "expect"; })) {
                return error_type::invalid_request;
            }
            fields.push_back({"expect", "100-continue", false});
        }
        const auto target = prepared_request->target();
        const bool connect = prepared_request->method() == "CONNECT";
        if (prepared_request->is_tunnel()) {
            owned_authority.assign(prepared_request->tunnel_authority());
        }
        // Request framing is method-independent (RFC 9110 section 9.3.2).
        // HEAD suppresses response payload, not explicitly supplied request
        // content. The caller must have established that its origin supports
        // HEAD content; requests without supplied content remain bodyless.
        const bool sends_body = http_client_request_storage_access::has_body(*prepared_request);
        // HTTP owns framing validation and automatic field generation. A
        // bodyless request still has a known zero-byte body for validating an
        // explicit Content-Length, but does not acquire Content-Length: 0.
        const std::optional<std::uint64_t> body_length = connect             ? std::nullopt
                                                         : upload != nullptr ? upload->config_.content_length_
                                                         : sends_body        ? std::optional<std::uint64_t>(prepared_request->body().size())
                                                                             : std::optional<std::uint64_t>(0);
        const bool emit_content_length = sends_body ||
                                         (upload != nullptr && upload->config_.content_length_.has_value());
        // Extended CONNECT cannot be encoded before received SETTINGS authorize
        // it. Keep an unbounded cursor with no provisional wire head; the sole
        // connection driver encodes and validates it before the first write.
        auto encoded = [&]() -> std::variant<http3_client_request_head, http3_client_request_head_failure> {
            if (prepared_request->is_tunnel() && !prepared_request->tunnel_protocol().empty()) {
                return http3_client_request_head(resource);
            }
            return encode_http3_client_request_head({.method_ = prepared_request->method(),
                                                        .scheme_ = connect ? std::string_view{} : std::string_view(owned_scheme),
                                                        .authority_ = connect && !prepared_request->is_tunnel() ? target : std::string_view(owned_authority),
                                                        .path_ = connect && !prepared_request->is_tunnel() ? std::string_view{} : target,
                                                        .fields_ = fields,
                                                        .body_length_ = body_length,
                                                        .emit_content_length_ = emit_content_length},
                limits, resource);
        }();
        if (encoded.index() != 0) {
            return error_type::request_encoding;
        }
        // Let HTTP validate the CONNECT authority-form first; this client then
        // reports its independent lack of a plain CONNECT tunnel capability.
        if (connect && !prepared_request->is_tunnel()) {
            return error_type::unsupported_tunnel;
        }
        if (std::get<0>(encoded).field_section_.size() > std::numeric_limits<std::size_t>::max() - http3_frame_header_max_bytes) {
            return error_type::request_encoding;
        }
        std::pmr::vector<char> headers(resource);
        headers.resize(http3_frame_header_max_bytes + std::get<0>(encoded).field_section_.size());
        auto frame = encode_http3_frame_header(headers,
            static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(encoded).field_section_.size());
        if ((frame.index() != 0)) {
            return error_type::request_encoding;
        }
        headers.resize(std::get<0>(frame) + std::get<0>(encoded).field_section_.size());
        std::copy(std::get<0>(encoded).field_section_.begin(), std::get<0>(encoded).field_section_.end(),
            headers.begin() + static_cast<std::ptrdiff_t>(std::get<0>(frame)));

        http3_data_write_plan data_plan(std::get<0>(encoded).body_plan_);
        if (normalized_request) {
            // All fallible work is complete. Retire the original request only
            // at this commit point; the cursor owns its normalized copy.
            http_client_request_storage retired(std::move(request));
            return std::variant<http3_client_request_write, error_type>(std::in_place_index<0>, prepared_tag_type{}, resource,
                std::move(*normalized_request), std::move(owned_scheme), std::move(owned_authority),
                std::move(headers), std::move(data_plan), limits);
        }
        return std::variant<http3_client_request_write, error_type>(std::in_place_index<0>, prepared_tag_type{}, resource,
            std::move(request), std::move(owned_scheme), std::move(owned_authority),
            std::move(headers), std::move(data_plan), limits);
    } catch (const std::bad_alloc&) {
        return error_type::out_of_memory;
    } catch (...) {
        return error_type::request_encoding;
    }
}

bool http3_client_request_write::prepare_connection_head(std::uint64_t stream_id, http3_client_sans_io_session_engine& engine) {
    if (state_ != state_type::headers || offered_ || segment_offset_ != 0) {
        return false;
    }
    if (engine.peer_settings() && engine.peer_settings()->max_field_section_size_) {
        field_limits_.max_decoded_bytes_ = static_cast<std::size_t>(std::min<std::uint64_t>(field_limits_.max_decoded_bytes_, *engine.peer_settings()->max_field_section_size_));
    }
    if (!request_.is_tunnel() && (!engine.peer_settings() || (engine.peer_settings()->qpack_max_table_capacity_ == 0 && !engine.peer_settings()->max_field_section_size_))) {
        return true;
    }
    std::pmr::vector<http3_field_section_field_view> fields(worker_pool_);
    for (const auto& field : http_client_request_storage_access::headers(request_)) {
        fields.push_back({field.name_, field.value_, false});
    }
    if (request_.upload() != nullptr && request_.upload()->config_.expectation_ == http_client_request_expectation::continue_value) {
        fields.push_back({"expect", "100-continue", false});
    }
    const bool connect = request_.method() == "CONNECT";
    const auto length = connect                                                  ? std::nullopt
                        : request_.upload() != nullptr                           ? request_.upload()->config_.content_length_
                        : http_client_request_storage_access::has_body(request_) ? std::optional<std::uint64_t>{request_.body().size()}
                                                                                 : std::optional<std::uint64_t>{0};
    const auto encoded = engine.encode_request_head(stream_id, {.method_ = request_.method(),
                                                                   .scheme_ = connect && request_.tunnel_protocol().empty() ? std::string_view{} : std::string_view(scheme_),
                                                                   .authority_ = connect && !request_.is_tunnel() ? request_.target() : std::string_view(authority_),
                                                                   .path_ = connect && !request_.is_tunnel() ? std::string_view{} : request_.target(),
                                                                   .fields_ = fields,
                                                                   .body_length_ = length,
                                                                   .emit_content_length_ = http_client_request_storage_access::has_body(request_) ||
                                                                                           (request_.upload() != nullptr && request_.upload()->config_.content_length_.has_value()),
                                                                   .protocol_ = request_.tunnel_protocol(),
                                                                   .peer_enable_connect_protocol_ = engine.peer_settings() && engine.peer_settings()->enable_connect_protocol_});
    if ((encoded.index() != 0)) {
        return false;
    }
    headers_.resize(http3_frame_header_max_bytes + std::get<0>(encoded).field_section_.size());
    const auto prefix = encode_http3_frame_header(headers_, static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(encoded).field_section_.size());
    if ((prefix.index() != 0)) {
        return false;
    }
    headers_.resize(std::get<0>(prefix) + std::get<0>(encoded).field_section_.size());
    std::copy(std::get<0>(encoded).field_section_.begin(), std::get<0>(encoded).field_section_.end(), headers_.begin() + static_cast<std::ptrdiff_t>(std::get<0>(prefix)));
    return true;
}

std::variant<http3_client_request_write::segment_type, http3_client_request_write::error_type>
http3_client_request_write::next() noexcept {
    if (state_ == state_type::finished || state_ == state_type::failed) {
        return error_type::invalid_state;
    }
    if (state_ == state_type::fin) {
        return segment_type{};
    }
    if (state_ == state_type::data_header && !chunk_pending_) {
        if (auto result_value = prepare_data(); (result_value.index() != 0)) {
            return std::get<1>(result_value);
        }
        if (state_ == state_type::fin || (state_ == state_type::data_header && !chunk_pending_)) {
            return segment_type{};
        }
    }
    const auto segment = active_segment();
    offered_ = !segment.empty();
    return segment;
}

http3_client_request_write::segment_type http3_client_request_write::active_segment() const noexcept {
    switch (state_) {
        case state_type::headers:
        case state_type::trailers:
            return segment_type(headers_).subspan(segment_offset_);
        case state_type::data_header:
            return segment_type(chunk_.frame_header_.data(), chunk_.frame_header_size_).subspan(segment_offset_);
        case state_type::data_body:
            return chunk_.payload_.subspan(segment_offset_);
        default:
            return {};
    }
}

std::variant<std::monostate, http3_client_request_write::error_type> http3_client_request_write::prepare_data() noexcept {
    if (!data_plan_ || state_ != state_type::data_header || chunk_pending_) {
        return error_type::invalid_state;
    }
    auto* upload = request_.output();
    if (upload != nullptr) {
        if (upload->stopped_ || (request_.tunnel() != nullptr ? !request_.tunnel()->accepted_ : !request_.upload()->content_released_) || (!upload->chunk_ready_ && !upload->end_requested_)) {
            return {};
        }
        if (upload->end_requested_ && !upload->chunk_ready_) {
            auto ending = data_plan_->plan_chunk(std::span<const char>{}, true);
            if ((ending.index() != 0)) {
                return fail_plan();
            }
            chunk_ = std::get<0>(ending);
            chunk_pending_ = true;
            segment_offset_ = 0;
            if (request_.upload() != nullptr && !request_.upload()->trailers_.empty()) {
                return prepare_trailers();
            }
            state_ = state_type::fin;
            return {};
        }
    }
    const auto body = upload != nullptr ? std::string_view(upload->chunk_) : request_.body();
    if (body_offset_ > body.size()) {
        return fail_plan();
    }
    const auto remaining = body.size() - body_offset_;
    const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, http3_var_int_max));
    const bool finishing = upload == nullptr && size == remaining;
    auto planned = data_plan_->plan_chunk(body.substr(body_offset_, size), finishing);
    if ((planned.index() != 0)) {
        return fail_plan();
    }
    chunk_ = std::get<0>(planned);
    chunk_pending_ = true;
    segment_offset_ = 0;
    if (!chunk_.emits_data_) {
        state_ = state_type::fin;
    }
    return {};
}

std::variant<std::monostate, http3_client_request_write::error_type>
http3_client_request_write::acknowledge(std::size_t count) noexcept {
    if (!offered_ || state_ == state_type::finished || state_ == state_type::failed || state_ == state_type::fin) {
        return error_type::invalid_state;
    }
    const auto segment = active_segment();
    if (count > segment.size()) {
        return error_type::excessive_acknowledgement;
    }
    if (count == 0) {
        return {};
    }
    segment_offset_ += count;
    // segment is already the unacknowledged suffix. Compare the accepted
    // amount with that suffix, not the cumulative offset from its beginning.
    if (count != segment.size()) {
        return {};
    }
    offered_ = false;
    segment_offset_ = 0;
    if (state_ == state_type::headers) {
        state_ = state_type::data_header;
    } else if (state_ == state_type::trailers) {
        state_ = state_type::fin;
    } else if (state_ == state_type::data_header) {
        state_ = state_type::data_body;
    } else if (state_ == state_type::data_body) {
        if (chunk_.finishing_) {
            state_ = state_type::fin;
        } else {
            const auto bytes_value = chunk_.payload_.size();
            if ((data_plan_->commit_payload(bytes_value, false).index() != 0)) {
                return fail_plan();
            }
            if (auto* upload = request_.output()) {
                upload->acknowledge_chunk();
                body_offset_ = 0;
            } else {
                body_offset_ += bytes_value;
            }
            chunk_pending_ = false;
            state_ = state_type::data_header;
        }
    } else {
        return error_type::invalid_state;
    }
    return {};
}

std::variant<std::monostate, http3_client_request_write::error_type>
http3_client_request_write::acknowledge_fin(bool successful) noexcept {
    if (state_ != state_type::fin || !chunk_pending_ || !data_plan_) {
        return error_type::invalid_state;
    }
    if (!successful) {
        state_ = state_type::failed;
        chunk_pending_ = false;
        return {};
    }
    if ((data_plan_->commit_payload(chunk_.payload_.size(), true).index() != 0)) {
        return fail_plan();
    }
    state_ = state_type::finished;
    if (auto* upload = request_.output()) {
        upload->finish();
    }
    chunk_pending_ = false;
    return {};
}

std::variant<std::monostate, http3_client_request_write::error_type> http3_client_request_write::prepare_trailers() noexcept {
    try {
        std::pmr::vector<http3_field_section_field_view> fields(worker_pool_);
        for (const auto& field : request_.upload()->trailers_) {
            fields.push_back({field.name(), field.value(), false});
        }
        auto encoded = encode_http3_request_trailers(fields, field_limits_, worker_pool_);
        if ((encoded.index() != 0)) {
            return fail_plan();
        }
        std::pmr::vector<char> frame(worker_pool_);
        frame.resize(http3_frame_header_max_bytes + (std::get<0>(encoded)).size());
        const auto prefix = encode_http3_frame_header(frame, static_cast<std::uint64_t>(http3_frame_type::headers), (std::get<0>(encoded)).size());
        if ((prefix.index() != 0)) {
            return fail_plan();
        }
        frame.resize(std::get<0>(prefix) + (std::get<0>(encoded)).size());
        std::copy((std::get<0>(encoded)).begin(), (std::get<0>(encoded)).end(), frame.begin() + static_cast<std::ptrdiff_t>(std::get<0>(prefix)));
        headers_.swap(frame);
        state_ = state_type::trailers;
        return {};
    } catch (...) {
        return fail_plan();
    }
}

bool http3_client_request_write::waiting_for_content() const noexcept {
    const auto* output = request_.output();
    return output != nullptr && state_ == state_type::data_header && !chunk_pending_ &&
           ((request_.tunnel() != nullptr ? !request_.tunnel()->accepted_ : !request_.upload()->content_released_) || (!output->chunk_ready_ && !output->end_requested_));
}
void http3_client_request_write::stop_sending() noexcept {
    state_ = state_type::finished;
    offered_ = false;
    chunk_pending_ = false;
    data_plan_.reset();
}

bool http3_client_request_write::fin_ready() const noexcept {
    return state_ == state_type::fin;
}
bool http3_client_request_write::finished() const noexcept {
    return state_ == state_type::finished;
}
bool http3_client_request_write::failed() const noexcept {
    return state_ == state_type::failed;
}

http_known_method http3_client_request_write::known_method() const noexcept {
    return classify_http_method(request_.method());
}

std::variant<std::monostate, http3_client_request_write::error_type> http3_client_request_write::fail_plan() noexcept {
    state_ = state_type::failed;
    chunk_pending_ = false;
    return error_type::data_plan;
}

}  // namespace ruvia::detail
