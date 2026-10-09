#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/bytes.h"
#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "body/http_body_buffer.h"
#include "body/http_stream_body_reader_errors.h"

namespace ruvia::detail {

inline constexpr std::size_t chunked_encoded_buffer_bytes = max_http_header_bytes;

template <typename stream_type>
class stream_body_reader final {
public:
    stream_body_reader(stream_type& stream, std::pmr::polymorphic_allocator<char> allocator,
        std::string_view initial_body_and_pipeline, http1_request_body_plan body_plan,
        protocol_byte_limit body_limit_value, ruvia::connection_scanner::entry_type& scanner_entry);
    ~stream_body_reader() = default;

    stream_body_reader(const stream_body_reader&) = delete;
    stream_body_reader& operator=(const stream_body_reader&) = delete;

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept;
    [[nodiscard]] const http_request_trailers& trailers() const& noexcept {
        return trailers_;
    }
    // Hands the pipelined suffix -- the bytes of the next request that arrived
    // in the same segment -- to `stash`, and drops this reader's claim on them.
    // The connection read buffer is deliberately untouched: every view in the
    // request being served still borrows it, so the session installs these bytes
    // itself once those views are dead.
    void take_pipeline(std::pmr::string& stash);

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read();
    task<std::string_view> read_all(std::pmr::string& body);

private:
    task<void> ensure_continue();
    void compact_pending();
    [[nodiscard]] std::string_view initial_pipeline_remainder() const noexcept;
    [[nodiscard]] std::string_view buffered_pipeline_remainder() const noexcept;
    void reset_pipeline_state() noexcept;
    void materialize_initial_remainder();
    task<void> read_more();
    task<std::string_view> read_known_length_all(std::pmr::string& body, std::size_t content_length);
    task<std::optional<std::span<const std::byte>>> read_known_length(std::size_t content_length);
    task<std::optional<std::span<const std::byte>>> read_chunked();
    task<std::optional<std::span<const std::byte>>> read_transfer_decoded_chunked();
    void decode_transfer_append(std::string_view input, std::pmr::string& target);
    [[nodiscard]] bool exceeds_limit(std::size_t bytes) const noexcept;
    void mark_finished() noexcept;

    stream_type& stream_;
    std::pmr::string buffer_;
    std::pmr::string transfer_output_;
    std::unique_ptr<http_transfer_coding_stack_decoder, pmr_object_deleter<http_transfer_coding_stack_decoder>>
        transfer_decoder_;
    std::string_view transfer_input_;
    std::string_view initial_body_and_pipeline_;
    http1_request_body_plan body_plan_;
    protocol_byte_limit body_limit_;
    http1_chunked_body_decoder chunk_decoder_;
    http_request_trailers trailers_;
    ruvia::connection_scanner::entry_type& scanner_entry_;
    std::size_t read_cursor_{0};
    std::size_t pending_compact_until_{0};
    std::size_t delivered_bytes_{0};
    bool finished_{false};
    bool continue_sent_{false};
};

}  // namespace ruvia::detail

#include "body/http_stream_body_reader.inl"
