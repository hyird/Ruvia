#pragma once

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/PmrString.h"
#include "ruvia/core/Socket.h"
#include "ruvia/core/Task.h"

#include "http/HttpStreamReadResult.h"
#include "http/TlsTunnelOutput.h"

namespace ruvia::detail {

template <typename Stream>
class HttpSocketTunnelTransport final {
public:
    explicit HttpSocketTunnelTransport(Stream& stream, TlsTunnelOutput* tlsOutput = nullptr)
        : stream_(stream),
          tlsOutput_(tlsOutput) {
        if constexpr (requires { stream_.next_layer(); }) {
            if (tlsOutput_ == nullptr) {
                throw std::logic_error("TLS tunnel requires its ciphertext writer");
            }
        }
    }
    [[nodiscard]] Task<HttpStreamReadResult> readMore(std::pmr::string& bytes) {
        const auto oldSize = bytes.size();
        resizePmrStringForOverwrite(bytes, oldSize + 4096);
        const auto result = co_await asyncAsio<std::size_t>([&](auto handler) {
            stream_.async_read_some(asio::buffer(bytes.data() + oldSize, 4096), std::move(handler));
        });
        bytes.resize(oldSize + result.result());
        if (result.errorCode() == asio::error::eof) {
            co_return HttpStreamReadResult::makeEnd();
        }
        if (result.errorCode()) {
            co_return HttpStreamReadResult::makeFailure(result.errorCode());
        }
        co_return result.result() == 0 ? HttpStreamReadResult::makeEnd() : HttpStreamReadResult::makeData();
    }
    [[nodiscard]] Task<std::error_code> writeBytes(std::string_view bytes, HttpStreamEnd end) {
        while (!bytes.empty()) {
            const auto chunk = bytes.substr(0, 4096);
            const auto result = co_await asyncAsio<std::size_t>([&](auto handler) {
                asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
            });
            if (result.errorCode()) {
                co_return result.errorCode();
            }
            if constexpr (requires { stream_.next_layer(); }) {
                if (const auto error = co_await tlsOutput_->flush()) {
                    co_return error;
                }
            }
            bytes.remove_prefix(chunk.size());
        }
        if (end == HttpStreamEnd::kEnd) {
            if constexpr (requires { stream_.next_layer(); }) {
                co_return co_await tlsOutput_->finish();
            } else {
                std::error_code error;
                stream_.shutdown(asio::ip::tcp::socket::shutdown_send, error);
                co_return error;
            }
        }
        co_return std::error_code{};
    }
    void abort() noexcept {
        if constexpr (requires { stream_.next_layer(); }) {
            tlsOutput_->abort();
        } else {
            closeSocket(stream_);
        }
    }

private:
    Stream& stream_;
    TlsTunnelOutput* tlsOutput_;
};

}  // namespace ruvia::detail
