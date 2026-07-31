#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/edge/detail/ResponseWriter.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

namespace ruvia::edge {

namespace http1_writer_detail {

template <typename Stream, typename ConstBufferSequence>
[[nodiscard]] asio::awaitable<std::pair<bool, std::size_t>> writeHttp1Buffers(Stream& stream, ConstBufferSequence buffers) {
    auto [ec, bytes] = co_await asio::async_write(stream, buffers, asio::as_tuple(asio::use_awaitable));
    co_return std::pair(!ec, bytes);
}

[[nodiscard]] inline ruvia::detail::Http1ServerClosePolicy http1ClosePolicy(ResponseReusePolicy policy) noexcept {
    return policy == ResponseReusePolicy::kAllow ? ruvia::detail::Http1ServerClosePolicy::kAllowReuse : ruvia::detail::Http1ServerClosePolicy::kCloseAfterResponse;
}

}  // namespace http1_writer_detail

// HTTP/1 response adapter. ruvia-http owns method/status body semantics,
// persistence, framing and head serialization. This class only drives the
// resulting plan over its stream and accounts for bytes actually transferred.
template <typename Stream>
class Http1ResponseWriter final {
public:
    Http1ResponseWriter(Stream& stream, const ruvia::detail::Http1ServerRequestParseState& parsed, std::pmr::memory_resource* resource)
        : stream_(stream),
          parsed_(parsed),
          resource_(resource),
          head_(std::pmr::polymorphic_allocator<char>(resource)),
          reusable_(parsed.connectionPlan.disposition() == ruvia::detail::Http1ConnectionDisposition::kReuse) {}

    [[nodiscard]] asio::awaitable<bool> respond(
        std::uint16_t status,
        const Headers& headers,
        std::string_view body,
        std::string_view cacheResult,
        std::optional<std::uint64_t> age,
        ResponseReusePolicy reusePolicy) {
        auto response = makeEdgeResponse(resource_, status, headers, cacheResult, age);
        response.body(body);

        auto connectionPlan = parsed_.connectionPlan;
        if (reusePolicy == ResponseReusePolicy::kClose) {
            connectionPlan = connectionPlan.requireClose();
        }
        const auto commit = ruvia::detail::http1CommitFinalResponse(response, connectionPlan);
        if (commit.failure() != nullptr) {
            reusable_ = false;
            co_return false;
        }
        connectionPlan = *commit.committed();
        reusable_ = connectionPlan.disposition() == ruvia::detail::Http1ConnectionDisposition::kReuse;

        const auto responsePlan = ruvia::detail::http1BufferedResponsePlan(
            ruvia::detail::httpBufferedResponseWritePlan(parsed_.request.knownMethod(), response),
            connectionPlan);
        head_.reset();
        ruvia::detail::appendResponseHead(response, head_, responsePlan.headPlan());
        const auto payload = responsePlan.sendBody() ? body : std::string_view{};
        const std::array<asio::const_buffer, 2> buffers{asio::buffer(head_.view()), asio::buffer(payload)};
        const auto [ok, bytes] = co_await http1_writer_detail::writeHttp1Buffers(stream_, buffers);
        bytes_ += bytes;
        co_return ok;
    }

    [[nodiscard]] asio::awaitable<bool> respondHead(
        std::uint16_t status,
        const Headers& headers,
        std::string_view cacheResult,
        std::optional<std::size_t> contentLength,
        ResponseReusePolicy reusePolicy) {
        auto response = makeEdgeResponse(resource_, status, headers, cacheResult, std::nullopt);
        if (contentLength.has_value()) {
            std::array<char, 20> digits;
            const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), *contentLength);
            if (ec != std::errc{}) {
                reusable_ = false;
                co_return false;
            }
            response.header("Content-Length", std::string_view(digits.data(), static_cast<std::size_t>(end - digits.data())));
        }

        const auto streamPlan = ruvia::detail::http1PlanConsumedResponseStream(parsed_, http1_writer_detail::http1ClosePolicy(reusePolicy));
        auto result = contentLength.has_value()
            ? ruvia::detail::prepareHttp1KnownLengthResponseStreamHead(
                  std::move(response),
                  *contentLength,
                  ruvia::detail::ResponseStreamKind::kGeneric,
                  streamPlan)
            : ruvia::detail::prepareHttp1ResponseStreamHead(
                  std::move(response),
                  ruvia::detail::ResponseStreamKind::kGeneric,
                  streamPlan,
                  ruvia::detail::ResponseTrailerIntent::kNone);
        auto* prepared = result.prepared();
        if (prepared == nullptr) {
            reusable_ = false;
            co_return false;
        }

        reusable_ = prepared->connectionPlan().disposition() == ruvia::detail::Http1ConnectionDisposition::kReuse;
        const auto& commitPlan = prepared->commitPlan();
        bodyOpen_ = commitPlan.headDisposition() == ruvia::detail::ResponseStreamHeadDisposition::kBodyOpen;
        chunked_ = bodyOpen_ && prepared->responseHeadPlan().chunkedStream() != nullptr;
        remainingContentBytes_ = bodyOpen_ && prepared->responseHeadPlan().knownLengthStream() != nullptr ? contentLength : std::nullopt;
        head_.reset();
        ruvia::detail::appendResponseHead(prepared->response(), head_, prepared->responseHeadPlan());
        const auto [ok, bytes] = co_await http1_writer_detail::writeHttp1Buffers(stream_, asio::buffer(head_.view()));
        bytes_ += bytes;
        co_return ok;
    }

    [[nodiscard]] asio::awaitable<bool> respondChunk(std::string_view chunk) {
        if (!bodyOpen_ || chunk.empty()) {
            co_return true;
        }
        if (remainingContentBytes_.has_value() && chunk.size() > *remainingContentBytes_) {
            reusable_ = false;
            co_return false;
        }
        if (!chunked_) {
            const auto [ok, bytes] = co_await http1_writer_detail::writeHttp1Buffers(stream_, asio::buffer(chunk));
            bytes_ += bytes;
            if (ok && remainingContentBytes_.has_value()) {
                *remainingContentBytes_ -= chunk.size();
            }
            co_return ok;
        }

        std::array<char, 18> chunkHead;
        const auto [end, ec] = std::to_chars(chunkHead.data(), chunkHead.data() + 16, chunk.size(), 16);
        if (ec != std::errc{}) {
            reusable_ = false;
            co_return false;
        }
        *end = '\r';
        *(end + 1) = '\n';
        const std::array<asio::const_buffer, 3> buffers{
            asio::buffer(chunkHead.data(), static_cast<std::size_t>(end - chunkHead.data()) + 2),
            asio::buffer(chunk),
            asio::buffer("\r\n", 2)};
        const auto [ok, bytes] = co_await http1_writer_detail::writeHttp1Buffers(stream_, buffers);
        bytes_ += bytes;
        co_return ok;
    }

    [[nodiscard]] asio::awaitable<bool> respondEnd() {
        if (!bodyOpen_) {
            co_return true;
        }
        bodyOpen_ = false;
        if (remainingContentBytes_.value_or(0) != 0) {
            reusable_ = false;
            co_return false;
        }
        if (!chunked_) {
            co_return true;
        }
        const auto [ok, bytes] = co_await http1_writer_detail::writeHttp1Buffers(stream_, asio::buffer("0\r\n\r\n", 5));
        bytes_ += bytes;
        co_return ok;
    }

    [[nodiscard]] std::size_t bytesWritten() const noexcept {
        return bytes_;
    }

    [[nodiscard]] bool connectionReusable() const noexcept {
        return reusable_;
    }

private:
    Stream& stream_;
    const ruvia::detail::Http1ServerRequestParseState& parsed_;
    std::pmr::memory_resource* resource_;
    ruvia::detail::ResponseHeadBuffer head_;
    std::size_t bytes_{0};
    std::optional<std::size_t> remainingContentBytes_;
    bool chunked_{false};
    bool bodyOpen_{false};
    bool reusable_{false};
};

// Parser rejections have no valid request method, but they still use the shared
// status/body/framing policy and the parser's exact protocol version.
template <typename Stream>
[[nodiscard]] asio::awaitable<void> writeHttp1StatusResponse(
    Stream& stream,
    ruvia::detail::Http1ServerConnectionPlan requestConnectionPlan,
    std::pmr::memory_resource* resource,
    std::uint16_t status) {
    const Headers noHeaders;
    auto response = makeEdgeResponse(resource, status, noHeaders, "MISS", std::nullopt);
    const auto commit = ruvia::detail::http1CommitFinalResponse(response, requestConnectionPlan.requireClose());
    if (commit.failure() != nullptr) {
        co_return;
    }
    const auto plan = ruvia::detail::http1BufferedResponsePlan(
        ruvia::detail::httpBufferedResponseWritePlan(HttpKnownMethod::kUnknown, response),
        *commit.committed());
    ruvia::detail::ResponseHeadBuffer head{std::pmr::polymorphic_allocator<char>(resource)};
    ruvia::detail::appendResponseHead(response, head, plan.headPlan());
    (void)co_await http1_writer_detail::writeHttp1Buffers(stream, asio::buffer(head.view()));
}

}  // namespace ruvia::edge
