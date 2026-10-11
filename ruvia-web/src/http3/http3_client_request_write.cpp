#include "http3/http3_client_request_write.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_request_writer.h"

#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"
#include "http3/http3_client_sans_io_session_engine.h"

namespace ruvia::detail {
namespace {
constexpr std::string_view request_scheme = "https";

// The single request-to-head mapping for both the provisional static-QPACK
// head and the connection-bound re-encoding. Plain CONNECT carries its target
// as authority; a tunnel carries its own authority; everything else uses the
// connection's origin authority. HTTP owns Content-Length and Expect
// generation from the declared body length and expectation.
http3_client_request_head_view request_head_view(const http_client_request_storage& request,
    std::string_view origin_authority, std::span<const http3_field_section_field_view> fields) noexcept {
    const bool connect = request.method() == "CONNECT";
    const bool plain_connect = connect && !request.is_tunnel();
    const auto* upload = request.upload();
    const bool sends_body = http_client_request_storage_access::has_body(request);
    // Request framing is method-independent (RFC 9110 section 9.3.2). HEAD
    // suppresses response payload, not explicitly supplied request content. A
    // bodyless request has a known zero-byte body but acquires no
    // Content-Length: 0.
    return {.method_ = request.method(),
        .scheme_ = connect && request.tunnel_protocol().empty() ? std::string_view{} : request_scheme,
        .authority_ = plain_connect         ? request.target()
                      : request.is_tunnel() ? request.tunnel_authority()
                                            : origin_authority,
        .path_ = plain_connect ? std::string_view{} : request.target(),
        .fields_ = fields,
        .body_length_ = connect             ? std::nullopt
                        : upload != nullptr ? upload->config_.content_length_
                        : sends_body        ? std::optional<std::uint64_t>(request.body().size())
                                            : std::optional<std::uint64_t>(0),
        .emit_content_length_ = sends_body || (upload != nullptr && upload->config_.content_length_.has_value()),
        .expectation_ = upload != nullptr ? upload->config_.expectation_ : http_client_request_expectation::none,
        .protocol_ = request.tunnel_protocol()};
}

std::pmr::vector<http3_field_section_field_view> request_fields(
    const http_client_request_storage& request, std::pmr::memory_resource* resource) {
    const auto& headers = http_client_request_storage_access::headers(request);
    std::pmr::vector<http3_field_section_field_view> fields(resource);
    fields.reserve(headers.size());
    for (const auto& field : headers) {
        fields.push_back({field.name_, field.value_, false});
    }
    return fields;
}
}  // namespace

// variant's in-place constructor has no noexcept specification. The commit
// relies on the selected alternative's constructor, without moving the cursor.
static_assert(std::is_nothrow_constructible_v<http3_client_request_write,
    http3_client_request_write::prepared_tag_type, std::pmr::memory_resource*, http_client_request_storage&&,
    std::string_view, std::optional<http3_headers_frame>&&, http3_data_write_plan, http3_field_section_limits>);

http3_client_request_write::http3_client_request_write(prepared_tag_type,
    std::pmr::memory_resource* resource, http_client_request_storage&& request, std::string_view authority,
    std::optional<http3_headers_frame>&& headers, http3_data_write_plan data_plan,
    http3_field_section_limits limits) noexcept
    : worker_pool_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      field_limits_(limits),
      request_(std::move(request)),
      authority_(authority),
      headers_(std::move(headers)),
      data_plan_(std::move(data_plan)) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(request_)>);
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
      authority_(other.authority_),
      headers_(std::move(other.headers_)),
      engine_(std::exchange(other.engine_, nullptr)),
      stream_id_(other.stream_id_),
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
    engine_ = nullptr;
    return std::optional<http_client_request_storage>(std::in_place, std::move(request_));
}

std::pmr::vector<http3_field_section_field_view> http3_client_request_write::request_fields() const {
    return detail::request_fields(request_, worker_pool_);
}

std::variant<http3_client_request_write, http3_client_request_write::error_type>
http3_client_request_write::create(http_client_request_storage&& request, std::string_view authority,
    std::pmr::memory_resource* worker_pool, http3_field_section_limits limits) noexcept {
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

        const bool plain_connect = prepared_request->method() == "CONNECT" && !prepared_request->is_tunnel();
        std::optional<http3_headers_frame> headers;
        http3_client_request_body_plan body_plan{};
        // Extended CONNECT cannot be encoded before received SETTINGS authorize
        // it. Keep an unbounded cursor with no provisional wire head; the sole
        // connection driver encodes and validates it before the first write.
        if (!prepared_request->is_tunnel() || prepared_request->tunnel_protocol().empty()) {
            const auto fields = detail::request_fields(*prepared_request, resource);
            auto encoded = encode_http3_client_request_head(
                request_head_view(*prepared_request, authority, fields), limits, resource);
            if (encoded.index() != 0) {
                return error_type::request_encoding;
            }
            body_plan = std::get<0>(encoded).body_plan_;
            headers.emplace(std::move(std::get<0>(encoded).headers_));
        }
        // Let HTTP validate the CONNECT authority-form first; this client then
        // reports its independent lack of a plain CONNECT tunnel capability.
        if (plain_connect) {
            return error_type::unsupported_tunnel;
        }

        http3_data_write_plan data_plan(body_plan);
        if (normalized_request) {
            // All fallible work is complete. Retire the original request only
            // at this commit point; the cursor owns its normalized copy.
            http_client_request_storage retired(std::move(request));
            return std::variant<http3_client_request_write, error_type>(std::in_place_index<0>, prepared_tag_type{}, resource,
                std::move(*normalized_request), authority, std::move(headers), std::move(data_plan), limits);
        }
        return std::variant<http3_client_request_write, error_type>(std::in_place_index<0>, prepared_tag_type{}, resource,
            std::move(request), authority, std::move(headers), std::move(data_plan), limits);
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
    const auto& peer = engine.effective_peer_settings();
    if (!request_.is_tunnel() && (!peer || (peer->qpack_max_table_capacity_ == 0 && !peer->max_field_section_size_))) {
        engine_ = &engine;
        stream_id_ = stream_id;
        return true;
    }
    const auto fields = request_fields();
    auto view = request_head_view(request_, authority_, fields);
    auto encoded = engine.encode_request_head(stream_id, view, field_limits_);
    if (encoded.index() != 0) {
        return false;
    }
    headers_.emplace(std::move(std::get<0>(encoded).headers_));
    engine_ = &engine;
    stream_id_ = stream_id;
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
            return headers_ ? headers_->frame().subspan(segment_offset_) : segment_type{};
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
    if (engine_ == nullptr) {
        return fail_plan();
    }
    try {
        std::pmr::vector<http3_field_section_field_view> fields(worker_pool_);
        for (const auto& field : request_.upload()->trailers_) {
            fields.push_back({field.name(), field.value(), false});
        }
        auto encoded = engine_->encode_request_trailers(stream_id_, fields, field_limits_);
        if ((encoded.index() != 0)) {
            return fail_plan();
        }
        headers_.emplace(std::move(std::get<0>(encoded)));
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
    engine_ = nullptr;
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
