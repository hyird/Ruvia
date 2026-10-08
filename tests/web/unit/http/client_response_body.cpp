#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/http/HttpResponseStream.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/HttpClientResponse.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/client/HttpClientResultBudget.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/http3/Http3ClientBodyBudget.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_identity.h"

namespace {
class TestWorker final {
public:
    explicit TestWorker(asio::io_context& io)
        : attachment(ruvia::attachEventLoop(io, {.queue_capacity = 8})),
          handle(attachment.loop().handle()) {}

    ruvia::EventLoopAttachment attachment;
    ruvia::WorkerHandle handle;
};

class LoopbackResponseServer final {
public:
    LoopbackResponseServer(asio::io_context& ioContext, const ruvia::WorkerHandle& worker,
        std::vector<std::string> bodies,
        std::chrono::milliseconds responseDelay = std::chrono::milliseconds::zero(),
        bool closeWithoutResponse = false,
        std::size_t responsesBeforeClose = 0)
        : ioContext_(ioContext),
          acceptor_(ioContext, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker),
          responseDelay_(responseDelay),
          closeWithoutResponse_(closeWithoutResponse),
          responsesBeforeClose_(responsesBeforeClose) {
        responses_.reserve(bodies.size());
        for (const auto& body : bodies) {
            responses_.push_back("HTTP/1.1 200 OK\r\nContent-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\nX-Peer: retained\r\nConnection: close\r\n\r\n" + body);
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        acceptNext();
    }

    [[nodiscard]] ruvia::Task<void> wait() {
        co_await done_.wait();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    void acceptNext() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(ioContext_);
        acceptor_.async_accept(*socket, [this, socket](const std::error_code& error) {
            if (error) {
                fail(error);
                return;
            }
            auto request = std::make_shared<asio::streambuf>();
            asio::async_read_until(*socket, *request, "\r\n\r\n",
                [this, socket, request](const std::error_code& readError, std::size_t) {
                    if (readError) {
                        fail(readError);
                        return;
                    }
                    if (closeWithoutResponse_ && nextResponse_ >= responsesBeforeClose_) {
                        std::error_code ignored;
                        socket->close(ignored);
                        done_.notify();
                        return;
                    }
                    const auto responseIndex = nextResponse_++;
                    auto writeResponse = [this, socket, responseIndex] {
                        asio::async_write(*socket, asio::buffer(responses_[responseIndex]),
                            [this, socket](const std::error_code& writeError, std::size_t) {
                                if (writeError) {
                                    fail(writeError);
                                    return;
                                }
                                if (nextResponse_ == responses_.size()) {
                                    done_.notify();
                                } else {
                                    acceptNext();
                                }
                            });
                    };
                    if (responseDelay_ == std::chrono::milliseconds::zero()) {
                        writeResponse();
                    } else {
                        auto timer = std::make_shared<asio::steady_timer>(ioContext_, responseDelay_);
                        timer->async_wait([timer, writeResponse = std::move(writeResponse)](
                                              const std::error_code& timerError) mutable {
                            if (!timerError) {
                                writeResponse();
                            }
                        });
                    }
                });
        });
    }

    void fail(const std::error_code& error) {
        if (failure_ == nullptr) {
            failure_ = std::make_exception_ptr(std::system_error(error));
            done_.notify();
        }
    }

    asio::io_context& ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::string> responses_;
    ruvia::WorkerSignal done_;
    std::exception_ptr failure_;
    std::size_t nextResponse_{0};
    std::chrono::milliseconds responseDelay_;
    bool closeWithoutResponse_;
    std::size_t responsesBeforeClose_;
};

[[nodiscard]] ruvia::HttpClientConfig localHttpClientConfig(std::uint16_t port) {
    return ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp,
        .host = "127.0.0.1",
        .port = port,
        .protocol = ruvia::HttpClientProtocol::kHttp1Only};
}

class UploadPeer final {
public:
    enum class Mode { kChunked,
        kKnownLength,
        kContinue,
        kContinueTimeout,
        kEarlyFinal };
    UploadPeer(asio::io_context& io, const ruvia::WorkerHandle& worker, Mode mode)
        : io_(io),
          acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker),
          mode_(mode) {}
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr error) { failure_ = error; complete_ = true; done_.notify(); });
    }
    ruvia::Task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string head;
    std::string body;

private:
    asio::awaitable<void> serve() {
        asio::ip::tcp::socket socket(io_);
        co_await acceptor_.async_accept(socket, asio::use_awaitable);
        std::string input;
        const auto headSize = co_await asio::async_read_until(socket, asio::dynamic_buffer(input), "\r\n\r\n", asio::use_awaitable);
        head.assign(input.data(), headSize);
        input.erase(0, headSize);
        if (mode_ == Mode::kEarlyFinal) {
            constexpr std::string_view reply = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno";
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
            co_return;
        }
        if (mode_ == Mode::kContinue) {
            if (!input.empty()) {
                throw std::runtime_error("upload arrived before 100 Continue");
            }
            constexpr std::string_view reply = "HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
        }
        if (mode_ == Mode::kKnownLength) {
            if (input.size() < 6) {
                co_await asio::async_read(socket, asio::dynamic_buffer(input), asio::transfer_exactly(6 - input.size()), asio::use_awaitable);
            }
        } else if (input.find("0\r\nx-end: retained\r\n\r\n") == std::string::npos) {
            co_await asio::async_read_until(socket, asio::dynamic_buffer(input), "0\r\nx-end: retained\r\n\r\n", asio::use_awaitable);
        }
        body = std::move(input);
        if (mode_ == Mode::kKnownLength) {
            asio::steady_timer completed(io_, std::chrono::milliseconds(2));
            co_await completed.async_wait(asio::use_awaitable);
        }
        constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
    }
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    ruvia::WorkerSignal done_;
    Mode mode_;
    std::exception_ptr failure_;
    bool complete_{};
};

class Http2UploadPeer final {
public:
    Http2UploadPeer(asio::io_context& io, const ruvia::WorkerHandle& worker, bool early)
        : io_(io),
          acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker),
          early_(early) {}
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr error) { failure_ = error; complete_ = true; done_.notify(); });
    }
    ruvia::Task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string body;
    std::string trailer;

private:
    asio::awaitable<void> flush(asio::ip::tcp::socket& socket, ruvia::Http2Connection& connection) {
        const auto output = connection.pendingOutput();
        if (!output.empty()) {
            co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            (void)connection.consumeOutput(output.size());
        }
    }
    asio::awaitable<void> serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        auto connection = ruvia::Http2Connection::server();
        std::optional<ruvia::Http2RequestHeadEvent> lease;
        std::uint32_t stream{};
        bool ended{};
        co_await flush(socket, connection);
        std::array<char, ruvia::kHttp2ClientPreface.size()> preface{};
        co_await asio::async_read(socket, asio::buffer(preface), asio::use_awaitable);
        if (connection.feed(std::string_view(preface.data(), preface.size())) != ruvia::Http2FeedResult::kAccepted) {
            throw std::runtime_error("invalid client preface");
        }
        while (!ended) {
            std::array<char, ruvia::kHttp2FrameHeaderBytes> header{};
            co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
            const auto parsed = ruvia::parseHttp2FrameHeader(header);
            std::string frame(header.data(), header.size());
            frame.resize(header.size() + parsed->length);
            if (parsed->length != 0) {
                co_await asio::async_read(socket, asio::buffer(frame.data() + header.size(), parsed->length), asio::use_awaitable);
            }
            if (connection.feed(frame) != ruvia::Http2FeedResult::kAccepted) {
                throw std::runtime_error("HTTP/2 upload peer protocol error");
            }
            while (auto event = connection.nextEvent()) {
                if (auto* head = event->requestHead()) {
                    stream = head->streamId();
                    lease.emplace(std::move(*head));
                    if (early_) {
                        ended = true;
                    } else {
                        const std::array<ruvia::HttpHeaderView, 1> links{{{"link", "</asset>; rel=preload"}}};
                        if (connection.submitInterimResponseHead(stream, ruvia::HttpInterimResponseHead(ruvia::HttpStatusCode::fromValue(103), links)) != ruvia::Http2SubmitStatus::kAccepted ||
                            connection.submitInterimResponseHead(stream, ruvia::HttpInterimResponseHead(ruvia::HttpStatusCode::fromValue(100))) != ruvia::Http2SubmitStatus::kAccepted) {
                            throw std::runtime_error("HTTP/2 upload peer interim error");
                        }
                    }
                }
                if (auto* chunk = event->messageBodyChunk()) {
                    body.append(chunk->bytes());
                }
                if (const auto* end = event->messageEnd()) {
                    ended = true;
                    if (!end->trailers().empty()) {
                        trailer = end->trailers()[0].value();
                    }
                }
            }
            co_await flush(socket, connection);
        }
        ruvia::HttpResponse response;
        response.status(ruvia::HttpStatusCode::fromValue(early_ ? 413 : 200));
        if (connection.submitStreamingResponseHead(stream, std::move(response)) != ruvia::Http2SubmitStatus::kAccepted ||
            connection.submitData(stream, "ok", ruvia::Http2EndStream::kEndStream) != ruvia::Http2DataSubmitStatus::kAccepted) {
            throw std::runtime_error("HTTP/2 upload peer final error");
        }
        co_await flush(socket, connection);
        // Keep the multiplexed transport alive until its explicit client shutdown.
        std::array<char, 1024> ignored{};
        std::error_code closed;
        while (!closed) {
            co_await socket.async_read_some(asio::buffer(ignored), asio::redirect_error(asio::use_awaitable, closed));
        }
    }
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    ruvia::WorkerSignal done_;
    bool early_{};
    bool complete_{};
    std::exception_ptr failure_;
};

struct GatedResponseSink final {
    explicit GatedResponseSink(const ruvia::WorkerHandle& worker)
        : entered(worker),
          release(worker) {}

    ruvia::WorkerSignal entered;
    ruvia::WorkerSignal release;
    std::string output;
    std::string_view borrowed;
    bool fail_write{};
};

ruvia::Task<void> writeGatedResponse(void* target, std::string_view chunk) {
    auto& sink = *static_cast<GatedResponseSink*>(target);
    sink.borrowed = chunk;
    sink.entered.notify();
    co_await sink.release.wait();
    if (sink.fail_write) {
        throw std::runtime_error("downstream output rejected chunk");
    }
    sink.output.append(chunk);
}

ruvia::Task<void> endGatedResponse(void*, std::span<const ruvia::HttpHeaderView>) {
    co_return;
}

ruvia::Task<ruvia::TimerSleepResult> sleepGatedResponse(
    void*, std::chrono::milliseconds, const ruvia::StopToken&) {
    co_return ruvia::TimerSleepResult::kElapsed;
}

void bindGatedResponse(void*, ruvia::Context*, ruvia::Task<ruvia::HttpResponse> (*)(ruvia::Context&)) {}
void releaseGatedResponse(void*) noexcept {}
bool gatedResponseCommitted(void*) noexcept {
    return false;
}
bool gatedResponseAborted(void*) noexcept {
    return false;
}

[[nodiscard]] ruvia::ResponseStreamWriter makeGatedResponseWriter(GatedResponseSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(*ruvia::detail::processResource(), &sink,
        &writeGatedResponse, &endGatedResponse, &sleepGatedResponse, &bindGatedResponse,
        &releaseGatedResponse, &gatedResponseCommitted, &gatedResponseAborted);
}

[[nodiscard]] std::string largeGzipResponseBody() {
    const std::string body(128 * 1024, 'z');
    auto result = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, body,
        {.maxEncodedBytes = body.size()});
    if (result.encoded() == nullptr) {
        throw std::runtime_error("could not encode fake HTTP response body");
    }
    auto encoded = std::move(*result.encoded()).takeBytes();
    return std::string(encoded.data(), encoded.size());
}

class Http2PartialBodyPeer final {
public:
    explicit Http2PartialBodyPeer(asio::io_context& io, const ruvia::WorkerHandle& worker,
        std::string body = "partial-body", std::string longHeader = {}, std::string longTrailer = {}, bool advertisements = false)
        : io_(io),
          acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          responseReady_(worker),
          body_(std::move(body)),
          longHeader_(std::move(longHeader)),
          longTrailer_(std::move(longTrailer)),
          advertisements_(advertisements),
          responseSent_(responseSentPromise_.get_future()) {}

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = failure;
            if (failure_ != nullptr) {
                responseReadyPublished_ = true;
                responseReady_.notify();
            }
        });
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    [[nodiscard]] ruvia::Task<void> waitForResponse() {
        while (!responseReadyPublished_) {
            co_await responseReady_.wait();
        }
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

    void waitForPartialResponse() {
        if (responseSent_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("HTTP/2 peer did not send the partial response");
        }
        responseSent_.get();
    }

    [[nodiscard]] bool observedClientClose() const noexcept {
        return observedClientClose_;
    }

    [[nodiscard]] ruvia::Task<ruvia::HttpPriority> waitForPriority() {
        while (!priorityObserved_ && !observedClientClose_ && failure_ == nullptr) {
            co_await responseReady_.wait();
        }
        rethrowFailure();
        if (!priorityObserved_) {
            throw std::runtime_error("HTTP/2 connection closed before priority update");
        }
        co_return *priorityObserved_;
    }

    void rethrowFailure() const {
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    asio::awaitable<void> sendPending(asio::ip::tcp::socket& socket,
        ruvia::Http2Connection& connection) {
        const auto output = connection.pendingOutput();
        if (!output.empty()) {
            co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            (void)connection.consumeOutput(output.size());
        }
    }

    asio::awaitable<void> serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        auto connection = ruvia::Http2Connection::server();
        co_await sendPending(socket, connection);

        std::array<char, ruvia::kHttp2ClientPreface.size()> preface{};
        co_await asio::async_read(socket, asio::buffer(preface), asio::use_awaitable);
        if (connection.feed(std::string_view(preface.data(), preface.size())) !=
            ruvia::Http2FeedResult::kAccepted) {
            throw std::runtime_error("HTTP/2 peer rejected the client preface");
        }

        std::uint32_t requestStream = 0;
        std::string requestFrame;
        std::optional<ruvia::Http2RequestHeadEvent> requestLease;
        while (requestStream == 0) {
            std::array<char, ruvia::kHttp2FrameHeaderBytes> header{};
            co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
            const auto frameHeader = ruvia::parseHttp2FrameHeader(std::span<const char>(header));
            if (!frameHeader.has_value()) {
                throw std::runtime_error("HTTP/2 peer received an incomplete frame header");
            }
            requestFrame.assign(header.data(), header.size());
            requestFrame.resize(header.size() + frameHeader->length);
            if (frameHeader->length != 0) {
                co_await asio::async_read(socket,
                    asio::buffer(requestFrame.data() + header.size(), frameHeader->length),
                    asio::use_awaitable);
            }
            if (connection.feed(requestFrame) == ruvia::Http2FeedResult::kProtocolFailure) {
                throw std::runtime_error("HTTP/2 peer rejected client frames");
            }
            while (auto event = connection.nextEvent()) {
                if (auto* request = event->requestHead()) {
                    requestStream = request->streamId();
                    requestLease.emplace(std::move(*request));
                }
            }
            co_await sendPending(socket, connection);
        }

        ruvia::HttpResponse response;
        response.status(ruvia::http_status::kOk);
        if (advertisements_ && connection.submitAlternativeServiceAdvertisement(requestStream, {}, "h3=\":443\"; ma=60") != ruvia::Http2SubmitStatus::kAccepted) {
            throw std::runtime_error("HTTP/2 peer could not submit ALTSVC");
        }
        if (!longHeader_.empty()) {
            response.header("x-long", longHeader_);
        }
        if (longTrailer_.empty()) {
            if (connection.submitStreamingResponseHead(requestStream, std::move(response)) !=
                ruvia::Http2SubmitStatus::kAccepted) {
                throw std::runtime_error("HTTP/2 peer could not submit response headers");
            }
        } else {
            const auto submitted = connection.submitStreamingResponseHead(requestStream,
                std::move(response), ruvia::http_response_stream_kind::generic,
                ruvia::http_response_trailer_intent::present);
            if (submitted.submitted() == nullptr) {
                throw std::runtime_error("HTTP/2 peer could not submit response headers");
            }
        }
        if (connection.submitData(requestStream, body_, ruvia::Http2EndStream::kKeepOpen) !=
            ruvia::Http2DataSubmitStatus::kAccepted) {
            throw std::runtime_error("HTTP/2 peer could not submit response data");
        }
        if (!longTrailer_.empty()) {
            const std::array trailers{ruvia::HttpHeaderView("x-long-trailer", longTrailer_)};
            const auto section = ruvia::validateHttpResponseTrailers(trailers);
            if (connection.finishResponse(requestStream, section) !=
                ruvia::Http2FinishResponseStatus::kAccepted) {
                throw std::runtime_error("HTTP/2 peer could not submit response trailers");
            }
        }
        // Keep the request lease until the client closes. Releasing it early
        // would invalidate the server-side request while its connection is active.
        co_await sendPending(socket, connection);
        responseReadyPublished_ = true;
        responseReady_.notify();
        responseSentPromise_.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (const auto received = co_await socket.async_read_some(asio::buffer(input),
                   asio::redirect_error(asio::use_awaitable, error))) {
            if (connection.feed(std::string_view(input.data(), received)) == ruvia::Http2FeedResult::kProtocolFailure) {
                throw std::runtime_error("HTTP/2 peer rejected late control input");
            }
            while (auto event = connection.nextEvent()) {
                if (const auto* update = event->priorityUpdate()) {
                    priorityObserved_ = update->fields.requestPriority();
                    responseReady_.notify();
                }
            }
            co_await sendPending(socket, connection);
        }
        observedClientClose_ = static_cast<bool>(error);
        responseReady_.notify();
        if (!requestLease.has_value() || connection.release(std::move(*requestLease)) !=
                                             ruvia::Http2ServerRequestReleaseStatus::kReleased) {
            throw std::runtime_error("HTTP/2 peer could not release its request lease");
        }
        requestFrame.clear();
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    ruvia::WorkerSignal responseReady_;
    std::string body_;
    std::string longHeader_;
    std::string longTrailer_;
    bool advertisements_{};
    std::promise<void> responseSentPromise_;
    std::future<void> responseSent_;
    std::exception_ptr failure_;
    bool responseReadyPublished_{false};
    bool observedClientClose_{false};
    std::optional<ruvia::HttpPriority> priorityObserved_{};
};

class Http1ChunkedResponsePeer final {
public:
    enum class EndMode : unsigned char { kTerminalGate,
        kWaitForClientClose };

    explicit Http1ChunkedResponsePeer(asio::io_context& io, std::string encoded,
        EndMode endMode = EndMode::kTerminalGate, std::string longHeader = {})
        : io_(io),
          acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          gate_(io),
          encoded_(std::move(encoded)),
          longHeader_(std::move(longHeader)),
          endMode_(endMode),
          done_(donePromise_.get_future()) {}

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = failure;
            donePromise_.set_value();
        });
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void releaseTerminalChunk() {
        releaseRequested_ = true;
        gate_.cancel();
    }

    [[nodiscard]] bool observedClientClose() const noexcept {
        return observedClientClose_;
    }

    void wait() {
        if (done_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("HTTP/1 peer did not finish");
        }
        done_.get();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    asio::awaitable<void> serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        std::array<char, 2 * sizeof(std::size_t)> chunkSize{};
        const auto [chunkEnd, chunkError] = std::to_chars(
            chunkSize.data(), chunkSize.data() + chunkSize.size(), encoded_.size(), 16);
        if (chunkError != std::errc{}) {
            throw std::runtime_error("HTTP/1 peer could not format chunk size");
        }
        std::string response =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n"
            "Trailer: X-End\r\nConnection: close\r\n";
        if (!longHeader_.empty()) {
            response += "X-Long: " + longHeader_ + "\r\n";
        }
        response += "\r\n";
        response.append(chunkSize.data(), chunkEnd);
        response += "\r\n" + encoded_ + "\r\n";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        if (endMode_ == EndMode::kWaitForClientClose) {
            std::array<char, 1024> input{};
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(input),
                asio::redirect_error(asio::use_awaitable, error))) {
            }
            observedClientClose_ = error == asio::error::eof ||
                                   error == asio::error::connection_reset ||
                                   error == asio::error::operation_aborted;
            co_return;
        }

        if (!releaseRequested_) {
            gate_.expires_after(std::chrono::seconds(5));
            std::error_code gateError;
            co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, gateError));
            if (gateError != asio::error::operation_aborted) {
                throw std::runtime_error("HTTP/1 terminal-chunk gate expired");
            }
        }

        constexpr std::string_view terminal = "0\r\nX-End: retained\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(terminal), asio::use_awaitable);
        std::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer gate_;
    std::string encoded_;
    std::string longHeader_;
    EndMode endMode_;
    std::promise<void> donePromise_;
    std::future<void> done_;
    std::exception_ptr failure_;
    bool releaseRequested_{false};
    bool observedClientClose_{false};
};

template <typename Operation>
void runOperation(TestWorker& worker, asio::io_context& io, Operation&& operation) {
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        try {
            co_await operation();
        } catch (...) {
            failure = std::current_exception();
        }
        worker.attachment.stop();
    };
    auto root = worker.attachment.loop().start(run());
    worker.attachment.run();
    root.get();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

RUVIA_TEST(client_body_chunks_preserve_octets_and_pending_data_does_not_invalidate_views) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    ruvia::detail::Http3ClientBodyBudget receiveBudget(32);
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        state.buffered.assign("\0\xff\xc3", 3);
        state.pending.assign("\xa9", 1);
        RUVIA_CHECK(state.bindHttp3BodyBudget(receiveBudget));
        RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{4});
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            const auto first = co_await state.consume_body<std::span<const std::byte>>();
            RUVIA_CHECK(first.has_value());
            RUVIA_CHECK_EQ(first->size(), std::size_t{3});
            RUVIA_CHECK((*first)[0] == std::byte{0});
            RUVIA_CHECK((*first)[1] == std::byte{0xff});
            state.pending.append("tail");
            state.reconcileProducerBodyBytes();
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{8});
            RUVIA_CHECK((*first)[2] == std::byte{0xc3});
            const auto next = co_await state.consume_body<std::span<const std::byte>>();
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{5});
            RUVIA_CHECK_EQ(next->size(), std::size_t{5});
            RUVIA_CHECK((*next)[0] == std::byte{0xa9});
            RUVIA_CHECK(!(co_await state.consume_body<std::string_view>()));
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http_client_handle_options_override_pool_timeout_and_start_when_operation_runs) {
    {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        LoopbackResponseServer server(
            io, worker.handle, {"slow"}, std::chrono::milliseconds(350));
        auto config = localHttpClientConfig(server.port());
        config.requestTimeout = std::chrono::milliseconds(50);
        ruvia::HttpClient client(worker.attachment.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::Task<void> {
            auto handle = client.withOptions({.timeout = std::chrono::seconds(2)});
            auto cold = handle.send({.target = "/pool-timeout-override"});
            (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            auto response = co_await std::move(cold);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            RUVIA_CHECK_EQ(response.status(), ruvia::HttpStatusCode::fromValue(200));
            RUVIA_CHECK(elapsed >= std::chrono::milliseconds(50));
            RUVIA_CHECK(elapsed < std::chrono::seconds(2));
            co_await server.wait();
            co_await client.shutdown();
        };
        runOperation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        LoopbackResponseServer server(
            io, worker.handle, {"late"}, std::chrono::milliseconds(350));
        auto config = localHttpClientConfig(server.port());
        config.requestTimeout = std::chrono::seconds(5);
        ruvia::HttpClient client(worker.attachment.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::Task<void> {
            auto base = client.withOptions({.timeout = std::chrono::seconds(5)});
            auto shortened = base.withOptions({.timeout = std::chrono::milliseconds(100)});
            auto extended = shortened.withOptions({.timeout = std::chrono::seconds(5)});
            auto cold = extended.send({.target = "/successive-minimum"});
            (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            bool timedOut = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::HttpClientError& error) {
                timedOut = error.code() == ruvia::HttpClientError::Code::kTimeout;
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            RUVIA_CHECK(timedOut);
            RUVIA_CHECK(elapsed >= std::chrono::milliseconds(50));
            RUVIA_CHECK(elapsed < std::chrono::seconds(2));
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // The peer's delayed response is expected to hit the timed-out socket.
            }
            co_await client.shutdown();
        };
        runOperation(worker, io, operation);
    }

    for (const bool stopBase : {true, false}) {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        LoopbackResponseServer server(
            io, worker.handle, {"cancelled"}, std::chrono::milliseconds(350));
        ruvia::HttpClient client(worker.attachment.loop(), localHttpClientConfig(server.port()));
        ruvia::StopSource baseStop;
        ruvia::StopSource derivedStop;
        server.start();

        auto operation = [&]() -> ruvia::Task<void> {
            asio::steady_timer stopTimer(io);
            stopTimer.expires_after(std::chrono::milliseconds(50));
            stopTimer.async_wait([&baseStop, &derivedStop, stopBase](const std::error_code& error) {
                if (!error) {
                    (stopBase ? baseStop : derivedStop).requestStop();
                }
            });
            auto base = client.withOptions({.stopToken = baseStop.token()});
            auto copied = base;
            auto derived = copied.withOptions({.stopToken = derivedStop.token()});
            bool cancelled = false;
            try {
                (void)co_await derived.send({.target = stopBase ? "/stop-base" : "/stop-derived"});
            } catch (const ruvia::HttpClientError& error) {
                cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
            }
            RUVIA_CHECK(cancelled);
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // The peer's delayed response is expected to hit the cancelled socket.
            }
            co_await client.shutdown();
        };
        runOperation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        LoopbackResponseServer server(
            io, worker.handle, {"retiring"}, std::chrono::milliseconds(350));
        ruvia::HttpClient client(worker.attachment.loop(), localHttpClientConfig(server.port()));
        server.start();

        auto operation = [&]() -> ruvia::Task<void> {
            asio::steady_timer stopTimer(io);
            stopTimer.expires_after(std::chrono::milliseconds(50));
            stopTimer.async_wait([&worker](const std::error_code& error) {
                if (!error) {
                    worker.attachment.stop();
                }
            });
            bool cancelled = false;
            try {
                (void)co_await client.send({.target = "/event-loop-stop"});
            } catch (const ruvia::HttpClientError& error) {
                cancelled = error.code() == ruvia::HttpClientError::Code::kClosing;
            }
            RUVIA_CHECK(cancelled);
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Retirement closes the in-flight TCP exchange.
            }
        };
        runOperation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        LoopbackResponseServer server(
            io, worker.handle, {"closing"}, std::chrono::milliseconds(350));
        ruvia::HttpClient client(worker.attachment.loop(), localHttpClientConfig(server.port()));
        server.start();

        auto operation = [&]() -> ruvia::Task<void> {
            asio::steady_timer closeTimer(io);
            closeTimer.expires_after(std::chrono::milliseconds(50));
            closeTimer.async_wait([&client](const std::error_code& error) {
                if (!error) {
                    client.close();
                }
            });
            auto cold = client.withOptions({}).send({.target = "/active-close"});
            bool closing = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::HttpClientError& error) {
                closing = error.code() == ruvia::HttpClientError::Code::kClosing;
            }
            RUVIA_CHECK(closing);
            co_await client.shutdown();
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Pool shutdown closes the in-flight TCP exchange before its delayed reply.
            }
        };
        runOperation(worker, io, operation);
    }
}

RUVIA_TEST(http1_full_response_queue_cancellation_and_deadline_finish_without_reading) {
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        LoopbackResponseServer server(io, worker.handle, {std::string(65536, 'q')});
        auto config = localHttpClientConfig(server.port());
        config.maxResponseBytes = 1024;
        ruvia::HttpClient client(worker.attachment.loop(), config);
        ruvia::StopSource stop;
        server.start();

        auto operation = [&]() -> ruvia::Task<void> {
            auto handle = client.withOptions({
                .timeout = deadline ? std::optional(std::chrono::milliseconds(40)) : std::nullopt,
                .stopToken = stop.token(),
            });
            auto response = co_await handle.send({.target = "/backpressure"});
            RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
            RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{1});
            // Keep the response alive without reading or abandoning it. The
            // producer must leave its full-queue wait using the terminal event.
            if (deadline) {
                (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(80));
            } else {
                stop.requestStop();
            }
            // Ordered worker turns drain cancellation publication and the
            // producer's scheduled wake, without body-reader side effects.
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::asyncAsio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            RUVIA_CHECK_EQ(client.stats().failedRequests, std::size_t{1});
            RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{0});
            co_await client.shutdown();
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Cancellation can race the peer's final socket write.
            }
        };
        runOperation(worker, io, operation);
    }
}

RUVIA_TEST(http1_full_response_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    LoopbackResponseServer server(io, worker.handle, {std::string(65536, 'q')});
    auto config = localHttpClientConfig(server.port());
    config.maxResponseBytes = 1024;
    ruvia::HttpClient client(worker.attachment.loop(), config);
    server.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::Task<void> {
        auto response = co_await client.send({.target = "/backpressure-stop"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
        worker.attachment.stop();
        // The response remains alive across shutdown; its destructor/read()
        // cannot be the event that wakes the producer.
        co_await client.shutdown();
        joined = true;
        try {
            co_await server.wait();
        } catch (const std::system_error&) {
        }
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK(joined);
    RUVIA_CHECK(!worker.handle.valid());
}

RUVIA_TEST(http1_transfer_gzip_full_queue_cancel_and_deadline_without_reading) {
    const auto encoded = largeGzipResponseBody();
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        Http1ChunkedResponsePeer peer(
            io, encoded, Http1ChunkedResponsePeer::EndMode::kWaitForClientClose);
        auto config = localHttpClientConfig(peer.port());
        config.maxResponseBytes = 1024;
        ruvia::HttpClient client(worker.attachment.loop(), config);
        ruvia::StopSource stop;
        peer.start();

        auto operation = [&]() -> ruvia::Task<void> {
            auto handle = client.withOptions({
                .timeout = deadline ? std::optional(std::chrono::milliseconds(200)) : std::nullopt,
                .stopToken = stop.token(),
            });
            auto response = co_await handle.send({.target = "/transfer-gzip-backpressure"});
            RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
            RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{1});
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::asyncAsio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            RUVIA_CHECK(client.stats().bytesReceived >= encoded.size());

            if (deadline) {
                (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(300));
            } else {
                stop.requestStop();
            }
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::asyncAsio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            RUVIA_CHECK_EQ(client.stats().failedRequests, std::size_t{1});
            RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{0});
            // Keep response alive through join. No body read or response
            // destruction may be what releases the producer's full queue.
            co_await client.shutdown();
        };
        runOperation(worker, io, operation);
        peer.wait();
        RUVIA_CHECK(peer.observedClientClose());
    }
}

RUVIA_TEST(http1_transfer_gzip_full_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    const auto encoded = largeGzipResponseBody();
    Http1ChunkedResponsePeer peer(
        io, encoded, Http1ChunkedResponsePeer::EndMode::kWaitForClientClose);
    auto config = localHttpClientConfig(peer.port());
    config.maxResponseBytes = 1024;
    ruvia::HttpClient client(worker.attachment.loop(), config);
    peer.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::Task<void> {
        auto response = co_await client.send({.target = "/transfer-gzip-stop"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
        RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{1});
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::asyncAsio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
            });
        }
        RUVIA_CHECK(client.stats().bytesReceived >= encoded.size());
        worker.attachment.stop();
        co_await client.shutdown();
        joined = true;
    };
    runOperation(worker, io, operation);
    peer.wait();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(peer.observedClientClose());
    RUVIA_CHECK(!worker.handle.valid());
}

RUVIA_TEST(http1_transfer_gzip_chunked_streams_before_terminal_chunk_and_bounds_each_decode) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    const std::string expected(128 * 1024, 'z');
    auto encodedResult = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, expected,
        {.maxEncodedBytes = expected.size()});
    RUVIA_CHECK(encodedResult.encoded() != nullptr);
    auto encoded = std::move(*encodedResult.encoded()).takeBytes();
    Http1ChunkedResponsePeer peer(io, std::string(encoded.data(), encoded.size()));
    auto config = localHttpClientConfig(peer.port());
    config.maxResponseBytes = 1024;
    ruvia::HttpClient client(worker.attachment.loop(), config);
    peer.start();

    std::size_t decodedBytes = 0;
    auto operation = [&]() -> ruvia::Task<void> {
        auto response = co_await client.send({.target = "/transfer-gzip"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
        RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{1});

        const auto first = co_await response.body().read();
        RUVIA_CHECK(first.has_value());
        RUVIA_CHECK(first->size() <= config.maxResponseBytes);
        RUVIA_CHECK(!response.body().complete());
        const auto firstView = std::string_view(
            reinterpret_cast<const char*>(first->data()), first->size());
        decodedBytes += first->size();

        // Let the producer decode already-buffered compressed bytes while the
        // consumer's borrowed view remains live; only pending storage may grow.
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::asyncAsio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
            });
        }
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(first->data()), first->size()),
            firstView);
        peer.releaseTerminalChunk();

        while (const auto chunk = co_await response.body().read()) {
            RUVIA_CHECK(chunk->size() <= config.maxResponseBytes);
            decodedBytes += chunk->size();
        }
        RUVIA_CHECK(response.body().complete());
        RUVIA_CHECK_EQ(response.trailer("x-end"), std::optional<std::string_view>("retained"));
        RUVIA_CHECK_EQ(client.stats().inFlightRequests, std::size_t{0});
        co_await client.shutdown();
    };
    runOperation(worker, io, operation);
    peer.wait();
    RUVIA_CHECK_EQ(decodedBytes, expected.size());
    RUVIA_CHECK(decodedBytes > config.maxResponseBytes);
}

RUVIA_TEST(http1_response_storage_remains_worker_owned_after_client_destruction) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    const std::string expected(128 * 1024, 'z');
    const auto encoded = largeGzipResponseBody();
    const std::string longHeader(32 * 1024, 'h');
    Http1ChunkedResponsePeer peer(io, encoded,
        Http1ChunkedResponsePeer::EndMode::kTerminalGate, longHeader);
    peer.start();

    auto operation = [&]() -> ruvia::Task<void> {
        std::optional<ruvia::HttpClientResponse> response;
        std::optional<ruvia::HttpClientResponseBytes> retainedBody;
        std::string collected;
        {
            ruvia::HttpClient client(worker.attachment.loop(), localHttpClientConfig(peer.port()));
            response.emplace(co_await client.send({.target = "/retained-response"}));
            const auto first = co_await response->body().read();
            RUVIA_CHECK(first.has_value());
            if (first) {
                collected.append(reinterpret_cast<const char*>(first->data()), first->size());
            }
            peer.releaseTerminalChunk();
            retainedBody.emplace(co_await response->body().readAll());
            co_await client.shutdown();
        }

        const auto headers = response->headers();
        const auto longValue = response->header("x-long");
        RUVIA_CHECK(longValue.has_value());
        RUVIA_CHECK_EQ(longValue->size(), longHeader.size());
        RUVIA_CHECK(std::ranges::all_of(*longValue, [](char value) { return value == 'h'; }));
        RUVIA_CHECK(!headers.empty());
        const auto text = co_await response->body().text();
        RUVIA_CHECK(!text.has_value());
        RUVIA_CHECK(!(co_await response->body().read()).has_value());
        const auto remaining = co_await response->body().readAll();
        RUVIA_CHECK(remaining.empty());
        const auto retainedView = retainedBody->bytes();
        collected.append(reinterpret_cast<const char*>(retainedView.data()), retainedView.size());
        RUVIA_CHECK_EQ(collected, expected);
        RUVIA_CHECK_EQ(response->trailer("x-end"), std::optional<std::string_view>("retained"));
        response.reset();
        RUVIA_CHECK_EQ(retainedBody->bytes().size(), retainedView.size());
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(retainedBody->bytes().data()),
                           retainedBody->bytes().size()),
            expected.substr(expected.size() - retainedView.size()));
        retainedBody.reset();
    };
    runOperation(worker, io, operation);
    peer.wait();
}

RUVIA_TEST(client_shutdown_does_not_join_a_gated_response_pipe_consumer) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    LoopbackResponseServer server(io, worker.handle, {std::string(64 * 1024, 'p')});
    server.start();

    auto operation = [&]() -> ruvia::Task<void> {
        auto client = std::make_unique<ruvia::HttpClient>(
            worker.attachment.loop(), localHttpClientConfig(server.port()));
        auto response = co_await client->send({.target = "/gated-pipe"});
        GatedResponseSink sink(worker.handle);
        auto writer = makeGatedResponseWriter(sink);
        bool pipeCompleted = false;
        bool pipeClosed = false;
        ruvia::TaskScope tasks(worker.handle);
        auto pipeTask = [&]() -> ruvia::Task<void> {
            try {
                co_await response.body().pipeTo(writer);
                pipeCompleted = true;
            } catch (const ruvia::HttpClientError& error) {
                pipeClosed = error.code() == ruvia::HttpClientError::Code::kClosing ||
                             error.code() == ruvia::HttpClientError::Code::kCancelled ||
                             error.code() == ruvia::HttpClientError::Code::kIoError;
            }
        };
        tasks.spawn(pipeTask());
        co_await sink.entered.wait();
        client->close();
        co_await client->shutdown();
        client.reset();
        sink.release.notify();
        co_await tasks.join();
        RUVIA_CHECK(pipeCompleted || pipeClosed);
        RUVIA_CHECK(!sink.output.empty());
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
        co_await server.wait();
    };
    runOperation(worker, io, operation);
}

RUVIA_TEST(client_response_cold_collection_survives_client_and_expires_with_response) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    LoopbackResponseServer server(io, worker.handle, {"cold-body"});
    server.start();

    auto operation = [&]() -> ruvia::Task<void> {
        auto client = std::make_unique<ruvia::HttpClient>(
            worker.attachment.loop(), localHttpClientConfig(server.port()));
        std::optional<ruvia::HttpClientResponse> response;
        response.emplace(co_await client->send({.target = "/cold-body"}));
        auto cold = response->body().readAll();
        co_await client->shutdown();
        client.reset();

        auto retained = co_await std::move(cold);
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(retained.bytes().data()),
                           retained.bytes().size()),
            std::string_view("cold-body"));
        auto expired = response->body().readAll();
        response.reset();
        bool rejected = false;
        try {
            (void)co_await std::move(expired);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        co_await server.wait();
    };
    runOperation(worker, io, operation);
}

RUVIA_TEST(http2_client_observes_alternative_service_frame_as_independently_owned_result) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    Http2PartialBodyPeer peer(io, worker.handle, "partial-body", {}, {}, true);
    peer.start();
    auto config = localHttpClientConfig(peer.port());
    config.protocol = ruvia::HttpClientProtocol::kHttp2Only;
    config.advertisements.receiveAlternativeServices = true;
    auto operation = [&]() -> ruvia::Task<void> {
        std::optional<ruvia::HttpClientAdvertisement> retained;
        {
            ruvia::HttpClient client(worker.attachment.loop(), config);
            std::exception_ptr failure;
            try {
                auto response = co_await client.send({.target = "/advertisement"});
                retained = client.nextAdvertisement();
                RUVIA_CHECK(retained && retained->alternativeService() && !retained->origins());
                if (retained && retained->alternativeService()) {
                    RUVIA_CHECK(retained->protocolVersion() == ruvia::HttpProtocolVersion::kHttp2 && retained->connectionSlot() == 0);
                    RUVIA_CHECK(retained->alternativeService()->streamId == 1);
                    RUVIA_CHECK(retained->alternativeService()->origin.empty());
                    RUVIA_CHECK(retained->alternativeService()->fieldValue == "h3=\":443\"; ma=60");
                }
                RUVIA_CHECK(!client.nextAdvertisement());
                RUVIA_CHECK(client.stats().droppedAdvertisements == 0);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }
        peer.rethrowFailure();
        if (retained && retained->alternativeService()) {
            RUVIA_CHECK(retained->alternativeService()->fieldValue == "h3=\":443\"; ma=60");
        }
    };
    runOperation(worker, io, operation);
}

RUVIA_TEST(http2_client_response_reprioritizes_live_stream_and_rejects_invalid_urgency) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    Http2PartialBodyPeer peer(io, worker.handle);
    peer.start();
    auto config = localHttpClientConfig(peer.port());
    config.protocol = ruvia::HttpClientProtocol::kHttp2Only;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::HttpClient client(worker.attachment.loop(), config);
        std::exception_ptr failure;
        try {
            auto response = co_await client.send({.target = "/priority"});
            response.reprioritize({.urgency = 0, .incremental = true});
            const auto priority = co_await peer.waitForPriority();
            RUVIA_CHECK(priority.urgency == 0 && priority.incremental);
            bool rejected = false;
            try {
                response.reprioritize({.urgency = 8});
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
            const auto payload = co_await response.body().text();
            RUVIA_CHECK(payload && *payload == "partial-body");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.rethrowFailure();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    runOperation(worker, io, operation);
}

RUVIA_TEST(http2_completed_response_storage_survives_client_teardown) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    const std::string body(8192, 'h');
    const std::string longHeader(32 * 1024, 'H');
    const std::string longTrailer(32 * 1024, 'T');
    Http2PartialBodyPeer peer(io, worker.handle, body, longHeader, longTrailer);
    Http2PartialBodyPeer queuedPeer(io, worker.handle, body, longHeader, longTrailer);
    peer.start();
    queuedPeer.start();
    auto config = localHttpClientConfig(peer.port());
    config.protocol = ruvia::HttpClientProtocol::kHttp2Only;

    auto operation = [&]() -> ruvia::Task<void> {
        std::optional<ruvia::HttpClientResponse> response;
        std::optional<std::span<const std::byte>> bodyView;
        auto client = std::make_unique<ruvia::HttpClient>(worker.attachment.loop(), config);
        response.emplace(co_await client->send({.target = "/retained-h2"}));
        co_await peer.waitForResponse();
        for (unsigned attempt = 0; response->trailers().empty() && attempt < 500; ++attempt) {
            (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(!response->trailers().empty());
        const auto chunk = co_await response->body().read();
        RUVIA_CHECK(chunk.has_value());
        bodyView = *chunk;
        auto cold = response->body().readAll();
        co_await client->shutdown();
        client.reset();

        const auto headers = response->headers();
        const auto trailers = response->trailers();
        RUVIA_CHECK(!headers.empty());
        RUVIA_CHECK(!trailers.empty());
        RUVIA_CHECK_EQ(response->header("x-long"),
            std::optional<std::string_view>(std::string_view(longHeader)));
        RUVIA_CHECK_EQ(response->trailer("x-long-trailer"),
            std::optional<std::string_view>(std::string_view(longTrailer)));
        RUVIA_CHECK(bodyView.has_value());
        if (bodyView) {
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bodyView->data()),
                               bodyView->size()),
                body);
        }

        LoopbackResponseServer nextServer(io, worker.handle, {"next-client"});
        nextServer.start();
        {
            ruvia::HttpClient nextClient(worker.attachment.loop(), localHttpClientConfig(nextServer.port()));
            auto nextResponse = co_await nextClient.send({.target = "/new-client"});
            auto nextBody = co_await nextResponse.body().readAll();
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(nextBody.bytes().data()),
                               nextBody.bytes().size()),
                std::string_view("next-client"));
            co_await nextClient.shutdown();
        }
        co_await nextServer.wait();
        const auto retainedHeader = std::ranges::find_if(headers, [](const auto& header) {
            return header.name() == "x-long";
        });
        const auto retainedTrailer = std::ranges::find_if(trailers, [](const auto& header) {
            return header.name() == "x-long-trailer";
        });
        RUVIA_CHECK(retainedHeader != headers.end());
        RUVIA_CHECK(retainedTrailer != trailers.end());
        if (retainedHeader != headers.end()) {
            RUVIA_CHECK_EQ(retainedHeader->value(), std::string_view(longHeader));
        }
        if (retainedTrailer != trailers.end()) {
            RUVIA_CHECK_EQ(retainedTrailer->value(), std::string_view(longTrailer));
        }
        if (bodyView) {
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bodyView->data()),
                               bodyView->size()),
                body);
        }
        auto retained = co_await std::move(cold);
        RUVIA_CHECK(retained.empty());
        response.reset();

        auto queuedConfig = config;
        queuedConfig.port = queuedPeer.port();
        auto queuedClient = std::make_unique<ruvia::HttpClient>(worker.attachment.loop(), queuedConfig);
        std::optional<ruvia::HttpClientResponse> queuedResponse;
        queuedResponse.emplace(co_await queuedClient->send({.target = "/queued-body"}));
        co_await queuedPeer.waitForResponse();
        for (unsigned attempt = 0; queuedResponse->trailers().empty() && attempt < 500; ++attempt) {
            (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(!queuedResponse->trailers().empty());
        auto queuedBody = queuedResponse->body().readAll();
        co_await queuedClient->shutdown();
        queuedClient.reset();
        auto queuedBytes = co_await std::move(queuedBody);
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(queuedBytes.bytes().data()),
                           queuedBytes.bytes().size()),
            body);
        RUVIA_CHECK_EQ(queuedResponse->header("x-long"),
            std::optional<std::string_view>(std::string_view(longHeader)));
        queuedResponse.reset();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(queuedBytes.bytes().data()),
                           queuedBytes.bytes().size()),
            body);
    };
    runOperation(worker, io, operation);
    peer.waitForPartialResponse();
    queuedPeer.waitForPartialResponse();
    peer.rethrowFailure();
    queuedPeer.rethrowFailure();
    RUVIA_CHECK(peer.observedClientClose());
    RUVIA_CHECK(queuedPeer.observedClientClose());
}

RUVIA_TEST(client_shutdown_keeps_incomplete_response_error_observable) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    Http2PartialBodyPeer peer(io, worker.handle);
    peer.start();
    auto config = localHttpClientConfig(peer.port());
    config.protocol = ruvia::HttpClientProtocol::kHttp2Only;

    auto operation = [&]() -> ruvia::Task<void> {
        std::optional<ruvia::HttpClientResponse> response;
        bool runningReadFailed = false;
        {
            ruvia::HttpClient client(worker.attachment.loop(), config);
            response.emplace(co_await client.send({.target = "/incomplete-after-close"}));
            asio::steady_timer closeTimer(io);
            closeTimer.expires_after(std::chrono::milliseconds(50));
            closeTimer.async_wait([&client](const std::error_code& error) {
                if (!error) {
                    client.close();
                }
            });
            try {
                (void)co_await response->body().readAll();
            } catch (const ruvia::HttpClientError& error) {
                runningReadFailed = error.code() == ruvia::HttpClientError::Code::kClosing ||
                                    error.code() == ruvia::HttpClientError::Code::kCancelled ||
                                    error.code() == ruvia::HttpClientError::Code::kIoError;
            }
            RUVIA_CHECK(runningReadFailed);
            co_await client.shutdown();
        }
        bool retainedResponseFailed = false;
        try {
            (void)co_await response->body().readAll();
        } catch (const ruvia::HttpClientError& error) {
            retainedResponseFailed = error.code() == ruvia::HttpClientError::Code::kClosing ||
                                     error.code() == ruvia::HttpClientError::Code::kCancelled ||
                                     error.code() == ruvia::HttpClientError::Code::kIoError;
        }
        RUVIA_CHECK(retainedResponseFailed);
        response.reset();
    };
    runOperation(worker, io, operation);
    peer.waitForPartialResponse();
    peer.rethrowFailure();
    RUVIA_CHECK(peer.observedClientClose());
}

RUVIA_TEST(http2_event_loop_stop_joins_reader_writer_with_a_partial_response_body) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    Http2PartialBodyPeer peer(io, worker.handle);
    peer.start();
    auto config = localHttpClientConfig(peer.port());
    config.protocol = ruvia::HttpClientProtocol::kHttp2Only;
    ruvia::HttpClient client(worker.attachment.loop(), config);

    auto operation = [&]() -> ruvia::Task<void> {
        auto response = co_await client.send({.target = "/partial"});
        RUVIA_CHECK_EQ(response.status(), ruvia::HttpStatusCode::fromValue(200));
        asio::steady_timer stopTimer(io);
        stopTimer.expires_after(std::chrono::milliseconds(100));
        stopTimer.async_wait([&worker](const std::error_code& error) {
            if (!error) {
                worker.attachment.stop();
            }
        });
        bool cancelled = false;
        try {
            (void)co_await response.body().readAll();
        } catch (const ruvia::HttpClientError& error) {
            // A started body operation can observe its client stop token before
            // the pool-close outcome. Both are terminal cancellation, not a
            // protocol failure from the peer's deliberately open stream.
            cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled ||
                        error.code() == ruvia::HttpClientError::Code::kClosing;
        }
        RUVIA_CHECK(cancelled);
    };
    runOperation(worker, io, operation);
    peer.waitForPartialResponse();
    peer.rethrowFailure();
    RUVIA_CHECK(peer.observedClientClose());
    RUVIA_CHECK(!client.worker().valid());
    RUVIA_CHECK(!client.worker().accepting());
}

RUVIA_TEST(configured_http_registry_handle_reclaims_repeated_real_tcp_operations) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    LoopbackResponseServer server(io, worker.handle, {"one", "two", "three", "four"});
    auto config = localHttpClientConfig(server.port());
    std::optional<ruvia::detail::HttpClientDefinition> definition;
    definition.emplace(ruvia::detail::HttpClientDefinition{
        std::pmr::string("default", &resource),
        ruvia::detail::HttpClientConfigStorage(config, &resource)});
    const auto definition_baseline = resource.liveAllocations();
    auto budget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 64});
    std::optional<ruvia::HttpClientResponse> retainedResponse;
    std::optional<ruvia::HttpClientResponseBytes> retainedBody;
    {
        ruvia::detail::HttpClientRegistry registry(io, worker.handle, &resource,
            std::span<const ruvia::detail::HttpClientDefinition>(&*definition, 1), budget);
        ruvia::operation_scope scope;
        server.start();
        auto operation = [&]() -> ruvia::Task<void> {
            auto handle = registry.get(scope, {.timeout = std::chrono::seconds(2)});
            auto response = co_await handle.send({.target = "/retained"});
            retainedBody.emplace(co_await response.body().readAll(64));
            retainedResponse.emplace(std::move(response));
            RUVIA_CHECK_EQ(budget->retainedBytes(), std::size_t{3});
            const auto headerBaseline = resource.liveAllocations();
            const auto peerHeader = std::ranges::find_if(
                retainedResponse->headers(), [](const auto& header) {
                    return header.name() == "X-Peer";
                });
            RUVIA_CHECK(peerHeader != retainedResponse->headers().end());
            if (peerHeader != retainedResponse->headers().end()) {
                RUVIA_CHECK_EQ(peerHeader->value(), std::string_view("retained"));
            }

            const std::array repeatedResponses{
                std::pair{"/repeat-1", std::string_view("two")},
                std::pair{"/repeat-2", std::string_view("three")},
                std::pair{"/repeat-3", std::string_view("four")},
            };
            for (const auto& [target, expectedBody] : repeatedResponses) {
                {
                    auto repeated = co_await registry.get(
                                                         scope, {.timeout = std::chrono::seconds(2)})
                                        .send({.target = target});
                    auto result = co_await repeated.body().readAll(64);
                    RUVIA_CHECK_EQ(std::string_view(
                                       reinterpret_cast<const char*>(result.bytes().data()),
                                       result.bytes().size()),
                        expectedBody);
                }
                RUVIA_CHECK_EQ(resource.liveAllocations(), headerBaseline);
                RUVIA_CHECK_EQ(budget->retainedBytes(), std::size_t{3});
                RUVIA_CHECK(peerHeader != retainedResponse->headers().end());
                if (peerHeader != retainedResponse->headers().end()) {
                    RUVIA_CHECK_EQ(peerHeader->value(), std::string_view("retained"));
                }
                RUVIA_CHECK_EQ(std::string_view(
                                   reinterpret_cast<const char*>(retainedBody->bytes().data()),
                                   retainedBody->bytes().size()),
                    std::string_view("one"));
            }
            co_await server.wait();

            const auto coldBaseline = resource.liveAllocations();
            {
                const std::string target = "/" + std::string(4096, 'c');
                auto cold = registry.get(scope, {.timeout = std::chrono::seconds(2)})
                                .send({.target = target});
                RUVIA_CHECK(resource.liveAllocations() > coldBaseline);
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), coldBaseline);

            retainedBody.reset();
            RUVIA_CHECK_EQ(budget->retainedBytes(), std::size_t{0});
            const auto beforeHeaderRelease = resource.liveAllocations();
            retainedResponse.reset();
            // Response storage has an independent owner; releasing it must not
            // retire the client's request/transport allocations.
            RUVIA_CHECK_EQ(resource.liveAllocations(), beforeHeaderRelease);
            scope.close();
            co_await scope.close_and_join();
            registry.closeNow();
            co_await registry.join();
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), definition_baseline);
    RUVIA_CHECK_EQ(budget->retainedBytes(), std::size_t{0});
    definition.reset();
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(configured_http_registry_handle_reclaims_io_failure_and_precancel) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    LoopbackResponseServer server(
        io, worker.handle, {"warm", "unused"}, std::chrono::milliseconds::zero(), true, 1);
    auto config = localHttpClientConfig(server.port());
    std::optional<ruvia::detail::HttpClientDefinition> definition;
    definition.emplace(ruvia::detail::HttpClientDefinition{
        std::pmr::string("default", &resource),
        ruvia::detail::HttpClientConfigStorage(config, &resource)});
    const auto definition_baseline = resource.liveAllocations();
    {
        ruvia::detail::HttpClientRegistry registry(io, worker.handle, &resource,
            std::span<const ruvia::detail::HttpClientDefinition>(&*definition, 1));
        ruvia::operation_scope scope;
        ruvia::StopSource preCancelled;
        preCancelled.requestStop();
        server.start();
        auto operation = [&]() -> ruvia::Task<void> {
            auto handle = registry.get(scope, {.timeout = std::chrono::seconds(2)});
            {
                auto warm = co_await handle.send({.target = "/warm"});
                auto body = co_await warm.body().readAll(64);
                RUVIA_CHECK_EQ(body.bytes().size(), std::size_t{4});
            }
            const auto warmBaseline = resource.liveAllocations();
            {
                const std::string target = "/" + std::string(4096, 'c');
                auto cold = handle.send({.target = target});
                RUVIA_CHECK(resource.liveAllocations() > warmBaseline);
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), warmBaseline);

            bool ioFailed = false;
            try {
                (void)co_await handle.send({.target = "/peer-closes"});
            } catch (const ruvia::HttpClientError& error) {
                ioFailed = error.code() == ruvia::HttpClientError::Code::kIoError;
            }
            RUVIA_CHECK(ioFailed);
            co_await server.wait();
            const auto failureBaseline = resource.liveAllocations();
            RUVIA_CHECK(failureBaseline > 0);

            auto stoppedHandle = registry.get(
                scope, {.timeout = std::chrono::seconds(2), .stopToken = preCancelled.token()});
            bool cancelled = false;
            try {
                (void)co_await stoppedHandle.send({.target = "/pre-cancelled"});
            } catch (const ruvia::HttpClientError& error) {
                cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
            }
            RUVIA_CHECK(cancelled);
            scope.close();
            co_await scope.close_and_join();
            registry.closeNow();
            co_await registry.join();
            RUVIA_CHECK(resource.liveAllocations() <= failureBaseline);
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), definition_baseline);
    definition.reset();
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(client_registry_aliases_share_the_worker_result_budget_domain) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    LoopbackResponseServer server(io, worker.handle, {"one", "two"});
    auto* const resource = std::pmr::get_default_resource();
    const auto config = localHttpClientConfig(server.port());
    const ruvia::detail::HttpClientDefinition definitions[]{
        {std::pmr::string("first", resource),
            ruvia::detail::HttpClientConfigStorage(config, resource)},
        {std::pmr::string("second", resource),
            ruvia::detail::HttpClientConfigStorage(config, resource)},
    };
    auto budgetDomain = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 3});
    ruvia::detail::HttpClientRegistry registry(
        io, worker.handle, resource, definitions, budgetDomain);
    ruvia::operation_scope scope;
    const auto firstClient = registry.get("first", scope);
    const auto secondClient = registry.get("second", scope);
    RUVIA_CHECK_EQ(firstClient.host(), std::string_view("127.0.0.1"));
    RUVIA_CHECK_EQ(secondClient.host(), std::string_view("127.0.0.1"));
    RUVIA_CHECK_EQ(firstClient.port(), server.port());
    RUVIA_CHECK_EQ(secondClient.port(), server.port());

    server.start();
    auto operation = [&]() -> ruvia::Task<void> {
        auto firstResponse = co_await firstClient.send({.target = "/first"});
        std::optional<ruvia::HttpClientResponseBytes> retained;
        retained.emplace(co_await firstResponse.body().readAll(3));
        RUVIA_CHECK_EQ(std::string_view(
                           reinterpret_cast<const char*>(retained->bytes().data()),
                           retained->bytes().size()),
            std::string_view("one"));
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{3});

        auto secondResponse = co_await secondClient.send({.target = "/second"});
        bool rejected = false;
        try {
            (void)co_await secondResponse.body().readAll(3);
        } catch (const ruvia::HttpClientError& error) {
            rejected = error.code() == ruvia::HttpClientError::Code::kResultBudgetExceeded;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{3});

        retained.reset();
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{0});
        auto retried = co_await secondResponse.body().readAll(3);
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{3});
        RUVIA_CHECK_EQ(std::string_view(
                           reinterpret_cast<const char*>(retried.bytes().data()),
                           retried.bytes().size()),
            std::string_view("two"));
        co_await server.wait();
        registry.closeNow();
        co_await registry.join();
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{0});
}

RUVIA_TEST(worker_capabilities_keep_result_budgets_independent) {
    std::optional<ruvia::HttpClientResponseBytes> firstResult;
    {
        auto& firstIo = ruvia::test::newTestIoContext();
        TestWorker firstWorker(firstIo);
        LoopbackResponseServer firstServer(firstIo, firstWorker.handle, {"one"});
        ruvia::WorkerMemory firstMemory;
        auto* const firstResource = firstMemory.resource();
        const auto firstConfig = localHttpClientConfig(firstServer.port());
        const ruvia::detail::HttpClientDefinition firstDefinition[]{
            {std::pmr::string("first", firstResource),
                ruvia::detail::HttpClientConfigStorage(firstConfig, firstResource)},
        };
        const ruvia::detail::WorkerCapabilityDefinitions firstDefinitions{
            .httpClients = firstDefinition};
        const ruvia::detail::WorkerCapabilityOptions options{
            .httpClientResultBudget = {.maxRetainedBytes = 3}};
        ruvia::detail::WorkerCapabilities firstCapabilities(
            firstIo, firstWorker.handle, firstResource, firstDefinitions, options);
        ruvia::operation_scope scope;
        const ruvia::StopToken stopToken;
        const auto client = firstCapabilities.clientRegistries().httpClient(
            "first", scope, stopToken);

        firstServer.start();
        auto operation = [&]() -> ruvia::Task<void> {
            auto response = co_await client.send({.target = "/first"});
            firstResult.emplace(co_await response.body().readAll(3));
            co_await firstServer.wait();
            firstCapabilities.closeNow();
            co_await firstCapabilities.join();
        };
        runOperation(firstWorker, firstIo, operation);
    }

    RUVIA_CHECK(firstResult.has_value());
    RUVIA_CHECK_EQ(firstResult->bytes().size(), std::size_t{3});
    {
        auto& secondIo = ruvia::test::newTestIoContext();
        TestWorker secondWorker(secondIo);
        LoopbackResponseServer secondServer(secondIo, secondWorker.handle, {"two"});
        ruvia::WorkerMemory secondMemory;
        auto* const secondResource = secondMemory.resource();
        const auto secondConfig = localHttpClientConfig(secondServer.port());
        const ruvia::detail::HttpClientDefinition definition[]{
            {std::pmr::string("second", secondResource),
                ruvia::detail::HttpClientConfigStorage(secondConfig, secondResource)},
        };
        const ruvia::detail::WorkerCapabilityDefinitions definitions{
            .httpClients = definition};
        const ruvia::detail::WorkerCapabilityOptions options{
            .httpClientResultBudget = {.maxRetainedBytes = 3}};
        ruvia::detail::WorkerCapabilities capabilities(
            secondIo, secondWorker.handle, secondResource, definitions, options);
        ruvia::operation_scope scope;
        const ruvia::StopToken stopToken;
        const auto client = capabilities.clientRegistries().httpClient(
            "second", scope, stopToken);

        secondServer.start();
        auto operation = [&]() -> ruvia::Task<void> {
            auto response = co_await client.send({.target = "/second"});
            auto result = co_await response.body().readAll(3);
            RUVIA_CHECK_EQ(result.bytes().size(), std::size_t{3});
            co_await secondServer.wait();
            capabilities.closeNow();
            co_await capabilities.join();
        };
        runOperation(secondWorker, secondIo, operation);
    }
    firstResult.reset();
}

RUVIA_TEST(client_body_consumed_buffer_wakes_backpressured_producer_before_waiting_for_data) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        state.buffered.assign(1024, 'a');
        bool produced = false;
        bool watchdogNeeded = false;
        ruvia::TaskScope tasks(worker.handle, {.resource = &resource});
        auto producer = [&]() -> ruvia::Task<void> {
            co_await state.spaceSignal.wait();
            RUVIA_CHECK(state.buffered.empty());
            state.pending.assign("next");
            produced = true;
            state.complete = true;
            state.dataSignal.notify();
        };
        auto watchdog = [&]() -> ruvia::Task<void> {
            (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(100));
            if (!produced) {
                watchdogNeeded = true;
                state.spaceSignal.notify();
            }
        };
        tasks.spawn(producer());
        tasks.spawn(watchdog());
        const auto first = co_await state.consume_body<std::string_view>();
        RUVIA_CHECK(first && first->size() == 1024 && first->front() == 'a');
        RUVIA_CHECK(!produced);  // Reading a view alone does not release it.
        const auto second = co_await state.consume_body<std::string_view>();
        RUVIA_CHECK(second && *second == "next");
        co_await tasks.join();
        RUVIA_CHECK(produced && !watchdogNeeded);
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(client_body_pipe_keeps_cursor_and_borrow_until_downstream_accepts_chunk) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        const std::string payload(1024, 'p');
        state.buffered.assign(payload);
        state.complete = true;
        GatedResponseSink sink(worker.handle);
        sink.fail_write = true;
        auto writer = makeGatedResponseWriter(sink);
        bool rejected = false;
        ruvia::TaskScope tasks(worker.handle, {.resource = &resource});
        auto pipe = [&]() -> ruvia::Task<void> {
            try {
                co_await state.consume_body<void>(&writer);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
        };
        tasks.spawn(pipe());
        co_await sink.entered.wait();
        RUVIA_CHECK_EQ(state.offset, std::size_t{0});
        RUVIA_CHECK_EQ(sink.borrowed, std::string_view(payload));
        // Network progress cannot mutate the chunk borrowed by the downstream.
        state.pending.assign("tail");
        RUVIA_CHECK_EQ(sink.borrowed, std::string_view(payload));
        sink.release.notify();
        co_await tasks.join();
        RUVIA_CHECK(rejected);
        RUVIA_CHECK(sink.output.empty());
        RUVIA_CHECK_EQ(state.offset, std::size_t{0});
        RUVIA_CHECK_EQ(std::string_view(state.buffered), std::string_view(payload));
        const auto retry = co_await state.consume_body<std::string_view>();
        RUVIA_CHECK(retry && *retry == payload);
        const auto tail = co_await state.consume_body<std::string_view>();
        RUVIA_CHECK(tail && *tail == "tail");
        RUVIA_CHECK(!(co_await state.consume_body<std::string_view>()));
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(client_body_collection_reclaims_temporaries_and_retains_results_and_headers) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    std::optional<ruvia::HttpClientResponseBytes> retained;
    ruvia::detail::Http3ClientBodyBudget receiveBudget(4096);
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        auto resultBudget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 4096});
        state.resultBudgetDomain = &resultBudget;
        const std::string payload(1024, '\xff');
        const std::string header(128, 'h');
        state.headers.push_back(ruvia::HttpHeader::copyOf("x-retained", header, &resource));
        state.buffered.assign(payload);
        state.pending.reserve(payload.size());
        RUVIA_CHECK(state.bindHttp3BodyBudget(receiveBudget));
        RUVIA_CHECK_EQ(receiveBudget.used(), payload.size());
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            const auto coldAllocationCount = resource.allocationCount();
            const auto coldBudget = resultBudget->retainedBytes();
            {
                auto cold = state.readAll(4096);
            }
            RUVIA_CHECK(!state.collectAll);
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), coldBudget);
            RUVIA_CHECK_EQ(receiveBudget.used(), payload.size());
            RUVIA_CHECK_EQ(resource.allocationCount(), coldAllocationCount);
            RUVIA_CHECK_EQ(std::string_view(state.buffered), payload);
            {
                const auto before = resource.liveAllocations();
                {
                    auto discarded = ruvia::make_scoped_operation(state.bodyOperationScope, state.readAll(4096));
                }
                RUVIA_CHECK_EQ(resource.liveAllocations(), before);
                RUVIA_CHECK_EQ(state.offset, std::size_t{0});
                RUVIA_CHECK(!state.collectAll);
            }
            retained.emplace(co_await state.readAll(4096));
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
            const auto baseline = resource.liveAllocations();
            const auto retainedBudgetBaseline = resultBudget->retainedBytes();
            for (int i = 0; i < 64; ++i) {
                state.offset = 0;
                state.buffered.assign(payload);
                state.pending.assign("\0\x80", 2);
                state.reconcileProducerBodyBytes();
                RUVIA_CHECK_EQ(receiveBudget.used(), payload.size() + 2);
                {
                    auto bytes = co_await state.readAll(4096);
                    const auto view = bytes.bytes();
                    RUVIA_CHECK_EQ(view.size(), payload.size() + 2);
                    RUVIA_CHECK(view[payload.size()] == std::byte{0});
                    RUVIA_CHECK(view.back() == std::byte{0x80});
                }
                RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
                RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
                RUVIA_CHECK_EQ(resultBudget->retainedBytes(), retainedBudgetBaseline);
                RUVIA_CHECK_EQ(state.headers.front().value(), std::string_view(header));
                RUVIA_CHECK_EQ(retained->size(), payload.size());
                RUVIA_CHECK(retained->bytes().front() == std::byte{0xff});
            }
            state.buffered.assign(payload);
            state.pending.assign("tail");
            state.offset = 0;
            state.reconcileProducerBodyBytes();
            RUVIA_CHECK_EQ(receiveBudget.used(), payload.size() + 4);
            const auto limitedBaseline = resource.liveAllocations();
            bool limited = false;
            try {
                (void)co_await state.readAll(1);
            } catch (const ruvia::HttpClientError& error) {
                limited = error.code() == ruvia::HttpClientError::Code::kResponseTooLarge;
            }
            RUVIA_CHECK(limited);
            RUVIA_CHECK_EQ(resource.liveAllocations(), limitedBaseline);
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), retainedBudgetBaseline);
            RUVIA_CHECK_EQ(std::string_view(state.buffered), payload);
            RUVIA_CHECK_EQ(std::string_view(state.pending), "tail");
            auto retried = co_await state.readAll(4096);
            const auto retriedBytes = retried.bytes();
            RUVIA_CHECK_EQ(retriedBytes.size(), payload.size() + 4);
            RUVIA_CHECK(retriedBytes.front() == std::byte{0xff});
            RUVIA_CHECK(retriedBytes[payload.size()] == std::byte{'t'});
            RUVIA_CHECK(retriedBytes.back() == std::byte{'l'});
            RUVIA_CHECK_EQ(std::string_view(state.buffered), "");
            RUVIA_CHECK_EQ(std::string_view(state.pending), "");

            state.buffered.assign("preserved on failure");
            state.reconcileProducerBodyBytes();
            state.failure = std::make_exception_ptr(std::runtime_error("transport failed"));
            bool failed = false;
            try {
                (void)co_await state.readAll(4096);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(std::string_view(state.buffered), "preserved on failure");
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
    RUVIA_CHECK(retained->bytes().back() == std::byte{0xff});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    const auto workerAllocations = resource.allocationCount();
    const auto workerDeallocations = resource.deallocationCount();
    retained.reset();
    RUVIA_CHECK_EQ(resource.allocationCount(), workerAllocations);
    RUVIA_CHECK_EQ(resource.deallocationCount(), workerDeallocations);
}

RUVIA_TEST(client_body_result_survives_response_client_worker_and_cross_thread_destruction) {
    static_assert(!std::is_copy_constructible_v<ruvia::HttpClientResponseBytes>);
    static_assert(!std::is_copy_assignable_v<ruvia::HttpClientResponseBytes>);
    static_assert(std::is_nothrow_move_constructible_v<ruvia::HttpClientResponseBytes>);
    static_assert(std::is_nothrow_move_assignable_v<ruvia::HttpClientResponseBytes>);

    std::optional<ruvia::HttpClientResponseBytes> retained;
    std::weak_ptr<ruvia::detail::HttpClientResultBudgetDomain> budgetLifetime;
    std::string expected;
    expected.reserve(1024);
    expected.push_back('\0');
    expected.push_back(static_cast<char>(0xff));
    expected.append(1022, 'r');
    {
        auto& io = ruvia::test::newTestIoContext();
        {
            TestWorker worker(io);
            {
                ruvia::HttpClient client(worker.attachment.loop(), {.host = "example.test"},
                    {.maxRetainedBytes = 4096});
                {
                    ruvia::test::CountingMemoryResource resource;
                    auto resultBudget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
                        ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 4096});
                    budgetLifetime = resultBudget;
                    {
                        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
                        state.resultBudgetDomain = &resultBudget;
                        state.buffered.assign(512, 'a');
                        state.complete = true;
                        auto operation = [&]() -> ruvia::Task<void> {
                            auto first = co_await state.readAll(4096);
                            RUVIA_CHECK_EQ(first.size(), std::size_t{512});
                            state.buffered.assign(expected);
                            auto replacement = co_await state.readAll(4096);
                            const auto* const firstAddress = first.bytes().data();
                            retained.emplace(std::move(first));
                            RUVIA_CHECK(retained->bytes().data() == firstAddress);
                            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{512} + expected.size());
                            const auto* const replacementAddress = replacement.bytes().data();
                            *retained = std::move(replacement);
                            RUVIA_CHECK(retained->bytes().data() == replacementAddress);
                            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), expected.size());
                            RUVIA_CHECK(first.empty());
                            RUVIA_CHECK(replacement.empty());
                            RUVIA_CHECK_EQ(retained->bytes().size(), expected.size());
                            co_await client.shutdown();
                        };
                        runOperation(worker, io, operation);
                    }
                    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
                    RUVIA_CHECK(resource.allocationCount() > 0);
                    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
                }
            }
        }
    }

    RUVIA_CHECK(!budgetLifetime.expired());
    std::atomic_bool contentsMatched{false};
    std::thread destroyer([bytes = std::move(*retained), expected = std::move(expected),
                              &contentsMatched]() {
        const auto actual = bytes.bytes();
        const auto wanted = std::as_bytes(std::span(expected.data(), expected.size()));
        contentsMatched.store(actual.size() == wanted.size() &&
                                  std::equal(actual.begin(), actual.end(), wanted.begin()),
            std::memory_order_release);
    });
    destroyer.join();
    RUVIA_CHECK(contentsMatched.load(std::memory_order_acquire));
    RUVIA_CHECK(retained->empty());
    RUVIA_CHECK(budgetLifetime.expired());
}

RUVIA_TEST(client_body_result_uses_pool_budget_after_client_and_worker_teardown) {
    std::optional<ruvia::HttpClientResponseBytes> retained;
    std::string expected(
        "\0\xff"
        "data",
        6);
    {
        auto& io = ruvia::test::newTestIoContext();
        {
            TestWorker worker(io);
            asio::ip::tcp::acceptor acceptor(
                io, {asio::ip::tcp::v4(), std::uint16_t{0}});
            asio::ip::tcp::socket serverSocket(io);
            ruvia::WorkerSignal serverDone(worker.handle);
            std::exception_ptr serverFailure;
            const std::string responseWire =
                "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n" +
                expected;
            acceptor.async_accept(serverSocket, [&](const std::error_code& error) {
                if (error) {
                    serverFailure = std::make_exception_ptr(std::system_error(error));
                    serverDone.notify();
                    return;
                }
                asio::async_write(serverSocket, asio::buffer(responseWire),
                    [&](const std::error_code& writeError, std::size_t) {
                        if (writeError) {
                            serverFailure = std::make_exception_ptr(std::system_error(writeError));
                        }
                        serverDone.notify();
                    });
            });
            {
                ruvia::HttpClient client(worker.attachment.loop(),
                    {.scheme = ruvia::HttpScheme::kHttp,
                        .host = "127.0.0.1",
                        .port = acceptor.local_endpoint().port(),
                        .protocol = ruvia::HttpClientProtocol::kHttp1Only},
                    {.maxRetainedBytes = expected.size()});
                auto operation = [&]() -> ruvia::Task<void> {
                    auto send = client.send({.target = "/"});
                    auto response = co_await std::move(send);
                    auto bytes = co_await response.body().readAll();
                    retained.emplace(std::move(bytes));
                    co_await serverDone.wait();
                    if (serverFailure != nullptr) {
                        std::rethrow_exception(serverFailure);
                    }
                    RUVIA_CHECK_EQ(retained->bytes().size(), expected.size());
                    co_await client.shutdown();
                };
                runOperation(worker, io, operation);
            }
        }
    }

    std::atomic_bool contentsMatched{false};
    std::thread destroyer([bytes = std::move(*retained), expected = std::move(expected),
                              &contentsMatched]() {
        const auto actual = bytes.bytes();
        const auto wanted = std::as_bytes(std::span(expected.data(), expected.size()));
        contentsMatched.store(actual.size() == wanted.size() &&
                                  std::equal(actual.begin(), actual.end(), wanted.begin()),
            std::memory_order_release);
    });
    destroyer.join();
    RUVIA_CHECK(contentsMatched.load(std::memory_order_acquire));
}

RUVIA_TEST(client_body_result_budget_rejects_without_consuming_then_retries_after_release) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        auto resultBudget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 4});
        state.resultBudgetDomain = &resultBudget;
        state.buffered.assign("old");
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            std::optional<ruvia::HttpClientResponseBytes> oldResult;
            oldResult.emplace(co_await state.readAll(4));
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{3});

            state.buffered.assign("new");
            bool exhausted = false;
            try {
                (void)co_await state.readAll(4);
            } catch (const ruvia::HttpClientError& error) {
                exhausted = error.code() == ruvia::HttpClientError::Code::kResultBudgetExceeded;
            }
            RUVIA_CHECK(exhausted);
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{3});
            RUVIA_CHECK_EQ(std::string_view(state.buffered), "new");
            RUVIA_CHECK(std::string_view(state.pending).empty());

            oldResult.reset();
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{0});
            auto retry = co_await state.readAll(4);
            RUVIA_CHECK_EQ(retry.bytes().size(), std::size_t{3});
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{3});
        };
        runOperation(worker, io, operation);
        RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(client_result_budget_validation_and_lease_exception_rollback) {
    bool rejectedZeroLimit = false;
    try {
        (void)std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 0});
    } catch (const std::invalid_argument&) {
        rejectedZeroLimit = true;
    }
    RUVIA_CHECK(rejectedZeroLimit);

    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    bool rejectedAtClientStartup = false;
    try {
        ruvia::HttpClient client(worker.attachment.loop(), {.host = "example.test"},
            {.maxRetainedBytes = 0});
    } catch (const std::invalid_argument&) {
        rejectedAtClientStartup = true;
    }
    RUVIA_CHECK(rejectedAtClientStartup);

    auto domain = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 8});
    bool threw = false;
    try {
        auto reservation = ruvia::detail::HttpClientResultBudgetLease::tryAcquire(domain, 5);
        RUVIA_CHECK(reservation.has_value());
        throw std::bad_alloc{};
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(domain->retainedBytes(), std::size_t{0});
}

RUVIA_TEST(client_body_collection_cancellation_joins_before_storage_is_released) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        state.buffered.assign(256, 'x');
        const auto baseline = resource.liveAllocations();
        bool cancelled = false;
        bool pendingBeforeCancel = false;
        std::exception_ptr failure;
        auto operation = [&]() -> ruvia::Task<void> {
            try {
                (void)co_await ruvia::make_scoped_operation(state.bodyOperationScope, state.readAll(4096));
            } catch (const std::system_error& error) {
                cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
            }
        };
        asio::co_spawn(io, ruvia::asAwaitable(operation()),
            [&worker, &failure](std::exception_ptr error) {
                failure = error;
                worker.attachment.stop();
            });
        asio::post(io, [&] {
            pendingBeforeCancel = !cancelled;
            state.failure = std::make_exception_ptr(
                std::system_error(std::make_error_code(std::errc::operation_canceled)));
            state.complete = true;
            state.dataSignal.notify();
        });
        worker.attachment.run();
        io.restart();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        RUVIA_CHECK(pendingBeforeCancel);
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http_client_upload_exchange_owns_chunks_and_trailers_and_drives_continue) {
    for (const auto mode : {UploadPeer::Mode::kChunked, UploadPeer::Mode::kKnownLength,
             UploadPeer::Mode::kContinue, UploadPeer::Mode::kContinueTimeout}) {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        UploadPeer peer(io, worker.handle, mode);
        auto config = localHttpClientConfig(peer.port());
        config.requestTimeout = std::chrono::seconds(3);
        ruvia::HttpClient client(worker.attachment.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::Task<void> {
            const bool known = mode == UploadPeer::Mode::kKnownLength;
            const bool expectContinue = mode == UploadPeer::Mode::kContinue || mode == UploadPeer::Mode::kContinueTimeout;
            auto exchange = co_await client.openRequest({.method = "POST", .target = "/upload"},
                {.contentLength = known ? std::optional<std::uint64_t>{6} : std::nullopt,
                    .expectation = expectContinue ? ruvia::HttpClientRequestExpectation::kContinue : ruvia::HttpClientRequestExpectation::kNone,
                    .maxChunkBytes = 3,
                    .continueTimeout = std::chrono::milliseconds(20)});
            {
                auto cold = exchange.body().write("bad");
            }
            {
                auto cold = exchange.response();
            }
            bool tooLarge = false;
            try {
                auto rejected = exchange.body().write("four");
            } catch (const std::length_error&) {
                tooLarge = true;
            }
            RUVIA_CHECK(tooLarge);
            std::string input = "abc";
            auto first = exchange.body().write(input);
            input.assign("xxx");
            co_await std::move(first);
            auto moved = std::move(exchange);
            co_await moved.body().write("def");
            std::string trailerValue = "retained";
            const std::array<ruvia::HttpHeaderView, 1> fields{{{"X-End", trailerValue}}};
            if (known) {
                co_await moved.body().end();
            } else {
                auto ending = moved.body().end(fields);
                trailerValue.assign("changed");
                co_await std::move(ending);
            }
            RUVIA_CHECK(moved.body().complete());
            auto response = co_await moved.response();
            auto bytes = co_await response.body().readAll(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), "ok");
            RUVIA_CHECK_EQ(response.informationalResponses().size(), mode == UploadPeer::Mode::kContinue ? std::size_t{2} : std::size_t{0});
            if (mode == UploadPeer::Mode::kContinue) {
                RUVIA_CHECK_EQ(response.informationalResponses()[0].status().value(), std::uint16_t{103});
                RUVIA_CHECK_EQ(response.informationalResponses()[0].headers()[0].value(), "</asset>; rel=preload");
            }
            co_await peer.wait();
            RUVIA_CHECK_EQ(peer.body, known ? "abcdef" : "3\r\nabc\r\n3\r\ndef\r\n0\r\nx-end: retained\r\n\r\n");
            RUVIA_CHECK((peer.head.find("Expect: 100-continue") != std::string::npos) == expectContinue);
            co_await client.shutdown();
        };
        runOperation(worker, io, operation);
    }
}

RUVIA_TEST(http_client_upload_exchange_preserves_early_final_response_and_stops_upload) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    UploadPeer peer(io, worker.handle, UploadPeer::Mode::kEarlyFinal);
    std::optional<ruvia::HttpClient> client(std::in_place, worker.attachment.loop(), localHttpClientConfig(peer.port()));
    peer.start();
    auto operation = [&]() -> ruvia::Task<void> {
        auto exchange = co_await client->openRequest({.method = "POST", .target = "/upload"},
            {.expectation = ruvia::HttpClientRequestExpectation::kContinue});
        auto response = co_await exchange.response();
        RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{413});
        bool cancelled = false;
        try {
            co_await exchange.body().write("rejected");
        } catch (const ruvia::HttpClientError& error) {
            cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
        }
        RUVIA_CHECK(cancelled);
        auto bytes = co_await response.body().readAll(16);
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), "no");
        co_await peer.wait();
        co_await client->shutdown();
        client.reset();
        // The response domain keeps the exchange's output signals alive after
        // client destruction, including the stopped, unfinished upload path.
        RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{413});
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), "no");
    };
    runOperation(worker, io, operation);
}

RUVIA_TEST(http_client_http2_upload_exchange_flow_control_trailers_and_early_final) {
    for (const bool early : {false, true}) {
        auto& io = ruvia::test::newTestIoContext();
        TestWorker worker(io);
        Http2UploadPeer peer(io, worker.handle, early);
        auto config = localHttpClientConfig(peer.port());
        config.protocol = ruvia::HttpClientProtocol::kHttp2Only;
        config.requestTimeout = std::chrono::seconds(3);
        ruvia::HttpClient client(worker.attachment.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::Task<void> {
            constexpr std::size_t chunkSize = 64 * 1024;
            auto exchange = co_await client.openRequest({.method = "POST", .target = "/upload"},
                {.contentLength = chunkSize * 4, .expectation = ruvia::HttpClientRequestExpectation::kContinue});
            if (!early) {
                const std::string input(chunkSize, 'p');
                for (unsigned i = 0; i < 4; ++i) {
                    co_await exchange.body().write(input);
                }
                const std::array<ruvia::HttpHeaderView, 1> trailers{{{"x-end", "retained"}}};
                co_await exchange.body().end(trailers);
            }
            auto response = co_await exchange.response();
            RUVIA_CHECK_EQ(response.status().value(), early ? 413 : 200);
            auto bytes = co_await response.body().readAll(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), "ok");
            if (!early) {
                RUVIA_CHECK_EQ(peer.body.size(), chunkSize * 4);
                RUVIA_CHECK_EQ(peer.trailer, "retained");
                RUVIA_CHECK_EQ(response.informationalResponses().size(), std::size_t{2});
                if (response.informationalResponses().size() == 2) {
                    RUVIA_CHECK_EQ(response.informationalResponses()[0].headers()[0].value(), "</asset>; rel=preload");
                }
            } else {
                bool stopped = false;
                try {
                    co_await exchange.body().end();
                } catch (const ruvia::HttpClientError& error) {
                    stopped = error.code() == ruvia::HttpClientError::Code::kCancelled;
                }
                RUVIA_CHECK(stopped);
            }
            co_await client.shutdown();
            co_await peer.wait();
        };
        runOperation(worker, io, operation);
    }
}

namespace {
void exercise_tls_response_retirement(ruvia::testing::TestContext& ruvia_ctx, bool join_before_reuse) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::tls_identity identity("localhost");
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const std::string large(262163, 'b');
    const std::array<std::string_view, 2> expected{"fresh connection body", "healthy reused body"};
    unsigned connections = 0;
    unsigned requests = 0;
    bool cancelled_socket_closed = false;
    std::exception_ptr peer_failure;
    ruvia::WorkerSignal peer_done(worker.handle);
    const auto serve = [&]() -> asio::awaitable<void> {
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context);
            ++connections;
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(large.size()) + "\r\n\r\n" + large;
            std::error_code error;
            co_await asio::async_write(stream, asio::buffer(response), asio::redirect_error(asio::use_awaitable, error));
            std::array<char, 1024> bytes{};
            while (co_await stream.async_read_some(asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error))) {
            }
            cancelled_socket_closed = error == asio::error::eof || error == asio::error::connection_reset || error == asio::ssl::error::stream_truncated;
        }
        asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context);
        ++connections;
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        for (const auto body : expected) {
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    auto config = localHttpClientConfig(acceptor.local_endpoint().port());
    config.scheme = ruvia::HttpScheme::kHttps;
    config.host = "localhost";
    const auto ca = identity.ca_file.string();
    config.caFile = ca;
    config.maxResponseBytes = 8192;
    config.protocol = join_before_reuse ? ruvia::HttpClientProtocol::kHttp1Only : ruvia::HttpClientProtocol::kNegotiate;
    ruvia::HttpClient client(worker.attachment.loop(), config);
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::StopSource cancellation;
        {
            auto response = co_await client.withOptions({.stopToken = cancellation.token()}).send({.target = "/partial"});
            const auto first = co_await response.body().text();
            RUVIA_CHECK(first.has_value());
            RUVIA_CHECK(!first->empty());
            RUVIA_CHECK(first->size() < large.size());
            RUVIA_CHECK(std::ranges::all_of(*first, [](char value) { return value == 'b'; }));
            cancellation.requestStop();
            // Cancellation itself must not invalidate a previously returned borrow.
            RUVIA_CHECK(std::ranges::all_of(*first, [](char value) { return value == 'b'; }));
        }
        if (join_before_reuse) {
            while (client.stats().inFlightRequests != 0) {
                (void)co_await ruvia::asyncAsio([&io](auto handler) {
                    asio::post(io, [handler = std::move(handler)]() mutable { handler(std::error_code{}); });
                });
            }
            RUVIA_CHECK_EQ(client.stats().failedRequests, std::size_t{1});
        }
        for (const auto body : expected) {
            auto response = co_await client.send({.target = "/next?exact=body"});
            RUVIA_CHECK(response.protocolVersion() == ruvia::HttpProtocolVersion::kHttp11);
            const auto bytes = co_await response.body().readAll();
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), body);
            RUVIA_CHECK(!(co_await response.body().read()));
        }
        co_await peer_done.wait();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        RUVIA_CHECK_EQ(client.stats().failedRequests, std::size_t{1});
        co_await client.shutdown();
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK(cancelled_socket_closed);
    RUVIA_CHECK_EQ(connections, 2U);
    RUVIA_CHECK_EQ(requests, 3U);
}
}  // namespace

RUVIA_TEST(http1_tls_partial_response_cancellation_retires_transport_and_preserves_healthy_reuse) {
    exercise_tls_response_retirement(ruvia_ctx, true);
}

RUVIA_TEST(http1_tls_queued_reconnect_joins_cancelled_producer_before_replacing_transport) {
    exercise_tls_response_retirement(ruvia_ctx, false);
}
