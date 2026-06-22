#pragma once

#include "../../runtime/AsioAwait.h"

#include "HttpResponseHead.h"
#include "HttpResponseStreamHead.h"
#include "HttpResponseTrailers.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/memory/MemoryPool.h"

#include <array>
#include <charconv>
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
        ResponseBodyMode mode) noexcept
        : stream_(stream),
          head_(head),
          scratch_(memory.resource()),
          trailers_(memory.resource()),
          scannerEntry_(scannerEntry),
          mode_(mode) {}

    [[nodiscard]] bool committed() const noexcept { return committed_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    static Task<void> writeThunk(void* target, std::string_view chunk) {
        co_await static_cast<ResponseStreamSink*>(target)->write(chunk);
    }

    static Task<void> endThunk(void* target) {
        co_await static_cast<ResponseStreamSink*>(target)->end();
    }

    static void addTrailerThunk(void* target, std::string_view name, std::string_view value) {
        static_cast<ResponseStreamSink*>(target)->addTrailer(name, value);
    }

    static void bindContextThunk(void* target, Context* context) noexcept {
        static_cast<ResponseStreamSink*>(target)->context_ = context;
    }

    static std::pmr::string& scratchThunk(void* target) noexcept {
        return static_cast<ResponseStreamSink*>(target)->scratch();
    }

private:
    [[nodiscard]] std::pmr::string& scratch() noexcept {
        clearPmrStringRetainingSmall(scratch_);
        return scratch_;
    }

    Task<void> commit() {
        if (committed_) {
            co_return;
        }
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
        if (context_ == nullptr) {
            throw std::logic_error("response stream context is not bound");
        }

        auto streamHead = prepareResponseStreamHead(*context_, mode_, ResponseStreamFraming::kHttp1Chunked);
        bodyForbidden_ = streamHead.bodyForbidden;

        head_.reset();
        appendResponseHead(streamHead.response, head_, streamHead.policy, true);
        // Mark committed before the write; a partial header flush must never be
        // followed by the normal error-response path on the same socket.
        committed_ = true;
        auto ec = co_await asyncError([this, headView = head_.view()](auto handler) mutable {
            asio::async_write(stream_, asio::buffer(headView), std::move(handler));
        });
        if (ec) {
            failed_ = true;
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit();
        if (bodyForbidden_) {
            throw std::logic_error("response status does not allow a stream body");
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
            failed_ = true;
            throw std::system_error(writeEc);
        }
        scannerEntry_.touch();
    }

    // RFC 9110 §6.5 trailer section: queued before the stream ends and flushed
    // here as chunked trailer fields. The value was validated for CR/LF/NUL on
    // entry, so it is safe to write verbatim.
    void addTrailer(std::string_view name, std::string_view value) {
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
        if (!responseTrailerFieldValid(name, value)) {
            throw std::invalid_argument("invalid response trailer field");
        }
        trailers_.append(name.data(), name.size());
        trailers_.append(": ");
        trailers_.append(value.data(), value.size());
        trailers_.append("\r\n");
    }

    Task<void> end() {
        if (ended_) {
            co_return;
        }
        co_await commit();
        if (bodyForbidden_) {
            ended_ = true;
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
        ended_ = true;
        if (ec) {
            failed_ = true;
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Stream& stream_;
    ResponseHeadBuffer& head_;
    std::pmr::string scratch_;
    std::pmr::string trailers_;
    ScannerEntry& scannerEntry_;
    Context* context_{nullptr};
    ResponseBodyMode mode_;
    bool committed_{false};
    bool ended_{false};
    bool bodyForbidden_{false};
    bool failed_{false};
};

}  // namespace ruvia::detail
