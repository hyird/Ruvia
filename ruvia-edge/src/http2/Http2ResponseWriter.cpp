#include "ruvia/edge/detail/http2/Http2ResponseWriter.h"

#include <chrono>
#include <span>
#include <string>

#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

namespace ruvia::edge {

asio::awaitable<bool> Http2ResponseWriter::respond(
    std::uint16_t status, const Headers& headers, std::string_view body,
    std::string_view cacheResult, std::optional<std::uint64_t> age, bool omitBody, bool) {
    submitBuffered(status, headers, body, cacheResult, age, omitBody);
    poke();
    ended_ = true;
    co_return true;
}

asio::awaitable<bool> Http2ResponseWriter::respondHead(
    std::uint16_t status, const Headers& headers, std::string_view cacheResult,
    bool hasBody, std::optional<std::size_t>, bool) {
    HttpResponse response(resource_);
    response.status(
        HttpStatusCode::tryFromValue(status).value_or(http_status::kInternalServerError));
    for (const auto& [name, value] : headers) {
        std::string lower(name);
        for (auto& c : lower) {
            c = toLowerAscii(c);
        }
        if (isConnectionOrFramingField(lower) ||
            connectionNominates(headers, name)) {
            continue;  // HTTP/2 frames the body itself; drop length/framing fields
        }
        response.header(name, value);
    }
    response.header("X-Cache", cacheResult);
    // No body and no Content-Length on the head: the body streams as DATA and
    // the length is unknown (this path serves chunked/close-delimited origins).
    const auto result = shared_.connection.submitStreamingResponseHead(
        streamId_, std::move(response), ruvia::detail::ResponseStreamKind::kGeneric,
        ruvia::detail::ResponseTrailerIntent::kNone);
    poke();
    if (result.submitted() == nullptr) {
        (void)shared_.connection.submitReset(
            streamId_, ruvia::detail::Http2ErrorCode::kInternalError);
        ended_ = true;
        co_return false;
    }
    bodyOpen_ = hasBody;
    if (!hasBody) {
        ended_ = true;  // the head carried END_STREAM
    }
    co_return true;
}

asio::awaitable<bool> Http2ResponseWriter::respondChunk(std::string_view chunk) {
    if (!bodyOpen_ || chunk.empty()) {
        co_return true;
    }
    for (;;) {
        auto* streamState = shared_.connection.stream(streamId_);
        if (streamState == nullptr || streamState->isAborted()) {
            co_return false;
        }
        // Only one flow-blocked submission may be outstanding per stream; wait
        // for the prior one to drain before offering the next chunk.
        if (shared_.connection.hasQueuedData(streamId_)) {
            if (!co_await waitForWindow()) {
                co_return false;
            }
            continue;
        }
        const auto status = shared_.connection.submitData(
            streamId_, chunk, ruvia::detail::Http2EndStream::kKeepOpen);
        poke();
        if (status == ruvia::detail::Http2DataSubmitStatus::kAccepted ||
            status == ruvia::detail::Http2DataSubmitStatus::kQueued) {
            bytes_ += chunk.size();
            co_return true;
        }
        if (status == ruvia::detail::Http2DataSubmitStatus::kBackpressured) {
            if (!co_await waitForWindow()) {
                co_return false;
            }
            continue;  // retry the same chunk once the window reopens
        }
        co_return false;  // kClosed / content-length / invalid state
    }
}

asio::awaitable<bool> Http2ResponseWriter::respondEnd() {
    if (!bodyOpen_ || ended_) {
        co_return true;
    }
    ended_ = true;
    auto* streamState = shared_.connection.stream(streamId_);
    if (streamState == nullptr || streamState->isAborted()) {
        co_return false;
    }
    const auto trailerResult =
        ruvia::detail::httpResponseTrailerSection(std::span<const HttpHeaderView>{});
    const auto* section = trailerResult.section();
    if (section == nullptr) {
        (void)shared_.connection.submitReset(
            streamId_, ruvia::detail::Http2ErrorCode::kInternalError);
        poke();
        co_return false;
    }
    // finishResponse terminates the stream: with an empty trailer section it
    // emits a zero-length DATA carrying END_STREAM (queued behind any still-
    // draining body when flow-control-blocked).
    const auto status = shared_.connection.finishResponse(streamId_, *section);
    poke();
    co_return status == ruvia::detail::Http2FinishSubmitStatus::kAccepted ||
        status == ruvia::detail::Http2FinishSubmitStatus::kQueued;
}

void Http2ResponseWriter::poke() noexcept { shared_.writeWake.cancel(); }

asio::awaitable<bool> Http2ResponseWriter::waitForWindow() {
    // Once the session is tearing down there is no reader to reopen the window
    // and no writer to drain it, so parking here would never make progress. A
    // handler resuming from its origin fetch after teardown began must unwind
    // now rather than register a waiter that only shutdown would cancel.
    if (shared_.shuttingDown) {
        co_return false;
    }
    drainTimer_.expires_at((std::chrono::steady_clock::time_point::max)());
    shared_.drainWaiters[streamId_] = &drainTimer_;
    co_await drainTimer_.async_wait(asio::as_tuple(asio::use_awaitable));
    shared_.drainWaiters.erase(streamId_);
    if (shared_.shuttingDown) {
        co_return false;
    }
    auto* streamState = shared_.connection.stream(streamId_);
    co_return streamState != nullptr && !streamState->isAborted();
}

void Http2ResponseWriter::submitBuffered(
    std::uint16_t status, const Headers& headers, std::string_view body,
    std::string_view cacheResult, std::optional<std::uint64_t> age, bool omitBody) {
    HttpResponse response(resource_);
    response.status(
        HttpStatusCode::tryFromValue(status).value_or(http_status::kInternalServerError));
    for (const auto& [name, value] : headers) {
        std::string lower(name);
        for (auto& c : lower) {
            c = toLowerAscii(c);
        }
        const bool nominated = connectionNominates(headers, name);
        if (isConnectionOrFramingField(lower) || nominated) {
            if (nominated || !(omitBody && lower == "content-length")) {
                continue;
            }
        }
        if (age && lower == "age") {
            continue;
        }
        response.header(name, value);
    }
    response.header("X-Cache", cacheResult);
    std::string ageText;  // must outlive submitResponseHead
    if (age) {
        ageText = std::to_string(*age);
        response.header("Age", ageText);
    }
    if (!omitBody) {
        response.body(body);
    }
    const auto plan = ruvia::detail::httpBufferedResponseWritePlan(method_, response);
    const auto submitted = shared_.connection.submitResponseHead(streamId_, response, plan);
    if (submitted.submitted() == nullptr) {
        (void)shared_.connection.submitReset(
            streamId_, ruvia::detail::Http2ErrorCode::kInternalError);
        return;
    }
    // A body larger than the flow-control window is accepted whole here: the
    // core copies the unsent suffix and dribbles it out as windows reopen.
    (void)shared_.connection.submitData(streamId_, omitBody ? std::string_view{} : body,
                                        ruvia::detail::Http2EndStream::kEndStream);
    bytes_ += omitBody ? 0 : body.size();
}

}  // namespace ruvia::edge
