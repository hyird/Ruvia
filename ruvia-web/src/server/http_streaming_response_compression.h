#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_content_encoder.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/error.h"
#include "ruvia/web/server_config.h"

#include "server/http_response_compression.h"
#include "server/worker_codecs.h"

namespace ruvia::detail {

// Owns the response-coding lifecycle of HTTP/1 and HTTP/2 stream sinks.
// The transport only delivers encoded slices; it never reconstructs whether a
// coding was selected, whether the body is suppressed, or whether an encoder
// exists. The encoder itself is borrowed from the worker's codecs for the
// stream's lifetime, not constructed per response.
class http_streaming_response_compression final {
public:
    // Application input encoded per step. It bounds the encoded buffer and the
    // codec work between two transport writes independently of the size of
    // one application write.
    static constexpr std::size_t input_slice_bytes = 16 * 1024;

    // `compression` is the server's compression policy, or nullptr when the
    // server compresses nothing.
    http_streaming_response_compression(std::pmr::memory_resource* resource, worker_codecs& codecs,
        http_response_coding_selection selection, const compression_config* compression) noexcept
        : codecs_(codecs),
          selection_(selection),
          compression_(compression),
          encoded_chunk_(pmr_resource_or_default(resource)) {}
    http_streaming_response_compression(std::pmr::memory_resource*, worker_codecs&&,
        http_response_coding_selection, const compression_config*) = delete;

    http_streaming_response_compression(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression& operator=(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression(http_streaming_response_compression&&) = delete;
    http_streaming_response_compression& operator=(http_streaming_response_compression&&) = delete;

    // Applies the representation headers before the protocol-owned stream head
    // is committed. A forbidden identity fallback, including a negotiation the
    // selection already reports as failed, is rejected here, while the encoder
    // itself is delayed until activate() has a final body plan.
    void prepare(const http_request& request, http_response& response) {
        if (!std::holds_alternative<unprepared_type>(state_)) {
            throw std::logic_error("streaming response compression is already prepared");
        }
        const auto request_method = request.known_method();
        const auto decision = prepare_response_compression(selection_, request_method, response,
            http_response_compression_source::stream,
            compression_ != nullptr ? http_response_coding_availability::identity_and_compression
                                    : http_response_coding_availability::identity_only);
        if (decision != http_response_compression_decision::encode || !stream_length_allows_encoding(response)) {
            if (http_response_coding_fallback_forbidden(selection_, request_method, response, &request)) {
                throw http_error(http_not_acceptable_coding_error());
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
        state_.emplace<active_type>(codecs_.acquire_stream_encoder(selection_.coding()));
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

    // The one slicing loop of both protocols: encodes one application write in
    // input_slice_bytes steps and hands every non-empty encoded slice to
    // `deliver` (the sink's framed write, returning task<void>). Only the last
    // slice flushes, so each application write ends with exactly one codec
    // flush, which keeps SSE events prompt without flushing every slice.
    template <typename deliver_type>
    task<void> write(std::string_view chunk, deliver_type deliver) {
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const auto count = (std::min)(input_slice_bytes, chunk.size() - offset);
            const auto slice = chunk.substr(offset, count);
            offset += count;
            encode_slice(slice, offset == chunk.size());
            if (!encoded_chunk_.empty()) {
                co_await deliver(std::string_view(encoded_chunk_));
            }
            release_output();
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
            active_state->lease_.encoder().finish(encoded_chunk_);
            state_.emplace<finished_type>();
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    [[nodiscard]] std::string_view output() const& noexcept {
        return encoded_chunk_;
    }
    std::string_view output() const&& = delete;

    // A sent chunk no longer has a transport borrower. Keep ordinary stream
    // buffers reusable without retaining unusually large writes for the life
    // of a long-running handler.
    void release_output() {
        constexpr std::size_t retained_bytes = 64 * 1024;
        http_clear_pmr_string_retaining_small(encoded_chunk_, retained_bytes);
    }

private:
    struct unprepared_type final {};
    struct identity_type final {};
    struct pending_type final {};
    struct suppressed_type final {};
    struct finished_type final {};
    struct failed_type final {};

    struct active_type final {
        explicit active_type(stream_encoder_lease lease) noexcept
            : lease_(std::move(lease)) {}

        stream_encoder_lease lease_;
    };

    using state_type = std::variant<unprepared_type, identity_type, pending_type, suppressed_type, finished_type, failed_type, active_type>;

    // A length the head declares sizes the stream like a buffered body; an
    // undeclared length leaves the decision to the negotiated coding.
    [[nodiscard]] bool stream_length_allows_encoding(const http_response& response) const noexcept {
        const auto declared = declared_content_length(response);
        return !declared.has_value() || declared_length_permits_encoding(selection_, *compression_, *declared);
    }

    void encode_slice(std::string_view input, bool flush) {
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            encoded_chunk_.clear();
            throw std::logic_error("streaming response encoder is not active");
        }
        encoded_chunk_.clear();
        try {
            active_state->lease_.encoder().write(input, encoded_chunk_, flush);
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    // A codec that failed mid-stream is never returned to the worker pool.
    void fail_encoder(active_type& active_state) noexcept {
        active_state.lease_.discard();
        abort();
    }

    worker_codecs& codecs_;
    http_response_coding_selection selection_;
    const compression_config* compression_;
    std::pmr::string encoded_chunk_;
    state_type state_;
};

}  // namespace ruvia::detail
