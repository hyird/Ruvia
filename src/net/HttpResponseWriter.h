#pragma once

#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <asio.hpp>

#include "ConnectionScanner.h"
#include "HttpDateCache.h"
#include "../AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

constexpr std::size_t kResponseHeadStackBytes = 512;
constexpr std::size_t kResponseHeadRetainedHeapBytes = 4 * 1024;
constexpr std::size_t kFileChunkBytes = 64 * 1024;

class ResponseHeadBuffer final {
public:
    explicit ResponseHeadBuffer(std::pmr::polymorphic_allocator<char> allocator) : heap_(allocator) {}

    void reset() noexcept;
    void append(std::string_view value);
    void append(char value);
    void appendUnsigned(std::uint64_t value);
    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] bool canAppendOnStack(std::size_t size) const noexcept;

    // Bulk fast path: returns a raw cursor when `bound` bytes are guaranteed to
    // fit in the stack buffer, so callers can emit without per-append checks.
    [[nodiscard]] char* stackCursor(std::size_t bound) noexcept {
        if (overflowed_ || bound > stack_.size() - used_) {
            return nullptr;
        }
        return stack_.data() + used_;
    }

    void commitStack(const char* end) noexcept {
        used_ = static_cast<std::size_t>(end - stack_.data());
    }

private:
    std::array<char, kResponseHeadStackBytes> stack_{};
    std::pmr::string heap_;
    std::size_t used_{0};
    bool overflowed_{false};
};

struct ResponseWritePolicy {
    bool bodyAllowed{true};
    bool autoContentLengthAllowed{true};
    bool explicitContentLengthAllowed{true};
    bool transferEncodingAllowed{true};
};

[[nodiscard]] ResponseWritePolicy responseWritePolicy(std::uint16_t statusCode) noexcept;
void appendResponseHead(
    const HttpResponse& response,
    ResponseHeadBuffer& head,
    ResponseWritePolicy policy,
    bool suppressAutoContentLength = false);
bool compressResponseBodyIfAccepted(
    const HttpRequestFlags& requestFlags,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    bool skipBody = false);
Task<void> writeFileZeroCopy(asio::ip::tcp::socket& socket, const FileBody& file, std::error_code& ec);

template <typename Stream>
Task<void> writeResponse(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer* reusableHead,
    std::pmr::vector<char>* fileChunkBuffer,
    const HttpResponse& response,
    bool skipBody,
    std::error_code& ec) {
    ResponseHeadBuffer localHead(memory.allocator<char>());
    auto& head = reusableHead == nullptr ? localHead : *reusableHead;
    head.reset();
    const auto policy = responseWritePolicy(response.statusCode());
    appendResponseHead(response, head, policy);
    if (response.hasFileBody()) {
        const auto& fileBody = response.fileBody();
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        if (ec || skipBody || !policy.bodyAllowed || fileBody.length == 0) {
            co_return;
        }

        if constexpr (std::is_same_v<std::remove_cvref_t<Stream>, asio::ip::tcp::socket>) {
            co_await writeFileZeroCopy(stream, fileBody, ec);
            if (ec != asio::error::operation_not_supported) {
                co_return;
            }
        }

#if defined(ASIO_HAS_FILE)
        asio::stream_file input(stream.get_executor());
        input.open(fileTokenPath(fileBody.file).string(), asio::stream_file::read_only, ec);
        if (ec) {
            ec = asio::error::operation_aborted;
            co_return;
        }
        input.seek(static_cast<std::int64_t>(fileBody.offset), asio::stream_file::seek_set, ec);
        if (ec) {
            ec = asio::error::operation_aborted;
            co_return;
        }

        std::pmr::vector<char> localChunk(memory.allocator<char>());
        auto& chunk = fileChunkBuffer == nullptr ? localChunk : *fileChunkBuffer;
        if (chunk.size() < kFileChunkBytes) {
            chunk.resize(kFileChunkBytes);
        }
        std::uint64_t remaining = fileBody.length;
        while (remaining > 0) {
            const auto nextRead = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
            auto [readEc, read] = co_await asyncResult<std::size_t>([&input, &chunk, nextRead](auto handler) mutable {
                input.async_read_some(asio::buffer(chunk.data(), nextRead), std::move(handler));
            });
            ec = readEc;
            if (ec || read == 0) {
                break;
            }
            remaining -= read;
            ec = co_await asyncError([&stream, &chunk, read](auto handler) mutable {
                asio::async_write(stream, asio::buffer(chunk.data(), read), std::move(handler));
            });
            if (ec) {
                co_return;
            }
        }
#else
        std::ifstream input(fileTokenPath(fileBody.file), std::ios::binary);
        if (!input) {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
            co_return;
        }
        input.seekg(static_cast<std::streamoff>(fileBody.offset), std::ios::beg);
        if (!input) {
            ec = std::make_error_code(std::errc::invalid_seek);
            co_return;
        }

        std::pmr::vector<char> localChunk(memory.allocator<char>());
        auto& chunk = fileChunkBuffer == nullptr ? localChunk : *fileChunkBuffer;
        if (chunk.size() < kFileChunkBytes) {
            chunk.resize(kFileChunkBytes);
        }
        std::uint64_t remaining = fileBody.length;
        while (remaining > 0) {
            const auto nextRead = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
            input.read(chunk.data(), static_cast<std::streamsize>(nextRead));
            const auto read = input.gcount();
            if (read <= 0) {
                ec = std::make_error_code(std::errc::io_error);
                co_return;
            }
            remaining -= static_cast<std::uint64_t>(read);
            ec = co_await asyncError([&stream, &chunk, read](auto handler) mutable {
                asio::async_write(stream, asio::buffer(chunk.data(), static_cast<std::size_t>(read)), std::move(handler));
            });
            if (ec) {
                co_return;
            }
        }
        ec = {};
#endif
        co_return;
    }

    const auto body = skipBody || !policy.bodyAllowed ? std::string_view{} : response.bodyBytes();
    if (body.empty()) {
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        co_return;
    }
    if (head.canAppendOnStack(body.size())) {
        head.append(body);
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        co_return;
    }
    const auto headView = head.view();
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(headView), asio::buffer(body)};
    ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
}

template <typename Stream, typename ScannerEntry>
class ResponseStreamSink final {
public:
    ResponseStreamSink(Stream& stream, WorkerMemory& memory, ResponseHeadBuffer& head, ScannerEntry& scannerEntry, ResponseBodyMode mode) noexcept
        : stream_(stream), memory_(memory), head_(head), scannerEntry_(scannerEntry), mode_(mode) {}
    [[nodiscard]] bool committed() const noexcept { return committed_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    static Task<void> writeThunk(void* target, std::string_view chunk) { co_await static_cast<ResponseStreamSink*>(target)->write(chunk); }
    static Task<void> endThunk(void* target) { co_await static_cast<ResponseStreamSink*>(target)->end(); }
    static void bindContextThunk(void* target, Context* context) noexcept { static_cast<ResponseStreamSink*>(target)->context_ = context; }
    static std::pmr::string& scratchThunk(void* target) noexcept { return static_cast<ResponseStreamSink*>(target)->scratch(); }

private:
    [[nodiscard]] std::pmr::string& scratch() noexcept { scratch_.clear(); return scratch_; }
    Task<void> commit() {
        if (committed_) { co_return; }
        if (ended_) { throw std::logic_error("response stream is already ended"); }
        if (context_ == nullptr) { throw std::logic_error("response stream context is not bound"); }
        auto response = context_->streamingHead(mode_ == ResponseBodyMode::kSse ? "text/event-stream" : std::string_view{});
        const auto policy = responseWritePolicy(response.statusCode());
        bodyForbidden_ = !policy.bodyAllowed;
        if (policy.transferEncodingAllowed) {
            response.setHeader("Transfer-Encoding", "chunked");
            if (mode_ == ResponseBodyMode::kSse) { response.setHeader("Cache-Control", "no-store"); }
        }
        head_.reset();
        appendResponseHead(response, head_, policy, true);
        // Mark committed BEFORE the write. Otherwise a mid-write failure
        // (broken pipe after partial header bytes already flushed) leaves
        // committed_=false and the server's exception path would emit a
        // second response head onto the same socket — framing corruption.
        committed_ = true;
        auto ec = co_await asyncError([this, headView = head_.view()](auto handler) mutable {
            asio::async_write(stream_, asio::buffer(headView), std::move(handler));
        });
        if (ec) { failed_ = true; throw std::system_error(ec); }
        scannerEntry_.touch();
    }
    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) { co_return; }
        co_await commit();
        if (bodyForbidden_) { throw std::logic_error("response status does not allow a stream body"); }
        std::array<char, 32> sizeBuffer{};
        const auto [ptr, ec] = std::to_chars(sizeBuffer.data(), sizeBuffer.data() + sizeBuffer.size(), chunk.size(), 16);
        if (ec != std::errc{}) { throw std::logic_error("failed to format response stream chunk size"); }
        const auto size = std::string_view(sizeBuffer.data(), static_cast<std::size_t>(ptr - sizeBuffer.data()));
        constexpr std::string_view crlf = "\r\n";
        const std::array<asio::const_buffer, 4> buffers{asio::buffer(size), asio::buffer(crlf), asio::buffer(chunk), asio::buffer(crlf)};
        const auto writeEc = co_await asyncError([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        if (writeEc) { failed_ = true; throw std::system_error(writeEc); }
        scannerEntry_.touch();
    }
    Task<void> end() {
        if (ended_) { co_return; }
        co_await commit();
        if (bodyForbidden_) {
            ended_ = true;
            co_return;
        }
        constexpr std::string_view finalChunk = "0\r\n\r\n";
        const auto ec = co_await asyncError([this, finalChunk](auto handler) mutable {
            asio::async_write(stream_, asio::buffer(finalChunk), std::move(handler));
        });
        ended_ = true;
        if (ec) { failed_ = true; throw std::system_error(ec); }
        scannerEntry_.touch();
    }

    Stream& stream_;
    WorkerMemory& memory_;
    ResponseHeadBuffer& head_;
    std::pmr::string scratch_{memory_.resource()};
    ScannerEntry& scannerEntry_;
    Context* context_{nullptr};
    ResponseBodyMode mode_;
    bool committed_{false};
    bool ended_{false};
    bool bodyForbidden_{false};
    bool failed_{false};
};

}  // namespace ruvia::detail
