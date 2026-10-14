#pragma once

#include "body/http_stream_body_reader_errors.h"

namespace ruvia::detail {

template <typename stream_type>
task<std::string_view> stream_body_reader<stream_type>::read_known_length_all(
    std::pmr::string& body, std::size_t content_length) {
    if (finished_) {
        if (body.empty() && delivered_bytes_ == 0 && initial_body_and_pipeline_.size() >= content_length) {
            co_return initial_body_and_pipeline_.substr(0, content_length);
        }
        co_return std::string_view(body);
    }
    if (exceeds_limit(content_length)) {
        throw_request_body_too_large();
    }

    const auto initial_body_bytes = std::min(content_length, initial_body_and_pipeline_.size());
    if (delivered_bytes_ == 0 && initial_body_bytes == content_length) {
        mark_finished();
        co_return initial_body_and_pipeline_.substr(0, content_length);
    }

    co_await ensure_continue();

    // The initial segment is shorter than the body, so it is all body and no
    // pipelined bytes follow it; an earlier read() may also have left a
    // buffered remainder. Take both before reading the socket.
    if (delivered_bytes_ < initial_body_bytes) {
        body.append(initial_body_and_pipeline_.substr(delivered_bytes_, initial_body_bytes - delivered_bytes_));
        delivered_bytes_ = initial_body_bytes;
    }
    initial_body_and_pipeline_ = {};
    if (read_cursor_ < buffer_.size()) {
        const auto chunk_bytes = std::min(buffer_.size() - read_cursor_, content_length - delivered_bytes_);
        body.append(buffer_.data() + read_cursor_, chunk_bytes);
        read_cursor_ += chunk_bytes;
        delivered_bytes_ += chunk_bytes;
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
        scanner_entry_.set_phase(connection_scanner::phase_type::reading_payload);
    }
    auto filled = body.size();
    while (delivered_bytes_ < content_length) {
        const auto remaining_body = content_length - delivered_bytes_;
        if (filled == body.size()) {
            const auto final_size = remaining_body > body.max_size() - filled ? body.max_size() : filled + remaining_body;
            const auto doubled = filled > body.max_size() / 2 ? body.max_size() : filled * 2;
            const auto target_capacity = std::min(final_size, std::max(doubled, known_length_min_read_bytes));
            if (target_capacity > body.capacity()) {
                body.reserve(target_capacity);
            }
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

template <typename stream_type>
task<std::optional<std::span<const std::byte>>> stream_body_reader<stream_type>::read_known_length(
    std::size_t content_length) {
    if (finished_) {
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

}  // namespace ruvia::detail
