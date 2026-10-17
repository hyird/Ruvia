#pragma once

#include <memory_resource>
#include <span>
#include <stdexcept>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/http/http1_interim_response_writer.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_head_buffer.h"

#include "context/http_interim_response_output.h"
#include "server/http1_response_output.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {
template <typename stream_type>
class http1_interim_response_sink final {
public:
    // `request` is the session's reused parse target; it is borrowed for every
    // dispatch that can reach output(). Pipelined responses still held back in
    // `pending` precede any interim head on the wire.
    http1_interim_response_sink(stream_type& stream, const http_request& request, std::pmr::memory_resource* resource,
        http1_response_output<stream_type>& pending)
        : stream_(stream),
          request_(request),
          resource_(resource),
          pending_(pending),
          output_(resource, this, write) {}
    [[nodiscard]] http_interim_response_output& output() noexcept {
        return output_;
    }

private:
    static task<void> write(void* raw, const http_interim_response_head& response) {
        auto& self = *static_cast<http1_interim_response_sink*>(raw);
        // A typical interim head fits the head buffer's inline storage; a
        // larger one is encoded again into heap storage of exactly its size.
        http_response_head_buffer head(self.resource_);
        const http1_interim_response_writer writer;
        auto encoded = writer.prepare(
            response, std::span<char>(head.stack_cursor(response_head_stack_bytes), response_head_stack_bytes));
        if (const auto* too_small = encoded.buffer_too_small()) {
            const auto head_bytes = too_small->required_head_bytes();
            head.append_generated(head_bytes, [&](char* out) noexcept {
                encoded = writer.prepare(response, std::span<char>(out, head_bytes));
            });
        }
        const auto* prepared = encoded.prepared();
        if (prepared == nullptr) {
            throw std::invalid_argument("invalid or oversized interim response head");
        }
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
        const auto completion = co_await async_asio<std::size_t>([&self, bytes = prepared->head()](auto handler) {
            asio::async_write(self.stream_, asio::buffer(bytes), std::move(handler));
        });
        if (completion.error_code()) {
            throw inbound_transport_error(completion.error_code(), "failed to write HTTP/1 interim response");
        }
    }
    stream_type& stream_;
    const http_request& request_;
    std::pmr::memory_resource* resource_;
    http1_response_output<stream_type>& pending_;
    http_interim_response_output output_;
};
}  // namespace ruvia::detail
