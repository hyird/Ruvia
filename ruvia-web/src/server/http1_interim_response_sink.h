#pragma once

#include <memory_resource>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/http/http1_interim_response_writer.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"

#include "context/http_interim_response_output.h"

namespace ruvia::detail {
template <typename stream_type>
class http1_interim_response_sink final {
public:
    // `request` is the session's reused parse target; it is borrowed for every
    // dispatch that can reach output().
    http1_interim_response_sink(stream_type& stream, const http_request& request, std::pmr::memory_resource* resource)
        : stream_(stream),
          request_(request),
          resource_(resource),
          output_(resource, this, write) {}
    [[nodiscard]] http_interim_response_output& output() noexcept {
        return output_;
    }

private:
    static task<void> write(void* raw, const http_interim_response_head& response) {
        auto& self = *static_cast<http1_interim_response_sink*>(raw);
        std::pmr::vector<char> scratch(max_http_header_bytes + 1024, self.resource_);
        const auto encoded = http1_interim_response_writer{}.prepare(response, scratch);
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
        const auto completion = co_await async_asio<std::size_t>([&self, bytes = prepared->head()](auto handler) {
            asio::async_write(self.stream_, asio::buffer(bytes), std::move(handler));
        });
        if (completion.error_code()) {
            throw std::system_error(completion.error_code());
        }
    }
    stream_type& stream_;
    const http_request& request_;
    std::pmr::memory_resource* resource_;
    http_interim_response_output output_;
};
}  // namespace ruvia::detail
