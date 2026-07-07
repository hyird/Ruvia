#pragma once

#include "runtime/AsioAwait.h"

#include "HttpResponseHead.h"
#include "HttpResponseStreamHead.h"
#include "HttpResponseStreamState.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/memory/MemoryPool.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

namespace ruvia::detail {

template <typename Stream, typename ScannerEntry>
class ResponseStreamSink final {
public:
    ResponseStreamSink(
        Stream& stream,
        WorkerMemory& memory,
        ResponseHeadBuffer& head,
        ScannerEntry& scannerEntry,
        ResponseBodyMode mode,
        ResponseStreamFraming framing,
        bool connectionWillClose) noexcept
        : stream_(stream),
          head_(head),
          scratch_(memory.resource()),
          trailers_(memory.resource()),
          scannerEntry_(scannerEntry),
          mode_(mode),
          framing_(framing),
          connectionWillClose_(connectionWillClose) {}

    [[nodiscard]] bool committed() const noexcept { return state_.committed(); }

    [[nodiscard]] bool aborted() const noexcept { return aborted_; }

    template <typename Sink>
    friend Task<void> responseStreamWriteThunk(void*, std::string_view);
    template <typename Sink>
    friend Task<void> responseStreamEndThunk(void*);
    template <typename Sink>
    friend Task<void> responseStreamSleepThunk(void*, std::chrono::milliseconds);
    template <typename Sink>
    friend void responseStreamAddTrailerThunk(void*, std::string_view, std::string_view);
    template <typename Sink>
    friend void responseStreamBindContextThunk(void*, Context*) noexcept;
    template <typename Sink>
    friend std::pmr::string& responseStreamScratchThunk(void*) noexcept;

private:
    void bindContext(Context* context) noexcept {
        state_.bindContext(context);
    }

    [[nodiscard]] std::pmr::string& scratch() noexcept {
        clearPmrStringRetainingSmall(scratch_);
        return scratch_;
    }

    Task<void> commit() {
        if (state_.committed()) {
            co_return;
        }

        auto streamHead = prepareResponseStreamHead(
            state_.requireContextBeforeCommit(),
            mode_,
            framing_,
            connectionWillClose_);

        head_.reset();
        appendResponseHead(streamHead.response(), head_, streamHead.policy(), true);
        // Mark committed before the write; a partial header flush must never be
        // followed by the normal error-response path on the same socket.
        state_.markCommitted(streamHead.bodyForbidden());
        auto ec = co_await asyncError([this, headView = head_.view()](auto handler) mutable {
            asio::async_write(stream_, asio::buffer(headView), std::move(handler));
        });
        if (ec) {
            aborted_ = true;
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Task<void> sleep(std::chrono::milliseconds duration) {
        asio::steady_timer timer(stream_.get_executor(), duration);
        const auto ec = co_await asyncError([&timer](auto handler) mutable {
            timer.async_wait(std::move(handler));
        });
        if (ec) {
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit();
        state_.ensureBodyAllowed();

        if (framing_ == ResponseStreamFraming::kHttp1CloseDelimited) {
            // No chunk framing: write the raw body bytes. The connection close
            // (forced once the stream ends) is what delimits the message.
            const auto rawEc = co_await asyncError([this, chunk](auto handler) mutable {
                asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
            });
            if (rawEc) {
                aborted_ = true;
                throw std::system_error(rawEc);
            }
            scannerEntry_.touch();
            co_return;
        }

        std::array<char, 32> sizeBuffer;
        const auto [ptr, ec] = std::to_chars(sizeBuffer.data(), sizeBuffer.data() + sizeBuffer.size(), chunk.size(), 16);
        if (ec != std::errc{}) {
            throw std::logic_error("failed to format response stream chunk size");
        }
        const auto size = std::string_view(sizeBuffer.data(), static_cast<std::size_t>(ptr - sizeBuffer.data()));
        constexpr std::string_view crlf = "\r\n";
        const std::array<asio::const_buffer, 4> buffers{
            asio::buffer(size),
            asio::buffer(crlf),
            asio::buffer(chunk),
            asio::buffer(crlf)};
        const auto writeEc = co_await asyncError([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        if (writeEc) {
            aborted_ = true;
            throw std::system_error(writeEc);
        }
        scannerEntry_.touch();
    }

    // RFC 9110 Section 6.5 trailers are queued before the stream ends and
    // flushed here as chunked trailer fields. The value was validated for
    // CR/LF/NUL on entry, so it is safe to write verbatim.
    void addTrailer(std::string_view name, std::string_view value) {
        state_.ensureTrailerAllowed(name, value);
        trailers_.append(name.data(), name.size());
        trailers_.append(": ");
        trailers_.append(value.data(), value.size());
        trailers_.append("\r\n");
    }

    Task<void> end() {
        if (state_.ended()) {
            co_return;
        }
        co_await commit();
        if (state_.bodyForbidden()) {
            state_.markEnded();
            co_return;
        }
        if (framing_ == ResponseStreamFraming::kHttp1CloseDelimited) {
            // No last-chunk terminator: the connection close delimits the body.
            // Trailers require chunked framing (RFC 9110 6.5), which a close-
            // delimited HTTP/1.0 response cannot carry, so any queued trailer is
            // undeliverable and dropped here.
            state_.markEnded();
            co_return;
        }

        // Last-chunk, then the (possibly empty) trailer section, then the
        // closing CRLF. With no trailers this is exactly "0\r\n\r\n".
        constexpr std::string_view lastChunk = "0\r\n";
        constexpr std::string_view crlf = "\r\n";
        const std::array<asio::const_buffer, 3> buffers{
            asio::buffer(lastChunk),
            asio::buffer(trailers_),
            asio::buffer(crlf)};
        const auto ec = co_await asyncError([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        state_.markEnded();
        if (ec) {
            aborted_ = true;
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Stream& stream_;
    ResponseHeadBuffer& head_;
    std::pmr::string scratch_;
    std::pmr::string trailers_;
    ScannerEntry& scannerEntry_;
    ResponseBodyMode mode_;
    ResponseStreamFraming framing_;
    bool connectionWillClose_;
    ResponseStreamState state_;
    bool aborted_{false};
};

}  // namespace ruvia::detail
