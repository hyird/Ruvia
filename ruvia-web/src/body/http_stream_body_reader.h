#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "body/http_body_buffer.h"
#include "context/http_interim_response_output.h"
#include "server/connection_scanner.h"
#include "util/operation_interrupt.h"

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
    std::pmr::string& body, std::size_t filled, std::size_t remaining) {
    const auto final_size = remaining > body.max_size() - filled ? body.max_size() : filled + remaining;
    const auto doubled = filled > body.max_size() / 2 ? body.max_size() : filled * 2;
    const auto target_capacity = std::min(final_size, std::max(doubled, known_length_min_read_bytes));
    if (target_capacity > body.capacity()) {
        body.reserve(target_capacity);
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
    stream_body_reader(stream_type& stream, std::pmr::polymorphic_allocator<char> allocator,
        std::string_view initial_body_and_pipeline, http1_request_body_plan body_plan,
        protocol_byte_limit body_limit_value, connection_scanner::entry& scanner_entry,
        http_interim_response_output& interim_output, const worker_handle& worker)
        : stream_(stream),
          buffer_(allocator),
          transfer_output_(allocator),
          transfer_decoder_(nullptr,
              pmr_object_deleter<http_transfer_coding_stack_decoder>{allocator.resource()}),
          initial_body_and_pipeline_(initial_body_and_pipeline),
          body_plan_(body_plan),
          body_limit_(body_limit_value),
          chunk_decoder_(http1_chunked_body_decoder_config{.body_limit_ = body_limit_value}),
          trailers_(allocator.resource()),
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
            transfer_decoder_ = make_pmr_object<http_transfer_coding_stack_decoder>(allocator.resource(),
                std::span<const http_transfer_coding>(codings.data(), codings.size()),
                allocator.resource(), body_limit_value);
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
        std::pmr::string& stash, std::string_view additional_pipeline = {}) {
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
        read_interrupt_ = &interrupt;
        try {
            auto chunk = co_await read_next();
            read_interrupt_ = nullptr;
            co_return chunk;
        } catch (...) {
            read_interrupt_ = nullptr;
            if (interrupt.interrupted()) {
                scanner_entry_.set_phase(connection_scanner::phase::idle);
            } else {
                state_ = state_type::failed;
            }
            throw;
        }
    }

    task<std::string_view> read_all(std::pmr::string& body) {
        require_not_failed();
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
                co_return co_await read_known_length_all(body, content_length);
            }
            if (body_plan_.without_body() != nullptr) {
                co_return std::string_view(body);
            }

            if (continue_pending_) {
                co_await send_continue();
            }
            while (auto chunk = co_await read_chunked()) {
                if (transfer_decoder_ != nullptr) {
                    decode_transfer_append(::ruvia::as_chars(*chunk), body);
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
            state_ = state_type::failed;
            throw;
        }
    }

private:
    enum class state_type : std::uint8_t { active,
        finished,
        failed };

    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
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
                buffer_.reserve(std::max(remainder.size(), target_reserve));
            }
            buffer_.append(remainder);
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
            read_space = buffer_.prepare(writable);
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

    task<std::string_view> read_known_length_all(std::pmr::string& body, std::size_t content_length) {
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
                body.reserve(after_preread);
            } else {
                (void)reserve_known_length_growth(body, after_preread, remaining_after_preread);
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
                const auto final_size = reserve_known_length_growth(body, filled, remaining_body);
                body.resize(std::min(final_size, body.capacity()));
            }
            const auto writable = std::min(remaining_body, body.size() - filled);
            auto read_completion =
                co_await ruvia::async_asio<std::size_t>([this, &body, filled, writable](auto handler) mutable {
                    stream_.async_read_some(asio::buffer(body.data() + filled, writable), std::move(handler));
                });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
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
                if (trailers_.append_http1(complete->trailers()).has_value()) {
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
            transfer_output_.resize(http_body_buffer_bytes);
        }
        return std::span<char>(transfer_output_);
    }

    // Decodes into the reused scratch and appends what the decoder produced, so
    // `target` only ever grows by written bytes.
    void decode_transfer_append(std::string_view input, std::pmr::string& target) {
        const auto output_window = transfer_output();
        for (;;) {
            const auto result_value = transfer_decoder_->decode(input, output_window);
            input.remove_prefix(std::min(input.size(), result_value.consumed_bytes()));
            if (const auto* output = result_value.output()) {
                target.append(output->bytes());
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
    std::pmr::string transfer_output_;
    std::unique_ptr<http_transfer_coding_stack_decoder, pmr_object_deleter<http_transfer_coding_stack_decoder>>
        transfer_decoder_;
    std::string_view transfer_input_;
    std::string_view initial_body_and_pipeline_;
    http1_request_body_plan body_plan_;
    protocol_byte_limit body_limit_;
    http1_chunked_body_decoder chunk_decoder_;
    http_request_trailers trailers_;
    connection_scanner::entry& scanner_entry_;
    http_interim_response_output& interim_output_;
    const worker_handle& worker_;
    asio::cancellation_signal read_cancellation_;
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
    lazy_buffered_body(stream_type& stream, std::pmr::polymorphic_allocator<char> worker_allocator,
        std::pmr::memory_resource* request_resource, std::string_view initial_body_and_pipeline,
        http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
        connection_scanner::entry& scanner_entry, http_interim_response_output& interim_output,
        const worker_handle& worker)
        : reader_(stream, worker_allocator, initial_body_and_pipeline, body_plan, body_limit_value, scanner_entry,
              interim_output, worker),
          body_(pmr_resource_or_default(request_resource)) {}

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

    [[nodiscard]] task<std::string_view> read_all() {
        return reader_.read_all(body_);
    }

    task<void> discard() {
        while (co_await reader_.read()) {
        }
    }

private:
    stream_body_reader<stream_type> reader_;
    std::pmr::string body_;
};

}  // namespace ruvia::detail
