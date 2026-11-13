#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio.hpp>
#include <asio/buffer.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http1_chunked_framing.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_head_buffer.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/context.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/web_worker.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "context/http_interim_response_output.h"
#include "http/http_socket_tunnel_transport.h"
#include "http/http_tunnel_session.h"
#include "http/request_body_loader.h"
#include "http2/http2_sans_io_session.h"
#include "integration/operation_interrupt.h"
#include "router/route_table.h"
#include "server/connection_scanner.h"
#include "server/http_buffered_response.h"
#include "server/http_connection_close.h"
#include "server/http_connection_state.h"
#include "server/http_request_admission.h"
#include "server/http_response_compression.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_response_writer.h"
#include "server/http_server_options.h"
#include "server/http_upgrade_route.h"
#include "server/inbound_buffer_budget.h"
#include "server/inbound_socket_stream.h"
#include "server/inbound_transport_error.h"
#include "server/multiplexed_stream_signal.h"
#include "server/response_file_source.h"
#include "server/worker_connections.h"

namespace ruvia::detail {

// Connection-private ownership of the Web product's request-count policy.
// HTTP still owns persistence and response framing; this connection state keeps
// an optional saturating remaining-response budget, contributes one typed close policy
// before a streamed head is committed, and records each completed route once.
// A stopping worker (which closes every connection) never allows reuse.
// Once the peer has ended its request direction behind pipelined requests,
// the sequence is those already received: they are served, then the
// connection closes without reading the transport again.
class http1_request_sequence final {
public:
    http1_request_sequence(
        std::optional<std::size_t> max_requests, const http_server_worker_state& worker_state)
        : requests_until_close_(max_requests),
          worker_state_(worker_state) {
        if (max_requests.has_value() && *max_requests == 0) {
            throw std::invalid_argument(
                "configured requests-per-connection limit must be greater than zero");
        }
    }

    http1_request_sequence(std::size_t, const http_server_worker_state&) = delete;

    http1_request_sequence(const http1_request_sequence&) = delete;
    http1_request_sequence& operator=(const http1_request_sequence&) = delete;
    http1_request_sequence(http1_request_sequence&&) = delete;
    http1_request_sequence& operator=(http1_request_sequence&&) = delete;

    [[nodiscard]] http1_close_policy next_response_close_policy() const noexcept {
        return budget_ends_with_next_response() || !http_server_worker_running(worker_state_)
                   ? http1_close_policy::close_after_response
                   : http1_close_policy::allow_reuse;
    }

    // Buffered response bytes have not been committed yet, so the request
    // budget may still tighten the protocol plan before Connection fields are
    // finalized.
    [[nodiscard]] http1_request_connection_plan complete_uncommitted_response(
        http1_request_connection_plan connection_plan) noexcept {
        const auto close_policy = next_response_close_policy();
        record_completion();
        return close_policy == http1_close_policy::close_after_response ? connection_plan.require_close()
                                                                        : connection_plan;
    }

    // A streamed head already carries the pre-commit close policy. Once bytes
    // are committed the plan cannot be tightened; fail fast if a caller tries
    // to complete a limit-ending response whose wire plan still permits reuse.
    void complete_committed_response(http1_request_connection_plan connection_plan) noexcept {
        if (budget_ends_with_next_response() &&
            connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        record_completion();
    }

    // A streamed response observed the peer's request-direction end (RFC 9112
    // section 9.6) behind requests it retained for the session.
    void mark_request_input_ended() noexcept {
        request_input_ended_ = true;
    }

    [[nodiscard]] bool request_input_ended() const noexcept {
        return request_input_ended_;
    }

private:
    [[nodiscard]] bool budget_ends_with_next_response() const noexcept {
        return requests_until_close_.has_value() && *requests_until_close_ == 1;
    }

    void record_completion() noexcept {
        // One is deliberately saturated so an impossible extra completion
        // cannot reopen reuse; absence remains unlimited.
        if (requests_until_close_.has_value() && *requests_until_close_ > 1) {
            --*requests_until_close_;
        }
    }

    std::optional<std::size_t> requests_until_close_;
    const http_server_worker_state& worker_state_;
    bool request_input_ended_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::size_t chunked_encoded_buffer_bytes = max_http_header_bytes;

[[noreturn]] inline void throw_request_body_too_large() {
    throw http_request_body_protocol_error(http_request_body_failure::too_large);
}

[[noreturn]] inline void throw_incomplete_request_body() {
    throw http_request_body_protocol_error(http_request_body_failure::incomplete);
}

// Classifies a decode result that produced no output: returns when the decoder
// completed or needs more input, throws for every failure.
inline void require_transfer_coding_progress(const http_transfer_coding_decode_result& result_value) {
    if (const auto* failure = result_value.failure()) {
        throw http_request_transfer_coding_error(failure->error());
    }
    if (result_value.decoder_failure() != nullptr) {
        throw std::runtime_error("transfer-coding decoder failure");
    }
    if (result_value.complete() == nullptr && result_value.need_input() == nullptr) {
        throw std::logic_error("unexpected transfer-coding decode result");
    }
}

inline void require_complete_transfer_coding(http_transfer_coding_stack_decoder& decoder) {
    const auto finish_result = decoder.finish_input();
    if (finish_result.complete() != nullptr) {
        return;
    }
    if (const auto* failure = finish_result.failure()) {
        throw http_request_transfer_coding_error(failure->error());
    }
    if (finish_result.decoder_failure() != nullptr) {
        throw std::runtime_error("transfer-coding decoder failure");
    }
    throw std::logic_error("unexpected transfer-coding finish result");
}

// Reserves the next growth step of a known-length body whose first `filled`
// bytes have arrived: double them, at least known_length_min_read_bytes, never
// past the declared final size. Returns that final size, saturated at max_size().
[[nodiscard]] inline std::size_t reserve_known_length_growth(
    std::string& body, inbound_buffer_reservation& body_reservation, std::size_t filled, std::size_t remaining) {
    const auto final_size = remaining > body.max_size() - filled ? body.max_size() : filled + remaining;
    const auto doubled = filled > body.max_size() / 2 ? body.max_size() : filled * 2;
    const auto target_capacity = std::min(final_size, std::max(doubled, known_length_min_read_bytes));
    if (target_capacity > body.capacity()) {
        inbound_reserve(body, body_reservation, target_capacity);
    }
    return final_size;
}

// Where a body reader leaves the pipelined suffix -- the bytes of the next
// request that arrived together with this one. The suffix normally still sits
// at the tail of the connection read buffer, where the session compacts it in
// place once the views borrowing that buffer are dead. Only bytes the reader
// over-read into its own buffer, which dies with the route, are copied into
// request-scoped storage.
struct body_pipeline_handover final {
    // Length of the suffix still at the tail of the connection read buffer;
    // zero when `stashed_` holds the pipeline.
    std::size_t read_buffer_suffix_bytes_{};
    std::string_view stashed_;
};

// Advances `decoder` over the chunked framing in `input` from `cursor`,
// skipping payload without decoding it. True once the body ended there,
// trailers included; false when it needs more input or the framing failed.
[[nodiscard]] inline bool skip_buffered_chunked_body(
    http1_chunked_body_decoder& decoder, std::string_view input, std::size_t& cursor) {
    for (;;) {
        const auto result_value = decoder.decode(input.substr(cursor));
        cursor += result_value.consumed_bytes();
        if (result_value.complete() != nullptr) {
            return true;
        }
        if (result_value.body_chunk() == nullptr) {
            return false;
        }
    }
}

// Length of the framed request body when all of it is already in
// `body_and_pipeline`, the input that followed its head, so it is consumed
// without I/O and the connection stays reusable (RFC 9112 section 9.3);
// nullopt when it still needs input or its chunked framing is invalid.
[[nodiscard]] inline std::optional<std::size_t> buffered_request_body_bytes(
    const http1_request_body_plan& body_plan, std::string_view body_and_pipeline,
    protocol_byte_limit body_limit = protocol_byte_limit::unlimited()) {
    if (const auto* known_length = body_plan.known_length()) {
        const auto content_length = known_length->content_length();
        return !body_limit.exceeds(content_length) && body_and_pipeline.size() >= content_length
                   ? std::optional<std::size_t>(content_length)
                   : std::nullopt;
    }
    if (body_plan.chunked() == nullptr) {
        return std::size_t{0};
    }
    http1_chunked_body_decoder decoder({.body_limit_ = body_limit});
    std::size_t cursor = 0;
    return skip_buffered_chunked_body(decoder, body_and_pipeline, cursor) ? std::optional<std::size_t>(cursor)
                                                                          : std::nullopt;
}

template <typename stream_type>
class stream_body_reader final {
public:
    // `interim_output` is the request's interim lane, which carries the 100
    // Continue an expectation asks for.
    stream_body_reader(stream_type& stream, inbound_buffer_account* inbound_buffers,
        std::string_view initial_body_and_pipeline, http1_request_body_plan body_plan,
        protocol_byte_limit body_limit_value, connection_scanner::entry& scanner_entry,
        http_interim_response_output& interim_output, const worker_ref& worker)
        : stream_(stream),
          buffer_reservation_(inbound_buffers),
          transfer_output_reservation_(inbound_buffers),
          initial_body_and_pipeline_(initial_body_and_pipeline),
          body_plan_(body_plan),
          body_limit_(body_limit_value),
          chunk_decoder_(http1_chunked_body_decoder_config{.body_limit_ = body_limit_value}),
          scanner_entry_(scanner_entry),
          interim_output_(interim_output),
          worker_(worker),
          state_(body_plan_.requires_consumption() ? state_type::active : state_type::finished) {
        const auto expectation_plan =
            body_plan_.expectation_plan(http_unsupported_expectation_policy::reject);
        continue_pending_ = expectation_plan == http_server_expectation_plan::send_continue;
        const auto* chunked = body_plan_.chunked();
        if (chunked != nullptr && !chunked->transfer_codings().empty()) {
            const auto& codings = chunked->transfer_codings();
            transfer_decoder_ = std::make_unique<http_transfer_coding_stack_decoder>(std::span<const http_transfer_coding>(codings.data(), codings.size()), body_limit_value);
        }
    }

    stream_body_reader(const stream_body_reader&) = delete;
    stream_body_reader& operator=(const stream_body_reader&) = delete;

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept {
        return state_ == state_type::finished ? http1_request_body_consumption::complete
                                              : http1_request_body_consumption::incomplete;
    }
    [[nodiscard]] const http_request_trailers& trailers() const& noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    const http_request_trailers& trailers() const&& = delete;
    // Consumes, without I/O, the rest of a body that arrived in full with
    // its head and that the route left unread or partly read, so the
    // connection stays reusable. A body that still needs input, or whose
    // read failed, stays incomplete.
    void consume_buffered_body() {
        // Reads keep borrowing the initial view only while it still holds unread
        // body bytes; once they moved to buffer_, the rest needs socket input.
        if (state_ != state_type::active || initial_body_and_pipeline_.empty()) {
            return;
        }
        if (const auto* known_length = body_plan_.known_length()) {
            if (initial_body_and_pipeline_.size() >= known_length->content_length()) {
                delivered_bytes_ = known_length->content_length();
                mark_finished();
            }
            return;
        }
        // Chunked framing resumes where read_chunked() left the decoder, at
        // read_cursor_ in the borrowed view.
        if (skip_buffered_chunked_body(chunk_decoder_, initial_body_and_pipeline_, read_cursor_)) {
            mark_finished();
        }
    }
    // Hands the pipelined suffix over and drops this reader's claim on it. The
    // connection read buffer is deliberately untouched: every view in the
    // request being served still borrows it. `stash` receives bytes when the
    // suffix lives in this reader's own buffer or a later reader supplies
    // `additional_pipeline`; those later bytes follow both existing sources.
    [[nodiscard]] body_pipeline_handover take_pipeline(
        std::string& stash, std::string_view additional_pipeline = {}) {
        // The body reader's suffix is either the tail of the connection read
        // buffer or its own over-read. A passive reader can add a later suffix:
        // in that case all three sources must outlive their readers, in order.
        const auto initial_pipeline = initial_pipeline_remainder();
        const auto buffered_pipeline = buffered_pipeline_remainder();
        body_pipeline_handover handover;
        if (buffered_pipeline.empty() && additional_pipeline.empty()) {
            handover.read_buffer_suffix_bytes_ = initial_pipeline.size();
        } else {
            stash.clear();
            stash.reserve(initial_pipeline.size() + buffered_pipeline.size() + additional_pipeline.size());
            stash.append(initial_pipeline);
            stash.append(buffered_pipeline);
            stash.append(additional_pipeline);
            handover.stashed_ = stash;
        }
        reset_pipeline_state();
        return handover;
    }
    [[nodiscard]] std::size_t pipeline_size() const noexcept {
        return initial_pipeline_remainder().size() + buffered_pipeline_remainder().size();
    }
    // Runs `observer(target)` once, when a read completes the body, before that
    // read returns; its exception fails the read. Once the body is complete no
    // read touches the transport again, so the observer may start one.
    using completion_observer_type = void (*)(void*);
    void observe_completion(void* target, completion_observer_type observer) noexcept {
        completion_target_ = target;
        completion_observer_ = observer;
    }

    // An interrupted read keeps the reader active: the bytes already buffered
    // and the decoder state wait for the next read. Until then the body clock
    // is paused, and a response that leaves the body incomplete closes the
    // connection after it.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        require_not_failed();
        operation_interrupt interrupt(
            interrupts_, worker_, operation, &read_cancellation_, &wake_read, "request body read");
        const read_interrupt_scope running(read_interrupt_, interrupt);
        try {
            co_return co_await read_next();
        } catch (...) {
            if (interrupt.interrupted()) {
                scanner_entry_.set_phase(connection_scanner::phase::idle);
            } else {
                state_ = state_type::failed;
            }
            throw;
        }
    }

    // Like read(), an interruption keeps the bytes already appended to `body`
    // and the decoder state; the next call resumes.
    task<std::string_view> read_all(std::string& body, inbound_buffer_reservation& body_reservation, operation_options operation = {}) {
        require_not_failed();
        operation_interrupt interrupt(
            interrupts_, worker_, operation, &read_cancellation_, &wake_read, "request body read");
        const read_interrupt_scope running(read_interrupt_, interrupt);
        try {
            if (const auto* known_length = body_plan_.known_length()) {
                const auto content_length = known_length->content_length();
                if (state_ == state_type::finished) {
                    if (body.empty() && delivered_bytes_ == 0 && initial_body_and_pipeline_.size() >= content_length) {
                        co_return initial_body_and_pipeline_.substr(0, content_length);
                    }
                    co_return std::string_view(body);
                }
                if (exceeds_limit(content_length)) {
                    throw_request_body_too_large();
                }
                // A complete preread body borrows the connection buffer and needs
                // no I/O coroutine; preserve that borrow until the session retires it.
                if (delivered_bytes_ == 0 && initial_body_and_pipeline_.size() >= content_length) {
                    mark_finished();
                    co_return initial_body_and_pipeline_.substr(0, content_length);
                }
                co_return co_await read_known_length_all(body, body_reservation, content_length);
            }
            if (body_plan_.without_body() != nullptr) {
                co_return std::string_view(body);
            }

            if (continue_pending_) {
                co_await send_continue();
            }
            while (auto chunk = co_await read_chunked()) {
                if (transfer_decoder_ != nullptr) {
                    decode_transfer_append(::ruvia::as_chars(*chunk), body, body_reservation);
                } else {
                    if (body_limit_.addition_exceeds(body.size(), chunk->size())) {
                        throw_request_body_too_large();
                    }
                    body.append(::ruvia::as_chars(*chunk));
                }
            }
            if (transfer_decoder_ != nullptr) {
                require_complete_transfer_coding(*transfer_decoder_);
            }
            mark_finished();
            co_return std::string_view(body);
        } catch (...) {
            if (interrupt.interrupted()) {
                scanner_entry_.set_phase(connection_scanner::phase::idle);
            } else {
                state_ = state_type::failed;
            }
            throw;
        }
    }

private:
    enum class state_type : std::uint8_t { active,
        finished,
        failed };

    // Publishes the running read's interrupt to the waits below it.
    class read_interrupt_scope final {
    public:
        read_interrupt_scope(const operation_interrupt*& slot, const operation_interrupt& interrupt) noexcept
            : slot_(slot) {
            slot_ = &interrupt;
        }
        ~read_interrupt_scope() {
            slot_ = nullptr;
        }
        read_interrupt_scope(const read_interrupt_scope&) = delete;
        read_interrupt_scope& operator=(const read_interrupt_scope&) = delete;

    private:
        const operation_interrupt*& slot_;
    };

    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
    }

    // The unread window never received bytes; an interrupted read keeps only
    // the `filled` prefix for the next one.
    void throw_if_read_interrupted_keeping(std::string& body, std::size_t filled) const {
        if (read_interrupt_ != nullptr && read_interrupt_->interrupted()) {
            body.resize(filled);
            read_interrupt_->throw_if_interrupted();
        }
    }

    void throw_if_read_interrupted() const {
        if (read_interrupt_ != nullptr) {
            read_interrupt_->throw_if_interrupted();
        }
    }

    task<std::optional<std::span<const std::byte>>> read_next() {
        if (body_plan_.chunked() != nullptr) {
            if (continue_pending_) {
                co_await send_continue();
            }
            co_return co_await read_transfer_decoded_chunked();
        }
        const auto* known_length = body_plan_.known_length();
        if (known_length == nullptr) {
            co_return std::nullopt;
        }
        if (exceeds_limit(known_length->content_length())) {
            throw_request_body_too_large();
        }
        if (continue_pending_) {
            co_await send_continue();
        }
        co_return co_await read_known_length(known_length->content_length());
    }

    task<void> send_continue() {
        co_await interim_output_.send_continue();
        continue_pending_ = false;
    }

    void require_not_failed() const {
        if (state_ == state_type::failed) {
            throw std::logic_error("request body consumption previously failed");
        }
    }

    [[nodiscard]] std::string_view initial_pipeline_remainder() const noexcept {
        if (initial_body_and_pipeline_.empty()) {
            return {};
        }
        if (body_plan_.chunked() != nullptr) {
            return initial_body_and_pipeline_.substr(
                std::min(read_cursor_, initial_body_and_pipeline_.size()));
        }
        const auto* known_length = body_plan_.known_length();
        if (known_length == nullptr) {
            return initial_body_and_pipeline_;
        }
        const auto initial_body_bytes =
            std::min(known_length->content_length(), initial_body_and_pipeline_.size());
        return initial_body_and_pipeline_.substr(initial_body_bytes);
    }

    [[nodiscard]] std::string_view buffered_pipeline_remainder() const noexcept {
        if (read_cursor_ >= buffer_.size()) {
            return {};
        }
        return std::string_view(buffer_.data() + read_cursor_, buffer_.size() - read_cursor_);
    }

    void reset_pipeline_state() noexcept {
        buffer_.clear();
        initial_body_and_pipeline_ = {};
        read_cursor_ = 0;
    }

    void materialize_initial_remainder(std::size_t extra_reserve = 0) {
        if (initial_body_and_pipeline_.empty()) {
            return;
        }
        const auto remainder =
            initial_body_and_pipeline_.substr(std::min(read_cursor_, initial_body_and_pipeline_.size()));
        buffer_.clear();
        if (!remainder.empty() || extra_reserve != 0) {
            const auto hard_limit =
                body_plan_.chunked() != nullptr ? chunked_encoded_buffer_bytes : body_limit_.read_ceiling();
            const auto target_reserve = extra_reserve > hard_limit - std::min(remainder.size(), hard_limit)
                                            ? hard_limit
                                            : remainder.size() + extra_reserve;
            if (target_reserve > buffer_.capacity()) {
                inbound_reserve(buffer_, buffer_reservation_, std::max(remainder.size(), target_reserve));
            }
            inbound_append(buffer_, buffer_reservation_, remainder);
        }
        initial_body_and_pipeline_ = {};
        read_cursor_ = 0;
    }

    task<void> read_more() {
        // Earlier body views are dead once more input is needed. Drop the consumed
        // prefix here; writable tail space is reused before compaction or growth.
        if (read_cursor_ != 0) {
            buffer_.consume(read_cursor_);
            read_cursor_ = 0;
        }
        const auto old_size = buffer_.size();
        const auto hard_limit =
            body_plan_.chunked() != nullptr ? chunked_encoded_buffer_bytes : body_limit_.read_ceiling();
        if (old_size >= hard_limit) {
            throw_request_body_too_large();
        }
        const auto writable = std::min<std::size_t>(http_body_buffer_bytes, hard_limit - old_size);
        auto read_space = buffer_.prepare_spare();
        if (read_space.empty()) {
            read_space = inbound_prepare(buffer_, buffer_reservation_, writable);
        } else {
            read_space = read_space.first(std::min(writable, read_space.size()));
        }

        throw_if_read_interrupted();
        scanner_entry_.set_phase(connection_scanner::phase::reading_payload);
        auto read_completion =
            co_await ruvia::async_asio<std::size_t>([this, read_space](auto handler) mutable {
                stream_.async_read_some(asio::buffer(read_space),
                    asio::bind_cancellation_slot(read_cancellation_.slot(), std::move(handler)));
            });
        const auto ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        if (ec == asio::error::operation_aborted) {
            throw_if_read_interrupted();
        }
        if (ec || bytes_read == 0) {
            throw_incomplete_request_body();
        }
        buffer_.commit(bytes_read);
        scanner_entry_.touch();
    }

    task<std::string_view> read_known_length_all(std::string& body, inbound_buffer_reservation& body_reservation, std::size_t content_length) {
        const auto initial_body_bytes = std::min(content_length, initial_body_and_pipeline_.size());
        if (continue_pending_) {
            co_await send_continue();
        }

        // The initial segment is shorter than the body, so it is all body and no
        // pipelined bytes follow it; an earlier read() may also have left a
        // buffered remainder. Take both before reading the socket.
        const auto initial_unread =
            delivered_bytes_ < initial_body_bytes ? initial_body_bytes - delivered_bytes_ : std::size_t{0};
        const auto remaining_after_initial = content_length - delivered_bytes_ - initial_unread;
        const auto buffered_unread =
            read_cursor_ < buffer_.size() ? std::min(buffer_.size() - read_cursor_, remaining_after_initial)
                                          : std::size_t{0};
        const auto preread_unread = initial_unread + buffered_unread;
        if (preread_unread != 0) {
            const auto after_preread = body.size() + preread_unread;
            const auto remaining_after_preread = remaining_after_initial - buffered_unread;
            if (remaining_after_preread == 0) {
                inbound_reserve(body, body_reservation, after_preread);
            } else {
                (void)reserve_known_length_growth(body, body_reservation, after_preread, remaining_after_preread);
            }
        }
        if (initial_unread != 0) {
            body.append(initial_body_and_pipeline_.substr(delivered_bytes_, initial_unread));
            delivered_bytes_ = initial_body_bytes;
        }
        initial_body_and_pipeline_ = {};
        if (buffered_unread != 0) {
            body.append(buffer_.data() + read_cursor_, buffered_unread);
            read_cursor_ += buffered_unread;
            delivered_bytes_ += buffered_unread;
        }

        // Read the rest straight into the tail of `body`, never past the declared
        // length, so no bytes of a pipelined request are taken. Capacity starts at
        // min(declared length, known_length_min_read_bytes) and then grows
        // geometrically from what has actually arrived, so a peer that sends only
        // headers cannot make the declared Content-Length allocate past that floor.
        // `body` is resized once per growth step and the reads fill the window up
        // to its size, so each byte is initialized at most once before a read
        // overwrites it; `filled` marks the bytes the socket has written.
        if (delivered_bytes_ < content_length) {
            scanner_entry_.set_phase(connection_scanner::phase::reading_payload);
        }
        auto filled = body.size();
        while (delivered_bytes_ < content_length) {
            const auto remaining_body = content_length - delivered_bytes_;
            if (filled == body.size()) {
                const auto final_size = reserve_known_length_growth(body, body_reservation, filled, remaining_body);
                inbound_resize(body, body_reservation, std::min(final_size, body.capacity()));
            }
            const auto writable = std::min(remaining_body, body.size() - filled);
            throw_if_read_interrupted_keeping(body, filled);
            auto read_completion =
                co_await ruvia::async_asio<std::size_t>([this, &body, filled, writable](auto handler) mutable {
                    stream_.async_read_some(asio::buffer(body.data() + filled, writable),
                        asio::bind_cancellation_slot(read_cancellation_.slot(), std::move(handler)));
                });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
            if (ec == asio::error::operation_aborted) {
                throw_if_read_interrupted_keeping(body, filled);
            }
            if (ec || bytes_read == 0) {
                // The unread window never received bytes; it must not survive as body.
                body.resize(filled);
                throw_incomplete_request_body();
            }
            filled += bytes_read;
            delivered_bytes_ += bytes_read;
            scanner_entry_.touch();
        }
        mark_finished();
        co_return std::string_view(body);
    }

    task<std::optional<std::span<const std::byte>>> read_known_length(std::size_t content_length) {
        if (state_ == state_type::finished) {
            co_return std::nullopt;
        }
        if (exceeds_limit(content_length)) {
            throw_request_body_too_large();
        }
        if (content_length == 0 || delivered_bytes_ == content_length) {
            mark_finished();
            co_return std::nullopt;
        }

        const auto initial_body_bytes = std::min(content_length, initial_body_and_pipeline_.size());
        if (delivered_bytes_ < initial_body_bytes) {
            const auto remaining_body = content_length - delivered_bytes_;
            const auto available = initial_body_bytes - delivered_bytes_;
            const auto chunk_bytes = std::min(available, remaining_body);
            auto chunk = initial_body_and_pipeline_.substr(delivered_bytes_, chunk_bytes);
            delivered_bytes_ += chunk_bytes;
            if (delivered_bytes_ == content_length) {
                mark_finished();
            }
            co_return ::ruvia::as_bytes(chunk);
        }

        // The initial segment is now fully consumed as body: a partial-body prefix
        // cannot be followed by pipelined bytes, so the whole borrowed view was
        // body. Drop it before reading into buffer_, so read_cursor_ addresses
        // buffer_ (mirrors the chunked path's materialize_initial_remainder()).
        if (initial_body_bytes == initial_body_and_pipeline_.size()) {
            initial_body_and_pipeline_ = {};
        }

        while (buffer_.size() <= read_cursor_) {
            co_await read_more();
        }

        const auto remaining_body = content_length - delivered_bytes_;
        const auto available = buffer_.size() - read_cursor_;
        const auto chunk_bytes = std::min(available, remaining_body);
        auto chunk = std::string_view(buffer_.data() + read_cursor_, chunk_bytes);
        read_cursor_ += chunk_bytes;
        delivered_bytes_ += chunk_bytes;
        if (delivered_bytes_ == content_length) {
            mark_finished();
        }

        co_return ::ruvia::as_bytes(chunk);
    }

    task<std::optional<std::span<const std::byte>>> read_chunked() {
        if (state_ == state_type::finished) {
            co_return std::nullopt;
        }

        for (;;) {
            const auto source_value = !initial_body_and_pipeline_.empty()
                                          ? std::string_view(initial_body_and_pipeline_)
                                          : buffer_.view();
            const auto result_value = chunk_decoder_.decode(source_value.substr(read_cursor_));
            // Advancing the cursor keeps the returned chunk view intact; the bytes
            // behind it are reclaimed only by read_more().
            read_cursor_ += result_value.consumed_bytes();
            if (const auto* body_chunk = result_value.body_chunk()) {
                co_return ::ruvia::as_bytes(body_chunk->bytes());
            }
            if (const auto* complete = result_value.complete()) {
                if (trailers_.append_received(complete->trailers()).has_value()) {
                    throw std::logic_error("HTTP/1 decoder published invalid request trailers");
                }
                co_return std::nullopt;
            }
            if (const auto* failure = result_value.failure()) {
                throw http_request_chunk_decode_error(failure->error());
            }
            if (result_value.need_more() == nullptr) {
                throw std::logic_error("unexpected HTTP/1 chunk decode result");
            }
            materialize_initial_remainder(http_body_buffer_bytes);
            co_await read_more();
        }
    }

    task<std::optional<std::span<const std::byte>>> read_transfer_decoded_chunked() {
        if (transfer_decoder_ == nullptr) {
            auto chunk = co_await read_chunked();
            if (!chunk) {
                mark_finished();
            }
            co_return chunk;
        }

        const auto output_window = transfer_output();

        // Keep the borrowed encoded chunk until the decoder reports its consumed
        // prefix. read_chunked() is called only after that view is empty, so its
        // compaction cannot invalidate decoder input across application reads.
        for (;;) {
            const auto result_value = transfer_decoder_->decode(transfer_input_, output_window);
            transfer_input_.remove_prefix(std::min(transfer_input_.size(), result_value.consumed_bytes()));
            if (const auto* output = result_value.output()) {
                co_return ::ruvia::as_bytes(output->bytes());
            }
            require_transfer_coding_progress(result_value);

            auto chunk = co_await read_chunked();
            if (!chunk) {
                require_complete_transfer_coding(*transfer_decoder_);
                mark_finished();
                co_return std::nullopt;
            }
            transfer_input_ = ::ruvia::as_chars(*chunk);
        }
    }

    // The transfer decoder's output scratch: a fixed window sized once per reader.
    [[nodiscard]] std::span<char> transfer_output() {
        if (transfer_output_.empty()) {
            inbound_resize(transfer_output_, transfer_output_reservation_, http_body_buffer_bytes);
        }
        return std::span<char>(transfer_output_);
    }

    // Decodes into the reused scratch and appends what the decoder produced, so
    // `target` only ever grows by written bytes.
    void decode_transfer_append(std::string_view input, std::string& target, inbound_buffer_reservation& target_reservation) {
        const auto output_window = transfer_output();
        for (;;) {
            const auto result_value = transfer_decoder_->decode(input, output_window);
            input.remove_prefix(std::min(input.size(), result_value.consumed_bytes()));
            if (const auto* output = result_value.output()) {
                inbound_append(target, target_reservation, output->bytes());
                continue;
            }
            require_transfer_coding_progress(result_value);
            return;
        }
    }

    [[nodiscard]] bool exceeds_limit(std::size_t bytes_value) const noexcept {
        return body_limit_.exceeds(bytes_value);
    }

    void mark_finished() {
        state_ = state_type::finished;
        scanner_entry_.set_phase(connection_scanner::phase::idle);
        if (const auto observer = std::exchange(completion_observer_, nullptr)) {
            observer(completion_target_);
        }
    }
    stream_type& stream_;
    http_io_buffer buffer_;
    inbound_buffer_reservation buffer_reservation_;
    std::string transfer_output_;
    inbound_buffer_reservation transfer_output_reservation_;
    asio::cancellation_signal read_cancellation_;
    std::unique_ptr<http_transfer_coding_stack_decoder>
        transfer_decoder_;
    std::string_view transfer_input_;
    std::string_view initial_body_and_pipeline_;
    http1_request_body_plan body_plan_;
    protocol_byte_limit body_limit_;
    http1_chunked_body_decoder chunk_decoder_;
    http_request_trailers trailers_;
    connection_scanner::entry& scanner_entry_;
    http_interim_response_output& interim_output_;
    const worker_ref& worker_;
    // The running read's interrupt; null between reads.
    const operation_interrupt* read_interrupt_{};
    // Offset of the first unconsumed byte in the active source: the borrowed
    // initial view while it is set, buffer_ otherwise. Consumed buffer_ bytes are
    // dropped only when read_more() needs room for socket input.
    std::size_t read_cursor_{0};
    std::size_t delivered_bytes_{0};
    // A failed read remains terminal and incomplete, including observer failures.
    state_type state_;
    bool continue_pending_{false};
    void* completion_target_{};
    completion_observer_type completion_observer_{};
    operation_interrupt_source interrupts_;
};

template <typename stream_type>
class lazy_buffered_body final {
public:
    lazy_buffered_body(stream_type& stream, inbound_buffer_account* inbound_buffers,
        std::string_view initial_body_and_pipeline, http1_request_body_plan body_plan,
        protocol_byte_limit body_limit_value, connection_scanner::entry& scanner_entry,
        http_interim_response_output& interim_output, const worker_ref& worker)
        : reader_(stream, inbound_buffers, initial_body_and_pipeline, body_plan, body_limit_value, scanner_entry,
              interim_output, worker),
          body_reservation_(inbound_buffers) {}

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept {
        return reader_.consumption();
    }
    [[nodiscard]] stream_body_reader<stream_type>& reader() & noexcept RUVIA_LIFETIMEBOUND {
        return reader_;
    }
    stream_body_reader<stream_type>& reader() && = delete;
    [[nodiscard]] const http_request_trailers& trailers() const& noexcept RUVIA_LIFETIMEBOUND {
        return reader_.trailers();
    }
    const http_request_trailers& trailers() const&& = delete;

    [[nodiscard]] std::size_t pipeline_size() const noexcept {
        return reader_.pipeline_size();
    }

    [[nodiscard]] task<std::string_view> read_all(operation_options operation) {
        return reader_.read_all(body_, body_reservation_, std::move(operation));
    }

    task<void> discard(operation_options operation) {
        const operation_steps steps(std::move(operation));
        while (co_await reader_.read(steps.next("request body read"))) {
        }
    }

private:
    stream_body_reader<stream_type> reader_;
    std::string body_;
    inbound_buffer_reservation body_reservation_;
};

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP2)
#endif

// What every HTTP/1 route dispatch needs from the session that owns the
// request: the transport, the scanner entry, the parsed head, the route table
// and services to run the handler with, the options, the response being built,
// and the keep-alive sequence. Each dispatcher takes this and adds only what
// its own kind of route needs.
//
// Passed by value, as its members were as separate arguments: the references
// cost nothing to copy and context_services is copied per dispatch either way.

namespace ruvia::detail {

template <typename stream_type>
struct http1_route_dispatch final {
    stream_type& stream_;
    connection_scanner::entry& scanner_entry_;
    const http1_server_request_parse_state& parsed_;
    http_response_coding_selection response_coding_;
    // Admission's CORS policy for this request; null when none applies.
    const http_cors_policy* cors_;
    const route_table& routes_;
    context_services base_route_services_;
    const http_server_options& options_;
    http_response& response_;
    http1_request_sequence& request_sequence_;
    inbound_buffer_account& inbound_buffers_;
    // The request was already buffered behind the previous one when the
    // session started it: the client sent it before that response finished.
    bool request_pipelined_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Commits a framework response that ends the connection: a rejection, a
// failed route, or a route response the connection cannot outlive. The
// response is the framework's own (an error or exception handler's output):
// failing to commit it leaves no response to fall back to.
[[nodiscard]] inline http1_session_request_completion close_with_framework_response(
    http_response& response, http1_request_connection_plan connection_plan) {
    return http1_session_request_completion::make_buffered_closing(
        http1_commit_final_response(response, connection_plan.require_close()));
}

// Commits a route's buffered response. A response that breaks the HTTP/1
// final-response rules becomes a rejected completion that the session answers
// through the exception handler instead of the route response.
template <typename make_completion_type>
[[nodiscard]] inline http1_session_request_completion commit_http1_route_response(http_response& response,
    http1_request_connection_plan connection_plan, make_completion_type make_completion) {
    try {
        return make_completion(http1_commit_final_response(response, connection_plan));
    } catch (const http1_response_head_error& error) {
        return http1_session_request_completion::make_rejected_final_response(connection_plan, error);
    }
}

[[nodiscard]] inline http1_session_request_completion finalize_buffered_route_response(http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    std::size_t consumed_bytes) {
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    return commit_http1_route_response(response, connection_plan,
        [consumed_bytes](http1_request_connection_plan committed) {
            return http1_session_request_completion::make_buffered_unrestored(committed, consumed_bytes);
        });
}

// Route responses for an upgrade request always close the connection.
[[nodiscard]] inline http1_session_request_completion finalize_closing_route_response(
    http_response& response, http1_request_connection_plan connection_plan) {
    return commit_http1_route_response(response, connection_plan.require_close(),
        [](http1_request_connection_plan committed) {
            return http1_session_request_completion::make_buffered_closing(committed);
        });
}

}  // namespace ruvia::detail

namespace ruvia::detail {

template <typename stream_type>
struct http_lazy_buffered_body_route_state final {
    std::optional<request_body_loader_binding<lazy_buffered_body<stream_type>>> body_;

    void emplace(stream_type& stream, inbound_buffer_account* inbound_buffers, std::string_view body_and_pipeline,
        http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
        connection_scanner::entry& scanner_entry, http_interim_response_output& interim_output,
        const worker_ref& worker) {
        body_.emplace(stream, inbound_buffers, body_and_pipeline, body_plan, body_limit_value,
            scanner_entry, interim_output, worker);
    }

    [[nodiscard]] context_services with_loader(const context_services& base_services) noexcept {
        auto services = base_services;
        services.bind_lazy_request_body(body_->facade());
        services.bind_request_trailers(body_->loader().trailers());
        return services;
    }

    [[nodiscard]] const lazy_buffered_body<stream_type>& body() const noexcept {
        return body_->loader();
    }

    [[nodiscard]] lazy_buffered_body<stream_type>& body() noexcept {
        return body_->loader();
    }
};

// Returns the setup failure (allocation included) so the caller answers it
// through the route failure path instead of unwinding the session.
template <typename stream_type>
[[nodiscard]] inline std::exception_ptr try_prepare_http_lazy_buffered_body_route(
    http_lazy_buffered_body_route_state<stream_type>& state_value,
    const http1_route_dispatch<stream_type>& d, protocol_byte_limit body_limit_value,
    std::string_view body_and_pipeline) {
    try {
        // Every HTTP/1 request's services carry its session's interim lane.
        state_value.emplace(d.stream_, &d.inbound_buffers_,
            body_and_pipeline, d.parsed_.body_plan(), body_limit_value, d.scanner_entry_,
            *d.base_route_services_.interim_output(), d.base_route_services_.worker());
        return nullptr;
    } catch (...) {
        return std::current_exception();
    }
}

// `request_bytes` is the connection's buffered input from the start of this
// request: its head, then whatever body and pipelined bytes followed it.
[[nodiscard]] inline std::string_view http_body_and_pipeline(
    const http1_server_request_head_ready& request_head, std::string_view request_bytes) noexcept {
    return request_bytes.substr(request_head.header_bytes());
}

inline task<http1_session_request_completion> complete_failed_http_route(
    connection_scanner::entry& scanner_entry, std::exception_ptr exception,
    const http1_server_request_parse_state& parsed_value, const route_table& routes_value, const context_services& exception_services,
    http_response& response) {
    if (auto* lifecycle = exception_services.connection_lifecycle_value()) {
        lifecycle->note_exception(exception);
    }
    response = co_await routes_value.handle_exception(
        parsed_value.request(), exception, exception_services);
    scanner_entry.touch();
    co_return close_with_framework_response(response, parsed_value.connection_plan());
}

// `request_bytes` is the connection's buffered input at dispatch, from the
// start of this request, the suffix included. A suffix still in that buffer
// becomes a consumed prefix; only bytes the body reader over-read go through
// `pipeline_stash`, request-scoped storage that outlives the returned
// completion. The read buffer is not touched here -- the response has not been
// written and the access log not recorded yet, and both still read views that
// borrow it.
template <typename take_pipeline_type>
[[nodiscard]] inline http1_session_request_completion complete_successful_http_body_route(
    connection_scanner::entry& scanner_entry, http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    http1_request_body_consumption body_consumption, std::size_t request_bytes,
    std::string& pipeline_stash, take_pipeline_type take_pipeline) {
    connection_plan = apply_request_body_consumption(connection_plan, body_consumption);
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    scanner_entry.touch();
    return commit_http1_route_response(response, connection_plan,
        [&](http1_request_connection_plan committed) {
            if (committed.disposition() == http1_close_policy::allow_reuse) {
                const auto pipeline = take_pipeline(pipeline_stash);
                if (!pipeline.stashed_.empty()) {
                    return http1_session_request_completion::make_buffered_pipeline_restore(
                        committed, pipeline.stashed_);
                }
                return http1_session_request_completion::make_buffered_unrestored(
                    committed, request_bytes - pipeline.read_buffer_suffix_bytes_);
            }
            return http1_session_request_completion::make_buffered_closing(committed);
        });
}

// A body the route left unread costs no reuse when it arrived in full with
// its head: it is consumed here without I/O.
template <typename stream_type>
[[nodiscard]] inline http1_session_request_completion complete_successful_http_body_route(
    connection_scanner::entry& scanner_entry, http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    std::size_t request_bytes, std::string& pipeline_stash, stream_body_reader<stream_type>& reader) {
    reader.consume_buffered_body();
    return complete_successful_http_body_route(scanner_entry, response, connection_plan, request_sequence,
        reader.consumption(), request_bytes, pipeline_stash,
        [&reader](std::string& stash) { return reader.take_pipeline(stash); });
}

// Completes a response produced without reading the request body (an
// unresolved route or a rate-limit rejection). `request_bytes` is the
// connection's buffered input from the start of this request. A body that
// arrived in full with the head is consumed without I/O and the connection
// stays reusable; any other unread body closes it after the response.
[[nodiscard]] inline http1_session_request_completion finalize_unread_body_response(http_response& response,
    const http1_server_request_parse_state& parsed_value, const http1_server_request_head_ready& request_head,
    std::string_view request_bytes, http1_request_sequence& request_sequence,
    protocol_byte_limit body_limit = protocol_byte_limit::unlimited()) {
    const auto body_bytes = buffered_request_body_bytes(
        parsed_value.body_plan(), http_body_and_pipeline(request_head, request_bytes), body_limit);
    const auto connection_plan = apply_request_body_consumption(parsed_value.connection_plan(),
        body_bytes ? http1_request_body_consumption::complete : http1_request_body_consumption::incomplete);
    return finalize_buffered_route_response(
        response, connection_plan, request_sequence, request_head.header_bytes() + body_bytes.value_or(0));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// HTTP/1 流式响应的读侧终止检测：请求体完整消费后由 route 自动启动（同 Go
// net/http background read）。单独到达的请求遇对端 FIN/RST 请求当前请求的 stop。
// pipeline 内（本请求排在前一请求之后到达，或其后已有后续请求字节）的干净结束
// （TCP FIN、TLS close_notify）是请求方向半关闭：不取消当前响应，并经
// request_input_ended() 交给连接，已收到的请求照常服务、不再读取；RST、TLS 截断
// 与其他错误始终中止。route 负责 cancel/join 后再释放 transport。
template <typename stream_type>
class http1_peer_eof_monitor final {
public:
    // `request_stop` is the request's own source; it outlives the monitor.
    // `request_pipelined`: the request itself arrived behind another one.
    http1_peer_eof_monitor(stream_type& stream, const worker_ref& worker,
        stop_source& request_stop, bool request_pipelined, std::size_t existing_pipeline_bytes)
        : stream_(stream),
          request_stop_(request_stop),
          tasks_(worker),
          allowance_(max_http_header_bytes - std::min(max_http_header_bytes, existing_pipeline_bytes)),
          pipelined_(request_pipelined || existing_pipeline_bytes != 0) {}
    http1_peer_eof_monitor(stream_type&, worker_ref&&,
        stop_source&, bool, std::size_t) = delete;
    void start() {
        tasks_.spawn(read_peer());
    }

    [[nodiscard]] bool peer_aborted() const noexcept {
        return peer_aborted_;
    }

    // The peer ended its request direction behind pipelined bytes. The
    // transport holds no more input; reading it again would only report the
    // same end. Final once join() has returned.
    [[nodiscard]] bool request_input_ended() const noexcept {
        return request_input_ended_;
    }

    [[nodiscard]] std::string_view pipeline() const noexcept {
        return input_.view();
    }

    void ensure_writable() const {
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
        if (peer_aborted_) {
            throw inbound_transport_error(asio::error::operation_aborted,
                "HTTP response stream peer has disconnected");
        }
    }

    // The sink requests the request stop itself on every write failure.
    void write_failed(const std::error_code& error) noexcept {
        peer_aborted_ = peer_aborted_ || is_inbound_connection_end(error);
        cancel();
    }

    void cancel() noexcept {
        // Set before emitting: an already-queued successful completion must
        // retain its bytes but must not schedule another read after teardown.
        closing_ = true;
        cancellation_.emit(asio::cancellation_type::terminal);
    }

    task<void> join() {
        if (!joined_) {
            joined_ = true;
            co_await tasks_.join();
        }
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    task<void> read_peer() {
        try {
            // Stop once this bounded look-ahead is full. No input is discarded;
            // a reset behind that many pipelined bytes then surfaces only once
            // the handler finishes and the session consumes those requests.
            while (!closing_ && input_.size() < allowance_) {
                // A quiet SSE needs no look-ahead allocation. After the first
                // application byte, grow like the connection read buffer
                // instead of reserving the full 64 KiB header allowance.
                char first_byte;
                const bool first_read = input_.capacity() == 0;
                if (!first_read && input_.size() == input_.capacity()) {
                    input_.reserve(std::min(
                        allowance_, std::max(initial_read_buffer_bytes, input_.capacity() * 2)));
                }
                const auto space = first_read ? std::span<char>(&first_byte, 1) : input_.prepare_spare();
                // Cancel this operation, not the socket's concurrent writer.
                // Asio's SSL io_op feeds received encrypted input into its
                // engine before honoring read cancellation; it remains there
                // for the session's next read after this task has joined.
                const auto completion = co_await async_asio<std::size_t>([this, space](auto handler) mutable {
                    stream_.async_read_some(asio::buffer(space),
                        asio::bind_cancellation_slot(cancellation_.slot(), std::move(handler)));
                });
                // A terminal cancellation can race a successful read. Preserve
                // every returned byte before inspecting the shutdown state.
                if (first_read) {
                    if (completion.result() != 0) {
                        input_.reserve(std::min(initial_read_buffer_bytes, allowance_));
                        input_.append(std::string_view(&first_byte, completion.result()));
                    }
                } else {
                    input_.commit(completion.result());
                }
                const auto error = completion.error_code();
                if (error) {
                    if (closing_ && error == asio::error::operation_aborted) {
                        co_return;
                    }
                    if (is_inbound_connection_end(error)) {
                        // RFC 9112 section 9.6: within a pipeline -- this request
                        // arrived behind another, or more follow it -- a clean
                        // end (TCP FIN, TLS close_notify) closes only the request
                        // direction; every request received gets its response.
                        // That no longer depends on when the FIN arrives. A
                        // request that arrived alone, a reset, and a TLS stream
                        // truncated without close_notify end the response.
                        if (error == asio::error::eof && (pipelined_ || input_.size() != 0)) {
                            request_input_ended_ = true;
                            co_return;
                        }
                        peer_aborted_ = true;
                        request_stop_.request_stop();
                        co_return;
                    }
                    throw inbound_transport_error(error, "failed to monitor HTTP response stream peer");
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
            request_stop_.request_stop();
        }
    }

    stream_type& stream_;
    stop_source& request_stop_;
    task_scope tasks_;
    http_io_buffer input_;
    asio::cancellation_signal cancellation_;
    std::size_t allowance_;
    std::exception_ptr failure_;
    bool pipelined_;
    bool closing_{};
    bool peer_aborted_{};
    bool request_input_ended_{};
    bool joined_{};
};

// Writes a response stream to the connection. The handler runs in the
// session's idle deadman phase (idle_timeout, like a buffered handler); only
// the sink's own socket writes count as `writing`, so write_timeout measures a
// slow reader and not the time a handler spends waiting for its next event.
template <typename stream_type>
class response_stream_sink final {
public:
    // `cors` is the application's CORS policy (null when none is configured);
    // like `request` and `request_body`, it must outlive the sink. `plan`
    // assumes the request body is left unread; `consumed_plan` is the plan for
    // the same request once `request_body` reports its complete consumption,
    // which commit checks so a route that read its body before streaming keeps
    // the connection reusable. `request_stop` is the request's own stop source,
    // which a peer close or a failed write requests; it outlives the sink.
    // `request_pipelined` tells whether the request arrived behind another.
    response_stream_sink(stream_type& stream, worker_codecs& codecs,
        http_response_head_buffer& head, connection_scanner::entry& scanner_entry,
        const worker_ref& worker_value, const http_request& request, const http_cors_policy* cors,
        http1_response_stream_plan plan, http1_response_stream_plan consumed_plan,
        lazy_buffered_body<stream_type>& request_body, http_response_coding_selection response_coding,
        const compression_config* compression,
        stop_source& request_stop, bool request_pipelined) noexcept
        : stream_(stream),
          head_(head),
          trailers_(),
          scanner_entry_(scanner_entry),
          worker_(worker_value),
          request_(request),
          cors_(cors),
          plan_(plan),
          consumed_plan_(consumed_plan),
          request_body_(request_body),
          connection_plan_(plan.request_connection_plan().require_close()),
          compression_(codecs, response_coding, compression),
          request_stop_(request_stop),
          request_pipelined_(request_pipelined) {
        request_body_.reader().observe_completion(this, &arm_peer_monitor_on_body_completion);
    }

    response_stream_sink(stream_type&, worker_codecs&, http_response_head_buffer&,
        connection_scanner::entry&, worker_ref&&, const http_request&, const http_cors_policy*,
        http1_response_stream_plan, http1_response_stream_plan,
        lazy_buffered_body<stream_type>&, http_response_coding_selection,
        const compression_config*, stop_source&, bool) = delete;

    response_stream_sink(const response_stream_sink&) = delete;
    response_stream_sink& operator=(const response_stream_sink&) = delete;

    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || (!state_.ended() && peer_aborted());
    }

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    // Routes the stop requests of the writer's output operations.
    [[nodiscard]] operation_interrupt_source& output_interrupts() & noexcept RUVIA_LIFETIMEBOUND {
        return output_interrupts_;
    }
    operation_interrupt_source& output_interrupts() && = delete;

    // Read-side termination detection, like Go net/http's background read:
    // it starts once the request body is complete, at dispatch or when the
    // handler's read completes it, and never while body bytes are still unread
    // on the transport. A finished stream does not start it.
    void arm_peer_monitor() {
        if (peer_monitor_ || peer_monitor_retired_ || state_.ended() || state_.aborted() ||
            request_body_.consumption() != http1_request_body_consumption::complete) {
            return;
        }
        peer_monitor_.emplace(
            stream_, worker_, request_stop_, request_pipelined_, request_body_.pipeline_size());
        try {
            peer_monitor_->start();
        } catch (...) {
            peer_monitor_.reset();
            throw;
        }
    }

    // Once the handler has returned: no later body consumption may start a
    // monitor, and a running one stops reading. finish_peer_monitor() joins it.
    void retire_peer_monitor() noexcept {
        peer_monitor_retired_ = true;
        request_body_.reader().observe_completion(nullptr, nullptr);
        if (peer_monitor_) {
            peer_monitor_->cancel();
        }
    }

    [[nodiscard]] bool peer_aborted() const noexcept {
        return peer_monitor_ && peer_monitor_->peer_aborted();
    }

    // Read once finish_peer_monitor() has returned.
    [[nodiscard]] bool request_input_ended() const noexcept {
        return peer_monitor_ && peer_monitor_->request_input_ended();
    }

    [[nodiscard]] bool peer_eof_monitoring() const noexcept {
        return peer_monitor_.has_value();
    }

    task<void> finish_peer_monitor() {
        if (peer_monitor_) {
            co_await peer_monitor_->join();
        }
    }

    [[nodiscard]] std::string_view monitored_pipeline() const noexcept {
        return peer_monitor_ ? peer_monitor_->pipeline() : std::string_view{};
    }

    task<stream_commit_result> commit() {
        if (state_.committed()) {
            throw std::logic_error("response stream is already committed");
        }
        co_await commit_head(http_response_trailer_intent::none);
        co_return state_.ended() ? stream_commit_result::completed : stream_commit_result::body;
    }

    template <typename sink>
    friend task<void> response_stream_write_thunk(void*, std::string_view);
    template <typename sink>
    friend task<void> response_stream_end_thunk(void*, std::span<const http_header_view>);
    template <typename sink>
    friend task<timer_sleep_result> response_stream_sleep_thunk(
        void*, std::chrono::milliseconds, const stop_token&);
    template <typename sink>
    friend void response_stream_bind_context_thunk(void*, context*);
    template <typename sink>
    friend void response_stream_release_context_thunk(void*) noexcept;

private:
    void ensure_peer_writable() const {
        if (peer_monitor_) {
            peer_monitor_->ensure_writable();
        }
    }

    void mark_write_failure(const std::error_code& error) noexcept {
        request_stop_.request_stop();
        if (peer_monitor_) {
            peer_monitor_->write_failed(error);
        }
        state_.mark_aborted();
    }

    [[noreturn]] void fail_write(const std::error_code& error, const char* what) {
        mark_write_failure(error);
        throw inbound_transport_error(error, what);
    }

    void bind_context(context* context_value) {
        state_.bind_context(context_value);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    // The caller's coroutine frame retains the buffers and temporary scanner phase.
    // Return the native awaiter instead of allocating another task frame per write.
    template <typename buffers_type>
    [[nodiscard]] auto write_to_stream(const buffers_type& buffers) {
        return ruvia::async_asio([this, &buffers](auto handler) mutable {
            ensure_peer_writable();
            asio::async_write(stream_, buffers, connection_write_progress(scanner_entry_), std::move(handler));
        });
    }

    // Copies a small TLS frame into the head buffer so it leaves as one
    // record; otherwise `buffers` stays the equivalent gather write. Plain TCP
    // sends the gather write as one segment already.
    void coalesce_tls_frame(std::array<asio::const_buffer, 3>& buffers, std::string_view first,
        std::string_view second, std::string_view third) noexcept {
        if constexpr (!std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
            if (first.size() + second.size() + third.size() > response_head_retained_heap_bytes) {
                return;
            }
            head_.reset();
            if (try_coalesce_tls_frame(head_, {first, second, third})) {
                buffers = {asio::buffer(head_.view()), asio::const_buffer{}, asio::const_buffer{}};
            }
        }
    }

    static void arm_peer_monitor_on_body_completion(void* target) {
        static_cast<response_stream_sink*>(target)->arm_peer_monitor();
    }

    task<void> commit_head(http_response_trailer_intent trailer_intent) {
        ensure_peer_writable();
        if (state_.committed()) {
            if (trailer_intent == http_response_trailer_intent::present) {
                state_.ensure_trailers_allowed();
            }
            co_return;
        }

        try {
            auto& context_value = state_.bound_context();
            if (context_access::output_committed(context_value)) {
                throw std::logic_error("the request's tunnel is already established");
            }
            auto* const interim_output = context_access::interim_output(context_value);
            auto response = co_await context_access::commit_streaming_head(context_value);
            validate_response_stream_status(request_, response);
            ensure_peer_writable();
            // The same final-representation order as a buffered response: the
            // application CORS policy first, then content coding.
            if (cors_ != nullptr) {
                (void)cors_->apply_response_headers(request_, response);
            }
            compression_.prepare(request_, response);
            // The request body is settled at commit: read in full by now, the
            // connection stays reusable; anything less closes it after the
            // stream, since the body bytes still in the socket would be read as
            // the next request.
            if (request_body_.consumption() == http1_request_body_consumption::complete) {
                plan_ = consumed_plan_;
            }
            auto stream_head =
                prepare_http1_response_stream_head(std::move(response), plan_, trailer_intent);
            compression_.activate(stream_head.commit_plan().body_plan());

            head_.reset();
            append_http1_response_head(stream_head.response(), head_, stream_head.response_head_plan());
            connection_plan_ = stream_head.connection_plan();
            // Mark committed before the write; a partial header flush must never be
            // followed by the normal error-response path on the same socket.
            if (interim_output != nullptr) {
                interim_output->commit_final();
            }
            state_.mark_committed(stream_head.commit_plan());
            arm_peer_monitor();
        } catch (...) {
            // Representation metadata and protocol framing are one pre-wire
            // transaction. Once either side has failed, no handler retry may
            // manufacture a second head from half-prepared compression state.
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto write_result =
                co_await write_to_stream(asio::buffer(head_.view()));
            ec = write_result.error_code();
        }
        if (ec) {
            fail_write(ec, "failed to write HTTP response stream head");
        }
    }

    task<timer_sleep_result>
    sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        co_return co_await sleep_for(worker_, duration, stop_token_value);
    }

    task<void> write(std::string_view chunk) {
        ensure_peer_writable();
        state_.ensure_body_allowed();
        if (chunk.empty()) {
            co_return;
        }

        if (compression_.active()) {
            try {
                co_await compression_.write(
                    chunk, [this](std::string_view encoded) { return write_encoded(encoded); });
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_return;
        }

        co_await write_encoded(chunk);
    }

    task<void> write_encoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }

        const http1_chunk_header chunk_header(chunk.size());
        std::array<asio::const_buffer, 3> buffers{};
        if (plan_.framing() == http_response_stream_framing::http1_close_delimited) {
            // No chunk framing: write the raw body bytes. The connection close
            // (forced once the stream ends) is what delimits the message.
            buffers[0] = asio::buffer(chunk);
        } else {
            buffers = {asio::buffer(chunk_header.view()), asio::buffer(chunk), asio::buffer(http1_chunk_data_terminator)};
            coalesce_tls_frame(buffers, chunk_header.view(), chunk, http1_chunk_data_terminator);
        }
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto write_result = co_await write_to_stream(buffers);
            ec = write_result.error_code();
        }
        if (ec) {
            fail_write(ec, "failed to write HTTP response stream body");
        }
    }

    task<void> end(std::span<const http_header_view> trailers) {
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }
        ensure_peer_writable();

        const auto trailer_section = validate_http_response_trailers(trailers);
        const auto trailer_intent = response_trailer_intent(trailer_section);
        http_clear_string_retaining_small(trailers_);
        if (!trailer_section.empty()) {
            append_http1_response_trailers(trailers_, trailer_section);
        }

        co_await commit_head(trailer_intent);
        ensure_peer_writable();
        if (state_.ended()) {
            co_return;
        }
        if (compression_.active()) {
            try {
                compression_.finish();
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_await write_encoded(compression_.output());
            compression_.release_output();
        }
        ensure_peer_writable();
        if (plan_.framing() == http_response_stream_framing::http1_close_delimited) {
            // No last-chunk terminator: the connection close delimits the body.
            state_.mark_ended();
            co_return;
        }

        // The protocol primitive owns the last-chunk and trailer-section delimiters;
        // this runtime layer only submits their byte views to the socket.
        std::array<asio::const_buffer, 3> buffers{asio::buffer(http1_last_chunk_prefix),
            asio::buffer(trailers_), asio::buffer(http1_trailer_section_terminator)};
        coalesce_tls_frame(buffers, http1_last_chunk_prefix, trailers_, http1_trailer_section_terminator);
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto write_result = co_await write_to_stream(buffers);
            ec = write_result.error_code();
        }
        if (ec) {
            fail_write(ec, "failed to write HTTP response stream end");
        }
        state_.mark_ended();
    }

    stream_type& stream_;
    http_response_head_buffer& head_;
    std::string trailers_;
    connection_scanner::entry& scanner_entry_;
    // The connection/server owns an address-stable handle for the complete
    // route dispatch. Streaming must not acquire shared ownership per request.
    const worker_ref& worker_;
    const http_request& request_;
    const http_cors_policy* cors_;
    http1_response_stream_plan plan_;
    http1_response_stream_plan consumed_plan_;
    lazy_buffered_body<stream_type>& request_body_;
    http1_request_connection_plan connection_plan_;
    http_streaming_response_compression compression_;
    response_stream_state state_;
    stop_source& request_stop_;
    bool request_pipelined_;
    std::optional<http1_peer_eof_monitor<stream_type>> peer_monitor_;
    bool peer_monitor_retired_{false};
    operation_interrupt_source output_interrupts_;
};

// `request_bytes` is the connection's buffered input at dispatch, from the
// start of this request, the suffix included; `pipeline_stash` is
// request-scoped storage for a pipelined request the body reader over-read.
template <typename stream_type>
task<http1_session_request_completion> dispatch_http_response_stream_route(http1_route_dispatch<stream_type> d,
    http_response_head_buffer& response_head, const http1_server_request_head_ready& request_head,
    const resolved_route& resolved, std::string_view request_bytes, protocol_byte_limit body_limit_value,
    std::string& pipeline_stash) {
    const auto body_and_pipeline = http_body_and_pipeline(request_head, request_bytes);
    http_lazy_buffered_body_route_state<stream_type> body_state;
    const auto body_setup_failure =
        try_prepare_http_lazy_buffered_body_route(body_state, d, body_limit_value, body_and_pipeline);
    if (body_setup_failure != nullptr) {
        co_return co_await complete_failed_http_route(d.scanner_entry_, body_setup_failure, d.parsed_,
            d.routes_, d.base_route_services_, d.response_);
    }
    const auto close_policy = d.request_sequence_.next_response_close_policy();
    // A body that arrived in full with the head does not cost the stream its
    // reuse even if the handler never reads it: completion consumes it
    // without I/O.
    const auto unread_body_consumption =
        buffered_request_body_bytes(d.parsed_.body_plan(), body_and_pipeline, body_limit_value)
            ? http1_request_body_consumption::complete
            : http1_request_body_consumption::incomplete;
    const auto stream_plan = http1_plan_response_stream(d.parsed_, close_policy, unread_body_consumption);
    using response_sink_type = response_stream_sink<stream_type>;
    const auto& route = resolved.route();
    const auto& endpoint = *route.endpoint().response_stream();
    body_reader stream_body_facade = make_body_reader_facade(body_state.body_->loader().reader());
    if (endpoint.request_body_mode() == request_body_mode::stream) {
        d.base_route_services_.bind_streaming_request_body(stream_body_facade);
        d.base_route_services_.bind_request_trailers(body_state.body().trailers());
    } else {
        d.base_route_services_ = body_state.with_loader(d.base_route_services_);
    }
    // The client closing the connection while the handler waits, or a failed
    // write, stops the request: context::stop_token() combines it with the
    // worker's stop, so every framework wait of the handler observes it.
    stop_source peer_stop;
    const stop_group request_stop(d.base_route_services_.get_stop_token(), peer_stop.token());
    const auto request_stop_token = request_stop.token();
    d.base_route_services_.bind_stop_token(request_stop_token);
    response_sink_type response_sink(d.stream_, *d.options_.codecs_, response_head, d.scanner_entry_,
        d.base_route_services_.worker(), d.parsed_.request(), d.cors_,
        stream_plan,
        http1_plan_response_stream(d.parsed_, close_policy, http1_request_body_consumption::complete),
        body_state.body(), d.response_coding_, d.options_.compression_ ? &*d.options_.compression_ : nullptr,
        peer_stop, d.request_pipelined_);

    // The handler stays in the session's idle deadman phase; the sink times
    // its own writes.
    std::optional<response_stream_dispatch_result> dispatched;
    std::exception_ptr escaped_failure;
    try {
        // This request arrived before the peer ended its request direction:
        // that end is no abandonment of it, and the transport has no input left.
        if (d.request_sequence_.request_input_ended()) {
            response_sink.retire_peer_monitor();
        }
        response_sink.arm_peer_monitor();
        dispatched.emplace(co_await dispatch_response_stream_with(response_sink, d.routes_, d.parsed_.request(),
            resolved, d.base_route_services_,
            [&response_sink] { return response_sink.peer_aborted(); }));
    } catch (...) {
        escaped_failure = std::current_exception();
    }
    // Even error recovery can throw. Drain the reader before the body, sink,
    // stream, or their borrowed request storage may be destroyed or reused.
    response_sink.retire_peer_monitor();
    if (response_sink.peer_eof_monitoring()) {
        try {
            co_await response_sink.finish_peer_monitor();
        } catch (...) {
            const auto monitor_failure = std::current_exception();
            const auto* reported_failure = dispatched ? dispatched->failed_after_commit() : nullptr;
            if (escaped_failure != nullptr || reported_failure != nullptr) {
                const auto prior_failure = escaped_failure != nullptr
                                               ? escaped_failure
                                               : reported_failure->exception();
                if (monitor_failure != prior_failure) {
                    d.options_.connection_failure_.invoke(
                        d.base_route_services_.remote_address(), monitor_failure);
                }
            } else {
                escaped_failure = monitor_failure;
            }
        }
    }
    if (response_sink.request_input_ended()) {
        d.request_sequence_.mark_request_input_ended();
    }
    if (escaped_failure != nullptr) {
        if (!response_sink.committed()) {
            std::rethrow_exception(escaped_failure);
        }
        dispatched.emplace(response_stream_dispatch_result::make_failed_after_commit(
            committed_response_stream_status(response_sink), escaped_failure));
    }
    auto& result_value = *dispatched;

    if (const auto* failed = result_value.failed_after_commit()) {
        if (auto* lifecycle = d.base_route_services_.connection_lifecycle_value()) {
            lifecycle->note_exception(failed->exception());
        }
        d.options_.connection_failure_.invoke(
            d.base_route_services_.remote_address(), failed->exception());
        co_return http1_session_request_completion::make_truncated_stream(
            response_sink.connection_plan(), failed->status());
    }
    // The cancelled read can deliver a peer end after dispatch already
    // returned. Classify only after joining, when its final result is known.
    if (result_value.aborted_before_commit() != nullptr || result_value.aborted_after_commit() != nullptr || response_sink.peer_aborted()) {
        if (!response_sink.committed()) {
            throw inbound_transport_error(asio::error::eof, "HTTP response stream aborted before commit");
        }
        co_return http1_session_request_completion::make_truncated_stream(
            response_sink.connection_plan(), committed_response_stream_status(response_sink));
    }

    if (auto* recovered = result_value.recovered_failure()) {
        d.base_route_services_.set_connection_close_reason(connection_close_reason::handler_error);
        d.response_ = std::move(*recovered).take_response();
        d.scanner_entry_.touch();
        co_return close_with_framework_response(d.response_, stream_plan.request_connection_plan());
    }
    auto& body_reader_value = body_state.body().reader();
    body_reader_value.consume_buffered_body();
    if (auto* route_response = result_value.route_response()) {
        d.response_ = std::move(*route_response).take_response();
        d.scanner_entry_.touch();
        co_return complete_successful_http_body_route(d.scanner_entry_, d.response_, d.parsed_.connection_plan(),
            d.request_sequence_, body_reader_value.consumption(), request_bytes.size(), pipeline_stash,
            [&body_reader_value, &response_sink](std::string& stash) {
                return body_reader_value.take_pipeline(stash, response_sink.monitored_pipeline());
            });
    }

    // Only the completed committed alternative remains.
    const auto committed_status = result_value.committed_status();

    auto connection_plan = response_sink.connection_plan();
    d.request_sequence_.complete_committed_response(connection_plan);
    // A reusable head promised a body read in full or buffered in full; a
    // read the handler failed after commit breaks that promise, so the
    // connection closes after the stream instead.
    if (body_reader_value.consumption() != http1_request_body_consumption::complete) {
        connection_plan = connection_plan.require_close();
    }
    if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
        co_return http1_session_request_completion::make_committed_stream(
            connection_plan, *committed_status, request_head.header_bytes());
    }
    // What followed the body is the next request, in the read buffer or in
    // the reader's own over-read.
    const auto pipeline = body_reader_value.take_pipeline(pipeline_stash, response_sink.monitored_pipeline());
    if (!pipeline.stashed_.empty()) {
        co_return http1_session_request_completion::make_committed_stream_pipeline_restore(
            connection_plan, *committed_status, pipeline.stashed_);
    }
    co_return http1_session_request_completion::make_committed_stream(
        connection_plan, *committed_status, request_bytes.size() - pipeline.read_buffer_suffix_bytes_);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Serialized output still bounded by one coalesced write. A pipelined
// connection holds small buffered responses back while their successors are
// already buffered, then sends them with the response that ends the run.
inline constexpr std::size_t pipelined_output_batch_bytes = std::size_t{16} * 1024;

// One write of a connection's serialized response output, optionally followed
// by a borrowed body. The output is the work set's head buffer: the current
// response's head, preceded by any pipelined responses held back for this
// write. A write starts with one non-blocking attempt on plain TCP; only a
// remainder suspends, through async_asio at the caller's co_await site so a
// response the socket accepts at once costs no coroutine frame:
//
//     if (!output.start(body)) {
//         auto completion = co_await async_asio<std::size_t>(
//             [&output](auto handler) { output.write_remaining(std::move(handler)); });
//         output.finish(completion.error_code(), completion.result());
//     }
//     const auto written = output.complete();
//
// complete() reports every held-back response whose head the write committed
// and empties the output. The output must be empty before anything else
// writes to the stream (interim heads, streams, upgrades, file bodies) and
// before the session reads or closes.
template <typename stream_type>
class http1_response_output final {
public:
    http1_response_output(stream_type& stream, http_response_head_buffer& output,
        http1_deferred_access_log& deferred_access, const access_log_sink& access_log,
        ip_address remote_address, connection_scanner::entry& progress) noexcept
        : stream_(&stream),
          output_(&output),
          deferred_access_(&deferred_access),
          access_log_(&access_log),
          remote_address_(remote_address),
          progress_(&progress) {}

    http1_response_output(const http1_response_output&) = delete;
    http1_response_output& operator=(const http1_response_output&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return output_->view().empty();
    }

    // Begins writing the output followed by `body`, which stays borrowed until
    // complete(). Returns true when the write already settled.
    // A final plain response half-closes TCP here only after every byte was
    // accepted, so its Linux send hint is flushed before any logging or frame
    // cleanup; the session's lingering close then waits for the peer's end.
    // A partial write falls back to ordinary async_write without that hint.
    // MSVC 19.51 can lose a required coroutine tail call when this synchronous
    // path is inlined into its caller.
    [[nodiscard]]
#if defined(_MSC_VER) && !defined(__clang__)
    __declspec(noinline)
#endif
    bool start(std::string_view body = {}, bool finish_connection = false) noexcept {
        body_ = body;
        written_ = {};
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
            const auto flags = finish_connection ? final_plain_tcp_send_flags : 0;
            const auto output = output_->view();
            const bool completed = body.empty()
                                       ? try_plain_tcp_sync_write(*stream_, asio::buffer(output), output.size(),
                                             flags, written_.error_, written_.bytes_)
                                       : try_plain_tcp_sync_write(*stream_,
                                             std::array<asio::const_buffer, 2>{asio::buffer(output), asio::buffer(body)},
                                             output.size() + body.size(), flags, written_.error_, written_.bytes_);
            if (written_.bytes_ != 0) {
                progress_->note_write_progress();
            }
            if (completed && !written_.error_ && finish_connection) {
                shutdown_http_connection_send(stream_->lowest_layer());
            }
            return completed;
        } else {
            return false;
        }
    }

    // Initiates the asynchronous write of what start() left. Every partial
    // transfer touches the scanner entry, so write_timeout measures
    // inactivity rather than the time to drain the whole output.
    template <typename handler_type>
    void write_remaining(handler_type&& handler) {
        progress_->set_phase(connection_scanner::phase::writing);
        async_write_after(*stream_, output_->view(), body_, written_.bytes_, *progress_, std::forward<handler_type>(handler));
    }

    [[nodiscard]] auto async_write_remaining() {
        return async_asio<std::size_t>(
            [this](auto handler) { this->write_remaining(std::move(handler)); });
    }

    // Records the asynchronous remainder's outcome.
    void finish(std::error_code error, std::size_t bytes) noexcept {
        written_.error_ = error;
        written_.bytes_ += bytes;
    }

    // Reports the held-back responses whose heads were written, empties the
    // output and returns the write's outcome.
    [[nodiscard]] connection_write_outcome complete() noexcept {
        deferred_access_->complete(*access_log_, remote_address_, written_.bytes_);
        output_->reset();
        body_ = {};
        return written_;
    }

    // Writes held-back output through a coroutine, for the rare writers
    // (interim heads, file responses) that need the output empty first. A
    // failed write leaves the connection unusable. The caller's scanner phase
    // (a handler's idle deadman, a body read, a request-head read) resumes
    // once the write settles.
    [[nodiscard]] task<std::error_code> flush() {
        if (empty()) {
            co_return std::error_code{};
        }
        if (!start()) {
            const connection_scanner::entry::phase_scope caller_phase(*progress_);
            const auto completion = co_await async_write_remaining();
            finish(completion.error_code(), completion.result());
        }
        co_return complete().error_;
    }

private:
    stream_type* stream_;
    http_response_head_buffer* output_;
    http1_deferred_access_log* deferred_access_;
    const access_log_sink* access_log_;
    ip_address remote_address_{};
    connection_scanner::entry* progress_;
    std::string_view body_;
    connection_write_outcome written_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {
// Reads more of a cleartext connection's first bytes, which so far match the
// prior-knowledge client preface (RFC 9113 §3.4), under the request-header
// deadlines. Returns false when the read ends the connection. Once the preface
// is complete, the HTTP/1 connection hands its pending bytes to the HTTP/2
// session and returns its working memory first.
template <typename stream_type>
task<bool> read_more_cleartext_http2_preface(
    stream_type& stream, connection_scanner::entry& scanner_entry, connection_read_buffer& read_buffer) {
    scanner_entry.read_request_head(connection_scanner::phase::reading_initial);
    const auto space = read_buffer.prepare_read();
    auto read_completion = co_await ruvia::async_asio<std::size_t>([&stream, space](auto handler) mutable {
        stream.async_read_some(asio::buffer(space), std::move(handler));
    });
    if (read_completion.error_code()) {
        co_return false;
    }
    read_buffer.commit_read(read_completion.result());
    scanner_entry.touch();
    co_return true;
}

template <typename stream_type>
class http1_interim_response_sink final {
public:
    // `request` is the session's reused parse target; it is borrowed for every
    // dispatch that can reach output(). Pipelined responses still held back in
    // `pending` precede any interim head on the wire.
    http1_interim_response_sink(stream_type& stream, const http_request& request,
        http1_response_output<stream_type>& pending, connection_scanner::entry& scanner_entry)
        : stream_(stream),
          request_(request),
          pending_(pending),
          scanner_entry_(scanner_entry),
          output_(this, write) {}
    [[nodiscard]] http_interim_response_output& output() noexcept {
        return output_;
    }

private:
    static task<void> write(void* raw, const http_interim_response_head& response) {
        auto& self = *static_cast<http1_interim_response_sink*>(raw);
        // A typical interim head fits the head buffer's inline storage; a
        // larger one is encoded again into heap storage of exactly its size.
        http_response_head_buffer head;
        auto encoded = prepare_http1_interim_response_head(
            response, std::span<char>(head.stack_cursor(response_head_stack_bytes), response_head_stack_bytes));
        if (const auto* too_small = std::get_if<http1_interim_response_buffer_too_small>(&encoded)) {
            const auto head_bytes = too_small->required_head_bytes();
            head.append_generated(head_bytes, [&](char* out) noexcept {
                encoded = prepare_http1_interim_response_head(response, std::span<char>(out, head_bytes));
            });
        }
        // Validation threw http1_response_head_error before any buffer was
        // touched; an exactly sized buffer always holds the prepared head.
        const auto* prepared = &std::get<prepared_http1_interim_response>(encoded);
        if (prepared->connection_disposition() != http1_interim_connection_disposition::unchanged) {
            throw std::invalid_argument("application interim response cannot close the exchange");
        }
        if (self.request_.protocol_version() == http_protocol_version::http10) {
            // RFC 9110 §15.2: a server MUST NOT send a 1xx response to an
            // HTTP/1.0 client, which would take it as the final response.
            // Interim heads are advisory, so the exchange simply omits it.
            co_return;
        }
        if (!self.pending_.empty()) {
            if (const auto error = co_await self.pending_.flush()) {
                throw inbound_transport_error(error, "failed to write HTTP/1 responses before an interim response");
            }
        }
        const connection_scanner::entry::write_scope write_stall_clock(self.scanner_entry_);
        const auto completion = co_await async_asio<std::size_t>([&self, bytes = prepared->head()](auto handler) {
            asio::async_write(
                self.stream_, asio::buffer(bytes), connection_write_progress(self.scanner_entry_), std::move(handler));
        });
        if (completion.error_code()) {
            throw inbound_transport_error(completion.error_code(), "failed to write HTTP/1 interim response");
        }
    }
    stream_type& stream_;
    const http_request& request_;
    http1_response_output<stream_type>& pending_;
    connection_scanner::entry& scanner_entry_;
    http_interim_response_output output_;
};

template <typename stream_type>
task<http1_session_request_completion> dispatch_http_stream_body_route(http1_route_dispatch<stream_type> d,
    const http1_server_request_head_ready& request_head, const route_resolution& route_resolution_value,
    protocol_byte_limit body_limit, std::string_view request_bytes, std::string& pipeline_stash) {
    const auto body_and_pipeline = http_body_and_pipeline(request_head, request_bytes);

    std::exception_ptr exception;
    std::optional<body_reader_binding<stream_body_reader<stream_type>>> body_reader;
    try {
        // The reader's buffers are inbound memory: they count against the
        // connection's and the worker's inbound budgets.
        body_reader.emplace(d.stream_, &d.inbound_buffers_,
            body_and_pipeline, d.parsed_.body_plan(), body_limit, d.scanner_entry_,
            *d.base_route_services_.interim_output(), d.base_route_services_.worker());
        auto services = d.base_route_services_;
        services.bind_streaming_request_body(body_reader->facade());
        services.bind_request_trailers(body_reader->reader().trailers());
        co_await d.routes_.dispatch_buffered_response(d.parsed_.request(), route_resolution_value, document_root_binding::none(), services,
            static_file_selection_mode::identity_only, d.response_);
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exception_services = d.base_route_services_;
        if (body_reader) {
            exception_services.bind_streaming_request_body(body_reader->facade());
            exception_services.bind_request_trailers(body_reader->reader().trailers());
        }
        co_return co_await complete_failed_http_route(d.scanner_entry_, exception, d.parsed_,
            d.routes_, exception_services, d.response_);
    }

    co_return complete_successful_http_body_route(d.scanner_entry_, d.response_, d.parsed_.connection_plan(),
        d.request_sequence_, request_bytes.size(), pipeline_stash, body_reader->reader());
}

// Admission already validated a CONNECT-UDP head (`udp`). HTTP/1 framing adds
// one rule of its own: a CONNECT request cannot carry request content. A
// successful handshake is access-logged through `tail` once its head reaches
// the transport; a buffered answer is left to the session's ordinary tail.
[[nodiscard]] inline http1_response_head_plan prepare_http1_tunnel_response_head_plan(
    const http_response& head_response, http_protocol_version version, bool udp) {
    if (!udp) {
        return prepare_http1_connect_response_head(head_response, version);
    }
    return prepare_http1_connect_udp_response_head(head_response);
}

template <typename stream_type>
struct http1_tunnel_upgrade_commit final {
    using transport_type = http_socket_tunnel_transport<stream_type>;

    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    bool udp_;
    std::optional<tls_tunnel_output>& tls_output_;

    // An HTTP/1.1 Upgrade (a request stream's accept_tunnel()) becomes the
    // 101 head; CONNECT and CONNECT-UDP keep their own heads.
    [[nodiscard]] task<transport_type> operator()(context& context_value, http_response& head_response) const {
        const auto& request = dispatch_.parsed_.request();
        const auto plan = !udp_ && request.known_method() != http_known_method::connect
                              ? prepare_http1_upgrade_response_head(head_response, request)
                              : prepare_http1_tunnel_response_head_plan(head_response, request.protocol_version(), udp_);
        http_response_head_buffer head;
        append_http1_response_head(head_response, head, plan);
        context_access::commit_upgrade(context_value);
        if (const auto ec = co_await write_http1_upgrade_response_head(dispatch_.stream_,
                dispatch_.parsed_.body_plan().expectations(), head_response.status(), head.view(),
                dispatch_.scanner_entry_);
            ec) {
            throw inbound_transport_error(ec, "CONNECT head write");
        }
        tail_.record_access(dispatch_.options_.access_log_, head_response.status());
        co_return make_transport();
    }

    [[nodiscard]] transport_type make_transport() const {
        if constexpr (requires { dispatch_.stream_.next_layer(); }) {
            tls_output_.emplace(*dispatch_.stream_.native_handle(), dispatch_.stream_.lowest_layer(),
                dispatch_.base_route_services_.worker(), dispatch_.base_route_services_.connection_lifecycle_value());
            tls_output_->start();
        }
        return transport_type(dispatch_.stream_, tls_output_ ? &*tls_output_ : nullptr, &dispatch_.scanner_entry_);
    }

    [[nodiscard]] task<websocket_upgrade_attempt<transport_type>> accept_websocket(
        context& context_value, const websocket_route_endpoint& endpoint, std::span<const http_header_view> headers) const {
        const auto validation = validate_websocket_handshake(dispatch_.parsed_.request(),
            dispatch_.parsed_.body_plan(), endpoint.handshake_validation());
        if (const auto* failure = validation.failure()) {
            co_return reject_request_stream_websocket(context_value, *failure);
        }
        const auto handshake = make_websocket_server_handshake(dispatch_.parsed_.request(),
            {.supported_subprotocols_ = endpoint.subprotocols(), .response_headers_ = headers, .deflate_ = endpoint.deflate()});
        context_access::commit_upgrade(context_value);
        if (const auto error = co_await write_websocket_handshake(dispatch_.stream_, handshake,
                dispatch_.parsed_.body_plan().expectations(), dispatch_.scanner_entry_)) {
            throw inbound_transport_error(error, "WebSocket handshake write");
        }
        tail_.record_access(dispatch_.options_.access_log_, http_status::switching_protocols);
        co_return websocket_upgrade_commit<transport_type>(
            make_transport(), handshake.compression(), handshake.subprotocol());
    }
};

template <typename stream_type>
[[nodiscard]] task<std::optional<http1_session_request_completion>> dispatch_http_tunnel_route(
    http1_route_dispatch<stream_type> d, http_response_tail& tail, const resolved_route& resolved, bool udp,
    std::string_view pending) {
    if (d.parsed_.body_plan().requires_consumption()) {
        // A generated rejection does not inherit the negotiated coding promise.
        tail.disable_response_coding();
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request(),
            http_error_info({.status_ = http_status::bad_request, .message_ = "CONNECT route does not accept HTTP request content"}), d.base_route_services_);
        co_return close_with_framework_response(d.response_, d.parsed_.connection_plan());
    }
    // The pending bytes remain owned by the HTTP input budget until acceptance
    // hands them to the tunnel. A refusal uses the ordinary framed sink; its
    // peer monitor starts only when that refusal is actually committed.
    http_lazy_buffered_body_route_state<stream_type> body_state;
    if (const auto setup_failure = try_prepare_http_lazy_buffered_body_route(
            body_state, d, protocol_byte_limit::unlimited(), pending)) {
        std::rethrow_exception(setup_failure);
    }
    stop_source peer_stop;
    const stop_group request_stop(d.base_route_services_.get_stop_token(), peer_stop.token());
    const auto request_stop_token = request_stop.token();
    d.base_route_services_.bind_stop_token(request_stop_token);
    http_response_head_buffer refusal_head;
    const auto refusal_plan = http1_plan_response_stream(d.parsed_, http1_close_policy::close_after_response,
        http1_request_body_consumption::complete);
    response_stream_sink<stream_type> refusal_sink(d.stream_, *d.options_.codecs_, refusal_head, d.scanner_entry_,
        d.base_route_services_.worker(), d.parsed_.request(), d.cors_, refusal_plan, refusal_plan, body_state.body(),
        d.response_coding_, d.options_.compression_ ? &*d.options_.compression_ : nullptr,
        peer_stop, d.request_pipelined_);
    auto refusal_writer = make_response_stream_writer(refusal_sink);
    if (is_request_stream_tunnel_route(resolved.route())) {
        d.base_route_services_.bind_response_stream(refusal_writer);
    }
    std::optional<tls_tunnel_output> tls_output;
    // The TLS output pump must be stopped and joined on every exit, including
    // a handler failure that execute_tunnel_upgrade_session surfaces.
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    const http1_tunnel_upgrade_commit<stream_type> commit{
        .dispatch_ = d,
        .tail_ = tail,
        .udp_ = udp,
        .tls_output_ = tls_output,
    };
    auto commit_websocket = [&commit](context& context_value, const websocket_route_endpoint& endpoint,
                                std::span<const http_header_view> headers) {
        return commit.accept_websocket(context_value, endpoint, headers);
    };
    try {
        buffered = co_await execute_tunnel_upgrade_session<http_socket_tunnel_transport<stream_type>>(
            d.routes_, d.parsed_.request(), resolved, d.base_route_services_,
            d.options_, d.scanner_entry_, d.inbound_buffers_, udp, pending,
            commit, commit_websocket);
    } catch (...) {
        failure = std::current_exception();
    }
    refusal_sink.retire_peer_monitor();
    try {
        co_await refusal_sink.finish_peer_monitor();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    if (tls_output) {
        if (failure) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (refusal_sink.committed()) {
        co_return http1_session_request_completion::make_committed_stream(
            refusal_sink.connection_plan().require_close(), committed_response_stream_status(refusal_sink), 0);
    }
    if (buffered) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan());
    }
    co_return std::nullopt;
}

// A rejected upgrade returns the exact HTTP/1 request completion that the
// session must write and clean up. A successful upgrade is access-logged
// through `tail` once its handshake reaches the transport, then transfers
// transport ownership to the websocket session, so no HTTP request completion
// remains.
template <typename stream_type>
struct http1_websocket_upgrade_commit final {
    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    const websocket_route_endpoint& websocket_endpoint_;

    [[nodiscard]] task<websocket_upgrade_commit<stream_type>> operator()(
        context& context_value, std::span<const http_header_view> response_headers_value) const {
        const auto handshake = make_websocket_server_handshake(dispatch_.parsed_.request(),
            {.supported_subprotocols_ = websocket_endpoint_.subprotocols(),
                .response_headers_ = response_headers_value,
                .deflate_ = websocket_endpoint_.deflate()});
        context_access::commit_upgrade(context_value);
        if (const auto ec = co_await write_websocket_handshake(dispatch_.stream_, handshake,
                dispatch_.parsed_.body_plan().expectations(), dispatch_.scanner_entry_);
            ec) {
            throw inbound_transport_error(ec, "WebSocket handshake write");
        }
        tail_.record_access(dispatch_.options_.access_log_, http_status::switching_protocols);
        co_return websocket_upgrade_commit<stream_type>(
            dispatch_.stream_, handshake.compression(), handshake.subprotocol());
    }
};

template <typename stream_type>
task<std::optional<http1_session_request_completion>> dispatch_http_websocket_route(
    http1_route_dispatch<stream_type> d, http_response_tail& tail, const resolved_route& resolved,
    std::string_view pending_frames) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    const auto handshake_validation = validate_websocket_handshake(
        d.parsed_.request(), d.parsed_.body_plan(), websocket_endpoint.handshake_validation());
    if (const auto* failure = handshake_validation.failure()) {
        // A generated rejection does not inherit the negotiated coding promise.
        tail.disable_response_coding();
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request(),
            http_error_info({.status_ = failure->protocol_error().status(), .code_ = failure->protocol_error().code(), .message_ = std::string_view(failure->protocol_error().what())}),
            d.base_route_services_);
        failure->apply_required_response_headers(d.response_);
        co_return close_with_framework_response(d.response_, d.parsed_.connection_plan());
    }
    auto buffered = co_await execute_websocket_upgrade_session<stream_type>(
        d.routes_, d.parsed_.request(), resolved, d.base_route_services_,
        d.options_, d.scanner_entry_, d.inbound_buffers_, pending_frames,
        http1_websocket_upgrade_commit<stream_type>{
            .dispatch_ = d,
            .tail_ = tail,
            .websocket_endpoint_ = websocket_endpoint,
        });
    if (buffered) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan());
    }
    co_return std::nullopt;
}

// RFC 9112 §3.2.2: an absolute-form target's authority is the request's host,
// and an HTTP/1.0 request may carry it without any Host field.
[[nodiscard]] inline std::string_view auto_https_redirect_authority(const http_request& request) noexcept {
    if (!request.authority().empty()) {
        return request.authority();
    }
    return request.header("Host").value_or(std::string_view{});
}

inline http_response make_auto_https_redirect_response(
    const http_request& request, std::uint16_t https_port) {
    http_response response;
    response.status(ruvia::http_status::permanent_redirect);

    const auto authority = parse_http_authority(auto_https_redirect_authority(request));
    const auto host = authority ? authority->host() : std::string_view{};
    auto path = request.path();
    if (path.empty() || path.front() != '/') {
        path = "/";
    }

    // The raw target retains the distinction between an absent query and an
    // empty query ("/x" versus "/x?"); query_string() alone cannot express it.
    const bool has_query = (request.target().find('?') != std::string_view::npos);
    const auto query = request.query_string();
    std::array<char, http_max_unsigned_decimal_size> port_buffer{};
    const auto port_digits = https_port == 443 ? std::string_view{}
                                               : http_format_unsigned(port_buffer, https_port);
    static constexpr std::string_view scheme_prefix = "https://";
    const std::size_t total_size = scheme_prefix.size() + host.size() +
                                   (port_digits.empty() ? 0U : 1U + port_digits.size()) +
                                   path.size() + (has_query ? 1U + query.size() : 0U);
    std::string location;
    location.reserve(total_size);
    location.append(scheme_prefix);
    location.append(host);
    if (!port_digits.empty()) {
        location.push_back(':');
        location.append(port_digits);
    }
    location.append(path);
    if (has_query) {
        location.push_back('?');
        location.append(query);
    }
    response.header("Location", location);
    // The Location is derived from the request's Host header, so this redirect
    // varies by Host. Mark it private so a shared cache never stores one Host's
    // redirect and serves it for another (a Host-header cache-poisoning open
    // redirect); a browser still caches it per-origin, keeping the HTTPS memory.
    response.header_stable_view("Cache-Control", "private");
    return response;
}

[[nodiscard]] inline std::optional<http_request_body_failure> content_length_limit_failure(
    const http1_request_body_plan& body_plan, protocol_byte_limit limit) noexcept {
    const auto* known_length = body_plan.known_length();
    return known_length == nullptr ? std::nullopt
                                   : http_request_body_size_failure(known_length->content_length(), limit);
}
// Whether a pipelined request can be served while earlier responses wait in
// the connection's output: its handling neither reads the socket (no request
// content, hence no 100 Continue) nor writes to it ahead of its own buffered
// response (no stream, tunnel or upgrade). Interim heads flush the output
// themselves.
[[nodiscard]] inline bool http1_request_follows_held_output(
    const http1_server_request_parse_state& parsed_value, const route_resolution& resolution) noexcept {
    if (parsed_value.body_plan().requires_consumption()) {
        return false;
    }
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        return true;
    }
    const auto* buffered = resolved->route().endpoint().buffered();
    return buffered != nullptr && buffered->request_body_mode() == request_body_mode::buffered;
}

// How a failed request-head read closes. A clean peer end (FIN, or TLS
// close_notify), or a timeout that the scanner signalled by cancelling the
// read, ends a connection that owes no response: TLS answers with
// close_notify. Any other failure closes at once.
[[nodiscard]] inline http_connection_close http1_request_head_read_close(
    const std::error_code& error, const connection_scanner::entry& scanner_entry) noexcept {
    return error == asio::error::eof || scanner_entry.close_requested() ? http_connection_close::notify
                                                                        : http_connection_close::abort;
}

// What every request of one connection shares: the resident state run_stream
// keeps for the connection's whole life. serve_requests borrows it while the
// connection is active.
template <typename stream_type>
struct http1_connection_session final {
    http_server_session_config& listener_;
    stream_type& stream_;
    connection_scanner::entry& scanner_entry_;
    // The connection's route services, bound to its inbound buffer budget.
    context_services& route_services_;
    inbound_buffer_account& inbound_buffers_;
    http1_request_sequence& request_sequence_;
    const http_request_admission_policy& admission_policy_;
    const response_file_io& file_output_;
    std::string_view remote_address_;
    // nginx-aligned wait semantics. The first request on a connection, and any
    // partially-received request header, are bounded by request_header_timeout
    // (reading_initial). The idle wait for the *next* request on a reused
    // keep-alive connection -- no bytes of it received yet -- is bounded by
    // idle_timeout (idle), matching nginx keepalive_timeout. Flips true
    // once a request has been served so later idle waits use the keepalive
    // deadline instead of the header deadline.
    bool served_keepalive_request_{false};
};

[[nodiscard]] inline std::exception_ptr try_start_buffered_response_write(
    http_response_head_buffer& response_head, const http_response& response,
    const http1_buffered_response_plan& planned_response, bool next_request_buffered,
    std::optional<http1_response_write_start>& started_write, bool coalesce_for_tls = false) {
    const auto held_output_bytes = response_head.view().size();
    try {
        started_write.emplace(start_response_write(response_head, response, planned_response,
            next_request_buffered ? pipelined_output_batch_bytes : 0, coalesce_for_tls));
        return nullptr;
    } catch (...) {
        if (response_head.view().size() != held_output_bytes) {
            // Never flush an orphan head or append an error response as
            // its body. Propagate the real failure and abort this connection.
            throw;
        }
        return std::current_exception();
    }
}

template <typename stream_type>
task<http_connection_close> worker_connections::run_stream(http_server_session_config& listener_value, stream_type& stream,
    tcp_socket_type& socket, context_services base_route_services) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, the keep-alive request sequence, the remote address, and the
    // inbound buffer budget. Everything a request needs beyond that is held
    // only while the connection is actively serving and given back the moment
    // it goes idle, so an idle keep-alive connection holds none of it: the
    // heavy working set (read buffer, request arena, parse result, response
    // head) is allocated from the worker heap, and the request state lives in
    // the frame of serve_requests.
    connection_scanner::entry scanner_entry;
    scanner_entry.bind_lifecycle(base_route_services.connection_lifecycle_value());
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    const auto admission_policy = http_request_admission_policy::server(options_);
    const response_file_io file_output{
        .worker_ = worker_,
        .stop_ = stop_token_,
        .timeout_ = options_.server_.write_timeout_,
    };
    inbound_buffer_account connection_buffers(
        *options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    base_route_services = base_route_services.with_inbound_buffers(connection_buffers);
    base_route_services.bind_routes(routes_);
    http1_request_sequence request_sequence(options_.server_.max_requests_per_connection_, state_);
    http1_connection_session<stream_type> session{
        .listener_ = listener_value,
        .stream_ = stream,
        .scanner_entry_ = scanner_entry,
        .route_services_ = base_route_services,
        .inbound_buffers_ = connection_buffers,
        .request_sequence_ = request_sequence,
        .admission_policy_ = admission_policy,
        .file_output_ = file_output,
        .remote_address_ = base_route_services.remote_address(),
    };
    // Connection-resident landing pad for the first bytes of a request that
    // arrives while the connection holds no work set (see the idle wait below).
    std::array<char, idle_resident_read_bytes> idle_read_buffer;

    if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
        enable_plain_tcp_sync_write(socket);
    }
    for (;;) {
        // Allocate-on-use / release-on-idle for the whole work set: the
        // connection holds none while it waits for its next request, and reads
        // that request's first bytes into the small connection-resident buffer
        // instead, so an idle keep-alive connection occupies no work set
        // (memory scales with in-flight requests, not total connections).
        // Reading directly -- rather than a bufferless readiness wait -- costs
        // no extra reactor pass when data is already queued. serve_requests
        // returns only once no input is buffered: a pipelined burst stays in
        // it. TLS reads through the stream as well, so plaintext the engine
        // already decrypted completes the read at once; whatever exceeds the
        // landing pad stays in the engine for the next read.
        //
        // A stopping worker has already closed the connection; it starts no
        // further read.
        if (!http_server_worker_running(state_)) {
            co_return http_connection_close::notify;
        }
        // Every request received before the peer ended its request direction
        // has been served; the transport only repeats that end.
        if (request_sequence.request_input_ended()) {
            co_return http_connection_close::notify;
        }
        // Idle wait for the next keep-alive request uses idle_timeout;
        // the connection's first request uses request_header_timeout.
        scanner_entry.read_request_head(session.served_keepalive_request_
                                            ? connection_scanner::phase::idle
                                            : connection_scanner::phase::reading_initial);
        auto idle_completion =
            co_await ruvia::async_asio<std::size_t>([&stream, &idle_read_buffer](auto handler) mutable {
                stream.async_read_some(asio::buffer(idle_read_buffer), std::move(handler));
            });
        const auto idle_ec = idle_completion.error_code();
        const auto idle_bytes = idle_completion.result();
        // A clean peer end or timeout is answered as in the request read loop.
        if (idle_ec) {
            co_return http1_request_head_read_close(idle_ec, scanner_entry);
        }
        auto work_set = make_connection_work_set();
        static_assert(idle_resident_read_bytes <= initial_read_buffer_bytes);
        work_set->read_buffer_.assign(std::string_view(idle_read_buffer.data(), idle_bytes));
        if (idle_bytes == idle_read_buffer.size()) {
            if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
                if (stream.non_blocking()) {
                    const auto read_space = work_set->read_buffer_.prepare_read();
                    if (!read_space.empty()) {
                        std::error_code extra_ec;
                        const auto extra_bytes = stream.read_some(asio::buffer(read_space), extra_ec);
                        if (!extra_ec && extra_bytes != 0) {
                            work_set->read_buffer_.commit_read(extra_bytes);
                        }
                    }
                }
            } else {
                for (int pending = SSL_pending(stream.native_handle()); pending > 0;
                    pending = SSL_pending(stream.native_handle())) {
                    const auto read_space = work_set->read_buffer_.prepare_read();
                    if (read_space.empty()) {
                        break;
                    }
                    std::size_t extra_bytes = 0;
                    const auto to_read = (std::min)(read_space.size(), static_cast<std::size_t>(pending));
                    if (SSL_read_ex(stream.native_handle(), read_space.data(), to_read, &extra_bytes) != 1 ||
                        extra_bytes == 0) {
                        break;
                    }
                    work_set->read_buffer_.commit_read(extra_bytes);
                }
            }
        }
        scanner_entry.touch();
        const auto served = co_await serve_requests(session, *work_set);
        if (const auto* close_mode = std::get_if<http_connection_close>(&served)) {
            co_return *close_mode;
        }
#if defined(RUVIA_ENABLE_HTTP2)
        if (std::get<http1_serve_handoff>(served) == http1_serve_handoff::cleartext_http2) {
            // The session consumes the buffered input in its first feed, before
            // it suspends, and reads into its own buffer afterwards: the HTTP/1
            // working memory is released before entering the HTTP/2 session.
            const std::string pending_input(work_set->read_buffer_.pending());
            work_set.reset();
            // The HTTP/1 request channels pointed into serve_requests' frame.
            auto http2_services = base_route_services;
            http2_services.bind_request_channels({});
            co_return co_await run_http2_sans_io_session(stream, routes_,
                http2_sans_io_session_context(std::move(http2_services), options_, scanner_entry), pending_input);
        }
#endif
    }
}

// Serves the connection's requests while it has input, and returns
// await_request once it holds no buffered bytes (run_stream then destroys the
// work set and waits for the next request), cleartext_http2 on a complete
// prior-knowledge preface, or how the connection closes. One frame per active
// period: a pipelined burst stays in it.
template <typename stream_type>
task<std::variant<http1_serve_handoff, http_connection_close>> worker_connections::serve_requests(
    http1_connection_session<stream_type>& session, connection_work_set& work_set) {
    auto& stream = session.stream_;
    auto& listener_value = session.listener_;
    auto& scanner_entry = session.scanner_entry_;
    auto& connection_buffers = session.inbound_buffers_;
    auto& request_sequence = session.request_sequence_;
    const auto& routes_value = routes_;
    auto& base_route_services = session.route_services_;
    const auto& admission_policy = session.admission_policy_;
    const auto& file_output = session.file_output_;
    const auto remote_address = session.remote_address_;
    constexpr bool plain_tcp = std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>;
    const auto* redirect = plain_tcp ? listener_value.redirect() : nullptr;
    auto& read_buffer = work_set.read_buffer_;
    auto& parsed_value = work_set.parsed_;
    auto& response_head = work_set.response_head_;
    auto& route_resolution_value = work_set.route_resolution_;
    // The first request of an active period arrived alone; every later one was
    // already (at least partly) buffered behind its predecessor: pipelined.
    bool request_pipelined = false;
    for (;;) {
        // Pipelined responses held back for one coalesced write. Bound to the
        // work set, which keeps any held output until it is written.
        http1_response_output<stream_type> response_output(stream, response_head,
            work_set.deferred_access_, options_.access_log_,
            base_route_services.get_conn_info().remote().address(), scanner_entry);

        // Work-set storage outlives this arena. Release its descriptor block
        // on every exit (including exceptions/cancellation) before the arena dies.
        struct request_header_lifetime final {
            http1_server_request_parse_state& parsed_;
            ~request_header_lifetime() {
                (void)std::move(parsed_).take_request();
            }
        } header_lifetime{parsed_value};
        http_response response;
        // Holds the next pipelined request when a body reader over-read it into
        // its own buffer, which dies with the route, until the read buffer is
        // cleaned up below. Declared before request_completion, which borrows
        // it; a suffix still in the read buffer is compacted in place instead.
        std::string pipeline_stash;
        http1_interim_response_sink interim_sink(
            stream, parsed_value.request(), response_output, scanner_entry);
        base_route_services.bind_interim_output(interim_sink.output());
        std::optional<http_request_admission> admission;
        std::optional<http1_session_request_completion> request_completion;
        // Rejections funnel through one co_await site after the read loop:
        // every co_await expression in a coroutine reserves its own frame
        // slots for the call's temporaries (GCC does not overlap them), so
        // inlining handle_error at each rejection site costs ~660 resident
        // bytes per site in every active connection's frame.
        std::optional<http_request_rejection> request_rejection;
        std::size_t header_search_offset = 0;
        // Request timing starts with the request's first bytes: the idle wait
        // above already has them; a pipelined request's arrived earlier.
        const auto request_start = options_.access_log_.callback_ ? std::chrono::steady_clock::now()
                                                                  : std::chrono::steady_clock::time_point{};
        for (;;) {
#if defined(RUVIA_ENABLE_HTTP2)
            if constexpr (plain_tcp) {
                // The client preface must be the first bytes of the connection
                // (RFC 9113 section 3.4). auto_https reserves the cleartext
                // listener for HTTP/1 redirects, and a connection that already
                // completed an HTTP/1 exchange stays HTTP/1, so both refuse
                // prior-knowledge HTTP/2.
                const auto preface_probe =
                    session.served_keepalive_request_ || redirect != nullptr
                        ? http2_cleartext_preface_probe::http1
                        : probe_http2_cleartext_preface(read_buffer.pending());
                if (preface_probe == http2_cleartext_preface_probe::complete_preface) {
                    co_return http1_serve_handoff::cleartext_http2;
                }
                if (preface_probe == http2_cleartext_preface_probe::drop_connection) {
                    base_route_services.set_connection_close_reason(connection_close_reason::protocol_error);
                    co_return http_connection_close::abort;
                }
                if (preface_probe == http2_cleartext_preface_probe::need_more_preface) {
                    if (!co_await read_more_cleartext_http2_preface(stream, scanner_entry, read_buffer)) {
                        co_return http_connection_close::abort;
                    }
                    continue;
                }
            }
#endif
            const auto buffer_view = read_buffer.pending();
            parsed_value.parse_head(buffer_view, header_search_offset);
            const auto* const ready_head = parsed_value.head_ready();
            if (ready_head != nullptr) {
                route_resolution_value = base_route_services.request_stream()
                                             ? resolve_request_stream_route(http1_request_stream_tunnel(
                                                   parsed_value.request(), parsed_value.body_plan().requires_consumption()))
                                             : routes_value.resolve(parsed_value.request());
            }
            // Held-back pipelined responses leave before anything that could
            // make the peer wait for them or reach the stream ahead of them:
            // a socket read, a rejection that closes, request content, or a
            // streamed, tunnelled or upgraded response.
            if (!response_output.empty() &&
                (ready_head == nullptr || !http1_request_follows_held_output(parsed_value, route_resolution_value))) {
                if (co_await response_output.flush()) {
                    co_return http_connection_close::abort;
                }
                scanner_entry.set_phase(connection_scanner::phase::idle);
            }
            if (const auto* request_head = ready_head) {
                base_route_services.note_connection_request();
                // Reset phase so request_header_timeout stops counting against dispatch
                // time. Body readers will set reading_payload on their own; upgraded
                // sessions enter long_lived; a response stream's sink times its own
                // writes; the buffered write path sets writing before responding.
                // Until one of those transitions, idle_timeout governs as the
                // deadman switch for hung handlers.
                scanner_entry.set_phase(connection_scanner::phase::idle);
                const auto expectation_plan =
                    parsed_value.body_plan().expectation_plan(http_unsupported_expectation_policy::reject);
                request_body_state initial_body;
                if (const auto* known = parsed_value.body_plan().known_length()) {
                    initial_body.framing_ = request_body_framing::known_length;
                    initial_body.content_length_ = known->content_length();
                } else if (parsed_value.body_plan().chunked() != nullptr) {
                    initial_body.framing_ = request_body_framing::chunked;
                }
                base_route_services.bind_body_state(initial_body);
                admission.emplace(admit_http_request(
                    {
                        .request_ = parsed_value.request(),
                        .resolution_ = route_resolution_value,
                        .coding_ = http_response_coding_selection_for(parsed_value.request()),
                        .expectation_ = &expectation_plan,
                        .start_ = request_start,
                    },
                    admission_policy, base_route_services));

                if (redirect != nullptr) {
                    if (admission->endpoint() == http_request_endpoint::rejection) {
                        request_rejection = *admission->rejection();
                        break;
                    }
                    if (auto_https_redirect_authority(parsed_value.request()).empty()) {
                        request_rejection = http_request_rejection::error(http_error_info(
                            {.status_ = ruvia::http_status::bad_request, .message_ = "missing Host header"}));
                        break;
                    }
                    response = make_auto_https_redirect_response(
                        parsed_value.request(), redirect->https_port_);
                    if (http_response_needs_not_acceptable(
                            admission->tail().coding_policy(), parsed_value.request(), response)) {
                        request_rejection =
                            http_request_rejection::error(http_error_info(http_not_acceptable_coding_error()));
                        break;
                    }
                    request_completion.emplace(close_with_framework_response(response, parsed_value.connection_plan()));
                    scanner_entry.touch();
                    break;
                }
                if (const auto* rejection = admission->rejection()) {
                    request_rejection = *rejection;
                    break;
                }
                if (const auto body_failure =
                        content_length_limit_failure(parsed_value.body_plan(), admission->body_limit())) {
                    request_rejection = http_request_rejection::error([&] { const auto err = http_request_body_protocol_error(*body_failure); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }());
                    break;
                }

                const auto* resolved = admission->resolved();
                if (resolved == nullptr) {
                    co_await routes_value.dispatch_buffered_response(parsed_value.request(),
                        route_resolution_value, options_.document_root_.binding(),
                        admission->services(), admission->static_file_mode(), response);
                    // An unresolved request never reads its body, whether the
                    // shared Web dispatch selected a document-root file, 404,
                    // 405, or OPTIONS response.
                    request_completion.emplace(finalize_unread_body_response(response, parsed_value,
                        *request_head, buffer_view, request_sequence, admission->body_limit()));
                    scanner_entry.touch();
                    break;
                }

                // One bundle of what every route dispatch below needs from
                // this session; each dispatcher adds only its own arguments.
                const http1_route_dispatch<stream_type> route_dispatch{
                    .stream_ = stream,
                    .scanner_entry_ = scanner_entry,
                    .parsed_ = parsed_value,
                    .response_coding_ = *admission->tail().coding_policy().selection(),
                    .cors_ = admission->tail().cors(options_),
                    .routes_ = routes_value,
                    .base_route_services_ = admission->services(),
                    .options_ = options_,
                    .response_ = response,
                    .request_sequence_ = request_sequence,
                    .inbound_buffers_ = connection_buffers,
                    .request_pipelined_ = request_pipelined,
                };

                const auto endpoint_kind = admission->endpoint();
                if (endpoint_kind == http_request_endpoint::tunnel ||
                    endpoint_kind == http_request_endpoint::websocket) {
                    const auto pending = buffer_view.substr(request_head->header_bytes());
                    auto completion = endpoint_kind == http_request_endpoint::tunnel
                                          ? co_await dispatch_http_tunnel_route(route_dispatch, admission->tail(),
                                                *resolved, admission->udp_tunnel(), pending)
                                          : co_await dispatch_http_websocket_route(
                                                route_dispatch, admission->tail(), *resolved, pending);
                    if (!completion) {
                        co_return endpoint_kind == http_request_endpoint::websocket&& stream.lowest_layer().is_open()
                            ? http_connection_close::linger
                            : http_connection_close::abort;
                    }
                    request_completion.emplace(std::move(*completion));
                    break;
                }

                if (endpoint_kind == http_request_endpoint::response_stream) {
                    request_completion.emplace(co_await dispatch_http_response_stream_route(route_dispatch,
                        response_head, *request_head, *resolved, buffer_view, admission->body_limit(),
                        pipeline_stash));
                    // The stream serialized its own head into the (then empty)
                    // output buffer; it is not response output to keep.
                    response_head.reset();
                    break;
                }
                if (endpoint_kind == http_request_endpoint::stream_body) {
                    request_completion.emplace(co_await dispatch_http_stream_body_route(route_dispatch,
                        *request_head, route_resolution_value, admission->body_limit(), buffer_view,
                        pipeline_stash));
                    break;
                }

                // Buffered-body dispatch, inlined into the session loop: this
                // is the hot path for every plain buffered route, and a
                // dedicated coroutine here would cost one frame allocation
                // per request.
                {
                    const bool has_body = parsed_value.body_plan().requires_consumption();
                    http_lazy_buffered_body_route_state<stream_type> body_state;
                    if (has_body) {
                        const auto body_and_pipeline = http_body_and_pipeline(*request_head, buffer_view);

                        // The body reader/loader setup can throw (e.g. constructing a
                        // transfer-coding decoder for a bad Transfer-Encoding), so it
                        // stays guarded. The dispatch turns any handler or routing
                        // failure into a response, so it sits outside the guard; the
                        // one exception it lets through is an inbound_transport_error
                        // (an interim head or 100 Continue the peer never received),
                        // which ends the connection through the session's caller.
                        if (const auto body_setup_exception = try_prepare_http_lazy_buffered_body_route(
                                body_state, route_dispatch, admission->body_limit(), body_and_pipeline)) {
                            request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                                body_setup_exception, parsed_value, routes_value,
                                admission->services(), response));
                            break;
                        }
                    }

                    std::optional<context_services> body_route_services;
                    if (has_body) {
                        body_route_services.emplace(body_state.with_loader(admission->services()));
                    }
                    const auto& route_services =
                        body_route_services ? *body_route_services : admission->services();
                    (void)co_await routes_value.dispatch_resolved_route(
                        parsed_value.request(), *resolved, route_services, nullptr, response);

                    if (has_body) {
                        request_completion.emplace(complete_successful_http_body_route(scanner_entry,
                            response, parsed_value.connection_plan(), request_sequence,
                            buffer_view.size(), pipeline_stash, body_state.body().reader()));
                    } else {
                        scanner_entry.touch();
                        request_completion.emplace(finalize_buffered_route_response(
                            response, parsed_value.connection_plan(), request_sequence, request_head->header_bytes()));
                    }
                    break;
                }
            }

            if (const auto* failure = parsed_value.failure()) {
                base_route_services.set_connection_close_reason(connection_close_reason::protocol_error);
                if constexpr (plain_tcp) {
                    if (redirect == nullptr &&
                        ruvia::should_drop_invalid_cleartext_http1_input(buffer_view, failure->source())) {
                        co_return http_connection_close::abort;
                    }
                }
                const auto error = failure->protocol_error();
                request_rejection = http_request_rejection::error(
                    copy_http_protocol_error_info(error));
                break;
            }

            header_search_offset = buffer_view.size();

            // With no request bytes yet on a reused connection this read is the
            // keepalive idle wait (idle_timeout); once any header bytes are
            // buffered, or on the first request, request_header_timeout governs.
            // Either way no request is admitted yet, and a stopping worker
            // (which already closed the connection) starts no read for one.
            if (!http_server_worker_running(state_)) {
                co_return http_connection_close::notify;
            }
            // The peer ended its request direction behind the requests already
            // buffered: nothing more arrives, as a clean peer end would report.
            if (request_sequence.request_input_ended()) {
                co_return http_connection_close::notify;
            }
            scanner_entry.read_request_head(connection_scanner::phase::reading_initial);
            const auto read_space = read_buffer.prepare_read();
            if (read_space.empty()) {
                // The buffer only fills at max_http_header_bytes, where
                // parse_head has already classified and rejected the head.
                throw std::logic_error("HTTP/1 request head filled the read buffer unrejected");
            }

            auto read_completion =
                co_await ruvia::async_asio<std::size_t>([&stream, read_space](auto handler) mutable {
                    stream.async_read_some(asio::buffer(read_space), std::move(handler));
                });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
            if (ec) {
                // A clean peer end, or a timeout that cancelled the read, is
                // answered with this side's close alert; anything else closes
                // immediately, as does a stopping worker.
                co_return http1_request_head_read_close(ec, scanner_entry);
            }
            read_buffer.commit_read(bytes_read);
            scanner_entry.touch();
        }

        // Shared exit for every rejection recorded above: one co_await site
        // keeps one set of call temporaries in the frame instead of one per
        // rejection branch. A request rejected before admission (malformed
        // head) runs with the connection's services and no coding promise.
        // A rejection is a generated terminal error: it must not inherit the
        // negotiated coding promise, or a client that accepts no coding would
        // see it replaced by a 406 through a second error-handler call.
        // A rate-limited request is well framed: like a 404 it keeps the
        // connection when its body is already in hand. Every other rejection
        // closes it.
        if (request_rejection) {
            if (admission) {
                admission->tail().disable_response_coding();
            }
            response = co_await respond_http_request_rejection(routes_value, parsed_value.request(), *request_rejection,
                admission ? admission->services() : base_route_services);
            const auto* rejected_head = parsed_value.head_ready();
            request_completion.emplace(
                request_rejection->rate_limit() != nullptr && rejected_head != nullptr
                    ? finalize_unread_body_response(response, parsed_value, *rejected_head,
                          read_buffer.pending(), request_sequence, admission->body_limit())
                    : close_with_framework_response(response, parsed_value.connection_plan()));
        }

        if (!request_completion) {
            throw std::logic_error("HTTP/1 request dispatch returned no terminal completion");
        }
        if (const auto* rejected = request_completion->rejected_final_response()) {
            request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                std::make_exception_ptr(rejected->exception()), parsed_value, routes_value, admission->services(), response));
        }
        const auto peer_ip = base_route_services.get_conn_info().remote().address();
        http_response_tail fallback_tail(parsed_value.request(),
            http_response_coding_policy::disabled(), peer_ip, peer_ip, request_start);
        auto& tail = admission ? admission->tail() : fallback_tail;
        auto connection_plan = request_completion->connection_plan();
        if (request_completion->buffered_response() != nullptr) {
            // Preparation may still run handler-side work (error recovery,
            // offloaded compression); it stays timed as idle. Only the socket
            // writes below run as `writing` (write_remaining() sets it).
            const auto& services = admission ? admission->services() : base_route_services;
            std::optional<http1_buffered_response_plan> planned_response;
            std::optional<http1_response_write_start> started_write;
            bool file_response = false;
            bool head_failure_recovered = false;
            std::optional<http1_file_response_body<stream_type>> file_body;
            for (;;) {
                const auto prepared = co_await tail.prepare_or_finish(
                    response, options_, routes_value, services);
                if (prepared.recovered_) {
                    connection_plan = http1_commit_final_response(response, connection_plan);
                    request_completion = request_completion->with_buffered_connection_plan(connection_plan);
                    connection_plan = request_completion->connection_plan();
                }
                planned_response.emplace(get_http1_buffered_response_plan(prepared.write_plan_, connection_plan));
                file_response = response.file_body() || response.has_multipart_file_body();
                if (file_response && !response_output.empty()) {
                    if (co_await response_output.flush()) {
                        co_return http_connection_close::abort;
                    }
                }
                // A response waits in the output only while the connection stays
                // open and already holds its next request, so the response that
                // ends a pipelined run carries the held ones in one write.
                const auto* const compaction = request_completion->buffer_completion().compaction();
                const bool next_request_buffered = connection_plan.disposition() == http1_close_policy::allow_reuse &&
                                                   compaction != nullptr &&
                                                   compaction->consumed_bytes() < read_buffer.pending().size() &&
                                                   http_server_worker_running(state_);
                // Head validation fails before touching output. Body coalescing can
                // fail after appending a head, leaving an incomplete buffered message.
                auto head_failure = try_start_buffered_response_write(
                    response_head, response, *planned_response, next_request_buffered, started_write, !plain_tcp);
                if (head_failure == nullptr && file_response) {
                    // The first file chunk is read (and the file validated)
                    // before its head leaves, so a file that cannot be served
                    // still gets an error response.
                    file_body.emplace(response, file_output);
                    if (!(co_await file_body->start(*planned_response))) {
                        const auto& file_failure = *file_body->failure();
                        options_.connection_failure_.invoke(remote_address, file_failure.fault());
                        head_failure = file_failure.response_error();
                        file_body.reset();
                        response_head.reset();
                    }
                }
                if (head_failure == nullptr) {
                    break;
                }
                // An error response that cannot be serialized either is a real
                // fault of the error handler.
                if (head_failure_recovered) {
                    std::rethrow_exception(head_failure);
                }
                // Nothing of this response reached the wire. Like a stream whose
                // head fails, it becomes the generic error response that closes
                // the connection; the complete responses held before it go first.
                if (co_await response_output.flush()) {
                    co_return http_connection_close::abort;
                }
                tail.disable_response_coding();
                request_completion.emplace(co_await complete_failed_http_route(scanner_entry, head_failure,
                    parsed_value, routes_value, services, response));
                connection_plan = request_completion->connection_plan();
                head_failure_recovered = true;
            }
            const auto& response_plan = *planned_response;
            const auto& write_start = *started_write;
            if (write_start.held_) {
                // Logged once the write that carries its head commits it.
                tail.defer_access(options_.access_log_, work_set.deferred_access_, write_start.head_bytes_,
                    response_plan.body_plan().response_status());
                scanner_entry.set_phase(connection_scanner::phase::idle);
            } else {
                std::optional<http1_buffered_response_write_result> write_result;
                if (file_response) {
                    scanner_entry.set_phase(connection_scanner::phase::writing);
                    write_result.emplace(co_await file_body->finish(
                        stream, response_head, response_plan, write_start, scanner_entry));
                    response_head.reset();
                    if (const auto* file_failure = file_body->failure()) {
                        options_.connection_failure_.invoke(remote_address, file_failure->fault());
                    }
                } else {
                    if (!response_output.start(write_start.body_,
                            connection_plan.disposition() == http1_close_policy::close_after_response &&
                                !stop_token_.stop_requested())) {
                        const auto completion = co_await response_output.async_write_remaining();
                        response_output.finish(completion.error_code(), completion.result());
                    }
                    const auto written = response_output.complete();
                    write_result.emplace(classify_http1_buffered_response_write(
                        response_plan, write_start.head_bytes_, written.error_, written.bytes_));
                }
                scanner_entry.set_phase(connection_scanner::phase::idle);
                if (const auto committed_status = write_result->committed_status_) {
                    tail.record_access(options_.access_log_, *committed_status);
                }
                if (!write_result->completed_) {
                    co_return http_connection_close::abort;
                }
            }
        } else if (const auto* committed = request_completion->committed_stream()) {
            scanner_entry.set_phase(connection_scanner::phase::idle);
            tail.record_access(options_.access_log_, committed->status());
            if (committed->truncated()) {
                co_return http_connection_close::abort;
            }
        } else {
            throw std::logic_error("HTTP/1 request completion has no wire alternative");
        }

        // A stopping worker ends the connection after the response.
        if (connection_plan.disposition() == http1_close_policy::close_after_response ||
            !http_server_worker_running(state_)) {
            co_return http_connection_close::linger;
        }
        read_buffer.complete_request(request_completion->buffer_completion());
        read_buffer.trim();
        // A request completed and the connection is being reused: the next
        // wait with no buffered bytes is a keepalive idle wait.
        session.served_keepalive_request_ = true;
        if (read_buffer.empty()) {
            co_return http1_serve_handoff::await_request;
        }
        request_pipelined = true;
    }
}

template task<http_connection_close> worker_connections::run_stream<inbound_socket_stream>(
    http_server_session_config&, inbound_socket_stream&, tcp_socket_type&, context_services);
template task<http_connection_close> worker_connections::run_stream<server_tls_stream>(
    http_server_session_config&, server_tls_stream&, tcp_socket_type&, context_services);
template task<std::variant<http1_serve_handoff, http_connection_close>> worker_connections::serve_requests<inbound_socket_stream>(
    http1_connection_session<inbound_socket_stream>&, connection_work_set&);
template task<std::variant<http1_serve_handoff, http_connection_close>> worker_connections::serve_requests<server_tls_stream>(
    http1_connection_session<server_tls_stream>&, connection_work_set&);

#if defined(RUVIA_ENABLE_HTTP2)
template <typename stream_type>
task<http_connection_close> worker_connections::run_http2(stream_type& stream, tcp_socket_type& socket,
    context_services services) {
    connection_scanner::entry scanner_entry;
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    co_return co_await run_http2_sans_io_session(
        stream, routes_, http2_sans_io_session_context(std::move(services), options_, scanner_entry));
}

template task<http_connection_close> worker_connections::run_http2<inbound_socket_stream>(
    inbound_socket_stream&, tcp_socket_type&, context_services);
template task<http_connection_close> worker_connections::run_http2<server_tls_stream>(
    server_tls_stream&, tcp_socket_type&, context_services);
#endif

}  // namespace ruvia::detail
