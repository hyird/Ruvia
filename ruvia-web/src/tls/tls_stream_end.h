#pragma once

#include <cstdint>
#include <system_error>

#include <asio/error.hpp>
#include <asio/ssl/error.hpp>

namespace ruvia::detail {

enum class http_stream_end : std::uint8_t { keep_open,
    end };
// end of input rather than a read failure: a TCP FIN or TLS close_notify
// (eof), or a TLS peer that closed TCP without close_notify (stream_truncated).
// Message-delimited protocols above the stream still detect a truncated
// message themselves; the transport only reports that no more bytes follow.
[[nodiscard]] inline bool is_stream_read_end(const std::error_code& error) noexcept {
    return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
}

}  // namespace ruvia::detail
