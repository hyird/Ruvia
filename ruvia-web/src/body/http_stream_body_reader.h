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
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "body/http_body_buffer.h"
#include "body/http_stream_body_reader_errors.h"
#include "context/http_interim_response_output.h"
#include "server/connection_scanner.h"

namespace ruvia::detail {

inline constexpr std::size_t chunked_encoded_buffer_bytes = max_http_header_bytes;

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

template <typename stream_type>
class stream_body_reader final {
public:
    // `interim_output` is the request's interim lane, which carries the 100
    // Continue an expectation asks for.
    stream_body_reader(stream_type& stream, std::pmr::polymorphic_allocator<char> allocator,
        std::string_view initial_body_and_pipeline, http1_request_body_plan body_plan,
        protocol_byte_limit body_limit_value, connection_scanner::entry_type& scanner_entry,
        http_interim_response_output& interim_output);
    ~stream_body_reader() = default;

    stream_body_reader(const stream_body_reader&) = delete;
    stream_body_reader& operator=(const stream_body_reader&) = delete;

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept;
    [[nodiscard]] const http_request_trailers& trailers() const& noexcept {
        return trailers_;
    }
    // Hands the pipelined suffix over and drops this reader's claim on it. The
    // connection read buffer is deliberately untouched: every view in the
    // request being served still borrows it. `stash` receives bytes only when
    // the suffix lives in this reader's own buffer.
    [[nodiscard]] body_pipeline_handover take_pipeline(std::pmr::string& stash);

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read();
    task<std::string_view> read_all(std::pmr::string& body);

private:
    task<void> send_continue();
    void require_not_failed() const;
    [[nodiscard]] std::string_view initial_pipeline_remainder() const noexcept;
    [[nodiscard]] std::string_view buffered_pipeline_remainder() const noexcept;
    void reset_pipeline_state() noexcept;
    void materialize_initial_remainder();
    task<void> read_more();
    task<std::string_view> read_known_length_all(std::pmr::string& body, std::size_t content_length);
    task<std::optional<std::span<const std::byte>>> read_known_length(std::size_t content_length);
    task<std::optional<std::span<const std::byte>>> read_chunked();
    task<std::optional<std::span<const std::byte>>> read_transfer_decoded_chunked();
    [[nodiscard]] std::span<char> transfer_output();
    void decode_transfer_append(std::string_view input, std::pmr::string& target);
    [[nodiscard]] bool exceeds_limit(std::size_t bytes) const noexcept;
    void mark_finished() noexcept;

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
    connection_scanner::entry_type& scanner_entry_;
    http_interim_response_output& interim_output_;
    // Offset of the first unconsumed byte in the active source: the borrowed
    // initial view while it is set, buffer_ otherwise. Consumed buffer_ bytes are
    // dropped only when read_more() needs room for socket input.
    std::size_t read_cursor_{0};
    std::size_t delivered_bytes_{0};
    bool finished_{false};
    // Set once a read fails: the framing position is unknown, so the body can
    // never be consumed further and must stay incomplete.
    bool failed_{false};
    bool continue_pending_{false};
};

}  // namespace ruvia::detail

#include "body/http_stream_body_reader.inl"
