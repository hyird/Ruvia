#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientPool.h"

#include <asio/error.hpp>
#include <asio/ssl/error.hpp>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "FetchStreamSource.h"
#include "HttpClientAccess.h"
#include "HttpClientDecodingStreamSource.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t kStreamReadChunk = 8192;
constexpr std::size_t kMaxChunkLineBytes = 1024;
constexpr std::size_t kMaxTrailerBytes = kMaxHttpHeaderBytes;

}  // namespace

// Pull-based HTTP/1.1 response-body source. It owns the pooled connection (via a moved
// ConnectionGuard) for the stream's lifetime and decodes the body per its framing on each
// readChunk(). A cleanly finished body releases the connection for reuse; anything else discards
// it. Content-Encoding is NOT decoded here (the caller receives the raw transfer bytes).
class Http1StreamSource final : public FetchStreamSource {
public:
    enum class Framing : std::uint8_t { kContentLength, kChunked, kClose };

    Http1StreamSource(
        HttpClientPool* pool,
        HttpClientPool::ConnectionGuard guard,
        std::uint16_t status,
        std::pmr::vector<FetchResponseHeader> headers,
        Framing framing,
        std::size_t contentLength,
        bool closeAfterResponse,
        std::pmr::string leftover,
        std::chrono::milliseconds idleTimeout,
        std::pmr::memory_resource* resource)
        : pool_(pool),
          guard_(std::move(guard)),
          headers_(std::move(headers)),
          buffer_(std::move(leftover)),
          resource_(resource),
          idleTimeout_(idleTimeout),
          clRemaining_(contentLength),
          framing_(framing),
          status_(status),
          closeAfterResponse_(closeAfterResponse) {}

    ~Http1StreamSource() override { finish(false); }

    [[nodiscard]] std::uint16_t status() const noexcept override { return status_; }
    [[nodiscard]] const std::pmr::vector<FetchResponseHeader>& headers() const noexcept override {
        return headers_;
    }
    [[nodiscard]] Task<std::pmr::string> readChunk() override {
        switch (framing_) {
            case Framing::kContentLength:
                return readContentLength();
            case Framing::kChunked:
                return readChunked();
            case Framing::kClose:
                return readClose();
        }
        return readContentLength();
    }
    void close() noexcept override { finish(false); }
    void destroy() noexcept override { destroyPmrObject(this, resource_); }

private:
    [[nodiscard]] std::pmr::string empty() const { return std::pmr::string(resource_); }

    // Release the connection exactly once: reusable → back to the pool; otherwise discarded.
    void finish(bool reusable) noexcept {
        if (finished_) {
            return;
        }
        finished_ = true;
        reusable = reusable && !closeAfterResponse_;
        if (guard_) {
            if (!reusable) {
                guard_->discard();
            }
            guard_.reset();
        }
    }

    // Append more socket bytes to buffer_. Returns bytes read (0 on EOF/error/timeout).
    Task<std::size_t> readMore(std::error_code& ecOut) {
        auto& conn = guard_->connection();
        const auto oldSize = buffer_.size();
        resizePmrStringForOverwrite(buffer_, oldSize + kStreamReadChunk);
        pool_->setDeadline(conn, idleTimeout_, HttpClientPool::Connection::DeadlineKind::kSocket);
        auto [ec, n] = co_await pool_->connReadSome(
            conn, asio::buffer(buffer_.data() + oldSize, kStreamReadChunk));
        const bool timedOut = pool_->finishDeadline(conn);
        buffer_.resize(oldSize + ((ec || n == 0) ? 0 : n));
        ecOut = timedOut ? asio::error::make_error_code(asio::error::timed_out) : ec;
        co_return timedOut ? 0 : n;
    }

    Task<std::pmr::string> readContentLength() {
        if (finished_) {
            co_return empty();  // already ended or closed; the connection is released
        }
        if (clRemaining_ == 0) {
            finish(/*reusable=*/buffer_.empty());
            co_return empty();
        }
        if (buffer_.empty()) {
            std::error_code ec;
            const auto n = co_await readMore(ec);
            if (n == 0) {
                finish(false);
                throw std::runtime_error("http client: truncated response body");
            }
        }
        const auto take = std::min(buffer_.size(), clRemaining_);
        std::pmr::string chunk(resource_);
        chunk.assign(buffer_.data(), take);
        buffer_.erase(0, take);
        clRemaining_ -= take;
        if (clRemaining_ == 0) {
            finish(/*reusable=*/buffer_.empty());  // leftover bytes = pipelined/garbage → discard
        }
        co_return chunk;
    }

    Task<std::pmr::string> readClose() {
        if (finished_) {
            co_return empty();
        }
        if (buffer_.empty()) {
            std::error_code ec;
            const auto n = co_await readMore(ec);
            if (n == 0) {
                finish(false);
                // Only a clean peer close (TCP EOF / TLS truncation) is the normal
                // end of a close-delimited body. readMore also returns 0 on an idle
                // timeout (ec == timed_out) or any other transport error, which
                // truncated the body -- surface that instead of reporting a short
                // body as complete, matching the buffered reader
                // (HttpClientPool::readCloseDelimitedResponseBody) and the sibling
                // Content-Length/chunked framings here, which all throw on a short read.
                if (ec && ec != asio::error::eof && ec != asio::ssl::error::stream_truncated) {
                    throw std::runtime_error("http client: truncated response body");
                }
                co_return empty();
            }
        }
        std::pmr::string chunk(resource_);
        chunk.swap(buffer_);
        co_return chunk;
    }

    Task<std::pmr::string> readChunked() {
        if (finished_ || chunkDone_) {
            co_return empty();  // ended or closed; the connection is released
        }
        if (needChunkCrlf_) {
            if (!co_await consumeCrlf()) {
                finish(false);
                throw std::runtime_error("http client: malformed chunk terminator");
            }
            needChunkCrlf_ = false;
        }
        if (chunkRemaining_ == 0) {
            const auto line = co_await readLine();
            auto sizeToken = std::string_view(line);
            if (const auto semi = sizeToken.find(';'); semi != std::string_view::npos) {
                sizeToken = sizeToken.substr(0, semi);
            }
            sizeToken = httpTrimOws(sizeToken);
            std::size_t chunkSize = 0;
            const auto [ptr, ec] = std::from_chars(
                sizeToken.data(), sizeToken.data() + sizeToken.size(), chunkSize, 16);
            if (sizeToken.empty() || ec != std::errc{} || ptr != sizeToken.data() + sizeToken.size()) {
                finish(false);
                throw std::runtime_error("http client: malformed chunk size");
            }
            if (chunkSize == 0) {
                co_await drainTrailers();
                chunkDone_ = true;
                // Reuse only if nothing trails the terminator (else it's pipelined/garbage).
                finish(/*reusable=*/buffer_.empty());
                co_return empty();
            }
            chunkRemaining_ = chunkSize;
        }
        if (buffer_.empty()) {
            std::error_code ec;
            const auto n = co_await readMore(ec);
            if (n == 0) {
                finish(false);
                throw std::runtime_error("http client: truncated chunked response");
            }
        }
        const auto take = std::min(buffer_.size(), chunkRemaining_);
        std::pmr::string chunk(resource_);
        chunk.assign(buffer_.data(), take);
        buffer_.erase(0, take);
        chunkRemaining_ -= take;
        if (chunkRemaining_ == 0) {
            needChunkCrlf_ = true;  // the chunk data is followed by its own CRLF
        }
        co_return chunk;
    }

    // Return the next CRLF-terminated line (without the CRLF), consuming it from buffer_.
    Task<std::pmr::string> readLine() {
        for (;;) {
            const auto nl = buffer_.find("\r\n");
            if (nl != std::pmr::string::npos) {
                std::pmr::string line(resource_);
                line.assign(buffer_.data(), nl);
                buffer_.erase(0, nl + 2);
                co_return line;
            }
            if (buffer_.size() > kMaxChunkLineBytes) {
                finish(false);
                throw std::runtime_error("http client: chunked line too long");
            }
            std::error_code ec;
            const auto n = co_await readMore(ec);
            if (n == 0) {
                finish(false);
                throw std::runtime_error("http client: truncated chunked response");
            }
        }
    }

    Task<bool> consumeCrlf() {
        while (buffer_.size() < 2) {
            std::error_code ec;
            const auto n = co_await readMore(ec);
            if (n == 0) {
                co_return false;
            }
        }
        if (buffer_[0] != '\r' || buffer_[1] != '\n') {
            co_return false;
        }
        buffer_.erase(0, 2);
        co_return true;
    }

    Task<void> drainTrailers() {
        std::size_t total = 0;
        for (;;) {
            const auto line = co_await readLine();
            if (line.empty()) {
                co_return;
            }
            total += line.size() + 2;
            if (total > kMaxTrailerBytes) {
                finish(false);
                throw std::runtime_error("http client: chunked trailers too large");
            }
        }
    }

    HttpClientPool* pool_;
    std::optional<HttpClientPool::ConnectionGuard> guard_;
    std::pmr::vector<FetchResponseHeader> headers_;
    std::pmr::string buffer_;   // unconsumed raw transfer bytes (leftover from headers + reads)
    std::pmr::memory_resource* resource_;
    std::chrono::milliseconds idleTimeout_;
    std::size_t clRemaining_{0};
    std::size_t chunkRemaining_{0};
    Framing framing_;
    std::uint16_t status_;
    bool finished_{false};
    bool closeAfterResponse_{false};
    bool chunkDone_{false};
    bool needChunkCrlf_{false};
};

Task<FetchResponseStream> HttpClientPool::fetchStream(
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* resource) {
    if (closing_) {
        throw std::runtime_error("http client pool is closed");
    }
    if (options.timeout.count() < 0) {
        throw std::invalid_argument("http client request timeout must not be negative");
    }
    auto* const requestResource = resource == nullptr ? resource_ : resource;
    // nginx-style inactivity timeouts; FetchOptions::timeout overrides them for this request.
    const auto readTimeout = options.timeout.count() > 0 ? options.timeout : config_.proxyReadTimeout;
    const auto sendTimeout = options.timeout.count() > 0 ? options.timeout : config_.proxySendTimeout;

    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    FetchResponse response = FetchResponseAccess::make(requestResource);
    HttpClientResponseHead head;
    try {
        auto& conn = guard.connection();
        if (!conn.connected) {
            co_await connectOne(conn);
        }
        head = co_await writeRequestAndReadHead(
            conn, path, options, readTimeout, sendTimeout, response, requestResource);
    } catch (...) {
        guard.discard();
        throw;
    }

    auto& readBuf = guard.connection().responseReadBuffer;
    std::pmr::string leftover(requestResource);
    if (readBuf.size() > head.bodyOffset) {
        leftover.assign(readBuf.data() + head.bodyOffset, readBuf.size() - head.bodyOffset);
    }

    Http1StreamSource::Framing framing = Http1StreamSource::Framing::kContentLength;
    std::size_t contentLength = 0;
    if (!head.responseMayHaveBody) {
        framing = Http1StreamSource::Framing::kContentLength;  // empty body
    } else if (head.hasTransferEncoding) {
        if (!head.isChunked) {
            guard.discard();
            throw std::runtime_error("http client: unsupported response Transfer-Encoding");
        }
        framing = Http1StreamSource::Framing::kChunked;
    } else if (head.hasContentLength) {
        framing = Http1StreamSource::Framing::kContentLength;
        contentLength = head.contentLength;
    } else {
        framing = Http1StreamSource::Framing::kClose;
    }

    auto* source = constructPmrObject<Http1StreamSource>(
        requestResource, this, std::move(guard), response.status(),
        std::move(FetchResponseAccess::headers(response)), framing, contentLength, head.closeAfterResponse,
        std::move(leftover), readTimeout, requestResource);
    std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter> stream(source);
    stream = maybeWrapDecodingStreamSource(
        std::move(stream), source->headers(), options.decodeStream, requestResource);
    co_return FetchResponseStreamAccess::make(std::move(stream));
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
