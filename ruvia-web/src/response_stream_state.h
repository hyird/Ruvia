#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "http_content_coding.h"
#include "http_content_negotiation.h"
#include "http_error.h"
#include "http_io_buffer.h"
#include "http_request.h"
#include "http_response.h"
#include "http_response_body.h"
#include "http_response_stream.h"
#include "http_server.h"
#include "response_coding_policy.h"
#include "task.h"

namespace ruvia {
class context;
}

namespace ruvia {

namespace detail {

class response_stream_state final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commit_plan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<ended_type>(state_);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return std::holds_alternative<aborted_before_commit_type>(state_) || std::holds_alternative<aborted_after_commit_type>(state_);
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        if (const auto* value = std::get_if<body_open_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<ended_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<aborted_after_commit_type>(&state_)) {
            return &value->plan_;
        }
        return nullptr;
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    void bind_context(context* context_value) {
        if (!std::holds_alternative<unbound_type>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context_value == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<bound_type>(context_value);
    }

    void release_context() noexcept {
        if (std::holds_alternative<bound_type>(state_)) {
            state_.emplace<detached_type>();
        }
    }

    [[nodiscard]] context& bound_context() const {
        const auto* bound = std::get_if<bound_type>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return *bound->context_;
    }

    void mark_committed(http_response_stream_commit_plan plan) {
        if (committed() || aborted()) {
            throw std::logic_error("response stream is already committed");
        }
        switch (plan.head_disposition()) {
            case http_response_stream_head_disposition::body_open:
                state_.emplace<body_open_type>(plan);
                break;
            case http_response_stream_head_disposition::message_ended:
                state_.emplace<ended_type>(plan);
                break;
        }
    }

    void mark_ended() {
        if (ended()) {
            return;
        }
        if (auto* value = std::get_if<body_open_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<ended_type>(plan);
            return;
        }
        throw std::logic_error("response stream is not committed");
    }

    void mark_aborted() noexcept {
        if (aborted()) {
            return;
        }
        if (const auto* committed_plan = commit_plan()) {
            // A body-suppressed commit is already logically ended, but its head
            // can still fail during transport delivery. Preserve that commit.
            const auto plan = *committed_plan;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        state_.emplace<aborted_before_commit_type>();
    }

    void ensure_body_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<body_open_type>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    // Trailers are optional metadata (RFC 9110 6.5): framing that cannot carry
    // them discards them, so only a status that can never carry content is a
    // handler error here. The caller writes trailers only if trailers_available().
    void ensure_trailers_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commit_plan();
        if (plan == nullptr || !plan->status_permits_trailers()) {
            throw std::logic_error("response status does not permit trailers");
        }
    }

private:
    struct unbound_type final {};

    struct detached_type final {};

    struct bound_type final {
        explicit bound_type(context* bound_context) noexcept :
            context_(bound_context) {}

        context* context_;
    };

    struct body_open_type final {
        explicit body_open_type(http_response_stream_commit_plan commit_plan) noexcept :
            plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct ended_type final {
        explicit ended_type(http_response_stream_commit_plan commit_plan) noexcept :
            plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct aborted_before_commit_type final {};

    struct aborted_after_commit_type final {
        explicit aborted_after_commit_type(http_response_stream_commit_plan commit_plan) noexcept :
            plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    using state_type = std::variant<unbound_type, bound_type, detached_type, body_open_type, ended_type, aborted_before_commit_type, aborted_after_commit_type>;

    state_type state_;
};

}  // namespace detail
}  // namespace ruvia

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
    http_streaming_response_compression(worker_codecs& codecs, http_response_coding_selection selection, const compression_config* compression) noexcept :
        codecs_(codecs),
        selection_(selection),
        compression_(compression) {}
    http_streaming_response_compression(worker_codecs&&, http_response_coding_selection, const compression_config*) = delete;

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
        const auto decision = prepare_response_compression(selection_, request_method, response, http_response_compression_source::stream, compression_ != nullptr ? http_response_coding_availability::identity_and_compression : http_response_coding_availability::identity_only);
        if (decision != http_response_compression_decision::encode || !stream_length_allows_encoding(response)) {
            if (http_response_coding_fallback_forbidden(selection_, request_method, response, &request)) {
                throw http_error(http_not_acceptable_coding_error());
            }
            state_.emplace<inactive_type>();
            return;
        }

        response.apply_content_encoding(http_content_coding_token(selection_.coding()));
        state_.emplace<pending_type>();
    }

    // Binds encoder ownership to the protocol body plan. Body-suppressed
    // responses retain their selected representation metadata but never create
    // an encoder for bytes that the protocol will not send.
    void activate(http_response_body_plan body_plan) {
        if (std::holds_alternative<inactive_type>(state_)) {
            return;
        }
        const auto* pending = std::get_if<pending_type>(&state_);
        if (pending == nullptr) {
            throw std::logic_error("streaming response compression is not pending");
        }
        if (body_plan.body_suppressed()) {
            state_.emplace<inactive_type>();
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
    // lifecycle in the same terminal state instead of leaving pending/inactive
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
        http_clear_string_retaining_small(encoded_chunk_, retained_bytes);
    }

private:
    struct unprepared_type final {};
    // Identity output and protocol-suppressed bodies both need no encoder.
    struct inactive_type final {};
    struct pending_type final {};
    struct finished_type final {};
    struct failed_type final {};

    struct active_type final {
        explicit active_type(stream_encoder_lease lease) noexcept :
            lease_(std::move(lease)) {}

        stream_encoder_lease lease_;
    };

    using state_type = std::variant<unprepared_type, inactive_type, pending_type, finished_type, failed_type, active_type>;

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
    std::string encoded_chunk_;
    state_type state_;
};

}  // namespace ruvia::detail
