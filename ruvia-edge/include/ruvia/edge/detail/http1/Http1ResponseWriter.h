#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/edge/detail/http1/Http1Wire.h"
#include "ruvia/edge/detail/ResponseWriter.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"

namespace ruvia::edge {

// HTTP/1 response writer: serializes the serve core's responses to wire bytes and
// counts them. Framing (Content-Length vs chunked) and the chunked terminator are
// this adapter's concern.
template <typename Stream>
class Http1ResponseWriter final : public ResponseWriter {
public:
    Http1ResponseWriter(
        Stream& stream,
        ruvia::detail::Http1ServerConnectionPlan connectionPlan) noexcept
        : stream_(stream),
          protocolVersion_(connectionPlan.protocolVersion()),
          reusable_(
              connectionPlan.disposition() ==
              ruvia::detail::Http1ConnectionDisposition::kReuse) {}

    asio::awaitable<bool> respond(
        std::uint16_t status,
        const Headers& headers,
        std::string_view body,
        std::string_view cacheResult,
        std::optional<std::uint64_t> age,
        bool omitBody,
        bool keepAlive) override {
        reusable_ = reusable_ && keepAlive;
        co_return co_await write(
            encodeResponse(
                protocolVersion_,
                status,
                headers,
                body,
                cacheResult,
                age,
                omitBody,
                reusable_));
    }

    asio::awaitable<bool> respondHead(
        std::uint16_t status,
        const Headers& headers,
        std::string_view cacheResult,
        bool hasBody,
        std::optional<std::size_t> contentLength,
        bool keepAlive) override {
        ClientFraming framing = ClientFraming::kNoBody;
        if (hasBody) {
            if (contentLength) {
                framing = ClientFraming::kLength;
            } else if (protocolVersion_ == HttpProtocolVersion::kHttp11) {
                framing = ClientFraming::kChunked;
            } else {
                framing = ClientFraming::kCloseDelimited;
            }
        }
        reusable_ = reusable_ && keepAlive &&
            framing != ClientFraming::kCloseDelimited;
        chunked_ = framing == ClientFraming::kChunked;
        co_return co_await write(encodeStreamingHead(
            protocolVersion_,
            status,
            headers,
            cacheResult,
            framing,
            contentLength.value_or(0),
            reusable_));
    }

    asio::awaitable<bool> respondChunk(std::string_view chunk) override {
        co_return co_await write(chunked_ ? encodeChunk(chunk) : std::string(chunk));
    }

    asio::awaitable<bool> respondEnd() override {
        if (chunked_) {
            co_return co_await write("0\r\n\r\n");
        }
        co_return true;
    }

    [[nodiscard]] std::size_t bytesWritten() const override { return bytes_; }

    [[nodiscard]] bool connectionReusable() const noexcept override {
        return reusable_;
    }

private:
    asio::awaitable<bool> write(std::string wire) {
        bytes_ += wire.size();
        auto [ec, n] = co_await asio::async_write(
            stream_, asio::buffer(wire.data(), wire.size()),
            asio::as_tuple(asio::use_awaitable));
        (void)n;
        co_return !ec;
    }

    Stream& stream_;
    HttpProtocolVersion protocolVersion_;
    std::size_t bytes_{0};
    bool chunked_{false};
    bool reusable_{false};
};

}  // namespace ruvia::edge
