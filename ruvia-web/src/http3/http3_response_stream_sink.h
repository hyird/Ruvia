#pragma once

#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/task.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/streaming.h"

#include "http3/http3_response_write.h"
#include "http3/http3_session.h"
#include "http3/http3_stream.h"
#include "server/inbound_transport_error.h"
#include "server/multiplexed_stream_signal.h"

namespace ruvia::detail {

// Streaming response sink of one HTTP/3 request stream, the method set
// dispatch_response_stream_with drives: commit() submits the streaming final
// head (an application Content-Length becomes the exact content total),
// write() submits DATA within the stream's output budget, waiting for
// acknowledgements when it is spent, and end() finishes the content or sends
// the trailer section. The HTTP/3 core owns the field mapping and framing.
class http3_response_stream_sink final
    : public multiplexed_response_stream_sink_base<http3_response_stream_sink, http3_stream> {
public:
    http3_response_stream_sink(http3_session& session, http3_stream& stream, multiplexed_stream_signal& signal,
        std::pmr::memory_resource* resource, http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding, const compression_config* compression,
        const cors_options* cors) noexcept
        : multiplexed_response_stream_sink_base(
              stream, signal, resource, request_method, codecs, response_coding, compression, cors),
          session_(session) {}

private:
    friend class multiplexed_response_stream_sink_base<http3_response_stream_sink, http3_stream>;

    static constexpr const char* terminated_error_message = "HTTP/3 connection ended during response stream";
    static constexpr const char* sleep_terminated_error_message = "HTTP/3 connection ended during response stream";
    static constexpr const char* reset_error_message = "HTTP/3 response stream reset";
    static constexpr const char* sleep_reset_error_message = "HTTP/3 response stream reset during sleep";

    [[nodiscard]] bool stream_aborted() const noexcept {
        return stream_.aborted();
    }

    void wake_writer() noexcept {
        session_.wake_writer();
    }

    [[nodiscard]] http_response_stream_commit_plan submit_streaming_head(
        http_response&& response, http_response_trailer_intent trailer_intent) {
        const auto head_result = session_.codec().submit_streaming_response_head(
            stream_.id(), request_method_, std::move(response), trailer_intent);
        const auto* submitted_head = std::get_if<http_response_stream_commit_plan>(&head_result);
        if (submitted_head != nullptr) {
            return *submitted_head;
        }
        if (std::get<http3_response_head_error>(head_result) == http3_response_head_error::closed) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), reset_error_message);
        }
        throw std::logic_error("HTTP/3 streaming response head submission failed");
    }

    task<void> write_encoded_data(std::string_view chunk) {
        const auto status = co_await write_http3_stream_data(session_, stream_.id(), signal_, chunk);
        if (status == http3_data_write_status::completed) {
            co_return;
        }
        if (status == http3_data_write_status::terminated) {
            throw inbound_transport_error(signal_.terminal_error(), terminated_error_message);
        }
        throw inbound_transport_error(
            std::make_error_code(std::errc::connection_reset), reset_error_message);
    }

    void finish_stream_response(const http_response_trailer_section& trailer_section) {
        http3_status finished{};
        try {
            finished = session_.codec().finish_response(stream_.id(), trailer_section);
        } catch (...) {
            session_.wake_writer();
            throw;
        }
        session_.wake_writer();
        if (finished != http3_status::accepted) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), reset_error_message);
        }
    }

    http3_session& session_;
};

}  // namespace ruvia::detail
