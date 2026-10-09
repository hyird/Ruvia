#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_content_encoder.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/error.h"

#include "server/http_response_compression.h"

namespace ruvia::detail {

// Owns the response-coding lifecycle shared by HTTP/1 and HTTP/2 stream sinks.
// The transport only submits output(); it never reconstructs whether a coding
// was selected, whether the body is suppressed, or whether an encoder exists.
class http_streaming_response_compression final {
public:
    http_streaming_response_compression(std::pmr::memory_resource* resource,
        http_response_coding_selection selection, http_response_coding_availability availability) noexcept
        : selection_(selection),
          availability_(availability),
          encoded_chunk_(pmr_resource_or_default(resource)) {}

    http_streaming_response_compression(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression& operator=(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression(http_streaming_response_compression&&) = delete;
    http_streaming_response_compression& operator=(http_streaming_response_compression&&) = delete;

    // Applies the representation headers before the protocol-owned stream head
    // is committed. A forbidden identity fallback is rejected here, while the
    // encoder itself is delayed until activate() has a final body plan.
    void prepare(http_known_method request_method, http_response& response, http_response_stream_kind kind) {
        if (!std::holds_alternative<unprepared_type>(state_)) {
            throw std::logic_error("streaming response compression is already prepared");
        }
        http_response_compression_source source;
        switch (kind) {
            case http_response_stream_kind::generic:
                source = http_response_compression_source::stream;
                break;
            case http_response_stream_kind::sse:
                source = http_response_compression_source::sse;
                break;
            default:
                throw std::invalid_argument("invalid response stream kind");
        }
        const auto decision = prepare_response_compression(
            selection_, request_method, response, source, availability_);
        if (decision != http_response_compression_decision::encode) {
            if (http_response_coding_fallback_forbidden(selection_, request_method, response)) {
                throw http_error({.status_ = ruvia::http_status::not_acceptable,
                    .code_ = "not_acceptable",
                    .message_ = "no acceptable response content coding"});
            }
            state_.emplace<identity_type>();
            return;
        }

        response.apply_content_encoding(http_content_coding_token(selection_.coding()));
        state_.emplace<pending_type>();
    }

    // Binds encoder ownership to the protocol body plan. Body-suppressed
    // responses retain their selected representation metadata but never create
    // an encoder for bytes that the protocol will not send.
    void activate(http_response_body_plan body_plan) {
        if (std::holds_alternative<identity_type>(state_) ||
            std::holds_alternative<suppressed_type>(state_)) {
            return;
        }
        const auto* pending = std::get_if<pending_type>(&state_);
        if (pending == nullptr) {
            throw std::logic_error("streaming response compression is not pending");
        }
        if (body_plan.body_suppressed()) {
            state_.emplace<suppressed_type>();
            return;
        }
        state_.emplace<active_type>(selection_.coding(), encoded_chunk_.get_allocator().resource());
    }

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<active_type>(state_);
    }

    // A stream-head transaction may prepare representation metadata before the
    // protocol layer accepts the head. If that later preparation fails, the
    // sink is terminal before any body byte can be retried; keep the encoder
    // lifecycle in the same terminal state instead of leaving Pending/Identity
    // behind for a second commit attempt.
    void abort() noexcept {
        encoded_chunk_.clear();
        state_.emplace<failed_type>();
    }

    void write(std::string_view input) {
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            encoded_chunk_.clear();
            throw std::logic_error("streaming response encoder is not active");
        }
        encoded_chunk_.clear();
        try {
            active_state->encoder_.write(input, encoded_chunk_, true);
        } catch (...) {
            abort();
            throw;
        }
    }

    void finish() {
        encoded_chunk_.clear();
        if (std::holds_alternative<finished_type>(state_)) {
            return;
        }
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            throw std::logic_error("streaming response encoder is not active");
        }
        try {
            active_state->encoder_.finish(encoded_chunk_);
            state_.emplace<finished_type>();
        } catch (...) {
            abort();
            throw;
        }
    }

    [[nodiscard]] std::string_view output() const& noexcept {
        return encoded_chunk_;
    }
    std::string_view output() const&& = delete;

private:
    struct unprepared_type final {};
    struct identity_type final {};
    struct pending_type final {};
    struct suppressed_type final {};
    struct finished_type final {};
    struct failed_type final {};

    struct active_type final {
        active_type(http_content_coding coding, std::pmr::memory_resource* resource)
            : encoder_(coding, resource) {}

        http_content_encoder encoder_;
    };

    using state_type = std::variant<unprepared_type, identity_type, pending_type, suppressed_type, finished_type, failed_type, active_type>;

    http_response_coding_selection selection_;
    http_response_coding_availability availability_;
    std::pmr::string encoded_chunk_;
    state_type state_;
};

}  // namespace ruvia::detail
